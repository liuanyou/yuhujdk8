/*
 * Copyright (c) 1999, 2013, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2008, 2009, 2010 Red Hat, Inc.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "ci/ciMethod.hpp"
#include "memory/resourceArea.hpp"
#include "oops/method.hpp"
#include "runtime/os.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/align.hpp"
#include "runtime/synchronizer.hpp"
#include "runtime/thread.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/sharedRuntime.hpp"
#include "interpreter/interpreter.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/llvmValue.hpp"
#include "yuhu/yuhuBuilder.hpp"
#include <dlfcn.h>
#include "yuhu/yuhuContext.hpp"
#include "yuhu/yuhuFunction.hpp"
#include "yuhu/yuhuDebugInformationRecorder.hpp"
#include "yuhu/yuhuPrologueAnalyzer.hpp"
#include "yuhu/yuhuRuntime.hpp"
#include "yuhu/yuhu_globals.hpp"
#include "yuhu/yuhuAddressPatcher.hpp"
#include "utilities/debug.hpp"
#include "memory/universe.hpp"
#include "oops/oop.inline.hpp"
#include "classfile/javaClasses.hpp"
#include "oops/typeArrayOop.hpp"

using namespace llvm;

YuhuBuilder::YuhuBuilder(YuhuCodeBuffer* code_buffer, YuhuFunction* function)
  : IRBuilder<>(YuhuContext::current()),
    _code_buffer(code_buffer),
    _function(function) {
}

// Helpers for accessing structures
// Use llvm::Type explicitly to avoid conflict with HotSpot's Type class
Value* YuhuBuilder::CreateAddressOfStructEntry(Value*      base,
                                                ByteSize    offset,
                                                llvm::Type* type,
                                                const char* name) {
  // GC-managed oops (addrspace 1) must be addressed with a GEP so the result
  // stays a derived pointer that RS4GC's liveness can track and relocate.
  // ptrtoint/inttoptr addressing strands the address in an integer, which is
  // invisible to RS4GC and ill-typed under the ni:1 DataLayout.
  llvm::PointerType* base_ptr_type = llvm::dyn_cast<llvm::PointerType>(base->getType());
  if (base_ptr_type != NULL && base_ptr_type->getAddressSpace() == 1) {
    return CreateGEP(
      YuhuType::jbyte_type(), base,
      LLVMValue::intptr_constant(in_bytes(offset)), name);
  }

  // Non-GC pointers (Thread*, Method*, ...): keep integer addressing.
  // LLVM 20+ uses opaque pointer types, so we can't get element type from PointerType
  // Calculate address as: base_as_int + offset
  Value* base_int = CreatePtrToInt(base, YuhuType::intptr_type());
  Value* byte_offset = LLVMValue::intptr_constant(in_bytes(offset));
  Value* result_int = CreateAdd(base_int, byte_offset, "field_addr_int");
  Value* result_ptr = CreateIntToPtr(result_int, type, name);
  return result_ptr;
}

LoadInst* YuhuBuilder::CreateValueOfStructEntry(Value*      base,
                                                 ByteSize    offset,
                                                 llvm::Type* type,
                                                 const char* name) {
  // DEBUG: Check DataLayout availability before CreateLoad
  // This is where the crash happens, so we check everything step by step
  static bool debug_printed = false;
  if (!debug_printed) {
    llvm::BasicBlock* bb = GetInsertBlock();
    if (bb) {
      llvm::Function* func = bb->getParent();
      if (func) {
        llvm::Module* mod = func->getParent();
        if (mod) {
          // Directly access DataLayout - this might crash if DataLayout is invalid
          const llvm::DataLayout& dl = mod->getDataLayout();
          std::string dlStr = dl.getStringRepresentation();
          debug_printed = true;
        } else {
          tty->print_cr("Yuhu: ERROR - Function has no parent Module!");
          fatal("Function has no parent Module");
        }
      } else {
        tty->print_cr("Yuhu: ERROR - BasicBlock has no parent Function!");
        fatal("BasicBlock has no parent Function");
      }
    } else {
      tty->print_cr("Yuhu: ERROR - IRBuilder has no insert block!");
      fatal("IRBuilder has no insert block");
    }
  }
  
  // LLVM 20+ requires explicit type parameter for CreateLoad
  return CreateLoad(
    type,
    CreateAddressOfStructEntry(
      base, offset, PointerType::getUnqual(type)),
    name);
}

// Helpers for accessing arrays

LoadInst* YuhuBuilder::CreateArrayLength(Value* arrayoop) {
  return CreateValueOfStructEntry(
    arrayoop, in_ByteSize(arrayOopDesc::length_offset_in_bytes()),
    YuhuType::jint_type(), "length");
}

Value* YuhuBuilder::CreateArrayAddress(Value*      arrayoop,
                                        llvm::Type* element_type,
                                        int         element_bytes,
                                        ByteSize    base_offset,
                                        Value*      index,
                                        const char* name) {
  Value* offset = CreateIntCast(index, YuhuType::intptr_type(), false);
  if (element_bytes != 1)
    offset = CreateShl(
      offset,
      LLVMValue::intptr_constant(exact_log2(element_bytes)));
  offset = CreateAdd(
    LLVMValue::intptr_constant(in_bytes(base_offset)), offset);

  // Same rule as CreateAddressOfStructEntry: element addresses of GC-managed
  // arrays must remain derived pointers (GEP), never integers.
  llvm::PointerType* array_ptr_type = llvm::dyn_cast<llvm::PointerType>(arrayoop->getType());
  if (array_ptr_type != NULL && array_ptr_type->getAddressSpace() == 1) {
    return CreateGEP(YuhuType::jbyte_type(), arrayoop, offset, name);
  }

  return CreateIntToPtr(
    CreateAdd(CreatePtrToInt(arrayoop, YuhuType::intptr_type()), offset),
    PointerType::getUnqual(element_type),
    name);
}

Value* YuhuBuilder::CreateArrayAddress(Value*      arrayoop,
                                        BasicType   basic_type,
                                        ByteSize    base_offset,
                                        Value*      index,
                                        const char* name) {
  return CreateArrayAddress(
    arrayoop,
    YuhuType::to_arrayType(basic_type),
    type2aelembytes(basic_type),
    base_offset, index, name);
}

Value* YuhuBuilder::CreateArrayAddress(Value*      arrayoop,
                                        BasicType   basic_type,
                                        Value*      index,
                                        const char* name) {
  return CreateArrayAddress(
    arrayoop, basic_type,
    in_ByteSize(arrayOopDesc::base_offset_in_bytes(basic_type)),
    index, name);
}

// Helpers for creating intrinsics and external functions.

llvm::Type* YuhuBuilder::make_type(char type, bool void_ok) {
  switch (type) {
    // Primitive types
  case 'c':
    return YuhuType::jbyte_type();
  case 'i':
    return YuhuType::jint_type();
  case 'l':
    return YuhuType::jlong_type();
  case 'x':
    return YuhuType::intptr_type();
  case 'f':
    return YuhuType::jfloat_type();
  case 'd':
    return YuhuType::jdouble_type();

    // Pointers to primitive types
  case 'C':
  case 'I':
  case 'L':
  case 'X':
  case 'F':
  case 'D':
    return PointerType::getUnqual(make_type(tolower(type), false));

    // VM objects
  case 'T':
    return YuhuType::thread_type();
  case 'M':
    return PointerType::getUnqual(YuhuType::monitor_type());
  case 'O':
    return YuhuType::oop_addrspace1_type(); // FIXED - heap object, should GC
  case 'K':
    return YuhuType::klass_type();

    // Miscellaneous
  case 'v':
    assert(void_ok, "should be");
    return YuhuType::void_type();
  case '1':
    return YuhuType::bit_type();

  default:
    ShouldNotReachHere();
    return YuhuType::void_type(); // Return void type as fallback
  }
}

llvm::FunctionType* YuhuBuilder::make_ftype(const char* params,
                                           const char* ret) {
  std::vector<llvm::Type*> param_types;
  for (const char* c = params; *c; c++)
    param_types.push_back(make_type(*c, false));

  assert(strlen(ret) == 1, "should be");
  llvm::Type *return_type = make_type(*ret, true);

  return FunctionType::get(return_type, param_types, false);
}

// Create an object representing an intrinsic or external function by
// referencing the symbol by name.  This is the LLVM-style approach,
// but it cannot be used on functions within libjvm.so its symbols
// are not exported.  Note that you cannot make this work simply by
// exporting the symbols, as some symbols have the same names as
// symbols in the standard libraries (eg, atan2, fabs) and would
// obscure them were they visible.
Value* YuhuBuilder::make_function(const char* name,
                                   const char* params,
                                   const char* ret) {
  return YuhuContext::current().get_external(name, make_ftype(params, ret));
}

// Create an object representing an external function by inlining a
// function pointer in the code.  This is not the LLVM way, but it's
// the only way to access functions in libjvm.so and functions like
// __kernel_dmb on ARM which is accessed via an absolute address.
Value* YuhuBuilder::make_function(address     func,
                                   const char* params,
                                   const char* ret) {
  return CreateIntToPtr(
    LLVMValue::intptr_constant((intptr_t) func),
    PointerType::getUnqual(make_ftype(params, ret)));
}

// VM calls

Value* YuhuBuilder::monitorenter() {
  return make_function(YuhuRuntime::monitorenter_stub(), "TM", "v");
}

Value* YuhuBuilder::monitorexit() {
  return make_function(YuhuRuntime::monitorexit_stub(), "TM", "v");
}

Value* YuhuBuilder::new_instance() {
  // Option A: (JavaThread*, Klass*) -> void
  return make_function(YuhuRuntime::new_instance_stub(), "TK", "v");
}

Value* YuhuBuilder::newarray() {
  return make_function(YuhuRuntime::newarray_stub(), "Tii", "v");
}

Value* YuhuBuilder::anewarray() {
  // Option A: (JavaThread*, Klass*, int) -> void
  return make_function(YuhuRuntime::anewarray_stub(), "TKi", "v");
}

Value* YuhuBuilder::multianewarray() {
  // Option A: (JavaThread*, Klass*, int, int*) -> void
  return make_function(YuhuRuntime::multianewarray_stub(), "TKiI", "v");
}

Value* YuhuBuilder::register_finalizer() {
  return make_function(YuhuRuntime::register_finalizer_stub(), "TO", "v");
}

// Klass pointer encoding/decoding (compressed class pointers support)
// Must match macroAssembler_aarch64.cpp implementation

Value* YuhuBuilder::encode_klass_not_null(Value* full_klass) {
  if (!UseCompressedClassPointers) {
    return full_klass; // return ptr, but caller never goes to this branch
  }

  // convert ptr to i64
    Value *full_klass_int = CreatePtrToInt(full_klass, YuhuType::intptr_type());
  
  // Encode logic from macroAssembler_aarch64.cpp:3337-3377
  if (Universe::narrow_klass_base() == NULL) {
    if (Universe::narrow_klass_shift() != 0) {
      assert(LogKlassAlignmentInBytes == Universe::narrow_klass_shift(), "decode alg wrong");
      return CreateLShr(full_klass_int, LLVMValue::intptr_constant(Universe::narrow_klass_shift()));
    }
    return full_klass_int; // return i64
  }
  
  // For other cases, use subtraction-based encoding
  Value *base = LLVMValue::intptr_constant((intptr_t)Universe::narrow_klass_base());
  Value *sub = CreateSub(full_klass_int, base);
  if (Universe::narrow_klass_shift() != 0) {
    assert(LogKlassAlignmentInBytes == Universe::narrow_klass_shift(), "decode alg wrong");
    return CreateLShr(sub, LLVMValue::intptr_constant(Universe::narrow_klass_shift()));
  }
  return sub; // return i64
}

Value* YuhuBuilder::decode_klass_not_null(Value* compressed_klass) {
  if (!UseCompressedClassPointers) {
    return compressed_klass; // return i32, but caller never goes to this branch
  }
  
  // Decode logic from macroAssembler_aarch64.cpp:3383-3427
  if (Universe::narrow_klass_base() == NULL) {
    if (Universe::narrow_klass_shift() != 0) {
      assert(LogKlassAlignmentInBytes == Universe::narrow_klass_shift(), "decode alg wrong");
      Value *shifted = CreateShl(
        CreateZExt(compressed_klass, YuhuType::intptr_type()),
        LLVMValue::intptr_constant(Universe::narrow_klass_shift()));
      return CreateIntToPtr(shifted, YuhuType::klass_type()); // return ptr
    }
    return CreateIntToPtr(CreateZExt(compressed_klass, YuhuType::intptr_type()), YuhuType::klass_type()); // return ptr
  }
  
  // For other cases, use addition-based decoding
  Value *base = LLVMValue::intptr_constant((intptr_t)Universe::narrow_klass_base());
  Value *shifted = CreateShl(
    CreateZExt(compressed_klass, YuhuType::intptr_type()),
    LLVMValue::intptr_constant(Universe::narrow_klass_shift()));
  return CreateIntToPtr(CreateAdd(base, shifted), YuhuType::klass_type()); // return ptr
}

void YuhuBuilder::store_klass_to_object(Value* object, Value* klass) {
    llvm::Type* klass_ptr_type;
    if (UseCompressedClassPointers) {
        klass_ptr_type = YuhuType::jint_type();
    } else {
        klass_ptr_type = YuhuType::klass_type();
    }
  Value *klass_addr = CreateAddressOfStructEntry(
    object, in_ByteSize(oopDesc::klass_offset_in_bytes()),
    PointerType::getUnqual(klass_ptr_type),
    "klass_addr");
  
  if (UseCompressedClassPointers) {
    // Encode and store 32-bit compressed klass
    Value *compressed = encode_klass_not_null(klass);
    CreateStore(CreateTrunc(compressed, YuhuType::jint_type()), klass_addr);
  } else {
    // Store full 64-bit klass pointer
    CreateStore(klass, klass_addr);
  }
}

Value* YuhuBuilder::load_klass_from_object(Value* object) {
    llvm::Type* klass_ptr_type;
    if (UseCompressedClassPointers) {
        klass_ptr_type = YuhuType::jint_type();
    } else {
        klass_ptr_type = YuhuType::klass_type();
    }
  Value *klass_addr = CreateAddressOfStructEntry(
    object, in_ByteSize(oopDesc::klass_offset_in_bytes()),
    PointerType::getUnqual(klass_ptr_type),
    "klass_addr");
  
  if (UseCompressedClassPointers) {
    // Load 32-bit compressed klass and decode
    Value *compressed = CreateLoad(YuhuType::jint_type(), klass_addr, "compressed_klass");
    return decode_klass_not_null(compressed);
  } else {
    // Load full 64-bit klass pointer
    return CreateLoad(YuhuType::klass_type(), klass_addr, "klass");
  }
}

Value* YuhuBuilder::safepoint() {
  return make_function((address) SafepointSynchronize::block, "T", "v"); // not in use
}

Value* YuhuBuilder::throw_ArithmeticException() {
  return make_function(
    (address) YuhuRuntime::throw_ArithmeticException_stub(), "TCi", "v");
}

Value* YuhuBuilder::throw_ArrayIndexOutOfBoundsException() {
  return make_function(
    (address) YuhuRuntime::throw_ArrayIndexOutOfBoundsException_stub(), "TCii", "v");
}

Value* YuhuBuilder::throw_ClassCastException() {
  return make_function(
    (address) YuhuRuntime::throw_ClassCastException_stub(), "TCi", "v");
}

Value* YuhuBuilder::throw_NullPointerException() {
  return make_function(
    (address) YuhuRuntime::throw_NullPointerException_stub(), "TCi", "v");
}

// High-level non-VM calls

Value* YuhuBuilder::f2i() {
  return make_function((address) SharedRuntime::f2i, "f", "i"); // leaf call
}

Value* YuhuBuilder::f2l() {
  return make_function((address) SharedRuntime::f2l, "f", "l"); // leaf call
}

Value* YuhuBuilder::d2i() {
  return make_function((address) SharedRuntime::d2i, "d", "i"); // leaf call
}

Value* YuhuBuilder::d2l() {
  return make_function((address) SharedRuntime::d2l, "d", "l"); // leaf call
}

Value* YuhuBuilder::is_subtype_of() {
  return make_function((address) YuhuRuntime::is_subtype_of, "KK", "c"); // leaf call
}

Value* YuhuBuilder::current_time_millis() {
  return make_function((address) os::javaTimeMillis, "", "l"); // leaf call
}

Value* YuhuBuilder::sin() {
  return make_function("llvm.sin.f64", "d", "d"); // leaf call
}

Value* YuhuBuilder::cos() {
  return make_function("llvm.cos.f64", "d", "d"); // leaf call
}

Value* YuhuBuilder::tan() {
  // Explicit cast to function pointer to resolve overload ambiguity
  return make_function((address) (double (*)(double))::tan, "d", "d"); // leaf call
}

Value* YuhuBuilder::atan2() {
  // Explicit cast to function pointer to resolve overload ambiguity
  return make_function((address) (double (*)(double, double))::atan2, "dd", "d"); // leaf call
}

Value* YuhuBuilder::sqrt() {
  return make_function("llvm.sqrt.f64", "d", "d"); // leaf call
}

Value* YuhuBuilder::log() {
  return make_function("llvm.log.f64", "d", "d"); // leaf call
}

Value* YuhuBuilder::log10() {
  return make_function("llvm.log10.f64", "d", "d"); // leaf call
}

Value* YuhuBuilder::pow() {
  return make_function("llvm.pow.f64", "dd", "d"); // leaf call
}

Value* YuhuBuilder::exp() {
  return make_function("llvm.exp.f64", "d", "d"); // leaf call
}

Value* YuhuBuilder::fabs() {
  // Explicit cast to function pointer to resolve overload ambiguity
  return make_function((address) (double (*)(double))::fabs, "d", "d"); // leaf call
}

Value* YuhuBuilder::unsafe_field_offset_to_byte_offset() {
  extern jlong Unsafe_field_offset_to_byte_offset(jlong field_offset);
  return make_function((address) Unsafe_field_offset_to_byte_offset, "l", "l");
}

Value* YuhuBuilder::osr_migration_end() {
  return make_function((address) SharedRuntime::OSR_migration_end, "C", "v");
}

// Semi-VM calls

Value* YuhuBuilder::throw_StackOverflowError() {
  // For AArch64, use SharedRuntime::throw_StackOverflowError
  return make_function((address) YuhuRuntime::throw_StackOverflowError_stub(), "T", "v");
}

CallInst* YuhuBuilder::CreateExperimentalDeoptimize(uint64_t statepoint_id) {
    llvm::Type* return_type = YuhuType::to_stackType(function()->target_method()->return_type());

    std::vector<llvm::Value*> deopt_operands;
    llvm::OperandBundleDef deopt_bundle("deopt", deopt_operands);

  // Get or create the llvm.experimental.deoptimize intrinsic declaration
  llvm::Function* deopt_intrinsic = llvm::Intrinsic::getDeclaration(
    GetInsertBlock()->getModule(),
    llvm::Intrinsic::experimental_deoptimize,
    { return_type });
  
  // Create the call with deopt bundle
  llvm::CallInst* call = CreateCall(deopt_intrinsic, {}, {deopt_bundle});
  
  // Attach custom Statepoint ID to distinguish deopt statepoints from GC statepoints
  // GC safepoints use DefaultStatepointID (0), deopt traps use 0x1000+
  // This allows runtime to differentiate between GC stackmaps and deopt stackmaps
  llvm::LLVMContext &Ctx = getContext();
  llvm::AttrBuilder AB(Ctx);
  AB.addAttribute("statepoint-id", std::to_string(statepoint_id));
  llvm::AttributeList Attrs = llvm::AttributeList::get(Ctx, llvm::AttributeList::FunctionIndex, AB);
  call->setAttributes(Attrs);
  return call;
}

llvm::CallInst* YuhuBuilder::CreateStackMap(uint64_t id, uint32_t num_shadow_bytes, llvm::ArrayRef<llvm::Value*> live_values) {
    llvm::Function* stackmap_intrinsic = llvm::Intrinsic::getDeclaration(
            GetInsertBlock()->getModule(),
            llvm::Intrinsic::experimental_stackmap);

    std::vector<llvm::Value*> args;
    args.push_back(llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(getContext()), id));
    args.push_back(llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(getContext()), num_shadow_bytes));
    args.insert(args.end(), live_values.begin(), live_values.end());

    llvm::CallInst* call = CreateCall(stackmap_intrinsic, args);
    return call;
}

Value* YuhuBuilder::debug_stack_overflow_check() {
  // Signature: "Txxxxxx" -> "v" (Thread*, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t -> void)
  return make_function((address) YuhuRuntime::debug_stack_overflow_check, "Txxxxxx", "v");
}

// Native-Java transition

Value* YuhuBuilder::check_special_condition_for_native_trans() {
  return make_function(
    (address) JavaThread::check_special_condition_for_native_trans,
    "T", "v");
}

Value* YuhuBuilder::frame_address() {
#if LLVM_VERSION_MAJOR >= 20
  // LLVM 20+ requires type information in intrinsic name
  // llvm.frameaddress.p0: returns pointer type (p0)
  return make_function("llvm.frameaddress.p0", "i", "C");
#else
  return make_function("llvm.frameaddress", "i", "C");
#endif
}

Value* YuhuBuilder::unimplemented() {
  return make_function((address) report_unimplemented, "Ci", "v"); // not in use
}

Value* YuhuBuilder::should_not_reach_here() {
  return make_function((address) report_should_not_reach_here, "Ci", "v"); // not in use
}

Value* YuhuBuilder::dump() {
  return make_function((address) YuhuRuntime::dump, "Cx", "v"); // not in use
}

// Public interface to low-level non-VM calls

CallInst* YuhuBuilder::CreateReadFramePointer() {
  // Create inline assembly: "mov $0, x29"
  // $0 is the output operand (result register)
  // "=r" means output to a general-purpose register
  llvm::FunctionType* asm_type = llvm::FunctionType::get(YuhuType::intptr_type(), false);
  llvm::InlineAsm* asm_func = llvm::InlineAsm::get(
    asm_type,
    "mov $0, x29",  // AArch64 assembly: move x29 (frame pointer) to output register
    "=r",           // Output constraint: =r means output to a register
    false,          // Has side effects: no
    false,          // Is align stack: no
    llvm::InlineAsm::AD_ATT    // Dialect: AT&T style (but for AArch64, this is ignored)
  );
  
  // LLVM 20+ requires FunctionType for CreateCall
  return CreateCall(asm_type, asm_func, std::vector<Value*>(), "fp");
}

CallInst* YuhuBuilder::CreateReadStackPointer() {
    return CreateReadRegister("sp");
}

CallInst* YuhuBuilder::CreateReadLinkRegister() {
    return CreateReadRegister("lr");
}

// CreateReadMethodRegister() removed: the compiled nmethod's Method* is now a
// compile-time constant baked in YuhuFunction::initialize via CreateInlineMetadata
// (x12 is caller-saved scratch with no ABI guarantee of holding the method).

CallInst* YuhuBuilder::CreateReadThreadRegister() {
  // Create inline assembly: "mov $0, x28"
  // $0 is the output operand (result register)
  // "=r" means output to a general-purpose register
  llvm::FunctionType* asm_type = llvm::FunctionType::get(YuhuType::intptr_type(), false);
  llvm::InlineAsm* asm_func = llvm::InlineAsm::get(
    asm_type,
    "mov $0, x28",  // AArch64 assembly: move x28 to output register
    "=r",           // Output constraint: =r means output to a register
    false,          // Has side effects: no
    false,          // Is align stack: no
    llvm::InlineAsm::AD_ATT    // Dialect: AT&T style (but for AArch64, this is ignored)
  );
  
  // LLVM 20+ requires FunctionType for CreateCall
  return CreateCall(asm_type, asm_func, std::vector<Value*>(), "rthread");
}

CallInst* YuhuBuilder::CreateReadX0Register() {
    // Create inline assembly: "mov $0, x0"
    llvm::FunctionType* asm_type = llvm::FunctionType::get(YuhuType::intptr_type(), false);
    llvm::InlineAsm* asm_func = llvm::InlineAsm::get(
            asm_type,
            "mov $0, x0",  // Move x20 to output register
            "=r",          // Output constraint: =r means output to a register
            false,         // Has side effects: no
            false,         // Is align stack: no
            llvm::InlineAsm::AD_ATT
    );

    return CreateCall(asm_type, asm_func, std::vector<Value*>(), "p7_arg"); // 8th int-like argument
}

LoadInst* YuhuBuilder::CreateLoadX0Slot() {
    return CreateLoad(YuhuType::intptr_type(), function()->x0_slot(), "load_x0_slot");
}

CallInst *YuhuBuilder::CreateReadCurrentPC() {
    // Create function type that returns i64 (the PC value)
    llvm::FunctionType* asm_type = llvm::FunctionType::get(
            YuhuType::intptr_type(),  // Return type: i64 for PC address
            {},                      // No input parameters
            false);

    llvm::InlineAsm* asm_func = llvm::InlineAsm::get(
            asm_type,
            "adr $0, .",    // AArch64: get current PC address to x0
            "=r",           // Output constraint: result goes to a register
            true,           // Has side effects: yes (prevents optimization)
            false,          // Is align stack: no
            llvm::InlineAsm::AD_ATT);

    return CreateCall(asm_type, asm_func, std::vector<Value*>());
}

CallInst* YuhuBuilder::CreateReadRegister(const char* reg_name) {
  // Generic register reader using @llvm.read_register intrinsic
  // Used for reading x12 (rmethod), x28 (rthread), x20 (esp), etc.
  YuhuContext& ctx = YuhuContext::current();
  llvm::Module* mod = ctx.module();
  
  // Create metadata string for the register name
  llvm::MDNode* md = llvm::MDNode::get(
    ctx,
    llvm::MDString::get(ctx, reg_name));
  
  // Get read_register intrinsic declaration
  llvm::Function* read_reg = llvm::Intrinsic::getDeclaration(
    mod,
    llvm::Intrinsic::read_register,
    {YuhuType::intptr_type()});
  
  // Create function type for read_register: (metadata) -> intptr_t
  llvm::FunctionType* func_type = llvm::FunctionType::get(
    YuhuType::intptr_type(),
    {llvm::Type::getMetadataTy(ctx)},
    false);
  
  // Call read_register with register name metadata
  std::vector<Value*> args;
  args.push_back(llvm::MetadataAsValue::get(ctx, md));
  return CreateCall(func_type, read_reg, args, reg_name);
}

CallInst* YuhuBuilder::CreateMemset(Value* dst,
                                     Value* value,
                                     Value* len,
                                     Value* align) {
  // Use LLVM's own intrinsic declaration to get the correct signature
  // memset overloads by {ptr_type, len_type}
  llvm::Module* mod = YuhuContext::current().module();
  llvm::Function* memset_func = llvm::Intrinsic::getDeclaration(
    mod,
    llvm::Intrinsic::memset,
    {dst->getType(), YuhuType::jlong_type()});
  
  // len must match the intrinsic's len type (i64 on 64-bit)
  Value* len64 = CreateZExt(len, YuhuType::jlong_type());
  
  std::vector<Value*> args = {
    dst,
    value,
    len64,
    LLVMValue::bit_constant(0)  // isVolatile = false
  };
  
  CallInst* call = CreateCall(memset_func, args);
  
  // Pass alignment via parameter attribute on dst (first argument)
  if (llvm::isa<llvm::ConstantInt>(align)) {
    unsigned align_value = llvm::cast<llvm::ConstantInt>(align)->getZExtValue();
    call->addParamAttr(0, Attribute::getWithAlignment(getContext(), llvm::Align(align_value)));
  }
  
  return call;
}

CallInst* YuhuBuilder::CreateUnimplemented(const char* file, int line) {
  // LLVM 20+ uses opaque pointer types, so we reconstruct FunctionType from signature
  // unimplemented signature: "Ci" -> "v" (char*, int -> void)
  llvm::FunctionType* func_type = llvm::FunctionType::get(
    YuhuType::void_type(),
    std::vector<llvm::Type*>{
      PointerType::getUnqual(YuhuType::jbyte_type()),
      YuhuType::jint_type()
    },
    false);
  std::vector<Value*> args;
  args.push_back(CreateIntToPtr(
    LLVMValue::intptr_constant((intptr_t) file),
    PointerType::getUnqual(YuhuType::jbyte_type())));
  args.push_back(LLVMValue::jint_constant(line));
  return CreateCall(func_type, unimplemented(), args);
}

CallInst* YuhuBuilder::CreateShouldNotReachHere(const char* file, int line) {
  // LLVM 20+ uses opaque pointer types, so we reconstruct FunctionType from signature
  // should_not_reach_here signature: "Ci" -> "v" (char*, int -> void)
  llvm::FunctionType* func_type = llvm::FunctionType::get(
    YuhuType::void_type(),
    std::vector<llvm::Type*>{
      PointerType::getUnqual(YuhuType::jbyte_type()),
      YuhuType::jint_type()
    },
    false);
  std::vector<Value*> args;
  args.push_back(CreateIntToPtr(
    LLVMValue::intptr_constant((intptr_t) file),
    PointerType::getUnqual(YuhuType::jbyte_type())));
  args.push_back(LLVMValue::jint_constant(line));
  return CreateCall(func_type, should_not_reach_here(), args);
}

#ifndef PRODUCT
CallInst* YuhuBuilder::CreateDump(Value* value) {
  const char *name;
  if (value->hasName())
    // XXX this leaks, but it's only debug code
    name = strdup(value->getName().str().c_str());
  else
    name = "unnamed_value";

  if (isa<PointerType>(value->getType()))
    value = CreatePtrToInt(value, YuhuType::intptr_type());
  else if (value->getType()->
           isIntegerTy()
           )
    value = CreateIntCast(value, YuhuType::intptr_type(), false);
  else
    Unimplemented();

  // LLVM 20+ uses opaque pointer types, so we reconstruct FunctionType from signature
  // dump signature: "Cx" -> "v" (char*, intptr -> void)
  llvm::FunctionType* func_type = llvm::FunctionType::get(
    YuhuType::void_type(),
    std::vector<llvm::Type*>{
      PointerType::getUnqual(YuhuType::jbyte_type()),
      YuhuType::intptr_type()
    },
    false);
  std::vector<Value*> args;
  args.push_back(CreateIntToPtr(
    LLVMValue::intptr_constant((intptr_t) name),
    PointerType::getUnqual(YuhuType::jbyte_type())));
  args.push_back(value);
  return CreateCall(func_type, dump(), args);
}
#endif // PRODUCT

// HotSpot memory barriers

void YuhuBuilder::CreateUpdateBarrierSet(BarrierSet* bs, Value* field) {
    if (bs->kind() != BarrierSet::CardTableModRef)
        Unimplemented();

    CardTableModRefBS* ctbs = (CardTableModRefBS*) bs;

    // 1. 计算卡表索引（仍然需要 ptrtoint，但只取地址值）
    Value* field_addr_int = CreatePtrToInt(field, YuhuType::intptr_type());
    Value* card_index = CreateLShr(field_addr_int,
                                   LLVMValue::intptr_constant(ctbs->card_shift));

    // 2. 将卡表基地址转为指针（只做一次，可缓存）
    Value* card_table_base_ptr = CreateIntToPtr(
            LLVMValue::intptr_constant((intptr_t)ctbs->byte_map_base),
            PointerType::getUnqual(YuhuType::jbyte_type()));

    // 3. 使用 GEP 计算卡表条目地址（关键改进！）
    Value* card_entry_ptr = CreateGEP(
            YuhuType::jbyte_type(),           // 元素类型
            card_table_base_ptr,              // 基指针
            card_index                        // 索引
    );

    // 4. 写入脏标记
    CreateStore(
            LLVMValue::jbyte_constant(CardTableModRefBS::dirty_card_val()),
            card_entry_ptr);
}

// Helpers for accessing the code buffer

Value* YuhuBuilder::code_buffer_address(int offset) {
  llvm::Value* base_pc = code_buffer()->base_pc();
  
  if (base_pc == NULL) {
    // For normal entry, base_pc is NULL because we no longer pass it as a parameter
    // Use a placeholder value (0) - this is only used for debug info recording
    // The actual PC will be calculated at runtime by HotSpot's stack walking code
    base_pc = LLVMValue::intptr_constant(0);
  }
  
  return CreateAdd(
    base_pc,
    LLVMValue::intptr_constant(offset));
}

Value* YuhuBuilder::CreateInlineOopForStaticField(ciField* field, const char* name) {
  // Get the klass that holds the static field and the field offset
  ciInstanceKlass* field_holder = field->holder();
  int field_offset = field->offset();
  
  // Determine if this is an object reference field
  bool is_object_field = !field->type()->is_primitive_type();
  bool is_volatile = field->is_volatile();
  
  // Convert ciInstanceKlass* to Klass*
  oop real_oop = field_holder->java_mirror()->get_oop();

    ResetNoHandleMark resetNoHandleMark;

    jobject jmirror = JNIHandles::make_local(real_oop);

    Module* mod = GetInsertBlock()->getModule();

    // generate unique id with statepoint id, this makes slot name unique
    uint64_t unique_id = YuhuStatepointIDGenerator::next();

    uint64_t temp_placeholder = unique_id & 0xFFFFFFFFFFFFULL;  // Use oop_id as temporary placeholder

    std::string slot_name = "__oop_slot_"+ std::to_string(unique_id);
    Constant* init = ConstantExpr::getIntToPtr(ConstantInt::get(YuhuType::jlong_type(), temp_placeholder), YuhuType::oop_addrspace1_type());

    auto gv = new GlobalVariable(*mod, YuhuType::oop_addrspace1_type(), false, GlobalValue::PrivateLinkage, init, slot_name);
    gv->setAlignment(Align(8));

    YuhuDebugInformationRecorder::get()->register_global_var(unique_id, GlobalVarType::oop, temp_placeholder, jmirror);

    auto load = new LoadInst(YuhuType::oop_addrspace1_type(), gv, "", GetInsertBlock());

    llvm::Value* mirror_oop = load;
  
  // Calculate field address: mirror + field_offset.  The mirror is a heap
  // oop, so use a GEP to keep the field address a derived pointer that RS4GC
  // can track (integer addressing would hide it from liveness).
  llvm::Value* field_offset_val = llvm::ConstantInt::get(YuhuType::jlong_type(), field_offset);
  llvm::Value* field_addr = CreateGEP(
    YuhuType::jbyte_type(), mirror_oop, field_offset_val, "static_field_addr");
  
  // Load field value based on type
  llvm::Value* field_value;
  if (is_object_field) {
      llvm::LoadInst* load_inst;
      if (UseCompressedOops) {
          load_inst = CreateLoad(YuhuType::jint_type(), field_addr, name ? name : "field_value");
      } else {
          // Object field: load as ptr addrspace(1)
          load_inst = CreateLoad(
                  YuhuType::oop_addrspace1_type(),
                  field_addr,
                  name ? name : "field_value");
      }
    
    // Set volatile ordering if needed
    if (is_volatile) {
        load_inst->setOrdering(llvm::AtomicOrdering::SequentiallyConsistent);
    }
    if (UseCompressedOops) {
        field_value = CreateDecodeHeapOop(load_inst);
    } else {
        field_value = load_inst;
    }
  } else {
    // Primitive field: load as appropriate type
    BasicType basic_type = field->type()->basic_type();
    llvm::Type* field_type = YuhuType::to_arrayType(basic_type);
    
    llvm::LoadInst* load_inst = CreateLoad(
      field_type,
      field_addr,
      name ? name : "field_value");
    
    // Set volatile ordering if needed
    if (is_volatile) {
      load_inst->setOrdering(llvm::AtomicOrdering::SequentiallyConsistent);
    }
    field_value = load_inst;
  }
  
  return field_value;
}

Value* YuhuBuilder::CreateInlineOop(ciObject* object, const char* name) {
  if (object == NULL || object->is_null_object()) {
    return llvm::ConstantPointerNull::get(
      llvm::PointerType::get(YuhuType::oop_addrspace1_type()->getContext(), 1));
  }

  Module* mod = GetInsertBlock()->getModule();

  // Non-String oop (e.g. klass mirror): instance of java.lang.Class is allocated in heap
  oop real_oop = object->get_oop();
  // Perhaps, for any class object, we can embed oop address in the IR and create oop relocation later

    ResetNoHandleMark resetNoHandleMark;

  // String oop: allocate unique oop_id and generate marker for deferred oop_index allocation
  // Allocate jobject and assign unique oop_id (like C1 does, but deferred to relocation phase)
  jobject jobj = JNIHandles::make_local(real_oop);

    // generate unique id with statepoint id, this makes slot name unique
    uint64_t unique_id = YuhuStatepointIDGenerator::next();

    uint64_t temp_placeholder = unique_id & 0xFFFFFFFFFFFFULL;  // Use oop_id as temporary placeholder

    std::string slot_name = "__oop_slot_"+ std::to_string(unique_id);
    Constant* init = ConstantExpr::getIntToPtr(ConstantInt::get(YuhuType::jlong_type(), temp_placeholder), YuhuType::oop_addrspace1_type());

    auto gv = new GlobalVariable(*mod, YuhuType::oop_addrspace1_type(), false, GlobalValue::PrivateLinkage, init, slot_name);
    gv->setAlignment(Align(8));

    YuhuDebugInformationRecorder::get()->register_global_var(unique_id, GlobalVarType::oop, temp_placeholder, jobj);

    auto load = new LoadInst(YuhuType::oop_addrspace1_type(), gv, "", GetInsertBlock());

    return load;
}

Value* YuhuBuilder::CreateInlineMetadata(::Metadata* metadata, llvm::PointerType* type, const char* name) {
  assert(metadata != NULL, "inlined metadata must not be NULL");
  assert(metadata->is_metaspace_object(), "sanity check");

  Module* mod = GetInsertBlock()->getModule();

  // The patch point's target operand carries the Metadata* itself, so LLVM
  // materializes the address as movz + movk(lsl #16) + movk(lsl #32) and then
  // branches to it. Staying below 48 bits is what pins that materialization to
  // exactly 3 instructions, which is the layout the patch stage walks.
  auto metadata_addr = (uint64_t)(uintptr_t)metadata;
  assert((metadata_addr & 0xFFFF000000000000ULL) == 0,
         "metadata address must fit in 48 bits");

  // generate unique id with statepoint id, this makes slot name unique
  uint64_t unique_id = YuhuStatepointIDGenerator::next();

  std::string slot_name = "__metadata_slot_"+ std::to_string(unique_id);
  Constant* init = ConstantExpr::getIntToPtr(ConstantInt::get(YuhuType::jlong_type(), metadata_addr), type);

  auto gv = new GlobalVariable(*mod, type, false, GlobalValue::PrivateLinkage, init, slot_name);
  gv->setAlignment(Align(8));

  YuhuDebugInformationRecorder::get()->register_global_var(unique_id, GlobalVarType::metadata, metadata_addr);

  auto load = new LoadInst(type, gv, "", GetInsertBlock());

  return load;
}

// Helpers for creating basic blocks.

BasicBlock* YuhuBuilder::GetBlockInsertionPoint() const {
  BasicBlock *cur = GetInsertBlock();

  // BasicBlock::Create takes an insertBefore argument, so
  // we need to find the block _after_ the current block
  Function::iterator iter = cur->getParent()->begin();
  Function::iterator end  = cur->getParent()->end();
  while (iter != end) {
    iter++;
    if (&*iter == cur) {
      iter++;
      break;
    }
  }

  if (iter == end)
    return NULL;
  else
    return &*iter; // Dereference iterator to get BasicBlock*
}

void YuhuBuilder::InsertStackMapAtBlockStart(llvm::BasicBlock* block, uint64_t id, llvm::ArrayRef<llvm::Value*> live_values) {
    BasicBlock::iterator firstNonPHIIt = block->getFirstNonPHIIt();
    SetInsertPoint(block, firstNonPHIIt);

    CreateStackMap(id, 0, live_values);
}

BasicBlock* YuhuBuilder::CreateBlock(BasicBlock* ip, const char* name) const {
  return BasicBlock::Create(
    YuhuContext::current(), name, GetInsertBlock()->getParent(), ip);
}

LoadInst* YuhuBuilder::CreateAtomicLoad(Value* ptr, llvm::Type* load_type, unsigned align, AtomicOrdering ordering, bool isVolatile, const char* name) {
  // LLVM 20 uses opaque pointers, so we need to get the type from the value being loaded
  // For atomic loads, we typically load a word-sized value
  // Since we don't know the exact type from the pointer, we use intptr_type as a default
  // The actual type should be determined by the caller based on the context
  if (!load_type) {
      load_type = YuhuType::intptr_type();
  }
  // LoadInst constructor: LoadInst(Type *Ty, Value *Ptr, const Twine &NameStr, bool isVolatile, Align Align, AtomicOrdering Order, SyncScope::ID SSID, ...)
  return Insert(new LoadInst(load_type, ptr, name, isVolatile, llvm::Align(align), ordering, llvm::SyncScope::System), name);
}

StoreInst* YuhuBuilder::CreateAtomicStore(Value* val, Value* ptr, unsigned align, AtomicOrdering ordering, bool isVolatile, const char* name) {
  // LLVM 20 uses opaque pointers
  // StoreInst constructor: StoreInst(Value *Val, Value *Ptr, bool isVolatile, Align Align, AtomicOrdering Order, SyncScope::ID SSID, ...)
  return Insert(new StoreInst(val, ptr, isVolatile, llvm::Align(align), ordering, llvm::SyncScope::System), name);
}

void YuhuBuilder::scan_and_generate_all_relocations(address llvm_code_start, size_t llvm_code_size, CodeBuffer* cb, address code_start, size_t adapter_size) {
    if (llvm_code_start == NULL || llvm_code_size == 0 || code_start == NULL) {
        return;
    }

    GrowableArray<RelocEntry> reloc_entries;

    // process patch points
    auto recorder = YuhuDebugInformationRecorder::get();
    auto unwind_call_sites = recorder->get_call_site_stack_maps_by_call_site_type(CallSiteType::unwind_call);
    for (int j = 0; j < unwind_call_sites->length(); ++j) {
        CallSiteInstructionHolder* call_site_holder = unwind_call_sites->at(j);
        assert(call_site_holder->call_target() == (uint64_t)&YuhuRuntime::go_unwind, "");
        uint64_t llvm_blr_offset = call_site_holder->instruction_offset() - 4;
        uint32_t* llvm_blr_instr = (uint32_t*)(llvm_code_start + llvm_blr_offset);
        assert(YuhuAddressScanner::is_blr_pattern(llvm_blr_instr), "");
        uint32_t* blr_instr = (uint32_t*)(code_start + adapter_size + llvm_blr_offset);
        assert(YuhuAddressScanner::is_blr_pattern(blr_instr), "");

        // Patch b instruction
        uint64_t unwind_handler_address = (uint64_t) (code_start + adapter_size + llvm_code_size);
        bool branch_unwind_handler_patched = YuhuAddressPatcher::patch_branch_unwind_handler(blr_instr, unwind_handler_address);
        assert(branch_unwind_handler_patched, "should patch successfully");
    }

    GrowableArray<uint64_t> copied_symbol_srcs; // symbol_entry->start
    GrowableArray<uint64_t> copied_symbol_dsts; // address in cb->consts()
    GrowableArray<std::pair<uint64_t, uint64_t>> copied_symbols;

    // process edge entries
    auto edge_entries = recorder->edge_entries();
    // sort by offset, adrp + ldr generates 2 edge entries, just processes the first one
    edge_entries->sort([](EdgeEntry** a, EdgeEntry** b) -> int {
        if ((*a)->offset < (*b)->offset) return -1;
        if ((*a)->offset > (*b)->offset) return 1;
        return 0;
    });
    for (int i = 0; i < edge_entries->length(); ++i) {
        auto edge = edge_entries->at(i);
        uint32_t offset_in_func = edge->offset;
        if (edge->edge_target_type == EdgeTargetType::got_symbol) {
            uint32_t* edge_instr = (uint32_t*)(code_start + adapter_size + offset_in_func);
            assert(YuhuAddressScanner::is_adrp_got_pattern(edge_instr), "should be adrp got instructions");
            uint64_t function_address = *(uint64_t*)(edge->target_address);
            if (recorder->contains_call_target(function_address)) {
                if (function_address == (uint64_t)&YuhuRuntime::go_unwind) {
                    bool nop_patched = YuhuAddressPatcher::patch_nop(edge_instr, 2);
                    assert(nop_patched, "should patch successfully");
                } else {
                    bool new_adrp_patched = YuhuAddressPatcher::patch_new_adrp(edge_instr, function_address);
                    assert(new_adrp_patched && YuhuAddressScanner::is_adrp_with_add_pattern(edge_instr),
                           "should patch successfully");

                    RelocEntry reloc_entry{};
                    reloc_entry.offset = adapter_size + offset_in_func;
                    reloc_entry.reloc_type = relocInfo::relocType::runtime_call_type;
                    reloc_entry.section_type = CodeBuffer::SECT_INSTS;
                    reloc_entries.append(reloc_entry);
                }
            } else {
                // it should be either data symbol or const symbol
                auto symbol_entry = recorder->get_symbol_by_range_addr(function_address);
                assert(symbol_entry != NULL, "Symbol should exist");

                address new_table_addr;
                int idx = copied_symbol_srcs.find(symbol_entry->start);
                if (idx >= 0) {
                    new_table_addr = (address)copied_symbol_dsts.at(idx);
                } else {
                    new_table_addr = cb->consts()->end();
                    size_t symbol_size = symbol_entry->end - symbol_entry->start;
                    memcpy(new_table_addr, (address) symbol_entry->start, symbol_size);
                    cb->consts()->set_end(new_table_addr + symbol_size);
                    copied_symbol_srcs.append(symbol_entry->start);
                    copied_symbol_dsts.append((uint64_t)new_table_addr);
                }

                // get new function address with offset
                address new_target_address = (function_address - symbol_entry->start) + new_table_addr;
                // target value should be copied successfully
                if (idx == -1) {
                    assert(*(uint64_t *) function_address == *(uint64_t *) (new_target_address), "target value should match");
                }
                bool new_jump_table_patched = YuhuAddressPatcher::patch_new_adrp(edge_instr, (uint64_t)new_target_address);
                assert(new_jump_table_patched && YuhuAddressScanner::is_adrp_with_add_pattern(edge_instr), "should patch successfully");

                RelocEntry reloc_entry{};
                reloc_entry.offset = adapter_size + offset_in_func;
                reloc_entry.reloc_type = relocInfo::relocType::internal_word_type;
                reloc_entry.target = (uint64_t) new_target_address;
                reloc_entry.section_type = CodeBuffer::SECT_INSTS;
                reloc_entries.append(reloc_entry);
                if (symbol_entry->section_type == SectionType::data_section) {
                    uint64_t target_value = *(uint64_t*)(function_address);
                    auto global_var = recorder->get_global_var_by_target_value(target_value);
                    assert(global_var != NULL, "global var should exist");
                    if (global_var->global_var_type == GlobalVarType::metadata) {
                        ::Metadata* metadata = (::Metadata*)(uintptr_t) target_value;
                        assert(metadata != NULL && metadata->is_metaspace_object(), "metadata must be a metaspace object");
                        // allocate_metadata_index() deduplicates, so several uses of the same
                        // Metadata* share one pool entry while each keeps its own relocation.
                        int metadata_index = cb->oop_recorder()->allocate_metadata_index(metadata);
                        assert(metadata_index > 0, "metadata_index must be > 0");

                        RelocEntry reloc_entry2{};
                        // consts section is at last end of code buffer, create metadata reloc at consts section
                        reloc_entry2.offset = new_target_address - code_start;
                        reloc_entry2.reloc_type = relocInfo::relocType::metadata_type;
                        reloc_entry2.spec_index = metadata_index;
                        reloc_entry2.section_type = CodeBuffer::SECT_CONSTS;
                        reloc_entries.append(reloc_entry2);
                    } else if (global_var->global_var_type == GlobalVarType::oop) {
                        jobject jobj = global_var->oop;
                        assert(jobj != NULL, "jobj must not be NULL");

                        // Allocate real oop_index from the final OopRecorder (like C1 does)
                        int oop_index = cb->oop_recorder()->allocate_oop_index(jobj);
                        // patch data directly
                        bool oop_index_patched = YuhuAddressPatcher::patch_oop_index((uint64_t*)new_target_address, oop_index);
                        assert(oop_index_patched, "should patch successfully");

                        RelocEntry reloc_entry2{};
                        reloc_entry2.offset = new_target_address - code_start;
                        reloc_entry2.reloc_type = relocInfo::relocType::oop_type;
                        reloc_entry2.spec_index = oop_index;
                        reloc_entry2.section_type = CodeBuffer::SECT_CONSTS;
                        reloc_entries.append(reloc_entry2);
                    } else {
                        ShouldNotReachHere();
                    }
                }
            }
        } else if (edge->edge_target_type == EdgeTargetType::const_symbol) {
            uint32_t* edge_instr = (uint32_t*)(code_start + adapter_size + offset_in_func);
            assert(YuhuAddressScanner::is_adrp_jump_table_pattern(edge_instr), "should be direct jump table pattern");

            auto const_symbol_entry = recorder->get_symbol_by_section_type_and_range_addr(SectionType::const_section, edge->target_address);
            assert(const_symbol_entry != NULL, "Const symbol should exist");

            address new_table_addr;
            int idx = copied_symbol_srcs.find(const_symbol_entry->start);
            if (idx >= 0) {
                new_table_addr = (address)copied_symbol_dsts.at(idx);
            } else {
                new_table_addr = cb->consts()->end();
                size_t symbol_size = const_symbol_entry->end - const_symbol_entry->start;
                memcpy(new_table_addr, (address) const_symbol_entry->start, symbol_size);
                cb->consts()->set_end(new_table_addr + symbol_size);
                copied_symbol_srcs.append(const_symbol_entry->start);
                copied_symbol_dsts.append((uint64_t)new_table_addr);
            }

            // get new function address with offset
            address new_target_address = (edge->target_address - const_symbol_entry->start) + new_table_addr;
            // simply compare first byte, may not be accurate, but better than nothing
            assert(*(uint8_t*)(edge->target_address) == *(uint8_t*)(new_target_address), "copied content should match");
            bool new_jump_table_patched = YuhuAddressPatcher::patch_new_adrp(edge_instr, (uint64_t)new_target_address);
            assert(new_jump_table_patched && YuhuAddressScanner::is_adrp_with_add_pattern(edge_instr), "should patch successfully");

            RelocEntry reloc_entry{};
            reloc_entry.offset = adapter_size + offset_in_func;
            reloc_entry.reloc_type = relocInfo::relocType::internal_word_type;
            reloc_entry.target = (uint64_t)new_target_address;
            reloc_entry.section_type = CodeBuffer::SECT_INSTS;
            reloc_entries.append(reloc_entry);
        } else {
            ShouldNotReachHere();
        }
        assert(i+1 < edge_entries->length(), "next edge should exist");
        auto next_edge = edge_entries->at(i+1);
        assert(next_edge->offset == edge->offset + 4 && next_edge->target_address == edge->target_address, "next edge is not expected");
        ++i;
    }

    // sort by offset, relocation should be registered in ascending order
    reloc_entries.sort([](RelocEntry* a, RelocEntry* b) -> int {
        if ((*a).offset < (*b).offset) return -1;
        if ((*a).offset > (*b).offset) return  1;
        return 0;
    });;

    for (int i = 0; i < reloc_entries.length(); ++i) {
        RelocEntry reloc_entry = reloc_entries.at(i);
        uint32_t *instr = (uint32_t *) (code_start + reloc_entry.offset);
        if (reloc_entry.reloc_type == relocInfo::relocType::oop_type) {
            RelocationHolder rspec = oop_Relocation::spec(reloc_entry.spec_index);
            if (reloc_entry.section_type == CodeBuffer::SECT_INSTS) {
                cb->insts()->relocate((address) instr, rspec);
            } else if (reloc_entry.section_type == CodeBuffer::SECT_CONSTS) {
                cb->consts()->relocate((address) instr, rspec);
            }
        } else if (reloc_entry.reloc_type == relocInfo::relocType::metadata_type) {
            RelocationHolder rspec = metadata_Relocation::spec(reloc_entry.spec_index);
            if (reloc_entry.section_type == CodeBuffer::SECT_INSTS) {
                cb->insts()->relocate((address) instr, rspec);
            } else if (reloc_entry.section_type == CodeBuffer::SECT_CONSTS) {
                cb->consts()->relocate((address) instr, rspec);
            }
        } else if (reloc_entry.reloc_type == relocInfo::relocType::poll_type) {
            assert(reloc_entry.section_type == CodeBuffer::SECT_INSTS, "");
            cb->relocate((address)(instr), reloc_entry.reloc_type);
        } else if (reloc_entry.reloc_type == relocInfo::relocType::runtime_call_type) {
            assert(reloc_entry.section_type == CodeBuffer::SECT_INSTS, "");
            cb->relocate((address)(instr), reloc_entry.reloc_type);
        } else if (reloc_entry.reloc_type == relocInfo::relocType::internal_word_type) {
            assert(reloc_entry.section_type == CodeBuffer::SECT_INSTS, "");
            cb->relocate((address)(instr), internal_word_Relocation::spec((address)reloc_entry.target));
        }
    }
}

Value* YuhuBuilder::CreateDecodeHeapOop(Value* compressed_oop) {
  // Check if compressed oops are enabled
  if (UseCompressedOops) {
    // Create a call to the runtime function that handles compressed oop decoding
    // We need to create intrinsic function that expands compressed oops
    // This is a simplified version that mimics the AArch64 macro assembler approach
    
    // For AArch64 with compressed oops, we need to:
    // 1. Cast the compressed oop (narrowOop, which is jint) to intptr
    // 2. Shift left by narrow_oop_shift
    // 3. Add the narrow_oop_base
    
    // Get the shift amount and base address from Universe
    int shift = Universe::narrow_oop_shift();
    address base = Universe::narrow_oop_base();
    
    // Convert compressed oop to intptr type
    Value* compressed_as_int = CreateIntCast(compressed_oop, YuhuType::intptr_type(), false);
    
    // Shift left by the narrow oop shift
    if (shift > 0) {
      Value* shifted = CreateShl(compressed_as_int, LLVMValue::intptr_constant(shift));
      // Add the base address
      Value* base_as_int = LLVMValue::intptr_constant((intptr_t)base);
      Value* result_int = CreateAdd(base_as_int, shifted);
      return CreateIntToPtr(result_int, YuhuType::oop_addrspace1_type()); // FIXED - object is allocated in heap
    } else {
      // If shift is 0, just add to base
      Value* base_as_int = LLVMValue::intptr_constant((intptr_t)base);
      Value* result_int = CreateAdd(base_as_int, compressed_as_int);
      return CreateIntToPtr(result_int, YuhuType::oop_addrspace1_type()); // FIXED - object is allocated in heap
    }
  } else {
    // If compressed oops are not used, just return the value as-is (cast to oop*)
    return CreateIntToPtr(CreateIntCast(compressed_oop, YuhuType::intptr_type(), false), 
                         YuhuType::oop_addrspace1_type()); // FIXED - object is allocated in heap
  }
}

Value* YuhuBuilder::CreateEncodeHeapOop(Value* oop) {
  // Check if compressed oops are enabled
  if (UseCompressedOops) {
    // Create a call to the runtime function that handles oop encoding
    // For AArch64 with compressed oops, we need to:
    // 1. Convert oop pointer to integer
    // 2. Subtract the narrow_oop_base
    // 3. Shift right by narrow_oop_shift
    
    address base = Universe::narrow_oop_base();
    int shift = Universe::narrow_oop_shift();
    
    // Convert oop pointer to integer
    Value* oop_as_int = CreatePtrToInt(oop, YuhuType::intptr_type());
    Value* base_as_int = LLVMValue::intptr_constant((intptr_t)base);
    
    // Subtract the base address
    Value* delta = CreateSub(oop_as_int, base_as_int);
    
    // Shift right by the narrow oop shift
    if (shift > 0) {
      Value* shifted = CreateLShr(delta, LLVMValue::intptr_constant(shift));
      return CreateIntCast(shifted, YuhuType::jint_type(), false);
    } else {
      return CreateIntCast(delta, YuhuType::jint_type(), false);
    }
  } else {
    // If compressed oops are not used, just return the pointer value cast to appropriate type
    return CreatePtrToInt(oop, YuhuType::intptr_type());
  }
}
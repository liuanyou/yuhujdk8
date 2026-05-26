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
#include "yuhu/yuhuVirtualAddressPatcher.hpp"
#include "utilities/debug.hpp"
#include "memory/universe.hpp"
#include "oops/oop.inline.hpp"
#include "classfile/javaClasses.hpp"
#include "oops/typeArrayOop.hpp"

using namespace llvm;

// Forward declaration of gc_safepoint_poll from yuhuRuntime.cpp
extern "C" void gc_safepoint_poll();

YuhuBuilder::YuhuBuilder(YuhuCodeBuffer* code_buffer, YuhuFunction* function)
  : IRBuilder<>(YuhuContext::current()),
    _code_buffer(code_buffer),
    _function(function),
    _pending_oops(new GrowableArray<jobject>(100)),
    _next_oop_id(0),
    _pending_metadata(new GrowableArray< ::Metadata*>(100)),
    _next_metadata_id(0) {
}

// Helpers for accessing structures
// Use llvm::Type explicitly to avoid conflict with HotSpot's Type class
Value* YuhuBuilder::CreateAddressOfStructEntry(Value*      base,
                                                ByteSize    offset,
                                                llvm::Type* type,
                                                const char* name) {
  // LLVM 20+ uses opaque pointer types, so we can't get element type from PointerType
  // Calculate address as: base_as_int + offset
  // This is more explicit and avoids potential GEP optimization issues
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

Value* YuhuBuilder::find_exception_handler() {
  // Option A signature: (JavaThread*, Method*, oop, int*, int) -> int
  // T = thread, K = Method* (metadata pointer), O = oop (exception),
  // I = int* (cp index array), i = int (num_indexes)
  return make_function(
    YuhuRuntime::find_exception_handler_stub(), "TKOIi", "i");
}

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
        LLVMValue::jint_constant(Universe::narrow_klass_shift()));
      return CreateIntToPtr(shifted, YuhuType::klass_type()); // return ptr
    }
    return CreateIntToPtr(CreateZExt(compressed_klass, YuhuType::intptr_type()), YuhuType::klass_type()); // return ptr
  }
  
  // For other cases, use addition-based decoding
  Value *base = LLVMValue::intptr_constant((intptr_t)Universe::narrow_klass_base());
  Value *shifted = CreateShl(
    CreateZExt(compressed_klass, YuhuType::intptr_type()),
    LLVMValue::jint_constant(Universe::narrow_klass_shift()));
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
  return make_function((address) SafepointSynchronize::block, "T", "v");
}

Value* YuhuBuilder::throw_ArithmeticException() {
  return make_function(
    (address) YuhuRuntime::throw_ArithmeticException, "TCi", "v");
}

Value* YuhuBuilder::throw_ArrayIndexOutOfBoundsException() {
  return make_function(
    (address) YuhuRuntime::throw_ArrayIndexOutOfBoundsException, "TCii", "v");
}

Value* YuhuBuilder::throw_ClassCastException() {
  return make_function(
    (address) YuhuRuntime::throw_ClassCastException, "TCi", "v");
}

Value* YuhuBuilder::throw_NullPointerException() {
  return make_function(
    (address) YuhuRuntime::throw_NullPointerException, "TCi", "v");
}

// High-level non-VM calls

Value* YuhuBuilder::f2i() {
  return make_function((address) SharedRuntime::f2i, "f", "i");
}

Value* YuhuBuilder::f2l() {
  return make_function((address) SharedRuntime::f2l, "f", "l");
}

Value* YuhuBuilder::d2i() {
  return make_function((address) SharedRuntime::d2i, "d", "i");
}

Value* YuhuBuilder::d2l() {
  return make_function((address) SharedRuntime::d2l, "d", "l");
}

Value* YuhuBuilder::is_subtype_of() {
  return make_function((address) YuhuRuntime::is_subtype_of, "KK", "c");
}

Value* YuhuBuilder::current_time_millis() {
  return make_function((address) os::javaTimeMillis, "", "l");
}

Value* YuhuBuilder::sin() {
  return make_function("llvm.sin.f64", "d", "d");
}

Value* YuhuBuilder::cos() {
  return make_function("llvm.cos.f64", "d", "d");
}

Value* YuhuBuilder::tan() {
  // Explicit cast to function pointer to resolve overload ambiguity
  return make_function((address) (double (*)(double))::tan, "d", "d");
}

Value* YuhuBuilder::atan2() {
  // Explicit cast to function pointer to resolve overload ambiguity
  return make_function((address) (double (*)(double, double))::atan2, "dd", "d");
}

Value* YuhuBuilder::sqrt() {
  return make_function("llvm.sqrt.f64", "d", "d");
}

Value* YuhuBuilder::log() {
  return make_function("llvm.log.f64", "d", "d");
}

Value* YuhuBuilder::log10() {
  return make_function("llvm.log10.f64", "d", "d");
}

Value* YuhuBuilder::pow() {
  return make_function("llvm.pow.f64", "dd", "d");
}

Value* YuhuBuilder::exp() {
  return make_function("llvm.exp.f64", "d", "d");
}

Value* YuhuBuilder::fabs() {
  // Explicit cast to function pointer to resolve overload ambiguity
  return make_function((address) (double (*)(double))::fabs, "d", "d");
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
  return make_function((address) SharedRuntime::throw_StackOverflowError, "T", "v");
}

Value* YuhuBuilder::uncommon_trap() {
  return make_function((address) YuhuRuntime::uncommon_trap, "Ti", "i");
}

void YuhuBuilder::CreateExperimentalDeoptimize(llvm::ArrayRef<llvm::OperandBundleDef> Bundles) {
  // Get or create the llvm.experimental.deoptimize intrinsic declaration
  llvm::Function* deopt_intrinsic = llvm::Intrinsic::getDeclaration(
    GetInsertBlock()->getModule(),
    llvm::Intrinsic::experimental_deoptimize);
  
  // Create the call with deopt bundle
  llvm::CallInst* call = CreateCall(deopt_intrinsic, {}, Bundles);
  
  // Attach custom Statepoint ID to distinguish deopt statepoints from GC statepoints
  // GC safepoints use DefaultStatepointID (0), deopt traps use 0x1000+
  // This allows runtime to differentiate between GC stackmaps and deopt stackmaps
  llvm::LLVMContext &Ctx = getContext();
  llvm::AttrBuilder AB(Ctx);
  AB.addAttribute("statepoint-id", "4096");
  llvm::AttributeList Attrs = llvm::AttributeList::get(Ctx, llvm::AttributeList::FunctionIndex, AB);
  call->setAttributes(Attrs);
}

Value* YuhuBuilder::debug_stack_overflow_check() {
  // Signature: "Txxxxxx" -> "v" (Thread*, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t -> void)
  return make_function((address) YuhuRuntime::debug_stack_overflow_check, "Txxxxxx", "v");
}

Value* YuhuBuilder::deoptimized_entry_point() {
  // For AArch64, we don't use CppInterpreter, so we need a different approach
  // TemplateInterpreter uses a different deoptimization mechanism
  // The signature is "iT" -> "v": void unpack_with_reexecution(int recurse, Thread* thread)
  // This is used when a callee gets deoptimized and we need to reexecute in the interpreter
  //
  // For AArch64 with TemplateInterpreter, deoptimization is handled by the same
  // SharedRuntime deoptimization blob used by C1 and C2 compilers.
  //
  // The deoptimization blob provides multiple entry points:
  // - unpack_with_reexecution(): for normal deoptimization with re-execution
  // - unpack_with_exception_in_tls(): when there's an exception in TLS
  //
  // YUHU DEOPTIMIZATION STUB APPROACH:
  // Each Yuhu function has its own per-function deoptimization stub that knows
  // how many parameters the function has. The stub restores x0-x7 from the
  // Yuhu frame's locals area before jumping to the standard deoptimization blob.

  address yuhu_stub = NULL;
  if (_function != NULL) {
    yuhu_stub = _function->deoptimization_stub();
  }

  if (yuhu_stub != NULL) {
    // Use per-function Yuhu deoptimization stub
    return make_function(yuhu_stub, "iT", "v");
  } else {
    // Fallback to standard deopt blob (may not work correctly due to missing x0-x7)
    DeoptimizationBlob* deopt_blob = SharedRuntime::deopt_blob();
    assert(deopt_blob != NULL, "deoptimization blob must have been created");

    // Return the reexecution entry point with signature "iT" -> "v"
    // (int recurse, Thread* thread) -> void
    return make_function(
      (address) deopt_blob->unpack_with_reexecution(),
      "iT", "v");
  }
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
  return make_function((address) report_unimplemented, "Ci", "v");
}

Value* YuhuBuilder::should_not_reach_here() {
  return make_function((address) report_should_not_reach_here, "Ci", "v");
}

Value* YuhuBuilder::dump() {
  return make_function((address) YuhuRuntime::dump, "Cx", "v");
}

// Public interface to low-level non-VM calls

CallInst* YuhuBuilder::CreateReadFramePointer() {
  // Read frame pointer register (x29) on AArch64 using inline assembly
  // LLVM's read_register intrinsic doesn't support x29 directly with "fp" name
  // So we use inline assembly to directly read the register
  YuhuContext& ctx = YuhuContext::current();
  
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

CallInst* YuhuBuilder::CreateReadMethodRegister() {
  // Read rmethod register (x12) on AArch64 using inline assembly
  // LLVM's read_register intrinsic doesn't support x12 (and other general-purpose registers except x0)
  // So we use inline assembly to directly read the register
  YuhuContext& ctx = YuhuContext::current();
  
  // Create inline assembly: "mov $0, x12"
  // $0 is the output operand (result register)
  // "=r" means output to a general-purpose register
  llvm::FunctionType* asm_type = llvm::FunctionType::get(YuhuType::intptr_type(), false);
  llvm::InlineAsm* asm_func = llvm::InlineAsm::get(
    asm_type,
    "mov $0, x12",  // AArch64 assembly: move x12 to output register
    "=r",           // Output constraint: =r means output to a register
    false,          // Has side effects: no
    false,          // Is align stack: no
    llvm::InlineAsm::AD_ATT    // Dialect: AT&T style (but for AArch64, this is ignored)
  );
  
  // LLVM 20+ requires FunctionType for CreateCall
  return CreateCall(asm_type, asm_func, std::vector<Value*>(), "rmethod");
}

CallInst* YuhuBuilder::CreateReadThreadRegister() {
  // Read rthread register (x28) on AArch64 using inline assembly
  // LLVM's read_register intrinsic doesn't support x28 (and other general-purpose registers except x0)
  // So we use inline assembly to directly read the register
  YuhuContext& ctx = YuhuContext::current();
  
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

void YuhuBuilder::CreateSaveX0ToX22() {
  // Save x0 to x22 (reserved register) immediately at function entry
  // This preserves p7 (8th parameter) before any code can pollute x0
  // x22 is safe because it's reserved via JTMB (+reserve-x22)
  YuhuContext& ctx = YuhuContext::current();
  
  // Create inline assembly: "mov x22, x0"
  llvm::FunctionType* asm_type = llvm::FunctionType::get(YuhuType::void_type(), false);
  llvm::InlineAsm* asm_func = llvm::InlineAsm::get(
    asm_type,
    "mov x22, x0",  // Move x0 to x22
    "~{x22}",             // No outputs
    true,           // Has side effects
    false,          // Is align stack: no
    llvm::InlineAsm::AD_ATT
  );
  
  CreateCall(asm_type, asm_func);
}

CallInst* YuhuBuilder::CreateReadX22Register() {
  // Read x22 register on AArch64 using inline assembly
  // This is used for reading saved p7 (8th parameter) value
  YuhuContext& ctx = YuhuContext::current();
  
  // Create inline assembly: "mov $0, x22"
  llvm::FunctionType* asm_type = llvm::FunctionType::get(YuhuType::intptr_type(), false);
  llvm::InlineAsm* asm_func = llvm::InlineAsm::get(
    asm_type,
    "mov $0, x22",  // Move x22 to output register
    "=r",          // Output constraint: =r means output to a register
    false,         // Has side effects: no
    false,         // Is align stack: no
    llvm::InlineAsm::AD_ATT
  );
  
  return CreateCall(asm_type, asm_func, std::vector<Value*>(), "p7_saved");
}

CallInst *YuhuBuilder::CreateReadCurrentPC() {
    YuhuContext &ctx = YuhuContext::current();

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

void YuhuBuilder::CreateWriteStackPointer(Value* new_sp) {
  // Write SP register (x31) on AArch64 using inline assembly
  // This is needed to actually modify the SP register, not just calculate a value
  // LLVM's write_register intrinsic doesn't support SP register modification in the same way
  // So we use inline assembly to directly modify the register
  YuhuContext& ctx = YuhuContext::current();
  
  // Create inline assembly: "mov sp, $0"
  // $0 is the input operand (new SP value)
  // "r" means input from a general-purpose register
  // SP (x31) is a special register, but "mov sp, xN" is a valid AArch64 instruction
  llvm::FunctionType* asm_type = llvm::FunctionType::get(
    llvm::Type::getVoidTy(ctx),
    {YuhuType::intptr_type()},  // Input: new SP value
    false);
  
  // Create inline assembly: "mov sp, $0"
  // $0 is the input operand (new SP value)
  // Constraint string: "r" means input from a general-purpose register
  // For InlineAsm::get, the constraint string format is: "output_constraints,input_constraints"
  // Since we have no output (void return) and one input, the constraint string is "r"
  llvm::InlineAsm* asm_func = llvm::InlineAsm::get(
    asm_type,
    "mov sp, $0",  // AArch64 assembly: move input to SP register (x31)
    "r,~{sp}",           // Constraint: "r" means input from a general-purpose register
    true,          // Has side effects: yes (modifies SP)
    true,         // Is align stack: no
    llvm::InlineAsm::AD_ATT    // Dialect: AT&T style (but for AArch64, this is ignored)
  );
  
  // LLVM 20+ requires FunctionType for CreateCall
  std::vector<Value*> args;
  args.push_back(new_sp);
  CreateCall(asm_type, asm_func, args);
}


void YuhuBuilder::CreateEpiloguePlaceholder() {
  // Insert a marker instruction (0xcafebabe) before LLVM epilogue
  // After compilation, we'll parse the prologue and replace this marker
  // with the correct "add sp, x29, #-imm" instruction.

//  YuhuContext& ctx = YuhuContext::current();
//
//  llvm::FunctionType* asm_type = llvm::FunctionType::get(
//    llvm::Type::getVoidTy(ctx),
//    false);
//
//  llvm::InlineAsm* asm_func = llvm::InlineAsm::get(
//    asm_type,
//    ".inst 0xcafebabe",
//    "",
//    true,
//    false,
//    llvm::InlineAsm::AD_ATT);
//
//  CreateCall(asm_type, asm_func, std::vector<Value*>());
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

  CreateStore(
    LLVMValue::jbyte_constant(CardTableModRefBS::dirty_card_val()),
    CreateIntToPtr(
      CreateAdd(
        LLVMValue::intptr_constant(
          (intptr_t) ((CardTableModRefBS *) bs)->byte_map_base),
        CreateLShr(
          CreatePtrToInt(field, YuhuType::intptr_type()),
          LLVMValue::intptr_constant(CardTableModRefBS::card_shift))),
      PointerType::getUnqual(YuhuType::jbyte_type())));
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

Value* YuhuBuilder::CreateInlineOopForStaticField(int cp_index,
                                                   YuhuStack* stack,
                                                   const char* name) {
  llvm::Module* mod = GetInsertBlock()->getModule();
    LLVMContext& ctx = mod->getContext();
  llvm::Type* i32_ty = llvm::Type::getInt32Ty(mod->getContext());
  llvm::Type* ptr_ty = llvm::PointerType::get(mod->getContext(), 0);
  llvm::Type* i64_ty = llvm::Type::getInt64Ty(mod->getContext());
  
  // Resolve the field at compile time to get Klass* and offset
  // Use function()->target_method() which is the method being compiled
  ciMethod* current_method = function()->target_method();
  
  // Get the field using ciEnv's API (uses GUARDED_VM_ENTRY internally)
  ciInstanceKlass* accessor = current_method->holder();
  ciField* field = function()->env()->get_field_by_index(accessor, cp_index);
  
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
    int oop_id = _next_oop_id++;

    // Record in pending_oops array indexed by oop_id
    while (_pending_oops->length() <= oop_id) {
        _pending_oops->append(NULL);
    }
    _pending_oops->at_put(oop_id, jmirror);

    // Generate marker + placeholder using inline assembly
    // Pattern: mov w19, #0xCAFE; movk w19, #0xBABE, lsl #16; mov w20, #oop_id; nop; nop;
    //          mov x0, #low16; movk x0, #mid-low16, lsl #16; movk x0, #mid-high16, lsl #32
    // Total: 8 instructions (3 marker + 2 nops + 3 placeholder) - C1 compatible format
    // Note: High 16 bits must be 0 (not 0xDEAF) because patch_oop only patches low 48 bits
    // The oop_id in marker will be used to look up the real jobject during relocation phase
    char asm_string[512];
    uint64_t temp_placeholder = oop_id & 0xFFFFFFFFFFFFULL;  // Use oop_id as temporary placeholder
    snprintf(asm_string, sizeof(asm_string),
             "mov w19, #0xCAFE\n"
             "movk w19, #0xBABE, lsl #16\n"
             "mov w20, #%d\n"                    // ← oop_id (not oop_index!)
             "nop\n"
             "nop\n"
             "mov ${0:x}, #0x%04lx\n"
             "movk ${0:x}, #0x%04lx, lsl #16\n"
             "movk ${0:x}, #0x%04lx, lsl #32",
             oop_id & 0xFFFF,  // oop_id for marker
             (temp_placeholder >> 0) & 0xFFFF,   // low 16 bits
             (temp_placeholder >> 16) & 0xFFFF,  // mid-low 16 bits
             (temp_placeholder >> 32) & 0xFFFF); // mid-high 16 bits

    llvm::FunctionType* asm_type = llvm::FunctionType::get(
            llvm::Type::getInt64Ty(ctx), {}, false);

    llvm::InlineAsm* marker_asm = llvm::InlineAsm::get(
            asm_type,
            asm_string,
            "=r,~{w19},~{w20},~{memory}",  // Output + clobbers
            true,            // Has side effects: yes (to prevent optimization)
            false,           // Is align stack: no
            llvm::InlineAsm::AD_ATT
    );

    // Emit the marker + placeholder assembly, return the pointer value
    llvm::Value* mirror_oop = CreateIntToPtr(
            CreateCall(asm_type, marker_asm, std::vector<llvm::Value*>()),
            YuhuType::oop_addrspace1_type());
  
  // Calculate field address: mirror + field_offset
  llvm::Value* field_offset_val = llvm::ConstantInt::get(i64_ty, field_offset);
  llvm::Value* field_addr_int = CreateAdd(
    CreatePtrToInt(mirror_oop, i64_ty),
    field_offset_val);
  llvm::Value* field_addr = CreateIntToPtr(field_addr_int, ptr_ty);
  
  // Load field value based on type
  llvm::Value* field_value;
  if (is_object_field) {
      llvm::LoadInst* load_inst;
      if (UseCompressedOops) {
          load_inst = CreateLoad(i32_ty, field_addr, name ? name : "field_value");
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

  Module* mod = YuhuContext::current().module();
  LLVMContext& ctx = mod->getContext();
  llvm::Type* i64_ty = llvm::Type::getInt64Ty(ctx);

  // Non-String oop (e.g. klass mirror): instance of java.lang.Class is allocated in heap
  oop real_oop = object->get_oop();
  // Perhaps, for any class object, we can embed oop address in the IR and create oop relocation later
//  if (real_oop != NULL && real_oop->klass() != SystemDictionary::String_klass()) {
//    uint64_t oop_addr = (uint64_t)(uintptr_t)real_oop;
//    return CreateIntToPtr(llvm::ConstantInt::get(i64_ty, oop_addr), YuhuType::oop_addrspace1_type()); // FIXED - instance of java.lang.Class is allocated in heap
//  }

    ResetNoHandleMark resetNoHandleMark;

  // String oop: allocate unique oop_id and generate marker for deferred oop_index allocation
  // Allocate jobject and assign unique oop_id (like C1 does, but deferred to relocation phase)
  jobject jstring = JNIHandles::make_local(real_oop);
  int oop_id = _next_oop_id++;
  
  // Record in pending_oops array indexed by oop_id
  while (_pending_oops->length() <= oop_id) {
    _pending_oops->append(NULL);
  }
  _pending_oops->at_put(oop_id, jstring);
  
  // Generate marker + placeholder using inline assembly
  // Pattern: mov w19, #0xCAFE; movk w19, #0xBABE, lsl #16; mov w20, #oop_id; nop; nop;
  //          mov x0, #low16; movk x0, #mid-low16, lsl #16; movk x0, #mid-high16, lsl #32
  // Total: 8 instructions (3 marker + 2 nops + 3 placeholder) - C1 compatible format
  // Note: High 16 bits must be 0 (not 0xDEAF) because patch_oop only patches low 48 bits
  // The oop_id in marker will be used to look up the real jobject during relocation phase
  char asm_string[512];
  uint64_t temp_placeholder = oop_id & 0xFFFFFFFFFFFFULL;  // Use oop_id as temporary placeholder
  snprintf(asm_string, sizeof(asm_string),
           "mov w19, #0xCAFE\n"
           "movk w19, #0xBABE, lsl #16\n"
           "mov w20, #%d\n"                    // ← oop_id (not oop_index!)
           "nop\n"
           "nop\n"
           "mov ${0:x}, #0x%04lx\n"
           "movk ${0:x}, #0x%04lx, lsl #16\n"
           "movk ${0:x}, #0x%04lx, lsl #32",
           oop_id & 0xFFFF,  // oop_id for marker
           (temp_placeholder >> 0) & 0xFFFF,   // low 16 bits
           (temp_placeholder >> 16) & 0xFFFF,  // mid-low 16 bits
           (temp_placeholder >> 32) & 0xFFFF); // mid-high 16 bits
  
  llvm::FunctionType* asm_type = llvm::FunctionType::get(
    llvm::Type::getInt64Ty(ctx), {}, false);
  
  llvm::InlineAsm* marker_asm = llvm::InlineAsm::get(
    asm_type,
    asm_string,
    "=r,~{w19},~{w20},~{memory}",  // Output + clobbers
    true,            // Has side effects: yes (to prevent optimization)
    false,           // Is align stack: no
    llvm::InlineAsm::AD_ATT
  );
  
  // Emit the marker + placeholder assembly, return the pointer value
  llvm::Value* str_oop = CreateIntToPtr(
    CreateCall(asm_type, marker_asm, std::vector<llvm::Value*>()),
    YuhuType::oop_addrspace1_type()); // FIXED - 2 reason: 1. string object is allocated in heap; 2. caller expects oop_addrspace1_type to avoid type mismatch
  
  return str_oop;
}

Value* YuhuBuilder::CreateInlineMetadata(::Metadata* metadata, llvm::PointerType* type, const char* name) {
  assert(metadata != NULL, "inlined metadata must not be NULL");
  assert(metadata->is_metaspace_object(), "sanity check");

  Module* mod = YuhuContext::current().module();
  LLVMContext& ctx = mod->getContext();

  // Allocate unique metadata_id and record the Metadata* in pending_metadata.
  // The marker scanner later uses metadata_id (in w20) to look up the entry,
  // verifies that the placeholder address matches, and emits a
  // metadata_Relocation::spec(metadata_index) at the placeholder PC.
  int metadata_id = _next_metadata_id++;
  while (_pending_metadata->length() <= metadata_id) {
    _pending_metadata->append(NULL);
  }
  _pending_metadata->at_put(metadata_id, metadata);

  // Use the Metadata* address directly in the placeholder (no temp_placeholder).
  // Metaspace addresses are stable and fit within 48 bits on AArch64, so the
  // 3-instruction mov/movk/movk sequence is sufficient.
  uint64_t metadata_addr = (uint64_t)(uintptr_t)metadata;
  assert((metadata_addr & 0xFFFF000000000000ULL) == 0,
         "metadata address must fit in 48 bits");

  // Generate marker + placeholder using inline assembly.
  // Pattern (mirrors CreateInlineOop, but with 0xDEAD low marker for metadata):
  //   mov  w19, #0xDEAD                  ; marker low  (distinguishes metadata from oop)
  //   movk w19, #0xBABE, lsl #16         ; marker high
  //   mov  w20, #metadata_id             ; index into _pending_metadata
  //   nop
  //   nop
  //   mov  xN,  #addr[15:0]              ; placeholder = real Metadata* (48 bits)
  //   movk xN,  #addr[31:16], lsl #16
  //   movk xN,  #addr[47:32], lsl #32
  // Total: 8 instructions (3 marker + 2 nops + 3 placeholder).
  // High 16 bits of the placeholder are zero (asserted above), matching
  // the existing scanner contract for 48-bit mov/movk/movk sequences.
  char asm_string[512];
  snprintf(asm_string, sizeof(asm_string),
           "mov w19, #0xDEAD\n"
           "movk w19, #0xBABE, lsl #16\n"
           "mov w20, #%d\n"                    // metadata_id
           "nop\n"
           "nop\n"
           "mov ${0:x}, #0x%04lx\n"
           "movk ${0:x}, #0x%04lx, lsl #16\n"
           "movk ${0:x}, #0x%04lx, lsl #32",
           metadata_id & 0xFFFF,                      // metadata_id for marker
           (metadata_addr >> 0)  & 0xFFFFULL,         // low 16 bits
           (metadata_addr >> 16) & 0xFFFFULL,         // mid-low 16 bits
           (metadata_addr >> 32) & 0xFFFFULL);        // mid-high 16 bits

  llvm::FunctionType* asm_type = llvm::FunctionType::get(
    llvm::Type::getInt64Ty(ctx), {}, false);

  llvm::InlineAsm* marker_asm = llvm::InlineAsm::get(
    asm_type,
    asm_string,
    "=r,~{w19},~{w20},~{memory}",  // Output + clobbers (w19/w20 hold marker)
    true,            // Has side effects: yes (prevent CSE/DCE)
    false,           // Is align stack: no
    llvm::InlineAsm::AD_ATT
  );

  // Emit the marker + placeholder, then cast i64 result to the requested
  // metadata pointer type so callers see the same value type as before.
  return CreateIntToPtr(
    CreateCall(asm_type, marker_asm, std::vector<llvm::Value*>()),
    type,
    name);
}

Value* YuhuBuilder::CreateInlineData(void*       data,
                                      size_t      size,
                                      llvm::Type* type,
                                      const char* name) {
  return CreateIntToPtr(
    code_buffer_address(code_buffer()->inline_data(data, size)),
    type,
    name);
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

BasicBlock* YuhuBuilder::CreateBlock(BasicBlock* ip, const char* name) const {
  return BasicBlock::Create(
    YuhuContext::current(), name, GetInsertBlock()->getParent(), ip);
}

LoadInst* YuhuBuilder::CreateAtomicLoad(Value* ptr, unsigned align, AtomicOrdering ordering, bool isVolatile, const char* name) {
  // LLVM 20 uses opaque pointers, so we need to get the type from the value being loaded
  // For atomic loads, we typically load a word-sized value
  // Since we don't know the exact type from the pointer, we use intptr_type as a default
  // The actual type should be determined by the caller based on the context
  llvm::Type* load_type = YuhuType::intptr_type();
  // LoadInst constructor: LoadInst(Type *Ty, Value *Ptr, const Twine &NameStr, bool isVolatile, Align Align, AtomicOrdering Order, SyncScope::ID SSID, ...)
  return Insert(new LoadInst(load_type, ptr, name, isVolatile, llvm::Align(align), ordering, llvm::SyncScope::System), name);
}

StoreInst* YuhuBuilder::CreateAtomicStore(Value* val, Value* ptr, unsigned align, AtomicOrdering ordering, bool isVolatile, const char* name) {
  // LLVM 20 uses opaque pointers
  // StoreInst constructor: StoreInst(Value *Val, Value *Ptr, bool isVolatile, Align Align, AtomicOrdering Order, SyncScope::ID SSID, ...)
  return Insert(new StoreInst(val, ptr, isVolatile, llvm::Align(align), ordering, llvm::SyncScope::System), name);
}

int YuhuBuilder::get_current_code_offset() const {
  // Get the current code offset from the macro assembler
  return code_buffer()->current_offset();
}

void YuhuBuilder::fixup_prologue_epilogue_markers(address code_start, size_t code_size) {
  if (code_start == NULL || code_size == 0) {
    return;
  }

  // Step 2: Use YuhuPrologueAnalyzer to extract the frame offset from "add x29, sp, #imm"
  int frame_offset_bytes = YuhuPrologueAnalyzer::extract_add_x29_sp_imm(code_start);

  // Step 2: Generate the correct SP restore instruction
  // We need to restore SP from x29 by subtracting the frame offset
  // If prologue was: add x29, sp, #32 (frame pointer = sp + 32)
  // Then epilogue should be: sub sp, x29, #32 (sp = frame pointer - 32)
  uint32_t restore_sp_instruction;
  if (frame_offset_bytes == 0) {
    // No offset needed, use "add sp, x29, #0"
    // Encoding: [31]=1(SF), [30:29]=00(op), [28:24]=10000(ADD), [23:22]=00, [21:10]=0, [9:5]=29(x29), [4:0]=31(sp)
    restore_sp_instruction = 0x91003D9F;
  } else {
    // Use SUB instruction to subtract the frame offset (64-bit version)
    // AArch64 SUB immediate (64-bit) encoding:
    //   [31]=1(SF for 64-bit), [30:29]=00, [28:24]=10010(SUB), [23:22]=00(shift), [21:10]=imm12, [9:5]=xn, [4:0]=xd
    //   sub sp, x29, #imm -> 0xD1... (bit 31=1 for 64-bit registers)
    //
    // Note: ADD/SUB immediate is NOT scaled (unlike LDR/STR), so use frame_offset_bytes directly
    // The immediate must fit in 12 bits (0-4095)

    // Encode: sub sp, x29, #frame_offset_bytes (64-bit version)
    // [31]=1, [30:24]=0b1010010, [23:22]=00, [21:10]=frame_offset_bytes, [9:5]=29(x29), [4:0]=31(sp)
    restore_sp_instruction = (0xD1 << 24) | ((frame_offset_bytes & 0xFFF) << 10) | (29 << 5) | 31;
  }

  // Step 3: Find and replace the marker
  // Scan only 4-byte aligned addresses to avoid alignment issues
  const uint32_t marker_value = 0xcafebabe;

  int replaced_count = 0;
  unsigned char* code_ptr = (unsigned char*)code_start;
  unsigned char* code_end = code_ptr + code_size;

  // Only scan 4-byte aligned addresses (pc += 4, not pc += 1)
  for (unsigned char* pc = code_ptr; pc < code_end - 4; pc += 4) {
    uint32_t inst = *(uint32_t*)pc;

    if (inst == marker_value) {
      // Found the marker! Replace it with SP restore instruction
      *(uint32_t*)pc = restore_sp_instruction;
      replaced_count++;
    }
  }
}

void YuhuBuilder::scan_and_generate_all_relocations(address llvm_code_start, size_t llvm_code_size, CodeBuffer* cb, address code_start, size_t adapter_size) {
    if (llvm_code_start == NULL || llvm_code_size == 0 || code_start == NULL) {
        return;
    }

    // oop_relocation related functions
    // Helper function to check if instruction sequence matches marker pattern
    auto is_marker_pattern = [](uint32_t* instr) -> bool {
        // Check instruction encoding (little-endian):
        // [0] mov w19, #0xCAFE       → 0x52995FD3
        // [1] movk w19, #0xBABE, lsl #16 → 0x72B757D3
        // [2] mov w20, #imm16        → 0x528xxxxxB4 (bits 5-20 contain imm16)
        // [3] nop                    → 0xD503201F
        // [4] nop                    → 0xD503201F

        if (instr[0] == 0x52995FD3 &&  // mov w19, #0xCAFE
            instr[1] == 0x72B757D3 &&  // movk w19, #0xBABE, lsl #16
            (instr[3] & 0xFFFFFFF0) == 0xD5032010 &&  // nop (allow low 4 bits variation)
            (instr[4] & 0xFFFFFFF0) == 0xD5032010) {  // nop
            return true;
        }
        return false;
    };

    // metadata_relocation marker pattern
    // Same shape as oop marker but with low immediate 0xDEAD (vs 0xCAFE).
    //   [0] mov  w19, #0xDEAD            → 0x529BD5B3
    //   [1] movk w19, #0xBABE, lsl #16   → 0x72B757D3
    //   [2] mov  w20, #metadata_id       → 0x528xxxxxB4 (bits 5-20 = metadata_id)
    //   [3] nop, [4] nop
    // Distinguished from last_Java_pc marker (which has 0xDEAD in the HIGH 16 bits).
    auto is_metadata_marker_pattern = [](uint32_t* instr) -> bool {
        if (instr[0] == 0x529BD5B3 &&  // mov w19, #0xDEAD
            instr[1] == 0x72B757D3 &&  // movk w19, #0xBABE, lsl #16
            (instr[3] & 0xFFFFFFF0) == 0xD5032010 &&  // nop
            (instr[4] & 0xFFFFFFF0) == 0xD5032010) {  // nop
            return true;
        }
        return false;
    };

    // Helper function to extract oop_id from marker
    auto extract_oop_id = [](uint32_t* instr) -> int {
        // Extract imm16 from: mov w20, #imm16
        // Encoding: 0x528xxxxxB4, where bits 5-20 contain imm16
        uint32_t mov_instr = instr[2];
        uint16_t imm16 = (mov_instr >> 5) & 0xFFFF;
        return (int)imm16;
    };

    // Helper function to check if 3 instructions form a mov/movk sequence
    auto is_mov_movk_sequence = [](uint32_t* instr) -> bool {
        // Check for mov/movk sequence (C1 compatible format):
        // [0] mov xN, #imm16         → 0xD28xxxxx (bit 31-23 = 0b110100101)
        // [1] movk xN, #imm16, lsl #16 → 0xF2Axxxxx (bit 31-23 = 0b1111001010)
        // [2] movk xN, #imm16, lsl #32 → 0xF2Cxxxxx (bit 31-23 = 0b1111001011)

        // All must be AArch64 mov/movk immediate instructions
        if ((instr[0] & 0xFF800000) != 0xD2800000 ||  // mov xN, #imm16
            (instr[1] & 0xFFE00000) != 0xF2A00000 ||  // movk xN, #imm16, lsl #16
            (instr[2] & 0xFFE00000) != 0xF2C00000) {  // movk xN, #imm16, lsl #32
            return false;
        }

        // Verify all 3 instructions use the same destination register
        int rd0 = instr[0] & 0x1F;  // bits 4-0
        int rd1 = instr[1] & 0x1F;
        int rd2 = instr[2] & 0x1F;

        return (rd0 == rd1 && rd1 == rd2);
    };

    // Helper function to extract 64-bit value from mov/movk sequence
    auto extract_from_movk_sequence = [](uint32_t* instr) -> uint64_t {
        uint16_t imm0 = (instr[0] >> 5) & 0xFFFF;   // bits [15:0]
        uint16_t imm1 = (instr[1] >> 5) & 0xFFFF;   // bits [31:16]
        uint16_t imm2 = (instr[2] >> 5) & 0xFFFF;   // bits [47:32]

        return ((uint64_t)imm2 << 32) | ((uint64_t)imm1 << 16) | (uint64_t)imm0;
    };

    auto patch_oop_index = [](uint32_t* instr, uint32_t* placeholder_instrs, int oop_index) -> bool {
        // Update marker: change w20 from oop_id to oop_index
        uint32_t new_mov_w20 = 0x52800000 | ((oop_index & 0xFFFF) << 5) | (instr[2] & 0x1F);
        instr[2] = new_mov_w20;

        // Update placeholder with real oop_index (high 16 bits = 0)
        uint64_t real_placeholder = oop_index & 0xFFFFFFFFFFFFULL;
        uint16_t imm0 = (real_placeholder >> 0) & 0xFFFF;
        uint16_t imm1 = (real_placeholder >> 16) & 0xFFFF;
        uint16_t imm2 = (real_placeholder >> 32) & 0xFFFF;

        // Patch the 3 mov/movk instructions
        placeholder_instrs[0] = 0xD2800000 | (imm0 << 5) | (placeholder_instrs[0] & 0x1F);           // mov xN, #low16
        placeholder_instrs[1] = 0xF2A00000 | (imm1 << 5) | (placeholder_instrs[1] & 0x1F);           // movk xN, #mid-low, lsl #16
        placeholder_instrs[2] = 0xF2C00000 | (imm2 << 5) | (placeholder_instrs[2] & 0x1F);           // movk xN, #mid-high, lsl #32
        return true;
    };

    auto patch_new_adrp_polling_page = [](uint32_t* instr, uint64_t function_address, uint32_t* blr_instr) -> bool {
        // Calculate new page offset for function_address
        uint32_t rd = instr[0] & 0x1F;  // Extract Rd first

        uint64_t adrp_addr = (uint64_t)instr;  // Address of ADRP instruction in CodeCache
        uint64_t func_page = function_address & ~0xFFFULL;  // Function's page address
        uint64_t new_pc_page = adrp_addr & ~0xFFFULL;  // ADRP's PC page

        int64_t page_diff = (int64_t)(func_page - new_pc_page) >> 12;  // Page difference

        // Encode new ADRP
        uint32_t new_adrp = (instr[0] & 0x9F000000) | rd;  // Keep opcode and Rd
        new_adrp |= (page_diff & 0x3) << 29;  // immlo (bits [30:29])
        new_adrp |= ((page_diff >> 2) & 0x7FFFF) << 5;  // immhi (bits [23:5])

        instr[0] = new_adrp;

        // Patch ldr instruction
        uint32_t ldr_wzr = 0xB9400000 | 31 | (rd << 5); // Rd = wzr (31), Rn = rd

        instr[1] = ldr_wzr;

        blr_instr[0] = 0xD503201F; // nop instruction
        return true;
    };

    auto patch_new_adrp = [](uint32_t* instr, uint64_t function_address) -> bool {
        // Calculate new page offset for function_address
        uint32_t rd = instr[0] & 0x1F;  // Extract Rd first

        uint64_t adrp_addr = (uint64_t)instr;  // Address of ADRP instruction in CodeCache
        uint64_t func_page = function_address & ~0xFFFULL;  // Function's page address
        uint64_t new_pc_page = adrp_addr & ~0xFFFULL;  // ADRP's PC page

        int64_t page_diff = (int64_t)(func_page - new_pc_page) >> 12;  // Page difference

        // Encode new ADRP
        uint32_t new_adrp = (instr[0] & 0x9F000000) | rd;  // Keep opcode and Rd
        new_adrp |= (page_diff & 0x3) << 29;  // immlo (bits [30:29])
        new_adrp |= ((page_diff >> 2) & 0x7FFFF) << 5;  // immhi (bits [23:5])

        instr[0] = new_adrp;

        // Patch add instruction
        // Calculate offset within page
        uint32_t new_page_offset = function_address & 0xFFF;  // Lower 12 bits

        // Encode ADD instruction: add x8, x8, #page_offset
        // ADD (immediate): 0x91000000 | (imm12 << 10) | (shift << 22) | Rn | Rd
        uint32_t new_add = 0x91000000;  // ADD opcode
        new_add |= (new_page_offset & 0xFFF) << 10;  // imm12 (bits [21:10])
        new_add |= rd;  // Rd = same register as ADRP
        new_add |= rd << 5;  // Rn = same register

        instr[1] = new_add;
        return true;
    };

    int marker_count = 0;
    int metadata_marker_count = 0;
    int adrp_count = 0;

    // Scan machine code for marker pattern
    for (size_t i = 0; i < llvm_code_size / 4; i++) {
        uint32_t* llvm_instr = (uint32_t*)(llvm_code_start + i * 4);

        if (is_marker_pattern(llvm_instr)) {
            // Extract oop_id from marker
            int oop_id = extract_oop_id(llvm_instr);

            // Look up the real jobject from pending_oops using oop_id
            assert(oop_id >= 0 && oop_id < _pending_oops->length(), "oop_id out of range");
            jobject jstring = _pending_oops->at(oop_id);
            assert(jstring != NULL, "jstring must not be NULL");

            // Allocate real oop_index from the final OopRecorder (like C1 does)
            int oop_index = cb->oop_recorder()->allocate_oop_index(jstring);

            // Placeholder is immediately after marker (5 instructions = 20 bytes)
            // Check if the next 3 instructions are mov/movk sequence
            uint32_t* llvm_placeholder_instrs = (uint32_t*)(llvm_code_start + (i + 5) * 4);

            if (!is_mov_movk_sequence(llvm_placeholder_instrs)) {
                continue;
            }
            // here we are manipulating actual machine code in code buffer
            uint32_t* instr = (uint32_t*)(code_start + i * 4 + adapter_size);
            assert(is_marker_pattern(instr), "should be marker pattern");
            uint32_t* placeholder_instrs = (uint32_t*)(code_start + (i + 5) * 4 + adapter_size);
            assert(is_mov_movk_sequence(placeholder_instrs), "should be mov/movk sequences");

            uint64_t full_placeholder = extract_from_movk_sequence(placeholder_instrs);
            int placeholder_oop_id = (int)(full_placeholder & 0xFFFFFFFFFFFFULL);

            if (placeholder_oop_id == oop_id) {
                bool oop_index_patched = patch_oop_index(instr, placeholder_instrs, oop_index);
                assert(oop_index_patched, "should patch successfully");

                // Generate HotSpot relocation record using CodeBuffer::relocate
                RelocationHolder rspec = oop_Relocation::spec(oop_index);
                cb->relocate((address)placeholder_instrs, rspec);
                marker_count++;
            }
        } else if (is_metadata_marker_pattern(llvm_instr)) {
            // Metadata marker handling.
            // Unlike oops, the placeholder already holds the real Metadata*
            // address (CreateInlineMetadata embeds the address directly).
            // We therefore only need to:
            //   1. extract metadata_id from w20
            //   2. look up the Metadata* in _pending_metadata
            //   3. sanity-check that the placeholder address matches
            //   4. allocate a metadata_index and emit metadata_Relocation
            // The instruction stream is NOT patched (the immediate is already correct).
            int metadata_id = extract_oop_id(llvm_instr);  // same imm16-from-w20 extraction

            assert(metadata_id >= 0 && metadata_id < _pending_metadata->length(),
                   "metadata_id out of range");
            ::Metadata *metadata = _pending_metadata->at(metadata_id);
            assert(metadata != NULL, "metadata must not be NULL");
            assert(metadata->is_metaspace_object(), "sanity check");

            // Placeholder is immediately after the 5-instruction marker block.
            uint32_t *llvm_placeholder_instrs = (uint32_t*) (llvm_code_start + (i + 5) * 4);
            if (!is_mov_movk_sequence(llvm_placeholder_instrs)) {
                continue;
            }

            // Locate the matching placeholder in the actual CodeBuffer.
            uint32_t *instr = (uint32_t*) (code_start + i * 4 + adapter_size);
            assert(is_metadata_marker_pattern(instr), "should be metadata marker pattern");
            uint32_t *placeholder_instrs = (uint32_t*) (code_start + (i + 5) * 4 + adapter_size);
            assert(is_mov_movk_sequence(placeholder_instrs), "should be mov/movk sequences");

            // Verify the embedded address matches the recorded Metadata*.
            // The placeholder encodes 48 bits; metaspace addresses fit in 48 bits.
            uint64_t embedded_addr = extract_from_movk_sequence(placeholder_instrs);
            uint64_t expected_addr = (uint64_t) (uintptr_t) metadata;
            assert((expected_addr & 0xFFFF000000000000ULL) == 0,
                   "metadata address must fit in 48 bits");
            assert(embedded_addr == expected_addr,
                   "placeholder address must match recorded Metadata*");

            // Allocate a metadata_index from the OopRecorder. Note:
            // metadata_Relocation::spec(idx) requires idx > 0 (idx==0 is reserved
            // for unrecorded). allocate_metadata_index() returns indices >= 1.
            int metadata_index = cb->oop_recorder()->allocate_metadata_index(metadata);
            assert(metadata_index > 0, "metadata_index must be > 0");

            // Emit the relocation record at the placeholder PC. The first
            // instruction of the mov/movk/movk triplet is the relocation
            // address (HotSpot AArch64 convention).
            RelocationHolder rspec = metadata_Relocation::spec(metadata_index);
            cb->relocate((address) placeholder_instrs, rspec);
            metadata_marker_count++;
        } else if (is_mov_movk_sequence(llvm_instr)) {
            uint64_t llvm_target_address = extract_from_movk_sequence(llvm_instr);
            CallSiteType call_type = YuhuDebugInformationRecorder::get()->get_call_site_type_by_helper_address_and_call_target_offset(llvm_target_address,
                                                                                                                                      i * 4);
            if (call_type != CallSiteType::vm_call && call_type != CallSiteType::java_call) {
                continue;
            }

            uint32_t *instr = (uint32_t*) (code_start + i * 4 + adapter_size);
            assert(is_mov_movk_sequence(instr), "should be mov/movk sequences");
            uint64_t target_address = extract_from_movk_sequence(instr);
            assert(llvm_target_address == target_address, "should be the same");

            cb->relocate((address)instr, relocInfo::runtime_call_type);
        } else if (YuhuVirtualAddressScanner::is_adrp_pattern(llvm_instr)) {
            // Locate target page
            int64_t page_offset = YuhuVirtualAddressScanner::extract_page_offset(llvm_instr);
            uint64_t pc_page = ((uint64_t)llvm_instr) & ~0xFFFULL;
            uint64_t target_page = pc_page + page_offset;

            uint32_t imm12 = (llvm_instr[1] >> 10) & 0xFFF;
            uint64_t offset = imm12 << 3;

            uint64_t target_address = target_page + offset;

            uint64_t function_address = *(uint64_t*)target_address;

            uint64_t llvm_blr_offset = YuhuDebugInformationRecorder::get()->get_call_site_blr_offset_by_helper_address_and_call_target_offset(function_address,
                                                                                                                                              i * 4);
            assert(llvm_blr_offset != 0, "should be valid offset");

            uint32_t* instr = (uint32_t*)(code_start + i * 4 + adapter_size);
            assert(YuhuVirtualAddressScanner::is_adrp_pattern(instr), "should be adrp instructions");
            uint32_t* blr_instr = (uint32_t*)(code_start + llvm_blr_offset + adapter_size);

            // Create relocation record
            uint64_t safepoint_poll_addr = (uint64_t)&gc_safepoint_poll;
            if (function_address == safepoint_poll_addr) {
                // Before patch :
                //
                // adrp   x8, <GOT_page>           # Points to GOT
                // ldr    x8, [x8, #<offset>]      # Loads function address from GOT
                // ...inst...
                // blr    x8                       # Branches to function

                // After patch :
                //
                // adrp   x8, <polling_page>       # Points to polling page
                // ldr    wzr, [x8]                # Loads polling page
                // ...inst...
                // nop                             # nop

                // Patch adrp instruction
                bool new_adrp_polling_page_patched = patch_new_adrp_polling_page(instr, (uint64)os::get_polling_page(), blr_instr);
                assert(new_adrp_polling_page_patched, "should patch successfully");
                cb->relocate((address)instr, relocInfo::poll_type);
            } else {
                // Before patch :
                //
                // adrp   x8, <GOT_page>           # Points to GOT
                // ldr    x8, [x8, #<offset>]      # Loads function address from GOT
                // blr    x8                       # Branches to function

                // After patch :
                //
                // adrp   x8, <function_page>      # Points to function's page
                // add    x8, x8, #<page_offset>   # Add offset within page
                // blr    x8                       # Branches to function
                bool new_adrp_patched = patch_new_adrp(instr, function_address);
                assert(new_adrp_patched, "should patch successfully");
                cb->relocate((address)instr, relocInfo::runtime_call_type);
            }
            adrp_count++;
        }
    }

    if (YuhuTraceOffset) {
        tty->print_cr("Yuhu: Found %d oop markers and generated %d relocation records",
                      marker_count, marker_count);
        tty->print_cr("Yuhu: Found %d metadata markers and generated %d relocation records",
                      metadata_marker_count, metadata_marker_count);
        tty->print_cr("Yuhu: Found %d adrp instructions and generated %d relocation records",
                      adrp_count, adrp_count);
        tty->flush();
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

// Callee-saved register preservation across Java method calls
// Save area is at [sp, #80] (right after LLVM spill slots, 10 words = 80 bytes)
// We save: x19, x20, x23, x25, x27 (5 registers = 40 bytes) + 8 bytes padding
// Total: 6 words = 48 bytes, maintaining 16-byte SP alignment

void YuhuBuilder::CreateSaveCalleeSavedRegisters() {
//  llvm::LLVMContext& ctx = getContext();
//  llvm::FunctionType* asm_type = llvm::FunctionType::get(
//    llvm::Type::getVoidTy(ctx), {}, false);
//
//  llvm::InlineAsm* save_asm = llvm::InlineAsm::get(
//    asm_type,
//    "stp x19, x20, [sp, #80]\n\t"
//    "stp x23, x25, [sp, #96]\n\t"
//    "str x27, [sp, #112]",
//    "~{memory}", true, true, llvm::InlineAsm::AD_ATT);
//
//  CreateCall(asm_type, save_asm, {});
}

void YuhuBuilder::CreateRestoreCalleeSavedRegisters() {
//  llvm::LLVMContext& ctx = getContext();
//  llvm::FunctionType* asm_type = llvm::FunctionType::get(
//    llvm::Type::getVoidTy(ctx), {}, false);
//
//  llvm::InlineAsm* restore_asm = llvm::InlineAsm::get(
//    asm_type,
//    "ldr x27, [sp, #112]\n\t"
//    "ldp x23, x25, [sp, #96]\n\t"
//    "ldp x19, x20, [sp, #80]",
//    "~{x19},~{x20},~{x23},~{x25},~{x27},~{memory}", true, false, llvm::InlineAsm::AD_ATT);
//
//  CreateCall(asm_type, restore_asm, {});
}
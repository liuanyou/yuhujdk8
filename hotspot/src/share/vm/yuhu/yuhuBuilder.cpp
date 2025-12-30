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
#include "runtime/synchronizer.hpp"
#include "runtime/thread.hpp"
#include "interpreter/interpreter.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/llvmValue.hpp"
#include "yuhu/yuhuBuilder.hpp"
#include "yuhu/yuhuContext.hpp"
#include "yuhu/yuhuRuntime.hpp"
#include "utilities/debug.hpp"

using namespace llvm;

YuhuBuilder::YuhuBuilder(YuhuCodeBuffer* code_buffer)
  : IRBuilder<>(YuhuContext::current()),
    _code_buffer(code_buffer) {
}

// Helpers for accessing structures
// Use llvm::Type explicitly to avoid conflict with HotSpot's Type class
Value* YuhuBuilder::CreateAddressOfStructEntry(Value*      base,
                                                ByteSize    offset,
                                                llvm::Type* type,
                                                const char* name) {
  // LLVM 20+ uses opaque pointer types, so we can't get element type from PointerType
  // Instead, we use CreateGEP with explicit index calculation
  // For struct access, we calculate the byte offset and use CreateGEP
  // Note: We use jbyte_type() as the element type for byte-level addressing
  Value* byte_offset = LLVMValue::intptr_constant(in_bytes(offset));
  Value* gep = CreateGEP(YuhuType::jbyte_type(), base, byte_offset);
  return CreateBitCast(gep, type, name);
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
          tty->print_cr("Yuhu: CreateLoad - Module DataLayout available: %s", dlStr.c_str());
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
    return YuhuType::oop_type();
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
  return make_function(
    (address) YuhuRuntime::find_exception_handler, "TIi", "i");
}

Value* YuhuBuilder::monitorenter() {
  return make_function((address) YuhuRuntime::monitorenter, "TM", "v");
}

Value* YuhuBuilder::monitorexit() {
  return make_function((address) YuhuRuntime::monitorexit, "TM", "v");
}

Value* YuhuBuilder::new_instance() {
  return make_function((address) YuhuRuntime::new_instance, "Ti", "v");
}

Value* YuhuBuilder::newarray() {
  return make_function((address) YuhuRuntime::newarray, "Tii", "v");
}

Value* YuhuBuilder::anewarray() {
  return make_function((address) YuhuRuntime::anewarray, "Tii", "v");
}

Value* YuhuBuilder::multianewarray() {
  return make_function((address) YuhuRuntime::multianewarray, "TiiI", "v");
}

Value* YuhuBuilder::register_finalizer() {
  return make_function((address) YuhuRuntime::register_finalizer, "TO", "v");
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

Value* YuhuBuilder::debug_stack_overflow_check() {
  // Signature: "Txxxxxx" -> "v" (Thread*, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t -> void)
  return make_function((address) YuhuRuntime::debug_stack_overflow_check, "Txxxxxx", "v");
}

Value* YuhuBuilder::deoptimized_entry_point() {
  // For AArch64, we don't use CppInterpreter, so we need a different approach
  // TemplateInterpreter uses a different deoptimization mechanism
  // The signature is "iT" -> "i": int main_loop(int recurse, Thread* thread)
  // This is used when a callee gets deoptimized and we need to reexecute in the interpreter
  // 
  // For AArch64 with TemplateInterpreter, deoptimization is handled differently:
  // - Deoptimization::unpack_frames handles frame creation
  // - The interpreter entry point is determined by the method kind
  // - We may need to create a stub function or use a different mechanism
  //
  // TODO: Implement proper deoptimized entry point for AArch64/TemplateInterpreter
  // For now, we return NULL which will cause a runtime error if called
  // This should be replaced with a proper implementation
  return NULL;
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

Value* YuhuBuilder::memset() {
  // LLVM 2.8 added a fifth isVolatile field for memset
  // introduced with LLVM r100304
  return make_function("llvm.memset.p0i8.i32", "Cciii", "v");
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
    "r",           // Constraint: "r" means input from a general-purpose register
    true,          // Has side effects: yes (modifies SP)
    false,         // Is align stack: no
    llvm::InlineAsm::AD_ATT    // Dialect: AT&T style (but for AArch64, this is ignored)
  );
  
  // LLVM 20+ requires FunctionType for CreateCall
  std::vector<Value*> args;
  args.push_back(new_sp);
  CreateCall(asm_type, asm_func, args);
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
  // LLVM 20+ uses opaque pointer types, so we reconstruct FunctionType from signature
  // memset signature: "Cciii" -> "v" (void*, i8, i32, i32, i32 -> void)
  llvm::FunctionType* func_type = llvm::FunctionType::get(
    YuhuType::void_type(),
    std::vector<llvm::Type*>{
      PointerType::getUnqual(YuhuType::jbyte_type()),
      YuhuType::jbyte_type(),
      YuhuType::jint_type(),
      YuhuType::jint_type(),
      YuhuType::jint_type()
    },
    false);
  std::vector<Value*> args;
  args.push_back(dst);
  args.push_back(value);
  args.push_back(len);
  args.push_back(align);
  args.push_back(LLVMValue::jint_constant(0));
  return CreateCall(func_type, memset(), args);
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
  
  // For normal entry, base_pc may be NULL (set in YuhuFunction::initialize)
  // In this case, we need to use an alternative method to get the PC address
  // For now, we'll use a placeholder value (0) and add the offset
  // This is safe because process_pc_slot is only used for debug info
  // TODO: In the future, we could use PC register or frame address
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

Value* YuhuBuilder::CreateInlineOop(jobject object, const char* name) {
  // LLVM 20+ requires explicit type parameter for CreateLoad
  return CreateLoad(
    YuhuType::oop_type(),
    CreateIntToPtr(
      code_buffer_address(code_buffer()->inline_oop(object)),
      PointerType::getUnqual(YuhuType::oop_type())),
    name);
}

Value* YuhuBuilder::CreateInlineMetadata(::Metadata* metadata, llvm::PointerType* type, const char* name) {
  assert(metadata != NULL, "inlined metadata must not be NULL");
  assert(metadata->is_metaspace_object(), "sanity check");
  // LLVM 20+ requires explicit type parameter for CreateLoad
  return CreateLoad(
    type,
    CreateIntToPtr(
      code_buffer_address(code_buffer()->inline_Metadata(metadata)),
      PointerType::getUnqual(type)),
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

void YuhuBuilder::insert_offset_marker(int virtual_offset) {
  // Insert an offset marker to create mapping between virtual offset and actual offset
  // This is used during machine code generation to build the virtual->actual offset mapping
  code_buffer()->insert_offset_marker(virtual_offset);
}

int YuhuBuilder::get_current_code_offset() const {
  // Get the current code offset from the macro assembler
  return code_buffer()->current_offset();
}

void YuhuBuilder::CreateOffsetMarker(int virtual_offset) {
  // Create a distinctive inline assembly marker that creates a recognizable pattern
  // in the generated machine code. The pattern consists of:
  // 1. A magic number (0xDEADBEEF) stored in a register
  // 2. The virtual offset value stored in another register
  // 3. Two consecutive NOP instructions
  // This creates a unique signature: mov w0, #0xDEADBEEF; mov w1, #virtual_offset; nop; nop
  
  YuhuContext& ctx = YuhuContext::current();
  
  // Create function type: void function(void) - no parameters needed
  llvm::FunctionType* asm_type = llvm::FunctionType::get(
    llvm::Type::getVoidTy(ctx),
    {},  // No parameters
    false);
  
  // Create inline assembly that generates a recognizable marker pattern
  // AArch64 instruction pattern:
  // mov w19, #0xBEEF     ; Magic number low 16 bits (w19 is callee-saved, less likely to interfere)
  // movk w19, #0xDEAD, lsl #16  ; Magic number high 16 bits
  // mov w20, #<virtual_offset>   ; Virtual offset (w20 is used for esp in Yuhu)
  // nop                          ; Marker boundary
  // nop                          ; Marker boundary
  char asm_string[256];
  snprintf(asm_string, sizeof(asm_string),
           "mov w19, #0xBEEF\n"
           "movk w19, #0xDEAD, lsl #16\n"
           "mov w20, #%d\n"
           "nop\n"
           "nop",
           virtual_offset & 0xFFFF);  // Ensure virtual_offset fits in 16-bit immediate
  
  llvm::InlineAsm* marker_asm = llvm::InlineAsm::get(
    asm_type,
    asm_string,
    "~{w19},~{w20},~{memory}",  // Clobbers w19, w20, and memory
    true,            // Has side effects: yes (to prevent optimization)
    false,           // Is align stack: no
    llvm::InlineAsm::AD_ATT
  );
  
  // Call the inline assembly
  CreateCall(asm_type, marker_asm, std::vector<llvm::Value*>());
  
  // Record the virtual offset for later mapping
  // Note: We don't record the actual offset here because we don't know it yet
  // It will be determined after machine code generation by scanning for the marker pattern
  code_buffer()->offset_mapper()->add_mapping(virtual_offset, -1);  // -1 indicates unknown actual offset
}

void YuhuBuilder::scan_and_update_offset_markers(address code_start, size_t code_size, YuhuOffsetMapper* mapper) {
  // Scan the generated machine code to find offset markers and update the mapper with actual offsets
  // Marker pattern (AArch64, 20 bytes total):
  //   mov w19, #0xBEEF             (4 bytes: 0x52817DEF or similar encoding)
  //   movk w19, #0xDEAD, lsl #16   (4 bytes: 0x72BBD5B3 or similar encoding)
  //   mov w20, #<virtual_offset>   (4 bytes: varies based on virtual_offset)
  //   nop                          (4 bytes: 0xD503201F)
  //   nop                          (4 bytes: 0xD503201F)
  // 
  // IMPORTANT: The marker indicates where the safepoint setup begins,
  // but the actual OopMap offset should be where last_java_pc is recorded.
  // This is typically at an 'adr' instruction that appears after the marker.
  // We scan forward from the marker to find the 'adr' instruction and use its offset.
  
  if (mapper == NULL) {
    tty->print_cr("Yuhu: scan_and_update_offset_markers - mapper is NULL");
    return;
  }
  
  tty->print_cr("Yuhu: Scanning machine code for offset markers: code_start=%p, code_size=%zu", 
                code_start, code_size);
  
  // AArch64 instruction encodings (little-endian)
  const uint32_t NOP_INSTRUCTION = 0xD503201F;  // nop instruction
  const uint32_t ADR_MASK = 0x9F000000;          // ADR instruction mask
  const uint32_t ADR_PATTERN = 0x10000000;       // ADR instruction pattern
  
  int markers_found = 0;
  
  // Scan the code 4 bytes at a time (instruction-aligned)
  for (size_t offset = 0; offset + 20 <= code_size; offset += 4) {
    address current = code_start + offset;
    uint32_t* instructions = (uint32_t*)current;
    
    // Check for the marker pattern:
    // 1. Check for two consecutive NOPs at the end (instructions[3] and [4])
    if (instructions[3] == NOP_INSTRUCTION && instructions[4] == NOP_INSTRUCTION) {
      uint32_t inst0 = instructions[0];
      uint32_t inst1 = instructions[1];
      uint32_t inst2 = instructions[2];
      
      // Check if inst0 is "mov w19, #<imm>" (MOV immediate to w19)
      bool is_mov_w19 = ((inst0 & 0xFFE00000) == 0x52800000) && ((inst0 & 0x1F) == 19);
      uint32_t imm_low = (inst0 >> 5) & 0xFFFF;
      
      // Check if inst1 is "movk w19, #<imm>, lsl #16" (MOVK with shift)
      bool is_movk_w19 = ((inst1 & 0xFFE00000) == 0x72A00000) && ((inst1 & 0x1F) == 19);
      uint32_t imm_high = (inst1 >> 5) & 0xFFFF;
      
      // Check if the magic number is 0xDEADBEEF
      if (is_mov_w19 && is_movk_w19 && imm_low == 0xBEEF && imm_high == 0xDEAD) {
        // 3. Extract virtual offset from inst2 (mov w20, #<virtual_offset>)
        bool is_mov_w20 = ((inst2 & 0xFFE00000) == 0x52800000) && ((inst2 & 0x1F) == 20);
        
        if (is_mov_w20) {
          int virtual_offset = (inst2 >> 5) & 0xFFFF;  // Extract 16-bit immediate
          int marker_offset = offset;  // Marker position
          
          tty->print_cr("Yuhu: Found offset marker at offset %d: virtual_offset=%d", 
                        marker_offset, virtual_offset);
          
          // CRITICAL: Scan forward from the marker to find the 'adr' instruction
          // that records the actual PC for last_java_pc
          // The 'adr' instruction typically appears within 100 bytes after the marker
          int actual_offset = marker_offset;  // Default to marker offset
          bool found_adr = false;
          
          for (size_t scan_offset = offset + 20; scan_offset < offset + 200 && scan_offset + 4 <= code_size; scan_offset += 4) {
            uint32_t inst = *((uint32_t*)(code_start + scan_offset));
            
            // Check if this is an ADR instruction
            // ADR encoding: |0|immlo|10000|immhi|Rd|
            // Mask: 1F000000, Pattern: 10000000
            if ((inst & ADR_MASK) == ADR_PATTERN) {
              actual_offset = scan_offset;
              found_adr = true;
              tty->print_cr("Yuhu: Found ADR instruction at offset %d for virtual_offset=%d", 
                            actual_offset, virtual_offset);
              break;
            }
          }
          
          if (!found_adr) {
            tty->print_cr("Yuhu: WARNING - No ADR instruction found after marker, using marker offset");
          }
          
          // Update the mapper with the actual offset
          if (mapper->has_mapping(virtual_offset)) {
            mapper->add_mapping(virtual_offset, actual_offset);
            markers_found++;
          } else {
            tty->print_cr("Yuhu: WARNING - Found marker for unknown virtual_offset=%d", virtual_offset);
          }
          
          // Skip past this marker to avoid re-scanning
          offset += 16;
        }
      }
    }
  }
  
  tty->print_cr("Yuhu: Finished scanning, found %d offset markers", markers_found);
  
  // Print the updated mappings
  if (markers_found > 0) {
    mapper->print_mappings();
  }
}
void YuhuBuilder::relocate_oopmaps(YuhuOffsetMapper* offset_mapper, ciEnv* env) {
  // This method should be called after machine code generation and marker scanning
  // to relocate OopMap offsets from virtual to actual values
  
  if (offset_mapper != NULL && env != NULL) {
    tty->print_cr("Yuhu: Relocating OopMaps using offset mapper with %d mappings", 
                  offset_mapper->num_mappings());
    
    // Print the mappings for debugging
    offset_mapper->print_mappings();
    
    // Get the OopMapSet from the DebugInformationRecorder
    OopMapSet* oopmaps = env->debug_info()->_oopmaps;
    if (oopmaps != NULL) {
      tty->print_cr("Yuhu: Found OopMapSet with %d maps", oopmaps->size());
      
      // Relocate each OopMap in the set
      int relocated_count = 0;
      int failed_count = 0;
      
      for (int i = 0; i < oopmaps->size(); i++) {
        OopMap* oopmap = oopmaps->at(i);
        if (oopmap != NULL) {
          int virtual_offset = oopmap->offset();  // Original virtual offset
          int actual_offset = offset_mapper->get_actual_offset(virtual_offset);
          
          if (actual_offset == -1) {
            tty->print_cr("Yuhu: WARNING - No actual offset found for virtual_offset=%d", virtual_offset);
            failed_count++;
            continue;
          }
          
          tty->print_cr("Yuhu: Relocating OopMap %d: virtual=%d -> actual=%d", 
                        i, virtual_offset, actual_offset);
          
          // Update the OopMap's offset to the actual offset
          if (actual_offset != virtual_offset) {
            oopmap->set_offset(actual_offset);
            tty->print_cr("Yuhu: Updated OopMap offset from %d to %d", 
                          virtual_offset, actual_offset);
            relocated_count++;
          }
        }
      }
      
      tty->print_cr("Yuhu: OopMap relocation summary: %d relocated, %d failed", 
                    relocated_count, failed_count);
      
      // CRITICAL: Sort the OopMapSet by offset after relocation
      // The OopMapSet::find_map_at_offset function requires entries to be sorted
      // in ascending order by offset for binary search to work correctly
      tty->print_cr("Yuhu: Sorting OopMapSet by offset...");
      oopmaps->sort_by_offset();

      // Print sorted offsets for verification
      tty->print_cr("Yuhu: OopMapSet after sorting:");
      for (int i = 0; i < oopmaps->size(); i++) {
        OopMap* oopmap = oopmaps->at(i);
        if (oopmap != NULL) {
          tty->print_cr("  OopMap %d: offset=%d", i, oopmap->offset());
        }
      }
    } else {
      tty->print_cr("Yuhu: Warning - No OopMapSet found in DebugInformationRecorder");
    }
  } else {
    tty->print_cr("Yuhu: Warning - No offset_mapper or env provided for OopMap relocation");
  }
}

void YuhuBuilder::adjust_oopmaps_pc_offset(ciEnv* env, int plus_offset) {
    if (env != NULL) {
        // Get the OopMapSet from the DebugInformationRecorder
        OopMapSet *oopmaps = env->debug_info()->_oopmaps;
        if (oopmaps != NULL) {
            for (int i = 0; i < oopmaps->size(); i++) {
                OopMap *oopmap = oopmaps->at(i);
                if (oopmap != NULL) {
                    oopmap->set_offset(oopmap->offset() + plus_offset);
                }
            }

            // Print sorted offsets for verification
            tty->print_cr("Yuhu: OopMapSet after adjusting:");
            for (int i = 0; i < oopmaps->size(); i++) {
                OopMap* oopmap = oopmaps->at(i);
                if (oopmap != NULL) {
                    tty->print_cr("  OopMap %d: offset=%d", i, oopmap->offset());
                }
            }
        }
    }
}

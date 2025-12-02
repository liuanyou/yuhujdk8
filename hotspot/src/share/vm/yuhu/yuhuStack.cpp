/*
 * Copyright (c) 1999, 2012, Oracle and/or its affiliates. All rights reserved.
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
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/yuhuFunction.hpp"
#include "yuhu/yuhuNativeWrapper.hpp"
#include "yuhu/yuhuStack.hpp"
#include "yuhu/yuhuType.hpp"

using namespace llvm;

void YuhuStack::initialize(Value* method) {
  bool setup_sp_and_method = (method != NULL);

  int locals_words  = max_locals();
  int extra_locals  = locals_words - arg_size();
  // For AArch64, header_words = 2 (frame pointer + return address)
  // This matches the frame layout: [fp, lr, ...]
  int header_words  = 2;
  int monitor_words = max_monitors()*frame::interpreter_frame_monitor_size();
  int stack_words   = max_stack();
  int frame_words   = header_words + monitor_words + stack_words;

  _extended_frame_size = frame_words + locals_words;

  // For AArch64, calculate the new stack pointer
  // Get current stack pointer using LLVM's frame address intrinsic
  Value *current_sp = builder()->CreatePtrToInt(
    builder()->CreateGetFrameAddress(),
    YuhuType::intptr_type(),
    "current_sp");
  
  // Calculate new stack pointer (allocate frame on stack)
  Value *stack_pointer = builder()->CreateSub(
    current_sp,
    LLVMValue::intptr_constant((frame_words + extra_locals) * wordSize),
    "new_sp");
  
  // Check for stack overflow
  CreateStackOverflowCheck(stack_pointer);
  
  // Initialize stack pointer storage
  initialize_stack_pointers(stack_pointer);
  if (setup_sp_and_method)
    CreateStoreStackPointer(stack_pointer);

  // Create the frame
  _frame = builder()->CreateIntToPtr(
    stack_pointer,
    PointerType::getUnqual(
      llvm::ArrayType::get(YuhuType::intptr_type(), extended_frame_size())),
    "frame");
  int offset = 0;

  // Expression stack
  _stack_slots_offset = offset;
  offset += stack_words;

  // Monitors
  _monitors_slots_offset = offset;
  offset += monitor_words;

  // Temporary oop slot
  _oop_tmp_slot_offset = offset++;

  // Method pointer
  _method_slot_offset = offset++;
  if (setup_sp_and_method) {
    builder()->CreateStore(
      method, slot_addr(method_slot_offset(), YuhuType::Method_type()));
  }

  // Unextended SP
  builder()->CreateStore(stack_pointer, slot_addr(offset++));

  // PC
  _pc_slot_offset = offset++;

  // Frame header
  // For AArch64, we use a simple frame marker (not Zero-specific)
  // Use a recognizable marker value for debugging
  builder()->CreateStore(
    LLVMValue::intptr_constant(0xDEADBEEF), slot_addr(offset++)); // Frame marker (YUHU_FRAME equivalent)
  Value *fp = slot_addr(offset++);
  _frame_pointer_addr = fp;  // Store frame pointer address

  // Local variables
  _locals_slots_offset = offset;
  offset += locals_words;

  // Push the frame
  assert(offset == extended_frame_size(), "should do");
  
  // For AArch64, frame pointer points to the frame header
  // Store the previous frame pointer (load from last_Java_fp if available, else 0)
  Value *prev_fp = builder()->CreateValueOfStructEntry(
    thread(),
    JavaThread::last_Java_fp_offset(),
    YuhuType::intptr_type(),
    "prev_fp");
  builder()->CreateStore(prev_fp, fp);
  
  // Update frame pointer to point to this frame
  CreateStoreFramePointer(
    builder()->CreatePtrToInt(fp, YuhuType::intptr_type()));
}

// Stack overflow check for AArch64
// AArch64 uses standard ABI stack, so we only need to check the ABI stack
void YuhuStack::CreateStackOverflowCheck(Value* sp) {
  BasicBlock *overflow = CreateBlock("stack_overflow");
  BasicBlock *ok       = CreateBlock("stack_ok");

  // Calculate stack bottom (stack_base - stack_size)
  Value *stack_base = builder()->CreateValueOfStructEntry(
    thread(),
    Thread::stack_base_offset(),
    YuhuType::intptr_type(),
    "stack_base");
  Value *stack_size = builder()->CreateValueOfStructEntry(
    thread(),
    Thread::stack_size_offset(),
    YuhuType::intptr_type(),
    "stack_size");
  Value *stack_bottom = builder()->CreateSub(stack_base, stack_size, "stack_bottom");
  
  // Calculate minimum required stack (stack_bottom + shadow pages)
  Value *min_stack = builder()->CreateAdd(
    stack_bottom,
    LLVMValue::intptr_constant(StackShadowPages * os::vm_page_size()),
    "min_stack");
  
  // Check if new stack pointer is below minimum
  builder()->CreateCondBr(
    builder()->CreateICmpULT(sp, min_stack),
    overflow, ok);

  // Handle overflow
  builder()->SetInsertPoint(overflow);
  // throw_StackOverflowError signature: "T" -> "v" (Thread* -> void)
#if LLVM_VERSION_MAJOR >= 20
  llvm::FunctionType* func_type = YuhuBuilder::make_ftype("T", "v");
  std::vector<Value*> args;
  args.push_back(thread());
  builder()->CreateCall(func_type, builder()->throw_StackOverflowError(), args);
#else
  builder()->CreateCall(builder()->throw_StackOverflowError(), thread());
#endif
  builder()->CreateRet(LLVMValue::jint_constant(0));

  builder()->SetInsertPoint(ok);
}

Value* YuhuStack::CreatePopFrame(int result_slots) {
  assert(result_slots >= 0 && result_slots <= 2, "should be");
  int locals_to_pop = max_locals() - result_slots;

  Value *fp = CreateLoadFramePointer();
  Value *sp = builder()->CreateAdd(
    fp,
    LLVMValue::intptr_constant((1 + locals_to_pop) * wordSize));

  CreateStoreStackPointer(sp);
  CreateStoreFramePointer(
    builder()->CreateLoad(
      YuhuType::intptr_type(),
      builder()->CreateIntToPtr(
        fp, PointerType::getUnqual(YuhuType::intptr_type()))));

  return sp;
}

Value* YuhuStack::slot_addr(int         offset,
                             llvm::Type* type,  // Use llvm::Type to avoid conflict with HotSpot's Type class
                             const char* name) const {
  bool needs_cast = type && type != YuhuType::intptr_type();

  // LLVM 20+ requires Type* for CreateStructGEP
  // LLVM 20 uses opaque pointer types, so we need to reconstruct the struct type
#if LLVM_VERSION_MAJOR >= 20
  // For opaque pointers, we need to use CreateGEP with indices
  // The frame is an array of intptr_t, so we use ArrayType
  llvm::Type* frame_type = llvm::ArrayType::get(YuhuType::intptr_type(), extended_frame_size());
  Value* result = builder()->CreateStructGEP(
    frame_type, _frame, offset, needs_cast ? "" : name);
#else
  Value* result = builder()->CreateStructGEP(
    _frame, offset, needs_cast ? "" : name);
#endif

  if (needs_cast) {
    result = builder()->CreateBitCast(
      result, PointerType::getUnqual(type), name);
  }
  return result;
}

// The bits that differentiate stacks with normal and native frames on top

YuhuStack* YuhuStack::CreateBuildAndPushFrame(YuhuFunction* function,
                                                Value*         method) {
  return new YuhuStackWithNormalFrame(function, method);
}
YuhuStack* YuhuStack::CreateBuildAndPushFrame(YuhuNativeWrapper* wrapper,
                                                Value*              method) {
  return new YuhuStackWithNativeFrame(wrapper, method);
}

YuhuStackWithNormalFrame::YuhuStackWithNormalFrame(YuhuFunction* function,
                                                     Value*         method)
  : YuhuStack(function), _function(function) {
  // For normal frames, the stack pointer and the method slot will
  // be set during each decache, so it is not necessary to do them
  // at the time the frame is created.  However, we set them for
  // non-PRODUCT builds to make crash dumps easier to understand.
  initialize(PRODUCT_ONLY(NULL) NOT_PRODUCT(method));
}
YuhuStackWithNativeFrame::YuhuStackWithNativeFrame(YuhuNativeWrapper* wrp,
                                                     Value*              method)
  : YuhuStack(wrp), _wrapper(wrp) {
  initialize(method);
}

int YuhuStackWithNormalFrame::arg_size() const {
  return function()->arg_size();
}
int YuhuStackWithNativeFrame::arg_size() const {
  return wrapper()->arg_size();
}

int YuhuStackWithNormalFrame::max_locals() const {
  return function()->max_locals();
}
int YuhuStackWithNativeFrame::max_locals() const {
  return wrapper()->arg_size();
}

int YuhuStackWithNormalFrame::max_stack() const {
  return function()->max_stack();
}
int YuhuStackWithNativeFrame::max_stack() const {
  return 0;
}

int YuhuStackWithNormalFrame::max_monitors() const {
  return function()->max_monitors();
}
int YuhuStackWithNativeFrame::max_monitors() const {
  return wrapper()->is_synchronized() ? 1 : 0;
}

BasicBlock* YuhuStackWithNormalFrame::CreateBlock(const char* name) const {
  return function()->CreateBlock(name);
}
BasicBlock* YuhuStackWithNativeFrame::CreateBlock(const char* name) const {
  return wrapper()->CreateBlock(name);
}

address YuhuStackWithNormalFrame::interpreter_entry_point() const {
  // For AArch64 with TemplateInterpreter, use AbstractInterpreter::entry_for_method
  // For now, return NULL as this needs proper implementation
  // TODO: Implement proper interpreter entry point for AArch64/TemplateInterpreter
  return NULL;
}
address YuhuStackWithNativeFrame::interpreter_entry_point() const {
  // For AArch64 with TemplateInterpreter, use AbstractInterpreter::entry_for_method
  // For now, return NULL as this needs proper implementation
  // TODO: Implement proper interpreter entry point for AArch64/TemplateInterpreter
  return NULL;
}

#ifndef PRODUCT
void YuhuStack::CreateAssertLastJavaSPIsNull() const {
#ifdef ASSERT
  BasicBlock *fail = CreateBlock("assert_failed");
  BasicBlock *pass = CreateBlock("assert_ok");

  builder()->CreateCondBr(
    builder()->CreateICmpEQ(
      builder()->CreateLoad(YuhuType::intptr_type(), last_Java_sp_addr()),
      LLVMValue::intptr_constant(0)),
    pass, fail);

  builder()->SetInsertPoint(fail);
  builder()->CreateShouldNotReachHere(__FILE__, __LINE__);
  builder()->CreateUnreachable();

  builder()->SetInsertPoint(pass);
#endif // ASSERT
}
#endif // !PRODUCT

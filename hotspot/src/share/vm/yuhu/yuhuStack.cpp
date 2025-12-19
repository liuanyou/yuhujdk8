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
  // For AArch64, header_words includes all frame header metadata:
  //   - oop_tmp (1 word)
  //   - method (1 word)
  //   - unextended_sp (1 word)
  //   - pc (1 word)
  //   - frame_marker (1 word)
  //   - frame_pointer_addr (1 word)
  // This matches SharkFrame::header_words = 6
  int header_words  = 6;
  int monitor_words = max_monitors()*frame::interpreter_frame_monitor_size();
  int stack_words   = max_stack();
  int frame_words   = header_words + monitor_words + stack_words;

  _extended_frame_size = frame_words + locals_words;

  // For AArch64, calculate the new stack pointer
  // Get actual stack pointer (SP register x31) using read_register intrinsic
  // NOTE: CreateGetFrameAddress() returns FP (frame pointer), not SP (stack pointer)
  // We need the actual SP to correctly allocate stack frames
  Value *current_sp = builder()->CreateReadStackPointer();
  
  // Calculate frame size in bytes
  // CRITICAL: frame_size_bytes MUST include space for LR and FP (2 words)
  // This matches C2's behavior: framesize includes return pc and rfp
  // See macroAssembler_aarch64.cpp:build_frame() and aarch64.ad:1539
  int frame_size_bytes = (extended_frame_size() + 2) * wordSize;  // +2 for LR and FP
  
  // AArch64 requires 16-byte stack alignment
  // Align frame size up to 16 bytes
  frame_size_bytes = align_size_up(frame_size_bytes, 16);
  
  // CRITICAL FIX: Allocate frame FIRST, then save LR and FP to frame top
  // This matches C2's behavior: sub(sp, sp, framesize) then stp(rfp, lr, Address(sp, framesize - 2*wordSize))
  // This ensures LR and FP are saved at the top of the frame (high address), not in expression stack area
  
  // Step 1: Allocate frame on stack (includes space for LR and FP)
  Value *stack_pointer = builder()->CreateSub(
    current_sp,
    LLVMValue::intptr_constant(frame_size_bytes),
    "new_sp");
  
  // Step 2: Save LR and FP to the top of the frame
  // LR at [stack_pointer + frame_size_bytes - 2 * wordSize] = [current_sp - 2 * wordSize]
  // FP at [stack_pointer + frame_size_bytes - 1 * wordSize] = [current_sp - 1 * wordSize]
  // Note: In AArch64, sender_sp = unextended_sp + frame_size = current_sp
  // So [sender_sp - 1] = [current_sp - 1] and [sender_sp - 2] = [current_sp - 2]
  
  // Read LR register (x30)
  Value *lr = builder()->CreateReadLinkRegister();
  
  // Read previous FP (from last_Java_fp)
  Value *prev_fp = builder()->CreateValueOfStructEntry(
    thread(),
    JavaThread::last_Java_fp_offset(),
    YuhuType::intptr_type(),
    "prev_fp");
  
  // Calculate addresses: [stack_pointer + frame_size_bytes - 2*wordSize] and [stack_pointer + frame_size_bytes - wordSize]
  int lr_offset_words = frame_size_bytes / wordSize - 2;  // LR at [frame_top - 2]
  int fp_offset_words = frame_size_bytes / wordSize - 1;   // FP at [frame_top - 1]
  
  // Save LR to [stack_pointer + lr_offset_words]
  Value *lr_save_addr = builder()->CreateIntToPtr(
    builder()->CreateGEP(YuhuType::intptr_type(),
                         builder()->CreateIntToPtr(stack_pointer, PointerType::getUnqual(YuhuType::intptr_type())),
                         LLVMValue::intptr_constant(lr_offset_words)),
    PointerType::getUnqual(YuhuType::intptr_type()));
  builder()->CreateStore(lr, lr_save_addr);
  
  // Save FP to [stack_pointer + fp_offset_words]
  Value *fp_save_addr = builder()->CreateIntToPtr(
    builder()->CreateGEP(YuhuType::intptr_type(),
                         builder()->CreateIntToPtr(stack_pointer, PointerType::getUnqual(YuhuType::intptr_type())),
                         LLVMValue::intptr_constant(fp_offset_words)),
    PointerType::getUnqual(YuhuType::intptr_type()));
  builder()->CreateStore(prev_fp, fp_save_addr);
  
  // Check for stack overflow - checks current_sp to ensure there's enough space
  // for both the new frame and throw_StackOverflowError (if needed)
  // Pass stack_pointer so we can calculate frame_size = current_sp - stack_pointer
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
  // Store the previous frame pointer (reuse prev_fp defined earlier at line 87)
  // prev_fp was already loaded from last_Java_fp and saved to [current_sp - 2]
  // Now we also store it to frame_pointer_addr for frame pointer chain
  builder()->CreateStore(prev_fp, fp);
  
  // Update frame pointer to point to this frame
  CreateStoreFramePointer(
    builder()->CreatePtrToInt(fp, YuhuType::intptr_type()));
}

// Stack overflow check for AArch64
// AArch64 uses standard ABI stack, so we only need to check the ABI stack
// This should match SharkStack::CreateStackOverflowCheck logic for ABI stack
// FIXED: Now checks current_sp instead of stack_pointer to ensure there's enough
// space for throw_StackOverflowError (which needs its own stack frame)
void YuhuStack::CreateStackOverflowCheck(Value* sp) {
  BasicBlock *overflow = CreateBlock("stack_overflow");
  BasicBlock *ok       = CreateBlock("stack_ok");

  // Get actual stack pointer (SP register x31) using read_register intrinsic
  // This is the stack pointer BEFORE allocating the new frame
  // NOTE: CreateGetFrameAddress() returns FP (frame pointer), not SP (stack pointer)
  Value *current_sp = builder()->CreateReadStackPointer();

  // Calculate stack bottom (stack_base - stack_size)
  // This is the lowest address of the thread's stack
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
  
  // Calculate available stack space from current SP to stack bottom
  Value *free_stack = builder()->CreateSub(current_sp, stack_bottom, "free_stack");

  // Calculate frame size (current_sp - stack_pointer)
  Value *frame_size = builder()->CreateSub(current_sp, sp, "frame_size");

  // Calculate minimum required space: StackShadowPages + frame_size
  // StackShadowPages provides space for throw_StackOverflowError and its call chain
  // frame_size is the size of the frame we're about to allocate
  Value *min_required = builder()->CreateAdd(
    LLVMValue::intptr_constant(StackShadowPages * os::vm_page_size()),
    frame_size,
    "min_required");
  
  // Check if we have enough space: free_stack >= min_required
  // If free_stack < min_required, we have a stack overflow
  builder()->CreateCondBr(
    builder()->CreateICmpULT(free_stack, min_required),
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

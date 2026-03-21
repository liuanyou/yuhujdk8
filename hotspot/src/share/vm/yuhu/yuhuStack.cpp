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
#include "yuhu/yuhu_globals.hpp"

using namespace llvm;

void YuhuStack::initialize(Value* method, llvm::AllocaInst* sp_storage_alloca, llvm::BasicBlock* exit_block) {
  bool setup_sp_and_method = (method != NULL);

  int locals_words  = max_locals();
  // For AArch64, header_words includes all frame header metadata:
  //   - oop_tmp (1 word)
  //   - method (1 word)
  //   - unextended_sp (1 word)
  //   - pc (1 word)
  //   - frame_marker (1 word)
  //   - frame_pointer_addr (1 word)
  // This matches SharkFrame::header_words = 6 and yuhu_frame_header_words
  int header_words  = yuhu_frame_header_words;
  int monitor_words = max_monitors()*frame::interpreter_frame_monitor_size();
  int stack_words   = max_stack();
  int frame_words   = header_words + monitor_words + stack_words;

  // Reserve space for ALL callee-saved registers (x19-x28 = 10 regs = 80 bytes)
  // This is the MAXIMUM LLVM could possibly need for register spills.
  // Because when llvm spills registers, it always assumes it is manipulating stack top.
  // But obviously stack top is also yuhu frame area. Creating these spill slots, it
  // prevents spills from corrupting Yuhu frame.
  _extended_frame_size = frame_words + locals_words + yuhu_llvm_spill_slots;

  // For AArch64, calculate the new stack pointer
  // Get actual stack pointer (SP register x31) using read_register intrinsic
  // NOTE: CreateGetFrameAddress() returns FP (frame pointer), not SP (stack pointer)
  // We need the actual SP to correctly allocate stack frames
  Value *current_sp = builder()->CreateReadStackPointer();
  
  // Calculate frame size in bytes
  // NOTE: LR and FP are saved by LLVM's prologue (stp x29, x30, [sp, #-16]!)
  // Yuhu frame only contains: locals, stack, header (NO separate LR/FP)
  int frame_size_bytes = extended_frame_size() * wordSize;
  
  // AArch64 requires 16-byte stack alignment
  // Align frame size up to 16 bytes
  frame_size_bytes = align_size_up(frame_size_bytes, 16);
  
  // Allocate Yuhu frame body (LLVM prologue already saved x29/x30)
  // After LLVM prologue: sp points to saved x29/x30 area
  // We allocate additional space for Yuhu frame body
  Value *stack_pointer = builder()->CreateSub(
    current_sp,
    LLVMValue::intptr_constant(frame_size_bytes),
    "new_sp");
  
  // Check for stack overflow - checks current_sp to ensure there's enough space
  // for both the new frame and throw_StackOverflowError (if needed)
  // Pass stack_pointer so we can calculate frame_size = current_sp - stack_pointer
  CreateStackOverflowCheck(stack_pointer, exit_block);

  // Update SP register
  builder()->CreateWriteStackPointer(stack_pointer);

  // Initialize stack pointer storage
  // For normal entry, sp_storage_alloca was created in function entry block
  // For native wrapper, sp_storage_alloca is NULL, so we create one here (less ideal)
  if (sp_storage_alloca == NULL) {
    // Native wrapper case: create alloca here (in middle of function)
    // This is less ideal but acceptable for native wrappers
    initialize_stack_pointers(stack_pointer, NULL);
    tty->print_cr("Yuhu: WARNING - sp_storage alloca created in middle of function for native wrapper");
    tty->flush();
  } else {
    // Normal entry case: use alloca from entry block
    initialize_stack_pointers(stack_pointer, sp_storage_alloca);
  }

  if (setup_sp_and_method)
    CreateStoreStackPointer(stack_pointer);

  // Create the frame
  _frame = builder()->CreateIntToPtr(
    stack_pointer,
    PointerType::getUnqual(
      llvm::ArrayType::get(YuhuType::intptr_type(), extended_frame_size())),
    "frame");
  int offset = 0;

  offset += yuhu_llvm_spill_slots;

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

  Value *current_fp = builder()->CreateReadFramePointer();

  // Store previous FP to frame pointer slot for frame pointer chain
  builder()->CreateStore(current_fp, fp);
  
  // Update frame pointer to point to this frame
  CreateStoreFramePointer(
    builder()->CreatePtrToInt(current_fp, YuhuType::intptr_type()));
  
  // Get addresses for last_Java_sp, last_Java_fp, and last_Java_pc
  Value *last_sp_addr = last_Java_sp_addr();
  Value *last_fp_addr = last_Java_fp_addr();
  Value *last_pc_addr = builder()->CreateAddressOfStructEntry(
    thread(),
    JavaThread::last_Java_pc_offset(),
    llvm::PointerType::getUnqual(YuhuType::intptr_type()),
    "last_Java_pc_addr");
  
  // Convert addresses to i64 for inline assembly (AArch64 uses 64-bit addresses)
  Value *sp_addr_i64 = builder()->CreatePtrToInt(last_sp_addr, YuhuType::intptr_type(), "sp_addr_i64");
  Value *fp_addr_i64 = builder()->CreatePtrToInt(last_fp_addr, YuhuType::intptr_type(), "fp_addr_i64");
  Value *pc_addr_i64 = builder()->CreatePtrToInt(last_pc_addr, YuhuType::intptr_type(), "pc_addr_i64");
  
  // Create inline assembly to store last_Java_sp
  // "str $1, [$0]" stores the value in $1 to the address in $0
  // "r" constraint means input from a general-purpose register
  // "memory" clobber prevents LLVM from optimizing memory operations
  YuhuContext& ctx = YuhuContext::current();
  llvm::FunctionType* store_asm_type = llvm::FunctionType::get(
    llvm::Type::getVoidTy(ctx),
    {YuhuType::intptr_type(), YuhuType::intptr_type()},  // addr, value
    false);
  
  llvm::InlineAsm* store_sp_asm = llvm::InlineAsm::get(
    store_asm_type,
    "str $1, [$0]",  // AArch64: store value ($1) to address ($0)
    "r,r,~{memory}",  // Both inputs in registers, clobber memory
    true,            // Has side effects: yes (writes to memory)
    false,           // Is align stack: no
    llvm::InlineAsm::AD_ATT
  );
  
  // Call inline assembly to store last_Java_sp
  // stack_pointer is already intptr_type (i64), no conversion needed
  std::vector<Value*> store_sp_args;
  store_sp_args.push_back(sp_addr_i64);
  store_sp_args.push_back(stack_pointer);
  builder()->CreateCall(store_asm_type, store_sp_asm, store_sp_args);

  llvm::InlineAsm* store_fp_asm = llvm::InlineAsm::get(
    store_asm_type,
    "str $1, [$0]",  // AArch64: store value ($1) to address ($0)
    "r,r,~{memory}",  // Both inputs in registers, clobber memory
    true,            // Has side effects: yes (writes to memory)
    false,           // Is align stack: no
    llvm::InlineAsm::AD_ATT
  );

  std::vector<Value*> store_fp_args;
  store_fp_args.push_back(fp_addr_i64);
  store_fp_args.push_back(current_fp);  // Use LLVM prologue's sp, where x29/x30 are saved
  builder()->CreateCall(store_asm_type, store_fp_asm, store_fp_args);

  Value* current_pc = builder()->CreateReadCurrentPC();

  llvm::InlineAsm* store_pc_asm = llvm::InlineAsm::get(
    store_asm_type,
    "str $1, [$0]",  // AArch64: store value ($1) to address ($0)
    "r,r,~{memory}",  // Both inputs in registers, clobber memory
    true,            // Has side effects: yes (writes to memory)
    false,           // Is align stack: no
    llvm::InlineAsm::AD_ATT
  );

  std::vector<Value*> store_pc_args;
  store_pc_args.push_back(pc_addr_i64);
  store_pc_args.push_back(current_pc);
  builder()->CreateCall(store_asm_type, store_pc_asm, store_pc_args);
  
  // DEBUG: Print confirmation
  tty->print_cr("YuhuStack::initialize: Created inline assembly stores for last_Java_sp, last_Java_fp, last_Java_pc");
  tty->flush();
}

// Stack overflow check for AArch64
// AArch64 uses standard ABI stack, so we only need to check the ABI stack
// This should match SharkStack::CreateStackOverflowCheck logic for ABI stack
// FIXED: Now checks current_sp instead of stack_pointer to ensure there's enough
// space for throw_StackOverflowError (which needs its own stack frame)
// exit_block: unified exit block to jump to on overflow (NULL = create ret directly)
void YuhuStack::CreateStackOverflowCheck(Value* sp, llvm::BasicBlock* exit_block) {
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
  // CRITICAL: Jump to the unified exit block instead of creating multiple rets
  // This ensures we only have ONE marker in the entire function
  // If exit_block is NULL (during initialize), create ret directly
  if (exit_block != NULL) {
    builder()->CreateBr(exit_block);
  } else {
    // Called during initialize before unified_exit_block exists
    // Create ret directly (this should rarely happen)
    builder()->CreateRet(LLVMValue::jint_constant(0));
  }

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
                                                Value*         method,
                                                llvm::AllocaInst* sp_storage_alloca) {
  return new YuhuStackWithNormalFrame(function, method, sp_storage_alloca);
}
YuhuStack* YuhuStack::CreateBuildAndPushFrame(YuhuNativeWrapper* wrapper,
                                                Value*              method) {
  return new YuhuStackWithNativeFrame(wrapper, method);
}

YuhuStackWithNormalFrame::YuhuStackWithNormalFrame(YuhuFunction* function,
                                                     Value*         method,
                                                     llvm::AllocaInst* sp_storage_alloca)
  : YuhuStack(function), _function(function) {
  // For normal frames, the stack pointer and the method slot will
  // be set during each decache, so it is not necessary to do them
  // at the time the frame is created.  However, we set them for
  // non-PRODUCT builds to make crash dumps easier to understand.
  initialize(PRODUCT_ONLY(NULL) NOT_PRODUCT(method), sp_storage_alloca, function->unified_exit_block());
}
YuhuStackWithNativeFrame::YuhuStackWithNativeFrame(YuhuNativeWrapper* wrp,
                                                     Value*              method)
  : YuhuStack(wrp), _wrapper(wrp) {
  // Native wrapper doesn't have sp_storage_alloca created in entry block
  // Pass NULL and let initialize() handle it
  // Pass NULL as exit_block because native wrappers handle their own returns
  initialize(method, NULL, NULL);
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

llvm::BasicBlock* YuhuStackWithNormalFrame::unified_exit_block() const {
  return function()->unified_exit_block();
}

llvm::BasicBlock* YuhuStackWithNativeFrame::unified_exit_block() const {
  return NULL;  // Native wrappers handle their own returns
}

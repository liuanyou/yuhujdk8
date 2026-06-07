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

void YuhuStack::initialize(Value* method, llvm::BasicBlock* exit_block) {
  int locals_words  = max_locals();
  // For AArch64, header_words includes all frame header metadata:
  //   - oop_tmp (1 word)
  //   - method (1 word)
  //   - unextended_sp (1 word)
  //   - pc (1 word)
  //   - frame_marker (1 word)
  //   - frame_pointer_addr (1 word)
  // This matches SharkFrame::header_words = 6 and yuhu_frame_header_words
  int header_words  = YUHU_FRAME_HEADER_WORDS;
  int monitor_words = max_monitors()*frame::interpreter_frame_monitor_size();
  int stack_words   = max_stack();
  int frame_words   = header_words + monitor_words + stack_words;

  _extended_frame_size = frame_words + locals_words;
  
  // Calculate frame size in bytes
  // NOTE: LR and FP are saved by LLVM's prologue (stp x29, x30, [sp, #-16]!)
  // Yuhu frame only contains: locals, stack, header (NO separate LR/FP)
  int frame_size_bytes = extended_frame_size() * wordSize;
  
  // AArch64 requires 16-byte stack alignment
  // Align frame size up to 16 bytes
  frame_size_bytes = align_size_up(frame_size_bytes, 16);

  // Convert back to words for alloca array size
  int aligned_frame_size_words = frame_size_bytes / wordSize;

  llvm::AllocaInst* extended_sp = builder()->CreateAlloca(YuhuType::intptr_type(),
                                                          ConstantInt::get(YuhuType::intptr_type(), aligned_frame_size_words),
                                                          "extended_sp");

  // Create the frame
  _frame = builder()->CreateBitCast(
    extended_sp,
    PointerType::getUnqual(
      llvm::ArrayType::get(YuhuType::intptr_type(), aligned_frame_size_words)),
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
    builder()->CreateStore(
      method, slot_addr(method_slot_offset(), YuhuType::Method_type()));

  // Should get the sp value after final allocation, include prologue/main frame/spill areas
  Value* final_sp = builder()->CreateReadStackPointer();

  // Unextended SP
  builder()->CreateStore(final_sp, slot_addr(offset++));

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
    // Called during initialize before unified_exit_block exists.
    // Emit a ret matching the function's declared return type so the verifier
    // accepts it for any return type (int / long / float / double / oop / void).
    llvm::Function* fn = builder()->GetInsertBlock()->getParent();
    llvm::Type* ret_ty = fn->getReturnType();
    if (ret_ty->isVoidTy()) {
      builder()->CreateRetVoid();
    } else {
      builder()->CreateRet(llvm::Constant::getNullValue(ret_ty));
    }
  }

  builder()->SetInsertPoint(ok);
}

llvm::LoadInst* YuhuStack::CreateLoadFramePointer(const char *name) {
    // For AArch64, load frame pointer from frame header
    // LLVM 20+ requires explicit type parameter for CreateLoad
    if (_frame_pointer_addr != NULL) {
        return builder()->CreateLoad(
                YuhuType::intptr_type(),
                _frame_pointer_addr,
                name);
    }
    // Fallback: load from last_Java_fp
    return builder()->CreateLoad(
            YuhuType::intptr_type(),
            builder()->CreateAddressOfStructEntry(
                    thread(),
                    JavaThread::last_Java_fp_offset(),
                    llvm::PointerType::getUnqual(YuhuType::intptr_type())),
            name);
}

llvm::StoreInst* YuhuStack::CreateStoreFramePointer(llvm::Value* value) {
    // For AArch64, store frame pointer in frame header and last_Java_fp
    if (_frame_pointer_addr != NULL) {
        builder()->CreateStore(value, _frame_pointer_addr);
    }
    // Also update last_Java_fp for frame anchor
    return builder()->CreateStore(
            value,
            builder()->CreateAddressOfStructEntry(
                    thread(),
                    JavaThread::last_Java_fp_offset(),
                    llvm::PointerType::getUnqual(YuhuType::intptr_type())));
}

llvm::Value* YuhuStack::last_Java_sp_addr() const {
    return builder()->CreateAddressOfStructEntry(
            thread(),
            JavaThread::last_Java_sp_offset(),
            llvm::PointerType::getUnqual(YuhuType::intptr_type()),
            "last_Java_sp_addr");
}
llvm::Value* YuhuStack::last_Java_fp_addr() const {
    return builder()->CreateAddressOfStructEntry(
            thread(),
            JavaThread::last_Java_fp_offset(),
            llvm::PointerType::getUnqual(YuhuType::intptr_type()),
            "last_Java_fp_addr");
}
llvm::Value* YuhuStack::last_Java_pc_addr() const {
    return builder()->CreateAddressOfStructEntry(
            thread(),
            JavaThread::last_Java_pc_offset(),
            llvm::PointerType::getUnqual(YuhuType::intptr_type()),
            "last_Java_pc_addr");
}

void YuhuStack::CreateSetLastJavaFrame() {
    // Legacy implementation - uses ADR instruction to read PC
    // TODO: This has a bug - ADR reads its own PC, not the return address after call
    // Kept for backward compatibility, but should use CreateSetLastJavaFrameWithPlaceholder()

    // Note that whenever _last_Java_sp != NULL other anchor fields
    // must be valid.  The profiler apparently depends on this.
    builder()->CreateStore(CreateLoadFramePointer(), last_Java_fp_addr());

    // CRITICAL: Use the unextended_sp (SP after frame allocation) instead of
    // current SP which may have been modified during execution
    llvm::Value* unextended_sp = builder()->CreateLoad(
            YuhuType::intptr_type(),
            slot_addr(unextended_sp_slot_offset()));
    builder()->CreateStore(unextended_sp, last_Java_sp_addr());

    // BUG: This uses CreateReadCurrentPC() which generates 'adr x0, .'
    // This reads the PC of the ADR instruction itself, NOT the return address after call!
    llvm::Value* current_pc = builder()->CreateReadCurrentPC();

    llvm::Value *pc_addr_i64 = builder()->CreatePtrToInt(last_Java_pc_addr(), YuhuType::intptr_type(), "pc_addr_i64");

    YuhuContext& ctx2 = YuhuContext::current();
    llvm::FunctionType* store_asm_type2 = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx2),
            {YuhuType::intptr_type(), YuhuType::intptr_type()},
            false);

    llvm::InlineAsm* store_pc_asm = llvm::InlineAsm::get(
            store_asm_type2,
            "str $1, [$0]",
            "r,r,~{memory}",
            true,
            false,
            llvm::InlineAsm::AD_ATT
    );

    std::vector<llvm::Value*> store_pc_args;
    store_pc_args.push_back(pc_addr_i64);
    store_pc_args.push_back(current_pc);
    builder()->CreateCall(store_asm_type2, store_pc_asm, store_pc_args);
}

// NEW: CreateSetLastJavaFrameWithPlaceholderPC - generates adr instruction with marker
// The adr instruction will be patched by JITLink plugin to point to return address after blr
// This generates: marker (mov/movk with virtual_offset) + adr + str pattern
void YuhuStack::CreateSetLastJavaFrameWithPlaceholderPC(uint64_t virtual_address) {
    // Extract virtual_offset from the virtual address (low 16 bits)
    int virtual_offset = virtual_address & 0xFFFF;
    
    // Store FP and SP (same as CreateSetLastJavaFrame)
    builder()->CreateStore(CreateLoadFramePointer(), last_Java_fp_addr());

    llvm::Value* unextended_sp = builder()->CreateLoad(
            YuhuType::intptr_type(),
            slot_addr(unextended_sp_slot_offset()));
    builder()->CreateStore(unextended_sp, last_Java_sp_addr());

    // Generate inline asm with marker + adr instruction
    // The marker embeds virtual_offset for correlation during patching
    // asm template:
    //   mov w19, #0xDEAD              - Marker magic (identifies this as last_Java_pc marker)
    //   movk w19, #virtual_offset, lsl #16 - Virtual offset for correlation
    //   adr x20, .+8                  - adr with dummy offset (will be patched)
    //   str x20, [$0]                 - Store to last_Java_pc
    
    llvm::Module* mod = builder()->GetInsertBlock()->getModule();
    llvm::LLVMContext& ctx = mod->getContext();
    
    // Create function type for inline asm: void (ptr)
    llvm::FunctionType* asm_type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx),
        {llvm::PointerType::getUnqual(llvm::Type::getInt64Ty(ctx))},
        false);
    
    // Inline asm string with marker and adr (no label needed)
    // Use virtual_offset in the marker for identification during patching
    char asm_buf[256];
    snprintf(asm_buf, sizeof(asm_buf),
        "mov w19, #%d\n" // Virtual offset (embedded for correlation)
        "movk w19, #0xDEAD, lsl #16\n"           // Marker magic
        "adr x20, .+8\n"               // adr with dummy offset (+8 bytes, will be patched)
        "str x20, [$0]\n",             // Store address to last_Java_pc
        virtual_offset);
    
    std::string asm_string(asm_buf);
    
    // Constraints: "r" means input operand goes to a general register
    // $0 will be replaced with the last_Java_pc address
    llvm::InlineAsm* asm_inst = llvm::InlineAsm::get(
        asm_type,
        asm_string,
        "r,~{x19},~{x20},~{memory}",  // Constraint string
        true, // hasSideEffects
        true  // isAlignStack
    );
    
    // Get the address of last_Java_pc
    llvm::Value* last_java_pc_addr = last_Java_pc_addr();
    
    // Create the inline asm call
    builder()->CreateCall(asm_inst, {last_java_pc_addr});
}

void YuhuStack::CreateSetLastJavaFrameWithPlaceholderNoPC(uint64_t virtual_address) {
    // Extract virtual_offset from the virtual address (low 16 bits)
    int virtual_offset = virtual_address & 0xFFFF;

    // Store FP and SP (same as CreateSetLastJavaFrame)
    builder()->CreateStore(CreateLoadFramePointer(), last_Java_fp_addr());

    llvm::Value* unextended_sp = builder()->CreateLoad(
            YuhuType::intptr_type(),
            slot_addr(unextended_sp_slot_offset()));
    builder()->CreateStore(unextended_sp, last_Java_sp_addr());

    // Generate inline asm with marker + adr instruction
    // The marker embeds virtual_offset for correlation during patching
    // asm template:
    //   mov w19, #0xDEAD              - Marker magic (identifies this as last_Java_pc marker)
    //   movk w19, #virtual_offset, lsl #16 - Virtual offset for correlation

    llvm::Module* mod = builder()->GetInsertBlock()->getModule();
    llvm::LLVMContext& ctx = mod->getContext();

    // Create function type for inline asm: void (ptr)
    llvm::FunctionType* asm_type = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx),
            {},
            false);

    // Inline asm string with marker and adr (no label needed)
    // Use virtual_offset in the marker for identification during patching
    char asm_buf[256];
    snprintf(asm_buf, sizeof(asm_buf),
             "mov w19, #%d\n" // Virtual offset (embedded for correlation)
             "movk w19, #0xDEAD, lsl #16\n",           // Marker magic
             virtual_offset);

    std::string asm_string(asm_buf);

    // Constraints: "r" means input operand goes to a general register
    // $0 will be replaced with the last_Java_pc address
    llvm::InlineAsm* asm_inst = llvm::InlineAsm::get(
            asm_type,
            asm_string,
            "~{x19},~{memory}",  // Constraint string
            true, // hasSideEffects
            true  // isAlignStack
    );

    // Create the inline asm call
    builder()->CreateCall(asm_inst, {});
}

void YuhuStack::CreateResetLastJavaFrame() {
    builder()->CreateStore(LLVMValue::intptr_constant(0), last_Java_sp_addr());
    builder()->CreateStore(LLVMValue::intptr_constant(0), last_Java_fp_addr());
    builder()->CreateStore(LLVMValue::intptr_constant(0), last_Java_pc_addr());
}

void YuhuStack::CreateResetLastJavaFrameWithNoPC() {
    builder()->CreateStore(LLVMValue::intptr_constant(0), last_Java_sp_addr());
    builder()->CreateStore(LLVMValue::intptr_constant(0), last_Java_fp_addr());
}

/**
 * It returns i64 value. And caller must convert return value to function type when doing CreateCall.
 *
 * @param virtual_address
 * @param call_target_va
 * @return
 */
llvm::Value* YuhuStack::CreateCallSitePlaceholderWithCallTarget(uint64_t virtual_address, uint64_t call_target_va, CallSiteType call_site_type) {
    // Extract virtual_offset from the virtual address (low 16 bits)
    int virtual_offset = virtual_address & 0xFFFF;
    int call_site_kind = (static_cast<uint8_t>(call_site_type)) & 0xFFFF;

    // Generate inline asm with marker + adr instruction
    // The marker embeds virtual_offset for correlation during patching
    // asm template:
    //   mov w19, #0xDEAD              - Marker magic (identifies this as last_Java_pc marker)
    //   movk w19, #virtual_offset, lsl #16 - Virtual offset for correlation

    llvm::Module* mod = builder()->GetInsertBlock()->getModule();
    llvm::LLVMContext& ctx = mod->getContext();

    // Inline asm string with marker and adr (no label needed)
    // Use virtual_offset in the marker for identification during patching
    char asm_string[512];
    snprintf(asm_string, sizeof(asm_string),
             "mov w19, #%d\n"
             "movk w19, #0xDEAD, lsl #16\n"
             "mov w20, #%d\n"
             "nop\n"
             "nop\n"
             "movz ${0:x}, #0x%04lx, lsl #0\n"
             "movk ${0:x}, #0x%04lx, lsl #16\n"
             "movk ${0:x}, #0x%04lx, lsl #32",
             virtual_offset,  // virtual_offset
             call_site_kind,  // call_site_type
             (call_target_va >> 0) & 0xFFFF,   // low 16 bits
             (call_target_va >> 16) & 0xFFFF,  // mid-low 16 bits
             (call_target_va >> 32) & 0xFFFF); // mid-high 16 bits

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

    // Create the inline asm call
    return builder()->CreateCall(asm_type, marker_asm, std::vector<llvm::Value*>());
}

void YuhuStack::CreateCallSitePlaceholder(uint64_t virtual_address) {
    // Extract virtual_offset from the virtual address (low 16 bits)
    int virtual_offset = virtual_address & 0xFFFF;

    // Generate inline asm with marker + adr instruction
    // The marker embeds virtual_offset for correlation during patching
    // asm template:
    //   mov w19, #0xDEAD              - Marker magic (identifies this as last_Java_pc marker)
    //   movk w19, #virtual_offset, lsl #16 - Virtual offset for correlation

    llvm::Module* mod = builder()->GetInsertBlock()->getModule();
    llvm::LLVMContext& ctx = mod->getContext();

    // Create function type for inline asm: void (ptr)
    llvm::FunctionType* asm_type = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx),
            {},
            false);

    // Inline asm string with marker and adr (no label needed)
    // Use virtual_offset in the marker for identification during patching
    char asm_buf[256];
    snprintf(asm_buf, sizeof(asm_buf),
             "mov w19, #%d\n" // Virtual offset (embedded for correlation)
             "movk w19, #0xDEAD, lsl #16\n",           // Marker magic
             virtual_offset);

    std::string asm_string(asm_buf);

    // Constraints: "r" means input operand goes to a general register
    // $0 will be replaced with the last_Java_pc address
    llvm::InlineAsm* asm_inst = llvm::InlineAsm::get(
            asm_type,
            asm_string,
            "~{x19},~{memory}",  // Constraint string
            true, // hasSideEffects
            true  // isAlignStack
    );

    // Create the inline asm call
    builder()->CreateCall(asm_inst, {});
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
  initialize(PRODUCT_ONLY(NULL) NOT_PRODUCT(method), function->unified_exit_block());
}
YuhuStackWithNativeFrame::YuhuStackWithNativeFrame(YuhuNativeWrapper* wrp,
                                                     Value*              method)
  : YuhuStack(wrp), _wrapper(wrp) {
  // Native wrapper doesn't have sp_storage_alloca created in entry block
  // Pass NULL and let initialize() handle it
  // Pass NULL as exit_block because native wrappers handle their own returns
  initialize(method, NULL);
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

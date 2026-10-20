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

// File-based logging for debugging
static FILE* yuhu_stack_map_log = NULL;
static void yuhu_stack_map_log_init() {
    if (yuhu_stack_map_log == NULL) {
        yuhu_stack_map_log = fopen(YuhuStackMapFile, "a");
    }
}
#define YUHU_STACK_MAP_LOG(fmt, ...) \
    do { \
        yuhu_stack_map_log_init(); \
        if (yuhu_stack_map_log) { \
            fprintf(yuhu_stack_map_log, fmt "\n", ##__VA_ARGS__); \
            fflush(yuhu_stack_map_log); \
        } \
    } while(0)

void YuhuStack::initialize(Value* method, YuhuInlineNodeOrMethod* inline_node_or_method, bool is_inlinee) {
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
  // No need to align 16 bytes here, llvm will handle it

  if (YuhuTraceInstalls) {
      ResourceMark rm;
      ciMethod* target = inline_node_or_method->target();
      if (YuhuStackMapFile != NULL) {
          YUHU_STACK_MAP_LOG("Yuhu: method %s.%s signature %s - stack is initialized with header_words=%d, monitor_words=%d, stack_words=%d, local_words=%d, extended_frame_size=%d",
                             target->holder()->name()->as_utf8(),
                             target->name()->as_utf8(),
                             target->signature()->as_symbol()->as_utf8(),
                             header_words, monitor_words, stack_words, locals_words, _extended_frame_size);
      } else {
          tty->print_cr(
                  "Yuhu: method %s.%s signature %s - stack is initialized with header_words=%d, monitor_words=%d, stack_words=%d, local_words=%d, extended_frame_size=%d",
                  target->holder()->name()->as_utf8(),
                  target->name()->as_utf8(),
                  target->signature()->as_symbol()->as_utf8(),
                  header_words, monitor_words, stack_words, locals_words, _extended_frame_size);
      }
  }

  uint64_t statepoint_id = YuhuStatepointIDGenerator::next();
  YuhuDebugInformationRecorder* recorder = YuhuDebugInformationRecorder::get();
  recorder->register_frame_layout_frame_fields(statepoint_id, inline_node_or_method, header_words, monitor_words, stack_words, locals_words, _extended_frame_size);

  llvm::AllocaInst* extended_sp = builder()->CreateAlloca(YuhuType::intptr_type(),
                                                          ConstantInt::get(YuhuType::intptr_type(), _extended_frame_size),
                                                          "extended_sp");

  std::vector<llvm::Value*> live_values;
  live_values.push_back(extended_sp);
  builder()->CreateStackMap(statepoint_id, 0, live_values); // create stack map to locate the offset from sp/fp

  // Create the frame, but don't clear memory to zero, will call CreateMemset if necessary
  _frame = builder()->CreateBitCast(
    extended_sp,
    PointerType::getUnqual(
      llvm::ArrayType::get(YuhuType::intptr_type(), _extended_frame_size)),
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

  // Method pointer.
  //
  // `method` is now the scope's compile-time constant Method* (baked in
  // YuhuFunction::initialize via CreateInlineMetadata from target()), NOT the
  // caller-saved x12 register -- so it names the correct method for the outer
  // frame and, since initialize() runs per Path-B inlinee too, for each inlinee.
  // This is a debug pre-fill only: `method` is NULL in PRODUCT builds
  // (PRODUCT_ONLY(NULL) at the YuhuStackWithNormalFrame call site), so the store
  // is skipped there; at runtime the decacher rewrites the slot from the state on
  // every call site regardless.
  _method_slot_offset = offset++;
  if (method != NULL) {
    builder()->CreateStore(
      method, slot_addr(method_slot_offset(), YuhuType::Method_type()));
  }

  // Should get the sp value after final allocation, include prologue/main frame/spill areas
  Value* final_sp = builder()->CreateReadStackPointer();

  // Unextended SP
  builder()->CreateStore(final_sp, slot_addr(offset++));

  // PC slot (reused as return slot for non-void methods)
  _return_slot_addr = slot_addr(offset++, YuhuType::intptr_type(), "return_slot");

  // Frame header
  // For AArch64, we use a simple frame marker (not Zero-specific)
  // Use a recognizable marker value for debugging.
  // 0xDEADBEEF: a plain Yuhu frame (bytecode-level inlining only, or no
  //              inlining at all).
  // 0xDEADBABE: a Yuhu frame whose compilation unit at least one Path-B
  //              (LLVM-IR / alwaysinline) callee was flattened into.  This is
  //              an end-to-end runtime signal that Path B actually engaged --
  //              stronger than the compile-time YuhuTraceInlining planner line,
  //              because it survives into a live stack dump and would visibly
  //              fall back to 0xDEADBEEF after the per-method flatten-miss
  //              back-off disables Path B on retry.  Only the caller's marker
  //              slot is observable to a runtime frame walker; an inlinee's
  //              own alloca-slot is dead after AlwaysInlinerPass merges allocas,
  //              so is_inlinee=true always keeps 0xDEADBEEF.
  const intptr_t frame_marker = !is_inlinee ? (intptr_t)0xDEADBABE : (intptr_t)0xDEADBEEF;
  builder()->CreateStore(
    LLVMValue::intptr_constant(frame_marker), slot_addr(offset++)); // Frame marker (YUHU_FRAME equivalent)
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

  // Update frame pointer to point to this frame.  This also writes
  // thread->last_Java_fp (see CreateStoreFramePointer), which is a
  // per-compilation singleton side effect: an inlinee must not touch it,
  // since its own frame is a private alloca that gets merged into the
  // caller's frame by LLVM's inliner well before anything walks last_Java_fp.
  if (!is_inlinee) {
    CreateStoreFramePointer(
      builder()->CreatePtrToInt(current_fp, YuhuType::intptr_type()));
  }
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

void YuhuStack::CreateResetLastJavaFrame() {
    builder()->CreateStore(LLVMValue::intptr_constant(0), last_Java_sp_addr());
    builder()->CreateStore(LLVMValue::intptr_constant(0), last_Java_fp_addr());
    builder()->CreateStore(LLVMValue::intptr_constant(0), last_Java_pc_addr());
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
                                                bool           is_inlinee) {
  return new YuhuStackWithNormalFrame(function, method, is_inlinee);
}
YuhuStack* YuhuStack::CreateBuildAndPushFrame(YuhuNativeWrapper* wrapper,
                                                Value*              method) {
  return new YuhuStackWithNativeFrame(wrapper, method);
}

YuhuStackWithNormalFrame::YuhuStackWithNormalFrame(YuhuFunction* function,
                                                     Value*         method,
                                                     bool           is_inlinee)
  : YuhuStack(function), _function(function) {
  // For normal frames, the stack pointer and the method slot will
  // be set during each decache, so it is not necessary to do them
  // at the time the frame is created.  However, we set them for
  // non-PRODUCT builds to make crash dumps easier to understand.
  initialize(PRODUCT_ONLY(NULL) NOT_PRODUCT(method), function->inline_node_or_method(), is_inlinee);
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

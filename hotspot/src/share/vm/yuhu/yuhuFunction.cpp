/*
 * Copyright (c) 1999, 2013, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2008, 2009 Red Hat, Inc.
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
#include "ci/ciTypeFlow.hpp"
#include "memory/allocation.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/llvmValue.hpp"
#include "yuhu/yuhuBuilder.hpp"
#include "yuhu/yuhuEntry.hpp"
#include "yuhu/yuhuFunction.hpp"
#include "yuhu/yuhuState.hpp"
#include "yuhu/yuhuTopLevelBlock.hpp"
#include "yuhu/yuhu_globals.hpp"
#include "utilities/debug.hpp"

using namespace llvm;

// Generate function signature for normal entry based on Java method parameters
// According to 021 design: LLVM function parameters directly correspond to Java method parameters
// For static methods: first parameter is NULL (x0), then Java parameters
// Method* and Thread* are read from registers (x12, x28), not passed as parameters
llvm::FunctionType* YuhuFunction::generate_normal_entry_point_type() const {
  std::vector<llvm::Type*> params;
  
  // For static methods, first parameter is NULL (x0 in AArch64 calling convention)
  // This is because i2c adapter passes NULL in x0 for static methods
  if (is_static()) {
    params.push_back(YuhuType::intptr_type());  // void* null (x0)
  }
  
  // Add Java method parameters
  ciSignature* sig = target()->signature();
  int param_count = sig->count();
  for (int i = 0; i < param_count; i++) {
    ciType* param_type = sig->type_at(i);
    llvm::Type* llvm_type = YuhuType::to_stackType(param_type);
    params.push_back(llvm_type);
  }
  
  // Return type is always int (jint)
  return FunctionType::get(YuhuType::jint_type(), params, false);
}

void YuhuFunction::initialize(const char *name) {
  // Create the function and add it to the Module immediately
  // This ensures the Function has a parent Module, which is required
  // for IRBuilder to access DataLayout (especially in LLVM 20+)
  _arg_base = NULL;
  _arg_count = NULL;
  _function = Function::Create(
    entry_point_type(),
    Function::ExternalLinkage,  // Changed from InternalLinkage to ExternalLinkage
                                // InternalLinkage functions are not exported to symbol table,
                                // making them invisible to ORC JIT lookup
                                // Use Function::ExternalLinkage instead of GlobalVariable::ExternalLinkage
    name,
    YuhuContext::current().module());  // Pass Module so Function is added automatically
  
  // Debug: Print linkage value to verify it's set correctly
  // In LLVM, ExternalLinkage should be 6, InternalLinkage should be 0
  int linkage_value = (int)_function->getLinkage();
  if (YuhuTraceInstalls) {
    tty->print_cr("YuhuFunction: Created function %s with linkage %d (ExternalLinkage expected: 6)", 
                   name, linkage_value);
  }
  
  // Verify linkage was set correctly
  // Note: In some LLVM versions, ExternalLinkage might be 0, but we want ExternalLinkage
  if (_function->getLinkage() != Function::ExternalLinkage) {
    // If linkage is not ExternalLinkage, explicitly set it
    _function->setLinkage(Function::ExternalLinkage);
    if (YuhuTraceInstalls) {
      tty->print_cr("YuhuFunction: Linkage was %d, explicitly set to ExternalLinkage (%d)", 
                     linkage_value, (int)_function->getLinkage());
    }
  }

  // Register reservation for x19-x28 is configured globally via the TargetMachine (-mattr).

  // Get our arguments
  Function::arg_iterator ai = function()->arg_begin();
  llvm::Value *method = NULL;  // Will be set below for both OSR and normal entry
  llvm::Value *osr_buf = NULL;  // Will be set for OSR entry only
  
  if (is_osr()) {
    // OSR entry: keep old signature for now (will be handled in phase 6)
    llvm::Argument *method_arg = ai++;
    method_arg->setName("method");
    method = method_arg;  // Store for later use in CreateBuildAndPushFrame
    llvm::Argument *osr_buf_arg = ai++;
    osr_buf_arg->setName("osr_buf");
    osr_buf = osr_buf_arg;  // Store for later use
    llvm::Argument *base_pc = ai++;
    base_pc->setName("base_pc");
    code_buffer()->set_base_pc(base_pc);
    llvm::Argument *thread = ai++;
    thread->setName("thread");
    set_thread(thread);
  } else {
    // Normal entry: new simplified signature
    // Parameters are: (static: void* null, then Java method parameters...)
    // Method* and Thread* are read from registers, not passed as parameters
    
    // For static methods, skip the first parameter (NULL)
    if (is_static()) {
      llvm::Argument *null_arg = ai++;
      null_arg->setName("null_arg");
      // Don't use null_arg, it's just a placeholder for x0
    }
    
    // Java method parameters are now direct function parameters
    // They will be used directly as local variables in YuhuNormalEntryCacher
    // No need to store them here, they're already in the function signature
    
    // Method* and Thread* are read from registers (x12, x28)
    // No longer passed as function parameters
    // Note: CreateReadMethodRegister/ThreadRegister return i64 (intptr_type),
    // but we need pointer types for CreateAddressOfStructEntry/CreateValueOfStructEntry
    
    // CRITICAL: For normal entry, we need to create thread_ptr EARLY (before creating blocks)
    // because YuhuTopLevelBlock objects use copy constructor which copies _thread.
    // If _thread is NULL when blocks are created, they will have NULL _thread forever.
    // 
    // However, we also need thread_ptr to be in the entry basic block to dominate all uses.
    // Solution: Create a temporary entry basic block NOW, create thread_ptr in it,
    // set _thread, then create blocks. Later, we'll use this same entry block or create a new one.
    
    // Create entry basic block early
    llvm::BasicBlock* early_entry_block = NULL;
    if (function()->empty()) {
      early_entry_block = llvm::BasicBlock::Create(
        YuhuContext::current(),
        "entry",
        function());
      builder()->SetInsertPoint(early_entry_block);
      
      // Create thread_ptr in this entry block
      llvm::Value *thread_int = builder()->CreateReadThreadRegister();
      llvm::Value *thread = builder()->CreateIntToPtr(
        thread_int,
        PointerType::getUnqual(YuhuType::oop_type()),
        "thread_ptr");
      set_thread(thread);
      
      // Create method_ptr as well
      llvm::Value *method_int = builder()->CreateReadMethodRegister();
      method = builder()->CreateIntToPtr(
        method_int,
        YuhuType::Method_type(),
        "method_ptr");
      
      tty->print_cr("Yuhu: Created method_ptr and thread_ptr in early entry basic block (before creating blocks)");
      tty->flush();
    } else {
      // Function already has basic blocks (should not happen for normal entry)
      // But if it does, try to set thread from existing blocks
      tty->print_cr("Yuhu: WARNING - Function already has basic blocks for normal entry!");
      tty->flush();
      method = NULL;
    }
    
    // arg_base and arg_count are no longer needed
    _arg_base = NULL;
    _arg_count = NULL;
    
    // base_pc is no longer needed (can be read from PC register if needed in the future)
    // For now, set to NULL
    code_buffer()->set_base_pc(NULL);
    
    // Store the entry block so we can add a branch later
    // This will be fixed when YuhuStack::CreateBuildAndPushFrame is called
    // For now, we'll leave entry block without a terminator and fix it at line 251
  }

  // Create the list of blocks
  tty->print_cr("=== Yuhu: Creating %d blocks ===", block_count());
  set_block_insertion_point(NULL);
  _blocks = NEW_RESOURCE_ARRAY(YuhuTopLevelBlock*, block_count());
  for (int i = 0; i < block_count(); i++) {
    ciTypeFlow::Block *b = flow()->pre_order_at(i);

    // Work around a bug in pre_order_at() that does not return
    // the correct pre-ordering.  If pre_order_at() were correct
    // this line could simply be:
    // _blocks[i] = new YuhuTopLevelBlock(this, b);
    _blocks[b->pre_order()] = new YuhuTopLevelBlock(this, b);
    tty->print_cr("  Block %d: bci=%d-%d, pre_order=%d", 
                  i, b->start(), b->limit(), b->pre_order());
  }
  tty->flush();

  // Walk the tree from the start block to determine which
  // blocks are entered and which blocks require phis
  YuhuTopLevelBlock *start_block = block(flow()->start_block_num());
  if (is_osr() && start_block->stack_depth_at_entry() != 0) {
    env()->record_method_not_compilable("can't compile OSR block with incoming stack-depth > 0");
    return;
  }
  assert(start_block->start() == flow()->start_bci(), "blocks out of order");
  
  tty->print_cr("=== Yuhu: Traversing blocks from start_block %d (bci=%d) ===", 
                start_block->index(), start_block->start());
  tty->flush();
  start_block->enter();

  // Initialize all entered blocks
  tty->print_cr("=== Yuhu: Initializing entered blocks ===");
  for (int i = 0; i < block_count(); i++) {
    if (block(i)->entered()) {
      block(i)->initialize();
      tty->print_cr("  Block %d (bci=%d): needs_phis=%d, entry_block=%s", 
                    i, block(i)->start(), block(i)->needs_phis(),
                    block(i)->entry_block() ? block(i)->entry_block()->getName().str().c_str() : "NULL");
    }
  }
  tty->flush();

  // Create and push our stack frame
  // For normal entry, method_ptr and thread_ptr should already be created in early_entry_block (line 141-165)
  // For OSR entry, thread should already be set from function arguments (line 119)
  
  // Use the early entry block if it exists, otherwise create a new one
  llvm::BasicBlock* stack_frame_block = NULL;
  if (!is_osr() && !function()->empty() && function()->front().getName() == "entry") {
    // Reuse the early entry block we created
    stack_frame_block = &function()->front();
    builder()->SetInsertPoint(stack_frame_block);
    set_block_insertion_point(NULL);
    tty->print_cr("Yuhu: Reusing early entry basic block for stack frame");
    tty->flush();
  } else {
    // Create a new entry basic block (for OSR or if early entry block doesn't exist)
    if (function()->empty()) {
      stack_frame_block = llvm::BasicBlock::Create(
        YuhuContext::current(),
        "entry",
        function());
      set_block_insertion_point(NULL);
    } else {
      set_block_insertion_point(&function()->front());
      stack_frame_block = CreateBlock();
    }
    builder()->SetInsertPoint(stack_frame_block);
    
    // For OSR entry, create method_ptr and thread_ptr if not already set
    if (is_osr()) {
      // OSR entry: method and thread should already be set from function arguments
      assert(method != NULL, "method should be set for OSR entry");
      assert(thread() != NULL, "thread should be set for OSR entry");
    } else {
      // Normal entry: should already be set in early_entry_block
      if (method == NULL || thread() == NULL) {
        // Fallback: create them now if they weren't created earlier
        if (method == NULL) {
          llvm::Value *method_int = builder()->CreateReadMethodRegister();
          method = builder()->CreateIntToPtr(
            method_int,
            YuhuType::Method_type(),
            "method_ptr");
        }
        if (thread() == NULL) {
          llvm::Value *thread_int = builder()->CreateReadThreadRegister();
          llvm::Value *thread = builder()->CreateIntToPtr(
            thread_int,
            PointerType::getUnqual(YuhuType::oop_type()),
            "thread_ptr");
          set_thread(thread);
        }
        tty->print_cr("Yuhu: Created method_ptr and thread_ptr in fallback entry basic block");
        tty->flush();
      }
    }
  }
  
  // Now create YuhuStack - at this point, _thread should be set for both OSR and normal entry
  _stack = YuhuStack::CreateBuildAndPushFrame(this, method);

  // For normal compilation, reset last_Java_sp at method entry
  // This ensures that the assertion in CreateSetLastJavaFrame() will pass
  // when the first VM call is made. If the method is called from another
  // compiled method, the caller's last_Java_sp may still be set, so we
  // reset it here to ensure a clean state.
  if (!is_osr()) {
    _stack->CreateResetLastJavaFrame();
  }

  // Create the entry state
  YuhuState *entry_state;
  if (is_osr()) {
    // OSR entry: osr_buf is already extracted from function arguments above
    entry_state = new YuhuOSREntryState(start_block, method, osr_buf);

    // Free the OSR buffer
    // osr_migration_end signature: "C" -> "v" (char* -> void)
#if LLVM_VERSION_MAJOR >= 20
    llvm::FunctionType* func_type = YuhuBuilder::make_ftype("C", "v");
    std::vector<llvm::Value*> args;
    args.push_back(osr_buf);
    builder()->CreateCall(func_type, builder()->osr_migration_end(), args);
#else
    builder()->CreateCall(builder()->osr_migration_end(), osr_buf);
#endif
  }
  else {
    tty->print_cr("=== Yuhu: Creating YuhuNormalEntryState ===");
    tty->print_cr("  Method: %s", target()->name()->as_utf8());
    tty->print_cr("  Signature: %s", target()->signature()->as_symbol()->as_utf8());
    tty->print_cr("  Max locals: %d, arg_size: %d", target()->max_locals(), target()->arg_size());
    tty->flush();
    
    entry_state = new YuhuNormalEntryState(start_block, method);
    
    // Print entry state local variable types
    tty->print_cr("  Entry state local variable types (max_locals=%d, arg_size=%d):", 
                  target()->max_locals(), target()->arg_size());
    for (int i = 0; i < target()->max_locals(); i++) {
      ciType* type = start_block->local_type_at_entry(i);
      YuhuValue* value = entry_state->local(i);
      tty->print_cr("    local[%d]: type=%s, basic_type=%s, value=%s, llvm_type=%s",
                    i,
                    type ? type->name() : "NULL",
                    type ? type2name(type->basic_type()) : "NULL",
                    value ? "non-NULL" : "NULL",
                    value && value->generic_value() ? 
                      (value->generic_value()->getType()->getTypeID() == llvm::Type::PointerTyID ? "ptr" : 
                       value->generic_value()->getType()->getTypeID() == llvm::Type::IntegerTyID ? "int" : "other") : "N/A");
    }
    tty->flush();

    // Lock if necessary
    if (is_synchronized()) {
      YuhuTopLevelBlock *locker =
        new YuhuTopLevelBlock(this, start_block->ciblock());
      locker->add_incoming(entry_state);

      set_block_insertion_point(start_block->entry_block());
      locker->acquire_method_lock();

      entry_state = locker->current_state();
    }
  }

  // Transition into the method proper
  tty->print_cr("=== Yuhu: Connecting entry_state to start_block %d ===", start_block->index());
  tty->flush();
  start_block->add_incoming(entry_state);
  builder()->CreateBr(start_block->entry_block());

  // Parse the blocks
  for (int i = 0; i < block_count(); i++) {
    if (!block(i)->entered())
      continue;

    if (i + 1 < block_count())
      set_block_insertion_point(block(i + 1)->entry_block());
    else
      set_block_insertion_point(NULL);

    // Debug: Print block info before emitting IR
    tty->print_cr("=== Yuhu: Emitting IR for block %d (bci=%d) ===", 
                  i, block(i)->start());
    tty->flush();
    
    block(i)->emit_IR();
    
    tty->print_cr("=== Yuhu: Finished emitting IR for block %d ===", i);
    tty->flush();
  }
  do_deferred_zero_checks();
}

class DeferredZeroCheck : public YuhuTargetInvariants {
 public:
  DeferredZeroCheck(YuhuTopLevelBlock* block, YuhuValue* value)
    : YuhuTargetInvariants(block),
      _block(block),
      _value(value),
      _bci(block->bci()),
      _state(block->current_state()->copy()),
      _check_block(builder()->GetInsertBlock()),
      _continue_block(function()->CreateBlock("not_zero")) {
    builder()->SetInsertPoint(continue_block());
  }

 private:
  YuhuTopLevelBlock* _block;
  YuhuValue*         _value;
  int                 _bci;
  YuhuState*         _state;
  BasicBlock*         _check_block;
  BasicBlock*         _continue_block;

 public:
  YuhuTopLevelBlock* block() const {
    return _block;
  }
  YuhuValue* value() const {
    return _value;
  }
  int bci() const {
    return _bci;
  }
  YuhuState* state() const {
    return _state;
  }
  BasicBlock* check_block() const {
    return _check_block;
  }
  BasicBlock* continue_block() const {
    return _continue_block;
  }

 public:
  YuhuFunction* function() const {
    return block()->function();
  }

 public:
  void process() const {
    builder()->SetInsertPoint(check_block());
    block()->do_deferred_zero_check(value(), bci(), state(), continue_block());
  }
};

void YuhuFunction::add_deferred_zero_check(YuhuTopLevelBlock* block,
                                            YuhuValue*         value) {
  deferred_zero_checks()->append(new DeferredZeroCheck(block, value));
}

void YuhuFunction::do_deferred_zero_checks() {
  for (int i = 0; i < deferred_zero_checks()->length(); i++)
    deferred_zero_checks()->at(i)->process();
}

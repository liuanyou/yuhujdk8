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
#include "asm/yuhu/yuhu_macroAssembler.hpp"
#include "ci/ciTypeFlow.hpp"
#include "code/debugInfo.hpp"
#include "code/location.hpp"
#include "memory/allocation.hpp"
#include "oops/method.hpp"
#include "runtime/sharedRuntime.hpp"
#include "utilities/debug.hpp"
#ifdef TARGET_ARCH_aarch64
#include "register_aarch64.hpp"
#endif
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/llvmValue.hpp"
#include "yuhu/yuhuBuilder.hpp"
#include "yuhu/yuhuEntry.hpp"
#include "yuhu/yuhuFunction.hpp"
#include "yuhu/yuhuState.hpp"
#include "yuhu/yuhuTopLevelBlock.hpp"
#include "yuhu/yuhu_globals.hpp"

using namespace llvm;

// Forward declaration of gc_safepoint_poll from yuhuRuntime.cpp
extern "C" void gc_safepoint_poll();

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
  } else {
    // Non-static: need dummy in x0 to align with Java calling convention
    // x0 = dummy/return value slot (unused)
    // x1 = this (first actual parameter)
    // x2+ = other parameters
    params.push_back(YuhuType::intptr_type());  // Dummy in x0
    params.push_back(YuhuType::oop_addrspace1_type());     // FIXED - should GC, this in x1
  }
  
  // Add Java method parameters
  ciSignature* sig = target()->signature();
  int param_count = sig->count();
  for (int i = 0; i < param_count; i++) {
    ciType* param_type = sig->type_at(i);
    llvm::Type* llvm_type = YuhuType::to_stackType(param_type);
    params.push_back(llvm_type);
  }
  
  // Return type must match the Java method's return type so the LLVM `ret`
  // value is placed in the correct register per AArch64 ABI:
  //   T_INT/T_BOOLEAN/T_BYTE/T_SHORT/T_CHAR -> i32 (low 32 bits, w0)
  //   T_LONG                                  -> i64 (full x0)
  //   T_FLOAT                                 -> float (s0)
  //   T_DOUBLE                                -> double (d0)
  //   T_OBJECT/T_ARRAY                        -> ptr addrspace(1) (x0, RS4GC tracked)
  //   T_VOID                                  -> void
  BasicType ret_basic = target()->return_type()->basic_type();
  llvm::Type* ret_llvm = (ret_basic == T_VOID)
                            ? YuhuType::void_type()
                            : YuhuType::to_stackType(ret_basic);
  return FunctionType::get(ret_llvm, params, false);
}

void YuhuFunction::initialize(const char *name) {
  // Set the function pointer in builder so it can access deoptimization stub
  builder()->set_function(this);

  // Initialize member variables
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

  // CRITICAL: Force LLVM to generate frame pointer (x29) setup
  // Without this, LLVM only saves x29/x30 but doesn't set x29 = sp
  // This is needed for proper debugging, stack walking, and exception handling
  _function->addFnAttr(llvm::Attribute::get(
    YuhuContext::current(),
    "frame-pointer",
    "all"));  // "all" means force frame pointer in all functions
  
  // Set GC strategy for RS4GC statepoint infrastructure
  _function->setGC("statepoint-example");
  
  // Debug: Print linkage value to verify it's set correctly
  // In LLVM, ExternalLinkage should be 6, InternalLinkage should be 0
  int linkage_value = (int)_function->getLinkage();
  if (YuhuTraceFunction) {
    tty->print_cr("YuhuFunction: Created function %s with linkage %d (ExternalLinkage expected: 6)", 
                   name, linkage_value);
  }
  
  // Verify linkage was set correctly
  // Note: In some LLVM versions, ExternalLinkage might be 0, but we want ExternalLinkage
  if (_function->getLinkage() != Function::ExternalLinkage) {
    // If linkage is not ExternalLinkage, explicitly set it
    _function->setLinkage(Function::ExternalLinkage);
    if (YuhuTraceFunction) {
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

      // CRITICAL: Save p7 from x0 to x22 (reserved register) immediately
      // This must be done BEFORE any code that might use x0
      // x22 is safe because it's reserved via JTMB (+reserve-x22)
      builder()->CreateSaveX0ToX22();

      // Create thread_ptr in this entry block
      llvm::Value *thread_int = builder()->CreateReadThreadRegister();
      llvm::Value *thread = builder()->CreateIntToPtr(
        thread_int,
        PointerType::getUnqual(YuhuType::thread_type()), // FIXED - JavaThread* is allocated in C heap
        "thread_ptr");
      set_thread(thread);

      // Create method_ptr as well
      llvm::Value *method_int = builder()->CreateReadMethodRegister();
      method = builder()->CreateIntToPtr(
        method_int,
        YuhuType::Method_type(),
        "method_ptr");
    } else {
      // Function already has basic blocks (should not happen for normal entry)
      // But if it does, try to set thread from existing blocks
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

  // CRITICAL: Create unified exit block BEFORE creating stack
  // This ensures that stack overflow check can jump to it
  _unified_exit_block = NULL;
  _return_slot = NULL;
  if (!is_osr()) {
    // For normal entry, create unified exit block now
    // Return slot will be set after stack creation (reuses pc_slot in frame header)
    _unified_exit_block = llvm::BasicBlock::Create(
      YuhuContext::current(),
      "unified_exit",
      function());

    // NOTE: No ret instruction created here yet.
    // The ret will be added after stack creation when return_slot_addr is available.
    // See CreateUnifiedExitBlock() which adds the ret after _return_slot is assigned.
  }

  // Create the list of blocks
  set_block_insertion_point(NULL);
  _blocks = NEW_RESOURCE_ARRAY(YuhuTopLevelBlock*, block_count());
  for (int i = 0; i < block_count(); i++) {
    ciTypeFlow::Block *b = flow()->pre_order_at(i);

    // Work around a bug in pre_order_at() that does not return
    // the correct pre-ordering.  If pre_order_at() were correct
    // this line could simply be:
    // _blocks[i] = new YuhuTopLevelBlock(this, b);
    _blocks[b->pre_order()] = new YuhuTopLevelBlock(this, b);
  }

  // Walk the tree from the start block to determine which
  // blocks are entered and which blocks require phis
  YuhuTopLevelBlock *start_block = block(flow()->start_block_num());
  if (is_osr() && start_block->stack_depth_at_entry() != 0) {
    env()->record_method_not_compilable("can't compile OSR block with incoming stack-depth > 0");
    return;
  }
  assert(start_block->start() == flow()->start_bci(), "blocks out of order");

  start_block->enter();

  // Initialize all entered blocks
  for (int i = 0; i < block_count(); i++) {
    if (block(i)->entered()) {
      block(i)->initialize();
    }
  }

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
            PointerType::getUnqual(YuhuType::thread_type()), // FIXED - JavaThread* is allocated in C heap
            "thread_ptr");
          set_thread(thread);
        }
      }
    }
  }
  
  // Now create YuhuStack - at this point, _thread should be set for both OSR and normal entry
  // Pass sp_storage_alloca so YuhuStack uses the alloca created in the entry block
  _stack = YuhuStack::CreateBuildAndPushFrame(this, method);

  // Set return slot to pc_slot in frame header (for non-void methods)
  if (!is_osr() && _unified_exit_block != NULL) {
      // Now create the ret instruction in the unified exit block
      llvm::BasicBlock* orig_insert_block = builder()->GetInsertBlock();
      builder()->SetInsertPoint(_unified_exit_block);

      BasicType ret_type = target()->return_type()->basic_type();

      if (ret_type == T_VOID) {
          builder()->CreateRetVoid();
      } else {
          _return_slot = stack()->return_slot_addr();

          llvm::Type* ret_llvm_type = YuhuType::to_stackType(ret_type);
          
          // Handle different return types from intptr slot
          if (ret_type == T_OBJECT || ret_type == T_ARRAY) {
              // Load as i64, then inttoptr to oop pointer
              llvm::Value* loaded = builder()->CreateLoad(YuhuType::intptr_type(), _return_slot);
              builder()->CreateRet(builder()->CreateIntToPtr(loaded, ret_llvm_type));
          } else if (ret_type == T_FLOAT || ret_type == T_DOUBLE) {
              // Load as i64, then bitcast to FP type
              llvm::Value* loaded = builder()->CreateLoad(YuhuType::intptr_type(), _return_slot);
              builder()->CreateRet(builder()->CreateBitCast(loaded, ret_llvm_type));
          } else {
              // T_INT, T_LONG, etc. — direct load
              builder()->CreateRet(builder()->CreateLoad(ret_llvm_type, _return_slot));
          }
      }

      // Restore original insert point
      if (orig_insert_block) {
          builder()->SetInsertPoint(orig_insert_block);
      }
  }

  // NOTE: We no longer call CreateResetLastJavaFrame() here.
  // The last_Java_sp/fp/pc are set directly in yuhuStack.cpp::initialize()
  // using inline assembly at the very beginning of the function.
  // Resetting them here would overwrite the correct values.

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
    entry_state = new YuhuNormalEntryState(start_block, method);

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
  start_block->add_incoming(entry_state);

    // Method entry safepoint poll: inserted after final SP allocation, last_Java_sp setup,
    // and argument copying — before transitioning into bytecode blocks.
    // This matches C2's sequence: build_frame() -> set_frame_complete() -> safepoint_poll.
    if (!is_osr()) {
        start_block->maybe_add_safepoint(true);
    }

  builder()->CreateBr(start_block->entry_block());

  // Parse the blocks
  for (int i = 0; i < block_count(); i++) {
    if (!block(i)->entered())
      continue;

    if (i + 1 < block_count())
      set_block_insertion_point(block(i + 1)->entry_block());
    else
      set_block_insertion_point(NULL);

    block(i)->emit_IR();
  }
  do_deferred_zero_checks();
  collect_handler_blocks_and_insert_stackmap();
}

void YuhuFunction::collect_handler_blocks_and_insert_stackmap() {
    GrowableArray<YuhuTopLevelBlock*> handler_blocks;
    GrowableArray<ciExceptionHandler*> exception_handlers;

    for (int i = 0; i < block_count(); i++) {
        YuhuTopLevelBlock* individual_block = block(i);
        for (int j = 0; j < individual_block->num_exceptions(); j++) {
            YuhuTopLevelBlock* handler_block = individual_block->exception(j);
            if (handler_block && !handler_blocks.contains(handler_block)) {
                handler_blocks.append(handler_block);
            }
        }
        for (int j =0; j < individual_block->num_exceptions(); j++) {
            ciExceptionHandler* exception_handler = individual_block->exc_handler(j);
            if (exception_handler && !exception_handlers.contains(exception_handler)) {
                exception_handlers.append(exception_handler);
            }
        }
    }

    // process handler blocks
    handler_blocks.sort([](YuhuTopLevelBlock** a, YuhuTopLevelBlock** b) -> int {
        if ((*a)->start() < (*b)->start()) return -1;
        if ((*a)->start() > (*b)->start()) return  1;
        return 0;
    });;
    if (handler_blocks.length()) {
        for (int i = 0; i < handler_blocks.length(); ++i) {
            std::vector<llvm::Value*> live_values;
            live_values.push_back(builder()->getInt32(handler_blocks.at(i)->start()));
            live_values.push_back(builder()->getInt32(handler_blocks.at(i)->limit()));
            live_values.push_back(builder()->getInt32(handler_blocks.at(i)->num_exceptions()));
            live_values.push_back(builder()->getInt32(handler_blocks.at(i)->num_successors()));
            builder()->InsertStackMapAtBlockStart(handler_blocks.at(i)->entry_block(), i, live_values);
        }
    }

    // process exception handlers
    exception_handlers.sort([](ciExceptionHandler** a, ciExceptionHandler** b) -> int {
        if ((*a)->start() < (*b)->start()) return -1;
        if ((*a)->start() > (*b)->start()) return  1;
        return 0;
    });;
    if (exception_handlers.length()) {
        for (int i = 0; i < exception_handlers.length(); ++i) {
            ciExceptionHandler* h = exception_handlers.at(i);
            YuhuDebugInformationRecorder::get()->register_exception_handler_info(h->start(), h->limit(), h->handler_bci(), h->is_catch_all());
        }
    }
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

// Get or create the unified exit block for all return paths
llvm::BasicBlock* YuhuFunction::unified_exit_block() {
  if (_unified_exit_block == NULL) {
    // Create the unified exit block
    _unified_exit_block = llvm::BasicBlock::Create(
      YuhuContext::current(),
      "unified_exit",
      function());

    // Set insert point to the exit block
    llvm::BasicBlock* orig_insert_block = builder()->GetInsertBlock();
    builder()->SetInsertPoint(_unified_exit_block);

    // Insert the epilogue marker (will be replaced with "add sp, x29, #0" after compilation)
//    builder()->CreateEpiloguePlaceholder();

    // Create ret instruction matching the actual function return type.
    BasicType ret_type = target()->return_type()->basic_type();
    if (ret_type == T_VOID) {
      builder()->CreateRetVoid();
    } else {
      // Use return_slot for OSR case too
      if (_return_slot == NULL) {
        _return_slot = stack()->return_slot_addr();
      }
      llvm::Type* ret_llvm_type = YuhuType::to_stackType(ret_type);
      
      // Handle different return types from intptr slot
      if (ret_type == T_OBJECT || ret_type == T_ARRAY) {
          llvm::Value* loaded = builder()->CreateLoad(YuhuType::intptr_type(), _return_slot);
          builder()->CreateRet(builder()->CreateIntToPtr(loaded, ret_llvm_type));
      } else if (ret_type == T_FLOAT || ret_type == T_DOUBLE) {
          llvm::Value* loaded = builder()->CreateLoad(YuhuType::intptr_type(), _return_slot);
          builder()->CreateRet(builder()->CreateBitCast(loaded, ret_llvm_type));
      } else {
          builder()->CreateRet(builder()->CreateLoad(ret_llvm_type, _return_slot));
      }
    }

    // Restore original insert point
    if (orig_insert_block) {
      builder()->SetInsertPoint(orig_insert_block);
    }
  } else {
    // Already created in initialize(), just return it
  }

  return _unified_exit_block;
}

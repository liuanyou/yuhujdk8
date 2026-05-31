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
#include "ci/ciField.hpp"
#include "ci/ciInstance.hpp"
#include "ci/ciObjArrayKlass.hpp"
#include "ci/ciStreams.hpp"
#include "ci/ciType.hpp"
#include "ci/ciTypeFlow.hpp"
#include "interpreter/bytecodes.hpp"
#include "memory/allocation.hpp"
#include "runtime/deoptimization.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/llvmValue.hpp"
#include "yuhu/yuhuBuilder.hpp"
#include "yuhu/yuhuCacheDecache.hpp"
#include "yuhu/yuhuConstant.hpp"
#include "yuhu/yuhuDebugInformationRecorder.hpp"
#include "yuhu/yuhuInliner.hpp"
#include "yuhu/yuhuState.hpp"
#include "yuhu/yuhuTopLevelBlock.hpp"
#include "yuhu/yuhuValue.hpp"
#include "yuhu/yuhu_globals.hpp"
#include "yuhu/yuhuRuntime.hpp"
#include "utilities/debug.hpp"

using namespace llvm;

// Forward declaration of gc_safepoint_poll from yuhuRuntime.cpp
extern "C" void gc_safepoint_poll();
extern "C" void handle_deoptimization();

void YuhuTopLevelBlock::scan_for_traps() {
  // Adopt trap information directly from ciTypeFlow's analysis.
  // ciTypeFlow has already scanned the bytecode and identified all traps,
  // including the exact BCI and constant pool index for each trap.
  // No need to re-scan bytecode - trust ciTypeFlow's results.
  if (ciblock()->has_trap()) {
    int trap_index = ciblock()->trap_index();
    int trap_request;
    
    if (trap_index < 0) {
      // ciTypeFlow already encoded reason/action (negative value)
      trap_request = trap_index;
    } else {
      // CP index only — use Reason_unloaded with default action
      trap_request = Deoptimization::make_trap_request(
        Deoptimization::Reason_unloaded,
        Deoptimization::Action_reinterpret,
        trap_index);
    }
    
    set_trap(trap_request, ciblock()->trap_bci());
  }
}

bool YuhuTopLevelBlock::static_field_ok_in_clinit(ciField* field) {
  assert(field->is_static(), "should be");

  // This code is lifted pretty much verbatim from C2's
  // Parse::static_field_ok_in_clinit() in parse3.cpp.
  bool access_OK = false;
  if (target()->holder()->is_subclass_of(field->holder())) {
    if (target()->is_static()) {
      if (target()->name() == ciSymbol::class_initializer_name()) {
        // It's OK to access static fields from the class initializer
        access_OK = true;
      }
    }
    else {
      if (target()->name() == ciSymbol::object_initializer_name()) {
        // It's also OK to access static fields inside a constructor,
        // because any thread calling the constructor must first have
        // synchronized on the class by executing a "new" bytecode.
        access_OK = true;
      }
    }
  }
  return access_OK;
}

YuhuState* YuhuTopLevelBlock::entry_state() {
  if (_entry_state == NULL) {
    assert(needs_phis(), "should do");
    _entry_state = new YuhuPHIState(this);
  }
  return _entry_state;
}

void YuhuTopLevelBlock::add_incoming(YuhuState* incoming_state) {
  BasicBlock* predecessor = builder()->GetInsertBlock();
  
  if (needs_phis()) {
    ((YuhuPHIState *) entry_state())->add_incoming(incoming_state);
  }
  else if (_entry_state == NULL) {
    _entry_state = incoming_state;
  }
  else {
    assert(entry_state()->equal_to(incoming_state), "should be");
  }
}

void YuhuTopLevelBlock::enter(YuhuTopLevelBlock* predecessor,
                               bool is_exception) {
  // This block requires phis:
  //  - if it is entered more than once
  //  - if it is an exception handler, because in which
  //    case we assume it's entered more than once.
  //  - if the predecessor will be compiled after this
  //    block, in which case we can't simple propagate
  //    the state forward.
  if (!needs_phis() &&
      (entered() ||
       is_exception ||
       (predecessor && predecessor->index() >= index())))
    _needs_phis = true;

  // Recurse into the tree
  if (!entered()) {
    _entered = true;

    scan_for_traps();
    if (!has_trap()) {
      for (int i = 0; i < num_successors(); i++) {
        successor(i)->enter(this, false);
      }
    }
    compute_exceptions();
    for (int i = 0; i < num_exceptions(); i++) {
      YuhuTopLevelBlock *handler = exception(i);
      if (handler)
        handler->enter(this, true);
    }
  }
}

void YuhuTopLevelBlock::initialize() {
  char name[28];
  snprintf(name, sizeof(name),
           "bci_%d%s",
           start(), is_backedge_copy() ? "_backedge_copy" : "");
  _entry_block = function()->CreateBlock(name);
}

void YuhuTopLevelBlock::decache_for_Java_call(ciMethod *callee) {
  YuhuJavaCallDecacher(function(), bci(), callee).scan(current_state());
  for (int i = 0; i < callee->arg_size(); i++)
    xpop();
}

void YuhuTopLevelBlock::cache_after_Java_call(ciMethod *callee, Value* call_result) {
  if (callee->return_type()->size()) {
    ciType *type;
    switch (callee->return_type()->basic_type()) {
    case T_BOOLEAN:
    case T_BYTE:
    case T_CHAR:
    case T_SHORT:
      type = ciType::make(T_INT);
      break;

    default:
      type = callee->return_type();
    }

    push(YuhuValue::create_generic(type, call_result, false));  // ← Pass actual call result!
  }
  YuhuJavaCallCacher(function(), callee).scan(current_state());
}

void YuhuTopLevelBlock::decache_for_VM_call(int virtual_offset) {
  YuhuVMCallDecacher decacher(function(), bci(), virtual_offset);
  decacher.scan(current_state());
}

void YuhuTopLevelBlock::cache_after_VM_call() {
  YuhuVMCallCacher(function()).scan(current_state());
}

void YuhuTopLevelBlock::decache_for_trap() {
  YuhuTrapDecacher(function(), bci()).scan(current_state());
}

void YuhuTopLevelBlock::emit_IR() {
  builder()->SetInsertPoint(entry_block());

  // Parse the bytecode
  parse_bytecode(start(), limit());

  // If this block falls through to the next then it won't have been
  // terminated by a bytecode and we have to add the branch ourselves
  if (falls_through() && !has_trap())
    do_branch(ciTypeFlow::FALL_THROUGH);
}

YuhuTopLevelBlock* YuhuTopLevelBlock::bci_successor(int bci) const {
  // XXX now with Linear Search Technology (tm)
  for (int i = 0; i < num_successors(); i++) {
    ciTypeFlow::Block *successor = ciblock()->successors()->at(i);
    if (successor->start() == bci)
      return function()->block(successor->pre_order());
  }
  ShouldNotReachHere();
  return NULL;  // Should never reach here, but needed for compiler
}

void YuhuTopLevelBlock::do_zero_check(YuhuValue *value) {
  if (value->is_phi() && value->as_phi()->all_incomers_zero_checked()) {
    function()->add_deferred_zero_check(this, value);
  }
  else {
    BasicBlock *continue_block = function()->CreateBlock("not_zero");
    YuhuState *saved_state = current_state();
    set_current_state(saved_state->copy());
    zero_check_value(value, continue_block);
    builder()->SetInsertPoint(continue_block);
    set_current_state(saved_state);
  }

  value->set_zero_checked(true);
}

void YuhuTopLevelBlock::do_deferred_zero_check(YuhuValue* value,
                                                int         bci,
                                                YuhuState* saved_state,
                                                BasicBlock* continue_block) {
  if (value->as_phi()->all_incomers_zero_checked()) {
    builder()->CreateBr(continue_block);
  }
  else {
    iter()->force_bci(start());
    set_current_state(saved_state);
    zero_check_value(value, continue_block);
  }
}

void YuhuTopLevelBlock::zero_check_value(YuhuValue* value,
                                          BasicBlock* continue_block) {
  BasicBlock *zero_block = builder()->CreateBlock(continue_block, "zero");

  Value *a, *b;
  switch (value->basic_type()) {
  case T_BYTE:
  case T_CHAR:
  case T_SHORT:
  case T_INT:
    a = value->jint_value();
    b = LLVMValue::jint_constant(0);
    break;
  case T_LONG:
    a = value->jlong_value();
    b = LLVMValue::jlong_constant(0);
    break;
  case T_OBJECT:
  case T_ARRAY:
    a = value->jobject_value();
    b = LLVMValue::LLVMValue::null();
    break;
  default:
    tty->print_cr("Unhandled type %s", type2name(value->basic_type()));
    ShouldNotReachHere();
  }

  builder()->CreateCondBr(
    builder()->CreateICmpNE(a, b), continue_block, zero_block);

  builder()->SetInsertPoint(zero_block);
  if (value->is_jobject()) {
    call_vm(
      builder()->throw_NullPointerException(),
      builder()->CreateIntToPtr(
        LLVMValue::intptr_constant((intptr_t) __FILE__),
        PointerType::getUnqual(YuhuType::jbyte_type())),
      LLVMValue::jint_constant(__LINE__),
      EX_CHECK_NONE);
  }
  else {
    call_vm(
      builder()->throw_ArithmeticException(),
      builder()->CreateIntToPtr(
        LLVMValue::intptr_constant((intptr_t) __FILE__),
        PointerType::getUnqual(YuhuType::jbyte_type())),
      LLVMValue::jint_constant(__LINE__),
      EX_CHECK_NONE);
  }

  Value *pending_exception = get_pending_exception();
  clear_pending_exception();
  handle_exception(pending_exception, EX_CHECK_FULL);
}

void YuhuTopLevelBlock::check_bounds(YuhuValue* array, YuhuValue* index) {
  BasicBlock *out_of_bounds = function()->CreateBlock("out_of_bounds");
  BasicBlock *in_bounds     = function()->CreateBlock("in_bounds");

  Value *length = builder()->CreateArrayLength(array->jarray_value());
  // we use an unsigned comparison to catch negative values
  builder()->CreateCondBr(
    builder()->CreateICmpULT(index->jint_value(), length),
    in_bounds, out_of_bounds);

  builder()->SetInsertPoint(out_of_bounds);
  YuhuState *saved_state = current_state()->copy();

  call_vm(
    builder()->throw_ArrayIndexOutOfBoundsException(),
    builder()->CreateIntToPtr(
      LLVMValue::intptr_constant((intptr_t) __FILE__),
      PointerType::getUnqual(YuhuType::jbyte_type())),
    LLVMValue::jint_constant(__LINE__),
    index->jint_value(),
    EX_CHECK_NONE);

  Value *pending_exception = get_pending_exception();
  clear_pending_exception();
  handle_exception(pending_exception, EX_CHECK_FULL);

  set_current_state(saved_state);

  builder()->SetInsertPoint(in_bounds);
}

void YuhuTopLevelBlock::check_pending_exception(int action) {
  assert(action & EAM_CHECK, "should be");

  BasicBlock *exception    = function()->CreateBlock("exception");
  BasicBlock *no_exception = function()->CreateBlock("no_exception");

  Value *pending_exception = get_pending_exception();
  builder()->CreateCondBr(
    builder()->CreateICmpEQ(pending_exception, LLVMValue::null()),
    no_exception, exception);

  builder()->SetInsertPoint(exception);
  YuhuState *saved_state = current_state()->copy();
  if (action & EAM_MONITOR_FUDGE) {
    // The top monitor is marked live, but the exception was thrown
    // while setting it up so we need to mark it dead before we enter
    // any exception handlers as they will not expect it to be there.
    set_num_monitors(num_monitors() - 1);
    action ^= EAM_MONITOR_FUDGE;
  }
  clear_pending_exception();
  handle_exception(pending_exception, action);
  set_current_state(saved_state);

  builder()->SetInsertPoint(no_exception);
}

void YuhuTopLevelBlock::compute_exceptions() {
  ciExceptionHandlerStream str(target(), start());

  int exc_count = str.count();
  _exc_handlers = new GrowableArray<ciExceptionHandler*>(exc_count);
  _exceptions   = new GrowableArray<YuhuTopLevelBlock*>(exc_count);

  int index = 0;
  for (; !str.is_done(); str.next()) {
    ciExceptionHandler *handler = str.handler();
    if (handler->handler_bci() == -1)
      break;
    _exc_handlers->append(handler);

    // Try and get this exception's handler from typeflow.  We should
    // do it this way always, really, except that typeflow sometimes
    // doesn't record exceptions, even loaded ones, and sometimes it
    // returns them with a different handler bci.  Why???
    YuhuTopLevelBlock *block = NULL;
    ciInstanceKlass* klass;
    if (handler->is_catch_all()) {
      klass = java_lang_Throwable_klass();
    }
    else {
      klass = handler->catch_klass();
    }
    for (int i = 0; i < ciblock()->exceptions()->length(); i++) {
      if (klass == ciblock()->exc_klasses()->at(i)) {
        block = function()->block(ciblock()->exceptions()->at(i)->pre_order());
        if (block->start() == handler->handler_bci())
          break;
        else
          block = NULL;
      }
    }

    // If typeflow let us down then try and figure it out ourselves
    if (block == NULL) {
      for (int i = 0; i < function()->block_count(); i++) {
        YuhuTopLevelBlock *candidate = function()->block(i);
        if (candidate->start() == handler->handler_bci()) {
          if (block != NULL) {
            NOT_PRODUCT(warning("there may be trouble ahead"));
            block = NULL;
            break;
          }
          block = candidate;
        }
      }
    }
    _exceptions->append(block);
  }
}

void YuhuTopLevelBlock::handle_exception(Value* exception, int action) {
  if (action & EAM_HANDLE && num_exceptions() != 0) {
    // Clear the stack and push the exception onto it
    while (xstack_depth())
      pop();
    push(YuhuValue::create_jobject(exception, true));

    // Work out how many options we have to check
    bool has_catch_all = exc_handler(num_exceptions() - 1)->is_catch_all();
    int num_options = num_exceptions();
    if (has_catch_all)
      num_options--;

    // Marshal any non-catch-all handlers
    if (num_options > 0) {
      bool all_loaded = true;
      for (int i = 0; i < num_options; i++) {
        if (!exc_handler(i)->catch_klass()->is_loaded()) {
          all_loaded = false;
          break;
        }
      }

      if (all_loaded)
        marshal_exception_fast(num_options);
      else
        marshal_exception_slow(num_options);
    }

    // Install the catch-all handler, if present
    if (has_catch_all) {
      YuhuTopLevelBlock* handler = this->exception(num_options);
      assert(handler != NULL, "catch-all handler cannot be unloaded");

      builder()->CreateBr(handler->entry_block());
      handler->add_incoming(current_state());
      return;
    }
  }

  // No exception handler was found; unwind and return
  handle_return(T_VOID, exception);
}

void YuhuTopLevelBlock::marshal_exception_fast(int num_options) {
  Value *exception_klass = builder()->load_klass_from_object(
    xstack(0)->jobject_value());

  for (int i = 0; i < num_options; i++) {
    Value *check_klass =
      builder()->CreateInlineMetadata(exc_handler(i)->catch_klass(), YuhuType::klass_type());

    BasicBlock *not_exact   = function()->CreateBlock("not_exact");
    BasicBlock *not_subtype = function()->CreateBlock("not_subtype");

    builder()->CreateCondBr(
      builder()->CreateICmpEQ(check_klass, exception_klass),
      handler_for_exception(i), not_exact);

    builder()->SetInsertPoint(not_exact);
    // LLVM 20+ requires FunctionType for CreateCall
    // LLVM 20+ uses opaque pointer types, reconstruct FunctionType from signature "KK" -> "c"
    llvm::FunctionType* func_type = YuhuBuilder::make_ftype("KK", "c");
    std::vector<Value*> args;
    args.push_back(check_klass);
    args.push_back(exception_klass);

      // Manual call site registration (can't use call_vm from here)
      uint64_t virtual_offset = code_buffer()->create_unique_offset();
      uint64_t last_java_pc_va = LAST_JAVA_PC_MAGIC | virtual_offset;  // For last_Java_pc
      uint64_t call_target_va = (virtual_offset << 32) | (virtual_offset << 16) | CALL_TARGET_MAGIC;

      // Extract actual helper address
      uint64_t helper_address = 0;

      Value* callee = builder()->is_subtype_of();  // Get the is_subtype_of function

      if (auto* CastInst = llvm::dyn_cast<llvm::ConstantExpr>(callee)) {
          if (CastInst->getOpcode() == llvm::Instruction::IntToPtr) {
              if (auto* IntConst = llvm::dyn_cast<llvm::ConstantInt>(CastInst->getOperand(0))) {
                  helper_address = IntConst->getZExtValue();
              }
          }
      }

      // Replace callee with virtual address
      if (helper_address != 0) {
          llvm::Module* mod = builder()->GetInsertBlock()->getModule();
          callee = builder()->CreateIntToPtr(
                  llvm::ConstantInt::get(llvm::Type::getInt64Ty(mod->getContext()), call_target_va),
                  callee->getType());

          stack()->CreateCallSitePlaceholder(last_java_pc_va);

          YuhuDebugInformationRecorder::get()->register_call_site(
                  virtual_offset, call_target_va, helper_address,
                  CallSiteType::vm_call, bci());
      }

    builder()->CreateCondBr(
      builder()->CreateICmpNE(
        builder()->CreateCall(func_type, callee, args),
        LLVMValue::jbyte_constant(0)),
      handler_for_exception(i), not_subtype);

    builder()->SetInsertPoint(not_subtype);
  }
}

void YuhuTopLevelBlock::marshal_exception_slow(int num_options) {
  // Option A: pass Method* and the exception oop explicitly so the C helper
  // does not need to walk the caller frame. The catch-klass cp indexes
  // remain (the slow path is precisely the case where at least one catch
  // klass is *not* loaded, so they cannot be pre-resolved at JIT time).
  int *indexes = NEW_RESOURCE_ARRAY(int, num_options);
  for (int i = 0; i < num_options; i++)
    indexes[i] = exc_handler(i)->catch_klass_index();

  Value *method_const = builder()->CreateInlineMetadata(
    target(), YuhuType::klass_type());
  Value *exception_oop = xstack(0)->jobject_value();

  Value *index = call_vm(
    builder()->find_exception_handler(),
    method_const,
    exception_oop,
    builder()->CreateInlineData(
      indexes,
      num_options * sizeof(int),
      PointerType::getUnqual(YuhuType::jint_type())),
    LLVMValue::jint_constant(num_options),
    EX_CHECK_NO_CATCH,
    YuhuType::jint_type());  // find_exception_handler returns int

  BasicBlock *no_handler = function()->CreateBlock("no_handler");
  SwitchInst *switchinst = builder()->CreateSwitch(
    index, no_handler, num_options);

  for (int i = 0; i < num_options; i++) {
    switchinst->addCase(
      LLVMValue::jint_constant(i),
      handler_for_exception(i));
  }

  builder()->SetInsertPoint(no_handler);
}

BasicBlock* YuhuTopLevelBlock::handler_for_exception(int index) {
  YuhuTopLevelBlock *successor = this->exception(index);
  if (successor) {
    successor->add_incoming(current_state());
    return successor->entry_block();
  }
  else {
    return make_trap(
      exc_handler(index)->handler_bci(),
      Deoptimization::make_trap_request(
        Deoptimization::Reason_unhandled,
        Deoptimization::Action_reinterpret));
  }
}

void YuhuTopLevelBlock::maybe_add_safepoint(bool is_method_entry_safepoint) {
  if (current_state()->has_safepointed())
    return;

    uint64_t virtual_offset = code_buffer()->create_unique_offset();
    uint64_t last_java_pc_va = LAST_JAVA_PC_MAGIC | virtual_offset;  // For last_Java_pc
    uint64_t call_target_va = (virtual_offset << 32) | (virtual_offset << 16) | CALL_TARGET_MAGIC;
    // call_target_va is not used in the CreateCall, just create one for no use
    YuhuDebugInformationRecorder::get()->register_call_site(virtual_offset,
                                                            call_target_va,
                                                            (uint64_t)&gc_safepoint_poll,
                                                            CallSiteType::safepoint_poll,
                                                            is_method_entry_safepoint ? -1 : bci());
    // we use virtual last java pc only, coz adrp instructions must be used for gc.safepoint_poll,
    // otherwise, poll_type relocation record can't be created. hence patching adr logic is a little different
    // than call site in CallSiteExtractorPlugin
    stack()->CreateCallSitePlaceholder(last_java_pc_va);
    llvm::Module* mod = builder()->GetInsertBlock()->getModule();
    llvm::FunctionType* poll_ftype = llvm::FunctionType::get(llvm::Type::getVoidTy(mod->getContext()), false);
    llvm::FunctionCallee poll_fn = mod->getOrInsertFunction("gc.safepoint_poll", poll_ftype);
    builder()->CreateCall(poll_ftype, poll_fn.getCallee(), {});

  current_state()->set_has_safepointed(true);
}

void YuhuTopLevelBlock::maybe_add_backedge_safepoint() {
  if (current_state()->has_safepointed())
    return;

  for (int i = 0; i < num_successors(); i++) {
    if (successor(i)->can_reach(this)) {
      maybe_add_safepoint();
      break;
    }
  }
}

bool YuhuTopLevelBlock::can_reach(YuhuTopLevelBlock* other) {
  for (int i = 0; i < function()->block_count(); i++)
    function()->block(i)->_can_reach_visited = false;

  return can_reach_helper(other);
}

bool YuhuTopLevelBlock::can_reach_helper(YuhuTopLevelBlock* other) {
  if (this == other)
    return true;

  if (_can_reach_visited)
    return false;
  _can_reach_visited = true;

  if (!has_trap()) {
    for (int i = 0; i < num_successors(); i++) {
      if (successor(i)->can_reach_helper(other))
        return true;
    }
  }

  for (int i = 0; i < num_exceptions(); i++) {
    YuhuTopLevelBlock *handler = exception(i);
    if (handler && handler->can_reach_helper(other))
      return true;
  }

  return false;
}

BasicBlock* YuhuTopLevelBlock::make_trap(int trap_bci, int trap_request) {
  BasicBlock *trap_block = function()->CreateBlock("trap");
  BasicBlock *orig_block = builder()->GetInsertBlock();
  builder()->SetInsertPoint(trap_block);

  int orig_bci = bci();
  iter()->force_bci(trap_bci);

  do_trap(trap_request);

  builder()->SetInsertPoint(orig_block);
  iter()->force_bci(orig_bci);

  return trap_block;
}

void YuhuTopLevelBlock::do_trap(int trap_request) {
  decache_for_trap();
  
  // Build deopt operand bundle with all live JVM state
  YuhuState* state = current_state();
  std::vector<llvm::Value*> deopt_operands;
  
  // 1. Local variables (in order 0..max_locals-1)
  for (int i = 0; i < max_locals(); i++) {
    YuhuValue* local = state->local(i);
    if (local != NULL) {
      // Push type metadata first
      BasicType basic_type = local->basic_type();
      deopt_operands.push_back(llvm::ConstantInt::get(builder()->getInt64Ty(), basic_type));
      
      llvm::Value* llvm_val = local->generic_value();
      
      // Cast to i64 for uniform representation
      if (llvm_val->getType()->isPointerTy()) {
        llvm_val = builder()->CreatePtrToInt(llvm_val, builder()->getInt64Ty());
      } else if (llvm_val->getType()->getIntegerBitWidth() != 64) {
        llvm_val = builder()->CreateIntCast(llvm_val, builder()->getInt64Ty(), false);
      }
      
      deopt_operands.push_back(llvm_val);
    } else {
      // Null local - push T_OBJECT type and null value
      deopt_operands.push_back(llvm::ConstantInt::get(builder()->getInt64Ty(), T_OBJECT));
      deopt_operands.push_back(llvm::ConstantInt::get(builder()->getInt64Ty(), 0));
    }
  }
  
  // 2. Expression stack (in order bottom..top)
  for (int i = 0; i < state->stack_depth(); i++) {
    YuhuValue* stack_val = state->stack(i);
    
    // Push type metadata first
    BasicType basic_type = stack_val->basic_type();
    deopt_operands.push_back(llvm::ConstantInt::get(builder()->getInt64Ty(), basic_type));
    
    llvm::Value* llvm_val = stack_val->generic_value();
    
    // Cast to i64
    if (llvm_val->getType()->isPointerTy()) {
      llvm_val = builder()->CreatePtrToInt(llvm_val, builder()->getInt64Ty());
    } else if (llvm_val->getType()->getIntegerBitWidth() != 64) {
      llvm_val = builder()->CreateIntCast(llvm_val, builder()->getInt64Ty(), false);
    }
    
    deopt_operands.push_back(llvm_val);
  }

  // 3. Monitors (locked objects) - in order 0..num_monitors-1
  int monitor_count = num_monitors();
  for (int i = 0; i < monitor_count; i++) {
    // Push type metadata (monitors are always T_OBJECT)
    deopt_operands.push_back(llvm::ConstantInt::get(builder()->getInt64Ty(), T_OBJECT));
    
    // Load the locked object from the monitor slot in the stack frame
    llvm::Value* monitor_object_addr = stack()->monitor_object_addr(i);
    llvm::Value* locked_obj = builder()->CreateLoad(
      YuhuType::oop_addrspace1_type(), 
      monitor_object_addr, 
      "monitor_object"
    );
    
    // Cast to i64 for uniform representation
    llvm::Value* llvm_val = builder()->CreatePtrToInt(locked_obj, builder()->getInt64Ty());
    deopt_operands.push_back(llvm_val);
  }

  // 4. bci
    int current_bci = bci();
    llvm::Value* bci_val = llvm::ConstantInt::get(builder()->getInt64Ty(), current_bci);
    deopt_operands.push_back(bci_val);

    // 5. num of locals
    int locals_num = max_locals();
    llvm::Value* locals_num_val = llvm::ConstantInt::get(builder()->getInt64Ty(), locals_num);
    deopt_operands.push_back(locals_num_val);

    // 6. num of expression stacks
    int stacks_num = state->stack_depth();
    llvm::Value* stacks_num_val = llvm::ConstantInt::get(builder()->getInt64Ty(), stacks_num);
    deopt_operands.push_back(stacks_num_val);

    // 7. num of monitors
    llvm::Value* monitor_count_val = llvm::ConstantInt::get(builder()->getInt64Ty(), monitor_count);
    deopt_operands.push_back(monitor_count_val);
    
    // Note: Each value in locals/stacks/monitors is preceded by its type metadata
    // Total operands = (locals * 2) + (stacks * 2) + (monitors * 2) + 4 metadata
  
  // Create deopt operand bundle
  llvm::OperandBundleDef deopt_bundle("deopt", deopt_operands);

    // NEW: Get unique virtual offset for this call site (MUST be before creating placeholders)
    uint64_t virtual_offset = code_buffer()->create_unique_offset();

    // NEW: Create dual virtual addresses with same virtual_offset
    uint64_t last_java_pc_va = LAST_JAVA_PC_MAGIC | virtual_offset;  // For last_Java_pc
    uint64_t call_target_va = (virtual_offset << 32) | (virtual_offset << 16) | CALL_TARGET_MAGIC;

    stack()->CreateCallSitePlaceholder(last_java_pc_va);

    // NEW: Register call site for later patching
    YuhuDebugInformationRecorder::get()->register_call_site(virtual_offset, call_target_va, (uint64_t)&handle_deoptimization, CallSiteType::deopt_call, bci());
  
  // Call @llvm.experimental.deoptimize intrinsic
  // This marks the call as a deoptimization point for LLVM's statepoint infrastructure
  // The deopt bundle preserves all live JVM state for frame reconstruction
  llvm::Value* deopt_result = builder()->CreateExperimentalDeoptimize({deopt_bundle});

  builder()->CreateRet(deopt_result);

  // CRITICAL: Jump to the unified exit block (contains epilogue marker and ret)
  // This ensures we only have ONE marker in the entire function
  // Remove it because Ret required by deoptimization is already a terminator
//  builder()->CreateBr(function()->unified_exit_block());
}

void YuhuTopLevelBlock::call_register_finalizer(Value *receiver) {
  BasicBlock *orig_block = builder()->GetInsertBlock();
  YuhuState *orig_state = current_state()->copy();

  BasicBlock *do_call = function()->CreateBlock("has_finalizer");
  BasicBlock *done    = function()->CreateBlock("done");

  Value *klass = builder()->load_klass_from_object(receiver);

  Value *access_flags = builder()->CreateValueOfStructEntry(
    klass,
    Klass::access_flags_offset(),
    YuhuType::jint_type(),
    "access_flags");

  builder()->CreateCondBr(
    builder()->CreateICmpNE(
      builder()->CreateAnd(
        access_flags,
        LLVMValue::jint_constant(JVM_ACC_HAS_FINALIZER)),
      LLVMValue::jint_constant(0)),
    do_call, done);

  builder()->SetInsertPoint(do_call);
  call_vm(builder()->register_finalizer(), receiver, EX_CHECK_FULL);
  BasicBlock *branch_block = builder()->GetInsertBlock();
  builder()->CreateBr(done);

  builder()->SetInsertPoint(done);
  current_state()->merge(orig_state, orig_block, branch_block);
}

void YuhuTopLevelBlock::handle_return(BasicType type, Value* exception) {
  assert (exception == NULL || type == T_VOID, "exception OR result, please");

  if (num_monitors()) {
    // Protect our exception across possible monitor release decaches
    if (exception)
      set_oop_tmp(exception);

    // We don't need to check for exceptions thrown here.  If
    // we're returning a value then we just carry on as normal:
    // the caller will see the pending exception and handle it.
    // If we're returning with an exception then that exception
    // takes priority and the release_lock one will be ignored.
    while (num_monitors())
      release_lock(EX_CHECK_NONE);

    // Reload the exception we're throwing
    if (exception)
      exception = get_oop_tmp();
  }

  if (exception) {
    builder()->CreateStore(exception, pending_exception_address());
  }

  // Store the return value into the function-scope return slot.
  // The unified exit block loads from this slot and returns it via ret.
  // This replaces the old CreatePopFrame approach which wrote to the caller's
  // interpreter expression stack — a location the i2c adapter never reads
  // after a compiled method return (the i2c adapter only consumes x0).
  if (type != T_VOID) {
    llvm::AllocaInst* ret_slot = function()->return_slot();
    assert(ret_slot != NULL, "return slot must exist for non-void returns");
    builder()->CreateStore(
      pop_result(type)->generic_value(),
      ret_slot);
  }

  // CRITICAL: Jump to the unified exit block instead of creating multiple rets
  // The unified exit block contains the epilogue marker and ret instruction
  // This ensures we only have ONE marker in the entire function
  builder()->CreateBr(function()->unified_exit_block());
}

void YuhuTopLevelBlock::do_arraylength() {
  YuhuValue *array = pop();
  check_null(array);
  Value *length = builder()->CreateArrayLength(array->jarray_value());
  push(YuhuValue::create_jint(length, false));
}

void YuhuTopLevelBlock::do_aload(BasicType basic_type) {
  YuhuValue *index = pop();
  YuhuValue *array = pop();

  check_null(array);
  check_bounds(array, index);

  // For T_OBJECT with compressed oops, array elements are stored as i32 (narrowOop),
  // not as ptr. We need to load as i32 first, then decompress.
  Value *value;
  if (basic_type == T_OBJECT && UseCompressedOops) {
    // Load as i32 (compressed pointer)
    llvm::Type* narrow_oop_type = YuhuType::jint_type();  // i32 for compressed oop
    Value* addr = builder()->CreateArrayAddress(
      array->jarray_value(), basic_type, index->jint_value());
    // Cast address to i32* for loading compressed pointer
    Value* narrow_addr = builder()->CreateBitCast(
      addr,
      llvm::PointerType::getUnqual(narrow_oop_type),
      "narrow_oop_addr");
    value = builder()->CreateLoad(narrow_oop_type, narrow_addr, "compressed_oop");
    
    // Decompress: uncompressed = heap_base + (compressed << 3)
    // If heap_base is NULL, then: uncompressed = compressed << 3
    address heap_base = Universe::narrow_oop_base();
    int shift = Universe::narrow_oop_shift();
    
    // value is i32 (compressed pointer), extend to i64 for arithmetic
    Value* compressed_intptr = builder()->CreateZExt(
      value, YuhuType::intptr_type(), "compressed_intptr");
    
    // Shift left by 3 (or by Universe::narrow_oop_shift())
    Value* shifted = builder()->CreateShl(
      compressed_intptr,
      LLVMValue::intptr_constant(shift),
      "compressed_shifted");
    
    if (heap_base != NULL) {
      // Add heap base
      value = builder()->CreateAdd(
        shifted,
        LLVMValue::intptr_constant((intptr_t)heap_base),
        "decompressed_oop");
    } else {
      value = shifted;
    }
    
    // Cast back to pointer type
    value = builder()->CreateIntToPtr(
      value,
      YuhuType::oop_addrspace1_type(), // FIXED - object is allocated in heap
      "decompressed_ptr");
  } else {
    // Normal load for non-compressed oops or other types
    llvm::Type* element_type = YuhuType::to_arrayType(basic_type);
    value = builder()->CreateLoad(
      element_type,  // Explicitly pass type for LLVM 20+
      builder()->CreateArrayAddress(
        array->jarray_value(), basic_type, index->jint_value()));
    
    // Do normal type conversion
    llvm::Type *stack_type = YuhuType::to_stackType(basic_type);  // Use llvm::Type to avoid conflict
    if (value->getType() != stack_type)
      value = builder()->CreateIntCast(value, stack_type, basic_type != T_CHAR);
  }

  switch (basic_type) {
  case T_BYTE:
  case T_CHAR:
  case T_SHORT:
  case T_INT:
    push(YuhuValue::create_jint(value, false));
    break;

  case T_LONG:
    push(YuhuValue::create_jlong(value, false));
    break;

  case T_FLOAT:
    push(YuhuValue::create_jfloat(value));
    break;

  case T_DOUBLE:
    push(YuhuValue::create_jdouble(value));
    break;

  case T_OBJECT:
    
    // You might expect that array->type()->is_array_klass() would
    // always be true, but it isn't.  If ciTypeFlow detects that a
    // value is always null then that value becomes an untyped null
    // object.  Yuhu doesn't presently support this, so a generic
    // T_OBJECT is created.  In this case we guess the type using
    // the BasicType we were supplied.  In reality the generated
    // code will never be used, as the null value will be caught
    // by the above null pointer check.
    // http://icedtea.classpath.org/bugzilla/show_bug.cgi?id=324
    push(
      YuhuValue::create_generic(
        array->type()->is_array_klass() ?
          ((ciArrayKlass *) array->type())->element_type() :
          ciType::make(basic_type),
        value, false));
    break;

  default:
    tty->print_cr("Unhandled type %s", type2name(basic_type));
    ShouldNotReachHere();
  }
}

void YuhuTopLevelBlock::do_astore(BasicType basic_type) {
  YuhuValue *svalue = pop();
  YuhuValue *index  = pop();
  YuhuValue *array  = pop();

  check_null(array);
  check_bounds(array, index);

  Value *value;
  switch (basic_type) {
  case T_BYTE:
  case T_CHAR:
  case T_SHORT:
  case T_INT:
    value = svalue->jint_value();
    break;

  case T_LONG:
    value = svalue->jlong_value();
    break;

  case T_FLOAT:
    value = svalue->jfloat_value();
    break;

  case T_DOUBLE:
    value = svalue->jdouble_value();
    break;

  case T_OBJECT:
    value = svalue->jobject_value();
    // XXX assignability check
    break;

  default:
    tty->print_cr("Unhandled type %s", type2name(basic_type));
    ShouldNotReachHere();
  }

  llvm::Type *array_type = YuhuType::to_arrayType(basic_type);  // Use llvm::Type to avoid conflict
  if (value->getType() != array_type)
    value = builder()->CreateIntCast(value, array_type, basic_type != T_CHAR);

  Value *addr = builder()->CreateArrayAddress(
    array->jarray_value(), basic_type, index->jint_value(), "addr");

  builder()->CreateStore(value, addr);

  if (basic_type == T_OBJECT) // XXX or T_ARRAY?
    builder()->CreateUpdateBarrierSet(oopDesc::bs(), addr);
}

void YuhuTopLevelBlock::do_return(BasicType type) {
  if (target()->intrinsic_id() == vmIntrinsics::_Object_init)
    call_register_finalizer(local(0)->jobject_value());
  maybe_add_safepoint();
  handle_return(type, NULL);
}

void YuhuTopLevelBlock::do_athrow() {
  YuhuValue *exception = pop();
  check_null(exception);
  handle_exception(exception->jobject_value(), EX_CHECK_FULL);
}

void YuhuTopLevelBlock::do_goto() {
  do_branch(ciTypeFlow::GOTO_TARGET);
}

void YuhuTopLevelBlock::do_jsr() {
  push(YuhuValue::address_constant(iter()->next_bci()));
  do_branch(ciTypeFlow::GOTO_TARGET);
}

void YuhuTopLevelBlock::do_ret() {
  assert(local(iter()->get_index())->address_value() ==
         successor(ciTypeFlow::GOTO_TARGET)->start(), "should be");
  do_branch(ciTypeFlow::GOTO_TARGET);
}

// All propagation of state from one block to the next (via
// dest->add_incoming) is handled by these methods:
//   do_branch
//   do_if_helper
//   do_switch
//   handle_exception

void YuhuTopLevelBlock::do_branch(int successor_index) {
  YuhuTopLevelBlock *dest = successor(successor_index);
  builder()->CreateBr(dest->entry_block());
  dest->add_incoming(current_state());
}

void YuhuTopLevelBlock::do_if(ICmpInst::Predicate p,
                               YuhuValue*         b,
                               YuhuValue*         a) {
  Value *llvm_a, *llvm_b;
  if (a->is_jobject()) {
    llvm_a = a->intptr_value(builder());
    llvm_b = b->intptr_value(builder());
  }
  else {
    llvm_a = a->jint_value();
    llvm_b = b->jint_value();
  }
  do_if_helper(p, llvm_b, llvm_a, current_state(), current_state());
}

void YuhuTopLevelBlock::do_if_helper(ICmpInst::Predicate p,
                                      Value*              b,
                                      Value*              a,
                                      YuhuState*         if_taken_state,
                                      YuhuState*         not_taken_state) {
  YuhuTopLevelBlock *if_taken  = successor(ciTypeFlow::IF_TAKEN);
  YuhuTopLevelBlock *not_taken = successor(ciTypeFlow::IF_NOT_TAKEN);

  builder()->CreateCondBr(
    builder()->CreateICmp(p, a, b),
    if_taken->entry_block(), not_taken->entry_block());

  if_taken->add_incoming(if_taken_state);
  not_taken->add_incoming(not_taken_state);
}

void YuhuTopLevelBlock::do_switch() {
  int len = switch_table_length();

  YuhuTopLevelBlock *dest_block = successor(ciTypeFlow::SWITCH_DEFAULT);
  SwitchInst *switchinst = builder()->CreateSwitch(
    pop()->jint_value(), dest_block->entry_block(), len);
  dest_block->add_incoming(current_state());

  for (int i = 0; i < len; i++) {
    int dest_bci = switch_dest(i);
    if (dest_bci != switch_default_dest()) {
      dest_block = bci_successor(dest_bci);
      switchinst->addCase(
        LLVMValue::jint_constant(switch_key(i)),
        dest_block->entry_block());
      dest_block->add_incoming(current_state());
    }
  }
}

ciMethod* YuhuTopLevelBlock::improve_virtual_call(ciMethod*   caller,
                                              ciInstanceKlass* klass,
                                              ciMethod*        dest_method,
                                              ciType*          receiver_type) {
  // If the method is obviously final then we are already done
  if (dest_method->can_be_statically_bound())
    return dest_method;

  // Array methods are all inherited from Object and are monomorphic
  if (receiver_type->is_array_klass() &&
      dest_method->holder() == java_lang_Object_klass())
    return dest_method;

  // This code can replace a virtual call with a direct call if this
  // class is the only one in the entire set of loaded classes that
  // implements this method.  This makes the compiled code dependent
  // on other classes that implement the method not being loaded, a
  // condition which is enforced by the dependency tracker.  If the
  // dependency tracker determines a method has become invalid it
  // will mark it for recompilation, causing running copies to be
  // deoptimized.  Yuhu currently can't deoptimize arbitrarily like
  // that, so this optimization cannot be used.
  // http://icedtea.classpath.org/bugzilla/show_bug.cgi?id=481

  // All other interesting cases are instance classes
  if (!receiver_type->is_instance_klass())
    return NULL;

  // Attempt to improve the receiver
  ciInstanceKlass* actual_receiver = klass;
  ciInstanceKlass *improved_receiver = receiver_type->as_instance_klass();
  if (improved_receiver->is_loaded() &&
      improved_receiver->is_initialized() &&
      !improved_receiver->is_interface() &&
      improved_receiver->is_subtype_of(actual_receiver)) {
    actual_receiver = improved_receiver;
  }

  // Attempt to find a monomorphic target for this call using
  // class heirachy analysis.
  ciInstanceKlass *calling_klass = caller->holder();
  ciMethod* monomorphic_target =
    dest_method->find_monomorphic_target(calling_klass, klass, actual_receiver);
  if (monomorphic_target != NULL) {
    assert(!monomorphic_target->is_abstract(), "shouldn't be");

    function()->dependencies()->assert_unique_concrete_method(actual_receiver, monomorphic_target);

    // Opto has a bunch of type checking here that I don't
    // understand.  It's to inhibit casting in one direction,
    // possibly because objects in Opto can have inexact
    // types, but I can't even tell which direction it
    // doesn't like.  For now I'm going to block *any* cast.
    if (monomorphic_target != dest_method) {
      if (YuhuPerformanceWarnings) {
        warning("found monomorphic target, but inhibited cast:");
        tty->print("  dest_method = ");
        dest_method->print_short_name(tty);
        tty->cr();
        tty->print("  monomorphic_target = ");
        monomorphic_target->print_short_name(tty);
        tty->cr();
      }
      monomorphic_target = NULL;
    }
  }

  // Replace the virtual call with a direct one.  This makes
  // us dependent on that target method not getting overridden
  // by dynamic class loading.
  if (monomorphic_target != NULL) {
    dependencies()->assert_unique_concrete_method(
      actual_receiver, monomorphic_target);
    return monomorphic_target;
  }

  // Because Opto distinguishes exact types from inexact ones
  // it can perform a further optimization to replace calls
  // with non-monomorphic targets if the receiver has an exact
  // type.  We don't mark types this way, so we can't do this.


  return NULL;
}

Value *YuhuTopLevelBlock::get_direct_callee(ciMethod* method, address* out_stub_addr) {
  // Generate call to static call stub that returns the _from_compiled_entry directly
  // This stub loads the Method* and jumps to _from_compiled_entry, avoiding
  // the problematic field access in the generated LLVM IR
  // Pass both the target method and the current method being compiled
  address stub_addr = YuhuRuntime::generate_static_call_stub(method, target());
  if (out_stub_addr != NULL) {
      *out_stub_addr = stub_addr;
  }
  
  // Return the stub address as an integer constant
  // This will be used to access the compiled entry point
  return builder()->CreateIntToPtr(
    LLVMValue::intptr_constant((intptr_t)stub_addr),
    YuhuType::intptr_type(),
    "direct_callee_stub");
}

Value *YuhuTopLevelBlock::get_virtual_callee(YuhuValue* receiver,
                                              ciMethod* call_method,
                                              int vtable_index,
                                              address* out_stub_addr) {
  // Generate a virtual call stub that performs vtable lookup at runtime.
  // The stub address becomes the compile-time constant call target.
  // call_method is the statically declared target from the bytecode.
  address stub_addr = YuhuRuntime::generate_virtual_call_stub(
    call_method, target(), vtable_index);
  if (out_stub_addr != NULL) {
      *out_stub_addr = stub_addr;
  }
  
  // Return the stub address as an integer constant
  return builder()->CreateIntToPtr(
    LLVMValue::intptr_constant((intptr_t)stub_addr),
    YuhuType::intptr_type(),
    "virtual_callee_stub");
}

Value* YuhuTopLevelBlock::get_interface_callee(YuhuValue *receiver,
                                                ciMethod*   call_method,
                                                address* out_stub_addr) {
  // Generate an interface call stub that performs itable lookup at runtime.
  // The stub address becomes the compile-time constant call target.
  address stub_addr = YuhuRuntime::generate_interface_call_stub(
    call_method, target());
  if (out_stub_addr != NULL) {
      *out_stub_addr = stub_addr;
  }
  
  // Return the stub address as an integer constant
  return builder()->CreateIntToPtr(
    LLVMValue::intptr_constant((intptr_t)stub_addr),
    YuhuType::intptr_type(),
    "interface_callee_stub");
}

void YuhuTopLevelBlock::do_call() {
  // Set frequently used booleans
  bool is_static = bc() == Bytecodes::_invokestatic;
  bool is_virtual = bc() == Bytecodes::_invokevirtual;
  bool is_interface = bc() == Bytecodes::_invokeinterface;

  // Find the method being called
  bool will_link;
  ciSignature* sig;
  ciMethod *dest_method = iter()->get_method(will_link, &sig);

  assert(will_link, "typeflow responsibility");
  assert(dest_method->is_static() == is_static, "must match bc");

  // Find the class of the method being called.  Note
  // that the superclass check in the second assertion
  // is to cope with a hole in the spec that allows for
  // invokeinterface instructions where the resolved
  // method is a virtual method in java.lang.Object.
  // javac doesn't generate code like that, but there's
  // no reason a compliant Java compiler might not.

  // Precise definitions:
  // holder_klass: The class that declares/owns the method being called
  // klass: The declared type of the receiver object (from the bytecode)
  // Case #1:
  //  interface List {
  //          void add(Object x);  // ← declared in List
  //  }
  //  class ArrayList implements List {
  //          void add(Object x) { ... }  // ← implementation
  //  }
  //  void test() {
  //      List obj = new ArrayList();  // ← declared type is List
  //      obj.add("item");              // ← invokeinterface List.add
  //  }
  //  At the invokeinterface instruction:
  //  holder_klass = List (where add() is declared)
  //  klass = List (the declared type of obj)
  // Case #2:
  //  void test() {
  //      ArrayList obj = new ArrayList();  // ← declared type is ArrayList
  //      ((List)obj).toString();            // ← invokeinterface List.toString
  //  }
  //  holder_klass = Object (where toString() is actually declared - List doesn't declare it)
  //  klass = List (the type after the cast)
  ciInstanceKlass *holder_klass  = dest_method->holder();
  assert(holder_klass->is_loaded(), "scan_for_traps responsibility");
  assert(holder_klass->is_interface() ||
         holder_klass->super() == NULL ||
         !is_interface, "must match bc");

  bool is_forced_virtual = is_interface && holder_klass == java_lang_Object_klass();

  ciKlass *holder = iter()->get_declared_method_holder();
  ciInstanceKlass *klass =
    ciEnv::get_instance_klass_for_declared_method_holder(holder);

  if (is_forced_virtual) {
    klass = java_lang_Object_klass();
  }

  // Find the receiver in the stack.  We do this before
  // trying to inline because the inliner can only use
  // zero-checked values, not being able to perform the
  // check itself.
  YuhuValue *receiver = NULL;
  if (!is_static) {
    receiver = xstack(dest_method->arg_size() - 1);
    check_null(receiver);
  }

  // Try to improve non-direct calls
  bool call_is_virtual = is_virtual || is_interface;
  ciMethod *call_method = dest_method;
  if (call_is_virtual) {
    ciMethod *optimized_method = improve_virtual_call(
      target(), klass, dest_method, receiver->type());
    if (optimized_method) {
      call_method = optimized_method;
      call_is_virtual = false;
    }
  }

  // Try to inline the call
  if (!call_is_virtual) {
    if (YuhuInliner::attempt_inline(call_method, current_state(), stack(), bci())) {
      return;
    }
  }

  // Find the method we are calling
  Value *callee;
  address compiled_entry_address = 0;
  if (call_is_virtual) {
    if (is_virtual || is_forced_virtual) {
      assert(klass->is_linked(), "scan_for_traps responsibility");
      int vtable_index = call_method->resolve_vtable_index(
        target()->holder(), klass);
      assert(vtable_index >= 0, "should be");
      callee = get_virtual_callee(receiver, call_method, vtable_index, &compiled_entry_address);
    }
    else {
      assert(is_interface, "should be");
      callee = get_interface_callee(receiver, call_method, &compiled_entry_address);
    }
  }
  else {
    // For direct calls (including optimized virtual calls), use get_direct_callee
    // which now returns the stub address that jumps to _from_compiled_entry
    callee = get_direct_callee(call_method, &compiled_entry_address);
  }

  // All callees (direct, virtual, interface) now return stub addresses
  // The stub handles loading _from_compiled_entry internally
  Value *from_compiled_entry = callee;

  // IMPORTANT: Build argument list BEFORE decache_for_Java_call()!
  // decache will pop all arguments from the stack via xpop(),
  // so we must collect them first.
  //
  // CRITICAL: Must match HotSpot's Java calling convention for AArch64
  // x0 = dummy/return value slot (unused for input parameters)
  // x1 = first parameter (NULL for static, receiver for non-static)
  // x2+ = remaining parameters
  // See: assembler_aarch64.hpp line 77-82
  std::vector<Value*> call_args;
  int arg_slots = call_method->arg_size();

  // Add dummy value for x0 (unused input slot, used for return value)
  call_args.push_back(LLVMValue::intptr_constant(0));

  if (is_static) {
    // Static: x1 = first parameter, x2+ = remaining parameters
    // Collect all Java arguments from stack in reverse order
    for (int i = arg_slots - 1; i >= 0; i--) {
      YuhuValue* v = xstack(i);
      switch (v->basic_type()) {
        case T_BOOLEAN:
        case T_BYTE:
        case T_CHAR:
        case T_SHORT:
        case T_INT:
          call_args.push_back(v->jint_value());
          break;
        case T_LONG:
          call_args.push_back(v->jlong_value());
          break;
        case T_FLOAT:
          call_args.push_back(v->jfloat_value());
          break;
        case T_DOUBLE:
          call_args.push_back(v->jdouble_value());
          break;
        case T_OBJECT:
        case T_ARRAY:
          call_args.push_back(v->jobject_value());
          break;
        default:
          ShouldNotReachHere();
      }
    }
  } else {
    // Non-static: x1 = receiver
    YuhuValue* recv_val = xstack(arg_slots - 1);
    // Explicit null check for method call receiver
    check_null(recv_val);
    call_args.push_back(recv_val->jobject_value());  // receiver in x1
    // Collect remaining Java arguments (excluding receiver)
    for (int i = arg_slots - 2; i >= 0; i--) {
      YuhuValue* v = xstack(i);
      switch (v->basic_type()) {
        case T_BOOLEAN:
        case T_BYTE:
        case T_CHAR:
        case T_SHORT:
        case T_INT:
          call_args.push_back(v->jint_value());
          break;
        case T_LONG:
          call_args.push_back(v->jlong_value());
          break;
        case T_FLOAT:
          call_args.push_back(v->jfloat_value());
          break;
        case T_DOUBLE:
          call_args.push_back(v->jdouble_value());
          break;
        case T_OBJECT:
        case T_ARRAY:
          call_args.push_back(v->jobject_value());
          break;
        default:
          ShouldNotReachHere();
      }
    }
  }

  // NOW it's safe to decache (this will xpop() all arguments)
  // This creates an OopMap at the call site
  decache_for_Java_call(call_method);

  // NEW: Get unique virtual offset for this call site (MUST be before creating placeholders)
  uint64_t virtual_offset = code_buffer()->create_unique_offset();
  
  // NEW: Create dual virtual addresses with same virtual_offset
  uint64_t last_java_pc_va = LAST_JAVA_PC_MAGIC | virtual_offset;  // For last_Java_pc
  uint64_t call_target_va = (virtual_offset << 32) | (virtual_offset << 16) | CALL_TARGET_MAGIC;

  stack()->CreateCallSitePlaceholder(last_java_pc_va);
  
  // NEW: Register call site for later patching
  YuhuDebugInformationRecorder::get()->register_call_site(virtual_offset, call_target_va, (uint64_t) compiled_entry_address, CallSiteType::java_call, bci());

  // Save callee-saved registers that Yuhu uses but interpreter may corrupt
//  builder()->CreateSaveCalleeSavedRegisters();

  // Cast from_compiled_entry to a function pointer matching the callee's signature
  // We need to construct the callee's FunctionType based on its Java signature
  std::vector<llvm::Type*> param_types;
  
  // First parameter: receiver (for non-static) or NULL (for static)
  // CRITICAL: Must match HotSpot's Java calling convention for AArch64
  // x0 = dummy/return value slot (unused for input)
  // x1 = receiver (for non-static) or NULL (for static)
  // x2+ = remaining parameters
  if (is_static) {
    param_types.push_back(YuhuType::intptr_type());  // Dummy in x0 (unused)
    // x1, x2, ... will be filled by actual parameters (no NULL placeholder needed)
  } else {
    param_types.push_back(YuhuType::intptr_type());  // Dummy in x0 (unused)
    param_types.push_back(YuhuType::oop_addrspace1_type());     // FIXED - use same type as function definition, receiver in x1
  }
  
  // Add Java method parameters
  for (int i = 0; i < sig->count(); i++) {
    ciType* param_type = sig->type_at(i);
    param_types.push_back(YuhuType::to_stackType(param_type));
  }

  // Use the actual return type of the method being called
  // This is critical: for methods returning objects (like array()), we need
  // to use oop_type (64-bit), not jint_type (32-bit)
  ciType* return_type = call_method->return_type();
  llvm::Type* llvm_return_type = YuhuType::to_stackType(return_type);

  // Create the correct function type with actual return type
  llvm::FunctionType* compiled_ftype = FunctionType::get(llvm_return_type, param_types, false);

  // NEW: Use virtual address placeholder as call target if we extracted the real address
  Value *compiled_entry_ptr;
    llvm::Module* mod = builder()->GetInsertBlock()->getModule();
    compiled_entry_ptr = builder()->CreateIntToPtr(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(mod->getContext()), call_target_va),
      PointerType::getUnqual(compiled_ftype),
      "compiled_entry_virtual");

  // Call the compiled entry and get the actual return value
  // Note: decache_for_Java_call() was already called above (line 1566)
  Value* call_result = builder()->CreateCall(
    compiled_ftype, compiled_entry_ptr, call_args);

  // Restore callee-saved registers from save area at [sp, #80]
//  builder()->CreateRestoreCalleeSavedRegisters();

  // NOTE: Unlike Shark, we use the correct function return type instead of jint.
  // Shark uses a special entry point that returns jint (deoptimization count),
  // but Yuhu calls standard _from_compiled_entry which returns the actual method result.
  // Therefore, call_result contains the actual return value (e.g., object reference for array()).
  // Deoptimization is detected through check_pending_exception() below, not through the return value.
  // The call_result is passed to cache_after_Java_call() below to properly store return values.

  // IMPORTANT: Create another OopMap at the return point
  // The decache_for_Java_call() above created an OopMap at the call site,
  // but we also need an OopMap at the return address because deoptimization
  // can happen when returning from the callee.
  // We use YuhuJavaCallDecacher again to create an OopMap at this return point.
  YuhuJavaCallDecacher(function(), bci(), call_method).scan(current_state());

  cache_after_Java_call(call_method, call_result);  // ← Pass call_result here!

  // Check for pending exceptions
  check_pending_exception(EX_CHECK_FULL);

  // Mark that a safepoint check has occurred
  current_state()->set_has_safepointed(true);
}

bool YuhuTopLevelBlock::static_subtype_check(ciKlass* check_klass,
                                              ciKlass* object_klass) {
  // If the class we're checking against is java.lang.Object
  // then this is a no brainer.  Apparently this can happen
  // in reflective code...
  if (check_klass == java_lang_Object_klass())
    return true;

  // Perform a subtype check.  NB in opto's code for this
  // (GraphKit::static_subtype_check) it says that static
  // interface types cannot be trusted, and if opto can't
  // trust them then I assume we can't either.
  if (object_klass->is_loaded() && !object_klass->is_interface()) {
    if (object_klass == check_klass)
      return true;

    if (check_klass->is_loaded() && object_klass->is_subtype_of(check_klass))
      return true;
  }

  return false;
}

void YuhuTopLevelBlock::do_instance_check() {
  // Get the class we're checking against
  bool will_link;
  ciKlass *check_klass = iter()->get_klass(will_link);

  // Get the class of the object we're checking
  ciKlass *object_klass = xstack(0)->type()->as_klass();

  // Can we optimize this check away?
  if (static_subtype_check(check_klass, object_klass)) {
    if (bc() == Bytecodes::_instanceof) {
      pop();
      push(YuhuValue::jint_constant(1));
    }
    return;
  }

  // Need to check this one at runtime
  if (will_link)
    do_full_instance_check(check_klass);
  else
    do_trapping_instance_check(check_klass);
}

bool YuhuTopLevelBlock::maybe_do_instanceof_if() {
  // Get the class we're checking against
  bool will_link;
  ciKlass *check_klass = iter()->get_klass(will_link);

  // If the class is unloaded then the instanceof
  // cannot possibly succeed.
  if (!will_link)
    return false;

  // Keep a copy of the object we're checking
  YuhuValue *old_object = xstack(0);

  // Get the class of the object we're checking
  ciKlass *object_klass = old_object->type()->as_klass();

  // If the instanceof can be optimized away at compile time
  // then any subsequent checkcasts will be too so we handle
  // it normally.
  if (static_subtype_check(check_klass, object_klass))
    return false;

  // Perform the instance check
  do_full_instance_check(check_klass);
  Value *result = pop()->jint_value();

  // Create the casted object
  YuhuValue *new_object = YuhuValue::create_generic(
    check_klass, old_object->jobject_value(), old_object->zero_checked());

  // Create two copies of the current state, one with the
  // original object and one with all instances of the
  // original object replaced with the new, casted object.
  YuhuState *new_state = current_state();
  YuhuState *old_state = new_state->copy();
  new_state->replace_all(old_object, new_object);

  // Perform the check-and-branch
  switch (iter()->next_bc()) {
  case Bytecodes::_ifeq:
    // branch if not an instance
    do_if_helper(
      ICmpInst::ICMP_EQ,
      LLVMValue::jint_constant(0), result,
      old_state, new_state);
    break;

  case Bytecodes::_ifne:
    // branch if an instance
    do_if_helper(
      ICmpInst::ICMP_NE,
      LLVMValue::jint_constant(0), result,
      new_state, old_state);
    break;

  default:
    ShouldNotReachHere();
  }

  return true;
}

void YuhuTopLevelBlock::do_full_instance_check(ciKlass* klass) {
  BasicBlock *not_null      = function()->CreateBlock("not_null");
  BasicBlock *subtype_check = function()->CreateBlock("subtype_check");
  BasicBlock *is_instance   = function()->CreateBlock("is_instance");
  BasicBlock *not_instance  = function()->CreateBlock("not_instance");
  BasicBlock *merge1        = function()->CreateBlock("merge1");
  BasicBlock *merge2        = function()->CreateBlock("merge2");

  enum InstanceCheckStates {
    IC_IS_NULL,
    IC_IS_INSTANCE,
    IC_NOT_INSTANCE,
  };

  // Pop the object off the stack
  Value *object = pop()->jobject_value();

  // Null objects aren't instances of anything
  builder()->CreateCondBr(
    builder()->CreateICmpEQ(object, LLVMValue::null()),
    merge2, not_null);
  BasicBlock *null_block = builder()->GetInsertBlock();

  // Get the class we're checking against
  builder()->SetInsertPoint(not_null);
  Value *check_klass = builder()->CreateInlineMetadata(klass, YuhuType::klass_type());

  // Get the class of the object being tested
  Value *object_klass = builder()->load_klass_from_object(object);

  // Perform the check
  builder()->CreateCondBr(
    builder()->CreateICmpEQ(check_klass, object_klass),
    is_instance, subtype_check);

  builder()->SetInsertPoint(subtype_check);

    // Manual call site registration (can't use call_vm from here)
    uint64_t virtual_offset = code_buffer()->create_unique_offset();
    uint64_t last_java_pc_va = LAST_JAVA_PC_MAGIC | virtual_offset;  // For last_Java_pc
    uint64_t call_target_va = (virtual_offset << 32) | (virtual_offset << 16) | CALL_TARGET_MAGIC;

    // Extract actual helper address
    uint64_t helper_address = 0;

  Value* callee = builder()->is_subtype_of();

    if (auto* CastInst = llvm::dyn_cast<llvm::ConstantExpr>(callee)) {
        if (CastInst->getOpcode() == llvm::Instruction::IntToPtr) {
            if (auto* IntConst = llvm::dyn_cast<llvm::ConstantInt>(CastInst->getOperand(0))) {
                helper_address = IntConst->getZExtValue();
            }
        }
    }

    // Replace callee with virtual address
    if (helper_address != 0) {
        llvm::Module* mod = builder()->GetInsertBlock()->getModule();
        callee = builder()->CreateIntToPtr(
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(mod->getContext()), call_target_va),
                callee->getType());

        stack()->CreateCallSitePlaceholder(last_java_pc_va);

        YuhuDebugInformationRecorder::get()->register_call_site(
                virtual_offset, call_target_va, helper_address,
                CallSiteType::vm_call, bci());
    }

  llvm::FunctionType* func_type = YuhuBuilder::make_ftype("KK", "c");
  std::vector<Value*> args;
  args.push_back(check_klass);
  args.push_back(object_klass);
  builder()->CreateCondBr(
    builder()->CreateICmpNE(
      builder()->CreateCall(func_type, callee, args),
      LLVMValue::jbyte_constant(0)),
    is_instance, not_instance);

  builder()->SetInsertPoint(is_instance);
  builder()->CreateBr(merge1);

  builder()->SetInsertPoint(not_instance);
  builder()->CreateBr(merge1);

  // First merge
  builder()->SetInsertPoint(merge1);
  PHINode *nonnull_result = builder()->CreatePHI(
    YuhuType::jint_type(), 0, "nonnull_result");
  nonnull_result->addIncoming(
    LLVMValue::jint_constant(IC_IS_INSTANCE), is_instance);
  nonnull_result->addIncoming(
    LLVMValue::jint_constant(IC_NOT_INSTANCE), not_instance);
  BasicBlock *nonnull_block = builder()->GetInsertBlock();
  builder()->CreateBr(merge2);

  // Second merge
  builder()->SetInsertPoint(merge2);
  PHINode *result = builder()->CreatePHI(
    YuhuType::jint_type(), 0, "result");
  result->addIncoming(LLVMValue::jint_constant(IC_IS_NULL), null_block);
  result->addIncoming(nonnull_result, nonnull_block);

  // Handle the result
  if (bc() == Bytecodes::_checkcast) {
    BasicBlock *failure = function()->CreateBlock("failure");
    BasicBlock *success = function()->CreateBlock("success");

    builder()->CreateCondBr(
      builder()->CreateICmpNE(
        result, LLVMValue::jint_constant(IC_NOT_INSTANCE)),
      success, failure);

    builder()->SetInsertPoint(failure);
    YuhuState *saved_state = current_state()->copy();

    call_vm(
      builder()->throw_ClassCastException(),
      builder()->CreateIntToPtr(
        LLVMValue::intptr_constant((intptr_t) __FILE__),
        PointerType::getUnqual(YuhuType::jbyte_type())),
      LLVMValue::jint_constant(__LINE__),
      EX_CHECK_NONE);

    Value *pending_exception = get_pending_exception();
    clear_pending_exception();
    handle_exception(pending_exception, EX_CHECK_FULL);

    set_current_state(saved_state);
    builder()->SetInsertPoint(success);
    push(YuhuValue::create_generic(klass, object, false));
  }
  else {
    push(
      YuhuValue::create_jint(
        builder()->CreateIntCast(
          builder()->CreateICmpEQ(
            result, LLVMValue::jint_constant(IC_IS_INSTANCE)),
          YuhuType::jint_type(), false), false));
  }
}

void YuhuTopLevelBlock::do_trapping_instance_check(ciKlass* klass) {
  BasicBlock *not_null = function()->CreateBlock("not_null");
  BasicBlock *is_null  = function()->CreateBlock("null");

  // Leave the object on the stack so it's there if we trap
  builder()->CreateCondBr(
    builder()->CreateICmpEQ(xstack(0)->jobject_value(), LLVMValue::null()),
    is_null, not_null);
  YuhuState *saved_state = current_state()->copy();

  // If it's not null then we need to trap
  builder()->SetInsertPoint(not_null);
  set_current_state(saved_state->copy());
  do_trap(
    Deoptimization::make_trap_request(
      Deoptimization::Reason_uninitialized,
      Deoptimization::Action_reinterpret));

  // If it's null then we're ok
  builder()->SetInsertPoint(is_null);
  set_current_state(saved_state);
  if (bc() == Bytecodes::_checkcast) {
    push(YuhuValue::create_generic(klass, pop()->jobject_value(), false));
  }
  else {
    pop();
    push(YuhuValue::jint_constant(0));
  }
}

void YuhuTopLevelBlock::do_new() {
  bool will_link;
  ciInstanceKlass* klass = iter()->get_klass(will_link)->as_instance_klass();
  assert(will_link, "typeflow responsibility");

  BasicBlock *got_tlab            = NULL;
  BasicBlock *heap_alloc          = NULL;
  BasicBlock *retry               = NULL;
  BasicBlock *got_heap            = NULL;
  BasicBlock *initialize          = NULL;
  BasicBlock *got_fast            = NULL;
  BasicBlock *slow_alloc_and_init = NULL;
  BasicBlock *got_slow            = NULL;
  BasicBlock *push_object         = NULL;

  YuhuState *fast_state = NULL;

  Value *tlab_object = NULL;
  Value *heap_object = NULL;
  Value *fast_object = NULL;
  Value *slow_object = NULL;
  Value *object      = NULL;

  // The fast path
  if (!Klass::layout_helper_needs_slow_path(klass->layout_helper())) {
    if (UseTLAB) {
      got_tlab          = function()->CreateBlock("got_tlab");
      heap_alloc        = function()->CreateBlock("heap_alloc");
    }
    retry               = function()->CreateBlock("retry");
    got_heap            = function()->CreateBlock("got_heap");
    initialize          = function()->CreateBlock("initialize");
    slow_alloc_and_init = function()->CreateBlock("slow_alloc_and_init");
    push_object         = function()->CreateBlock("push_object");

    size_t size_in_bytes = klass->size_helper() << LogHeapWordSize;

    // Thread local allocation
    if (UseTLAB) {
      Value *top_addr = builder()->CreateAddressOfStructEntry(
        thread(), Thread::tlab_top_offset(),
        PointerType::getUnqual(YuhuType::intptr_type()),
        "top_addr");

      Value *end = builder()->CreateValueOfStructEntry(
        thread(), Thread::tlab_end_offset(),
        YuhuType::intptr_type(),
        "end");

      Value *old_top = builder()->CreateLoad(YuhuType::intptr_type(), top_addr, "old_top");
      Value *new_top = builder()->CreateAdd(
        old_top, LLVMValue::intptr_constant(size_in_bytes));

      builder()->CreateCondBr(
        builder()->CreateICmpULE(new_top, end),
        got_tlab, heap_alloc);

      builder()->SetInsertPoint(got_tlab);
      tlab_object = builder()->CreateIntToPtr(
        old_top, YuhuType::oop_addrspace1_type(), "tlab_object"); // FIXED - tlab object is still in Eden, part of heap, should GC

      builder()->CreateStore(new_top, top_addr);
      builder()->CreateBr(initialize);

      builder()->SetInsertPoint(heap_alloc);
    }

    // Heap allocation
    Value *top_addr = builder()->CreateIntToPtr(
        LLVMValue::intptr_constant((intptr_t) Universe::heap()->top_addr()),
      PointerType::getUnqual(YuhuType::intptr_type()),
      "top_addr");

    Value *end = builder()->CreateLoad(
      YuhuType::intptr_type(),  // Explicitly pass type for LLVM 20+
      builder()->CreateIntToPtr(
        LLVMValue::intptr_constant((intptr_t) Universe::heap()->end_addr()),
        PointerType::getUnqual(YuhuType::intptr_type())),
      "end");

    builder()->CreateBr(retry);
    builder()->SetInsertPoint(retry);

    Value *old_top = builder()->CreateLoad(YuhuType::intptr_type(), top_addr, "top");
    Value *new_top = builder()->CreateAdd(
      old_top, LLVMValue::intptr_constant(size_in_bytes));

    builder()->CreateCondBr(
      builder()->CreateICmpULE(new_top, end),
      got_heap, slow_alloc_and_init);

    builder()->SetInsertPoint(got_heap);
    heap_object = builder()->CreateIntToPtr(
      old_top, YuhuType::oop_addrspace1_type(), "heap_object"); // FIXED - object is allocated in heap

    // LLVM 20+ requires alignment and success/failure ordering for CreateAtomicCmpXchg
#if LLVM_VERSION_MAJOR >= 20
    Value *cmpxchg_result = builder()->CreateAtomicCmpXchg(
      top_addr, old_top, new_top,
      llvm::MaybeAlign(HeapWordSize),  // Alignment
      llvm::AtomicOrdering::SequentiallyConsistent,  // Success ordering
      llvm::AtomicOrdering::SequentiallyConsistent);  // Failure ordering
    // Extract the success flag from the result (second element of the struct)
    Value *success = builder()->CreateExtractValue(cmpxchg_result, 1, "success");
#else
    Value *check = builder()->CreateAtomicCmpXchg(top_addr, old_top, new_top, llvm::AtomicOrdering::SequentiallyConsistent);
#endif
    builder()->CreateCondBr(
#if LLVM_VERSION_MAJOR >= 20
      success,
#else
      builder()->CreateICmpEQ(old_top, check),
#endif
      initialize, retry);

    // Initialize the object
    builder()->SetInsertPoint(initialize);
    if (tlab_object) {
      PHINode *phi = builder()->CreatePHI(
        YuhuType::oop_addrspace1_type(), 0, "fast_object"); // FIXED - phi node should use oop_addrspace1_type as used in slow path and fast path
      phi->addIncoming(tlab_object, got_tlab);
      phi->addIncoming(heap_object, got_heap);
      fast_object = phi;
    }
    else {
      fast_object = heap_object;
    }

      // Convert oop (ptr addrspace(1)) → intptr_t → ptr (address space 0)
      Value* fast_object_int = builder()->CreatePtrToInt(fast_object, YuhuType::intptr_type());
      Value* memset_ptr = builder()->CreateIntToPtr(fast_object_int, PointerType::getUnqual(YuhuType::jbyte_type()));

    builder()->CreateMemset(memset_ptr,
      LLVMValue::jbyte_constant(0),
      LLVMValue::jint_constant(size_in_bytes),
      LLVMValue::jint_constant(HeapWordSize));

    Value *mark_addr = builder()->CreateAddressOfStructEntry(
      fast_object, in_ByteSize(oopDesc::mark_offset_in_bytes()),
      PointerType::getUnqual(YuhuType::intptr_type()),
      "mark_addr");

    // Set the mark
    intptr_t mark;
    if (UseBiasedLocking) {
      mark = (intptr_t) markOopDesc::biased_locking_prototype();
    }
    else {
      mark = (intptr_t) markOopDesc::prototype();
    }
    builder()->CreateStore(LLVMValue::intptr_constant(mark), mark_addr);

    // Set the class
    Value *rtklass = builder()->CreateInlineMetadata(klass, YuhuType::klass_type());
    builder()->store_klass_to_object(fast_object, rtklass);
    got_fast = builder()->GetInsertBlock();

    builder()->CreateBr(push_object);
    builder()->SetInsertPoint(slow_alloc_and_init);
    fast_state = current_state()->copy();
  }

  // The slow path
  // Option A: pass resolved Klass* directly (the same constant used by the
  // fast path above on line ~1964), instead of the cp index.
  call_vm(
    builder()->new_instance(),
    builder()->CreateInlineMetadata(klass, YuhuType::klass_type()),
    EX_CHECK_FULL);
  slow_object = get_vm_result();
  got_slow = builder()->GetInsertBlock();

  // Push the object
  if (push_object) {
    builder()->CreateBr(push_object);
    builder()->SetInsertPoint(push_object);
  }
  if (fast_object) {
    PHINode *phi = builder()->CreatePHI(YuhuType::oop_addrspace1_type(), 0, "object"); // FIXED - phi node should use oop_addrspace1_type as used in slow path and fast path
    phi->addIncoming(fast_object, got_fast);
    phi->addIncoming(slow_object, got_slow);
    object = phi;
    current_state()->merge(fast_state, got_fast, got_slow);
  }
  else {
    object = slow_object;
  }

  push(YuhuValue::create_jobject(object, true));
}

void YuhuTopLevelBlock::do_newarray() {
  BasicType type = (BasicType) iter()->get_index();

  call_vm(
    builder()->newarray(),
    LLVMValue::jint_constant(type),
    pop()->jint_value(),
    EX_CHECK_FULL);

  ciArrayKlass *array_klass = ciArrayKlass::make(ciType::make(type));
  push(YuhuValue::create_generic(array_klass, get_vm_result(), true));
}

void YuhuTopLevelBlock::do_anewarray() {
  bool will_link;
  ciKlass *klass = iter()->get_klass(will_link);
  assert(will_link, "typeflow responsibility");

  ciObjArrayKlass *array_klass = ciObjArrayKlass::make(klass);
  if (!array_klass->is_loaded()) {
    Unimplemented();
  }

  // Option A: pass the resolved element Klass* directly.
  call_vm(
    builder()->anewarray(),
    builder()->CreateInlineMetadata(klass, YuhuType::klass_type()),
    pop()->jint_value(),
    EX_CHECK_FULL);

  push(YuhuValue::create_generic(array_klass, get_vm_result(), true));
}

void YuhuTopLevelBlock::do_multianewarray() {
  bool will_link;
  ciArrayKlass *array_klass = iter()->get_klass(will_link)->as_array_klass();
  assert(will_link, "typeflow responsibility");

  // The dimensions are stack values, so we use their slots for the
  // dimensions array.  Note that we are storing them in the reverse
  // of normal stack order.
  int ndims = iter()->get_dimensions();

  Value *dimensions = stack()->slot_addr(
    stack()->stack_slots_offset() + max_stack() - xstack_depth(),
    llvm::ArrayType::get(YuhuType::jint_type(), ndims),  // Use llvm::ArrayType to avoid conflict
    "dimensions");

  for (int i = 0; i < ndims; i++) {
    builder()->CreateStore(
      xstack(ndims - 1 - i)->jint_value(),
      builder()->CreateStructGEP(llvm::ArrayType::get(YuhuType::jint_type(), ndims), dimensions, i));
  }

  // Option A: pass the resolved array Klass* directly.
  call_vm(
    builder()->multianewarray(),
    builder()->CreateInlineMetadata(array_klass, YuhuType::klass_type()),
    LLVMValue::jint_constant(ndims),
    builder()->CreateStructGEP(llvm::ArrayType::get(YuhuType::jint_type(), ndims), dimensions, 0),
    EX_CHECK_FULL);

  // Now we can pop the dimensions off the stack
  for (int i = 0; i < ndims; i++)
    pop();

  push(YuhuValue::create_generic(array_klass, get_vm_result(), true));
}

void YuhuTopLevelBlock::acquire_method_lock() {
  Value *lockee;
  if (target()->is_static()) {
    lockee = builder()->CreateInlineOop(target()->holder()->java_mirror());
  }
  else
    lockee = local(0)->jobject_value();

  iter()->force_bci(start()); // for the decache in acquire_lock
  acquire_lock(lockee, EX_CHECK_NO_CATCH);
}

void YuhuTopLevelBlock::do_monitorenter() {
  YuhuValue *lockee = pop();
  check_null(lockee);
  acquire_lock(lockee->jobject_value(), EX_CHECK_FULL);
}

void YuhuTopLevelBlock::do_monitorexit() {
  pop(); // don't need this (monitors are block structured)
  release_lock(EX_CHECK_NO_CATCH);
}

void YuhuTopLevelBlock::acquire_lock(Value *lockee, int exception_action) {
  BasicBlock *try_recursive = function()->CreateBlock("try_recursive");
  BasicBlock *got_recursive = function()->CreateBlock("got_recursive");
  BasicBlock *not_recursive = function()->CreateBlock("not_recursive");
  BasicBlock *acquired_fast = function()->CreateBlock("acquired_fast");
  BasicBlock *lock_acquired = function()->CreateBlock("lock_acquired");

  int monitor = num_monitors();
  Value *monitor_addr        = stack()->monitor_addr(monitor);
  Value *monitor_object_addr = stack()->monitor_object_addr(monitor);
  Value *monitor_header_addr = stack()->monitor_header_addr(monitor);

  // Store the object and mark the slot as live
  builder()->CreateStore(lockee, monitor_object_addr);
  set_num_monitors(monitor + 1);

  // Try a simple lock
  Value *mark_addr = builder()->CreateAddressOfStructEntry(
    lockee, in_ByteSize(oopDesc::mark_offset_in_bytes()),
    PointerType::getUnqual(YuhuType::intptr_type()),
    "mark_addr");

  Value *mark = builder()->CreateLoad(YuhuType::intptr_type(), mark_addr, "mark");
  Value *disp = builder()->CreateOr(
    mark, LLVMValue::intptr_constant(markOopDesc::unlocked_value), "disp");
  builder()->CreateStore(disp, monitor_header_addr);

  Value *lock = builder()->CreatePtrToInt(
    monitor_header_addr, YuhuType::intptr_type());
  // LLVM 20+ requires alignment and success/failure ordering for CreateAtomicCmpXchg
#if LLVM_VERSION_MAJOR >= 20
  Value *check = builder()->CreateAtomicCmpXchg(
    mark_addr, disp, lock,
    llvm::MaybeAlign(HeapWordSize),  // Alignment
    llvm::AtomicOrdering::Acquire,  // Success ordering
    llvm::AtomicOrdering::Acquire);  // Failure ordering
#else
  Value *check = builder()->CreateAtomicCmpXchg(mark_addr, disp, lock, llvm::AtomicOrdering::Acquire);
#endif
  builder()->CreateCondBr(
    builder()->CreateICmpEQ(disp, check),
    acquired_fast, try_recursive);

  // Locking failed, but maybe this thread already owns it
  builder()->SetInsertPoint(try_recursive);
  Value *addr = builder()->CreateAnd(
    disp,
    LLVMValue::intptr_constant(~markOopDesc::lock_mask_in_place));

  // NB we use the entire stack, but JavaThread::is_lock_owned()
  // uses a more limited range.  I don't think it hurts though...
  Value *stack_limit = builder()->CreateValueOfStructEntry(
    thread(), Thread::stack_base_offset(),
    YuhuType::intptr_type(),
    "stack_limit");

  assert(sizeof(size_t) == sizeof(intptr_t), "should be");
  Value *stack_size = builder()->CreateValueOfStructEntry(
    thread(), Thread::stack_size_offset(),
    YuhuType::intptr_type(),
    "stack_size");

  Value *stack_start =
    builder()->CreateSub(stack_limit, stack_size, "stack_start");

  builder()->CreateCondBr(
    builder()->CreateAnd(
      builder()->CreateICmpUGE(addr, stack_start),
      builder()->CreateICmpULT(addr, stack_limit)),
    got_recursive, not_recursive);

  builder()->SetInsertPoint(got_recursive);
  builder()->CreateStore(LLVMValue::intptr_constant(0), monitor_header_addr);
  builder()->CreateBr(acquired_fast);

  // Create an edge for the state merge
  builder()->SetInsertPoint(acquired_fast);
  YuhuState *fast_state = current_state()->copy();
  builder()->CreateBr(lock_acquired);

  // It's not a recursive case so we need to drop into the runtime
  builder()->SetInsertPoint(not_recursive);
  call_vm(
    builder()->monitorenter(), monitor_addr,
    exception_action | EAM_MONITOR_FUDGE);
  BasicBlock *acquired_slow = builder()->GetInsertBlock();
  builder()->CreateBr(lock_acquired);

  // All done
  builder()->SetInsertPoint(lock_acquired);
  current_state()->merge(fast_state, acquired_fast, acquired_slow);
}

void YuhuTopLevelBlock::release_lock(int exception_action) {
  BasicBlock *not_recursive = function()->CreateBlock("not_recursive");
  BasicBlock *released_fast = function()->CreateBlock("released_fast");
  BasicBlock *slow_path     = function()->CreateBlock("slow_path");
  BasicBlock *lock_released = function()->CreateBlock("lock_released");

  int monitor = num_monitors() - 1;
  Value *monitor_addr        = stack()->monitor_addr(monitor);
  Value *monitor_object_addr = stack()->monitor_object_addr(monitor);
  Value *monitor_header_addr = stack()->monitor_header_addr(monitor);

  // If it is recursive then we're already done
  Value *disp = builder()->CreateLoad(YuhuType::intptr_type(), monitor_header_addr);
  builder()->CreateCondBr(
    builder()->CreateICmpEQ(disp, LLVMValue::intptr_constant(0)),
    released_fast, not_recursive);

  // Try a simple unlock
  builder()->SetInsertPoint(not_recursive);

  Value *lock = builder()->CreatePtrToInt(
    monitor_header_addr, YuhuType::intptr_type());

  Value *lockee = builder()->CreateLoad(YuhuType::oop_addrspace1_type(), monitor_object_addr);// FIXED - locked object is allocated in heap

  Value *mark_addr = builder()->CreateAddressOfStructEntry(
    lockee, in_ByteSize(oopDesc::mark_offset_in_bytes()),
    PointerType::getUnqual(YuhuType::intptr_type()),
    "mark_addr");

  // LLVM 20+ requires alignment and success/failure ordering for CreateAtomicCmpXchg
#if LLVM_VERSION_MAJOR >= 20
  Value *check = builder()->CreateAtomicCmpXchg(
    mark_addr, lock, disp,
    llvm::MaybeAlign(HeapWordSize),  // Alignment
    llvm::AtomicOrdering::Release,  // Success ordering
    llvm::AtomicOrdering::Release);  // Failure ordering
#else
  Value *check = builder()->CreateAtomicCmpXchg(mark_addr, lock, disp, llvm::AtomicOrdering::Release);
#endif
  builder()->CreateCondBr(
    builder()->CreateICmpEQ(lock, check),
    released_fast, slow_path);

  // Create an edge for the state merge
  builder()->SetInsertPoint(released_fast);
  YuhuState *fast_state = current_state()->copy();
  builder()->CreateBr(lock_released);

  // Need to drop into the runtime to release this one
  builder()->SetInsertPoint(slow_path);
  call_vm(builder()->monitorexit(), monitor_addr, exception_action);
  BasicBlock *released_slow = builder()->GetInsertBlock();
  builder()->CreateBr(lock_released);

  // All done
  builder()->SetInsertPoint(lock_released);
  current_state()->merge(fast_state, released_fast, released_slow);

  // The object slot is now dead
  set_num_monitors(monitor);
}

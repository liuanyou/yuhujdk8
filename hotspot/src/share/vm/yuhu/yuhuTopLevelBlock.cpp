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
#include "yuhu/yuhuInliner.hpp"
#include "yuhu/yuhuState.hpp"
#include "yuhu/yuhuTopLevelBlock.hpp"
#include "yuhu/yuhuValue.hpp"
#include "yuhu/yuhu_globals.hpp"
#include "utilities/debug.hpp"

using namespace llvm;

void YuhuTopLevelBlock::scan_for_traps() {
  // If typeflow found a trap then don't scan past it
  int limit_bci = ciblock()->has_trap() ? ciblock()->trap_bci() : limit();

  // Scan the bytecode for traps that are always hit
  iter()->reset_to_bci(start());
  while (iter()->next_bci() < limit_bci) {
    iter()->next();

    ciField *field;
    ciMethod *method;
    ciInstanceKlass *klass;
    bool will_link;
    bool is_field;

    switch (bc()) {
    case Bytecodes::_ldc:
    case Bytecodes::_ldc_w:
    case Bytecodes::_ldc2_w:
      if (!YuhuConstant::for_ldc(iter())->is_loaded()) {
        set_trap(
          Deoptimization::make_trap_request(
            Deoptimization::Reason_uninitialized,
            Deoptimization::Action_reinterpret), bci());
        return;
      }
      break;

    case Bytecodes::_getfield:
    case Bytecodes::_getstatic:
    case Bytecodes::_putfield:
    case Bytecodes::_putstatic:
      field = iter()->get_field(will_link);
      assert(will_link, "typeflow responsibility");
      is_field = (bc() == Bytecodes::_getfield || bc() == Bytecodes::_putfield);

      // If the bytecode does not match the field then bail out to
      // the interpreter to throw an IncompatibleClassChangeError
      if (is_field == field->is_static()) {
        set_trap(
          Deoptimization::make_trap_request(
            Deoptimization::Reason_unhandled,
            Deoptimization::Action_none), bci());
        return;
      }

      // Bail out if we are trying to access a static variable
      // before the class initializer has completed.
      if (!is_field && !field->holder()->is_initialized()) {
        if (!static_field_ok_in_clinit(field)) {
          set_trap(
            Deoptimization::make_trap_request(
              Deoptimization::Reason_uninitialized,
              Deoptimization::Action_reinterpret), bci());
          return;
        }
      }
      break;

    case Bytecodes::_invokestatic:
    case Bytecodes::_invokespecial:
    case Bytecodes::_invokevirtual:
    case Bytecodes::_invokeinterface:
      ciSignature* sig;
      method = iter()->get_method(will_link, &sig);
      assert(will_link, "typeflow responsibility");
      // We can't compile calls to method handle intrinsics, because we use
      // the interpreter entry points and they expect the top frame to be an
      // interpreter frame. We need to implement the intrinsics for Yuhu.
      if (method->is_method_handle_intrinsic() || method->is_compiled_lambda_form()) {
        if (YuhuPerformanceWarnings) {
          warning("JSR292 optimization not yet implemented in Yuhu");
        }
        set_trap(
          Deoptimization::make_trap_request(
            Deoptimization::Reason_unhandled,
            Deoptimization::Action_make_not_compilable), bci());
          return;
      }
      if (!method->holder()->is_linked()) {
        set_trap(
          Deoptimization::make_trap_request(
            Deoptimization::Reason_uninitialized,
            Deoptimization::Action_reinterpret), bci());
          return;
      }

      if (bc() == Bytecodes::_invokevirtual) {
        klass = ciEnv::get_instance_klass_for_declared_method_holder(
          iter()->get_declared_method_holder());
        if (!klass->is_linked()) {
          set_trap(
            Deoptimization::make_trap_request(
              Deoptimization::Reason_uninitialized,
              Deoptimization::Action_reinterpret), bci());
            return;
        }
      }
      break;

    case Bytecodes::_new:
      klass = iter()->get_klass(will_link)->as_instance_klass();
      assert(will_link, "typeflow responsibility");

      // Bail out if the class is unloaded
      if (iter()->is_unresolved_klass() || !klass->is_initialized()) {
        set_trap(
          Deoptimization::make_trap_request(
            Deoptimization::Reason_uninitialized,
            Deoptimization::Action_reinterpret), bci());
        return;
      }

      // Bail out if the class cannot be instantiated
      if (klass->is_abstract() || klass->is_interface() ||
          klass->name() == ciSymbol::java_lang_Class()) {
        set_trap(
          Deoptimization::make_trap_request(
            Deoptimization::Reason_unhandled,
            Deoptimization::Action_reinterpret), bci());
        return;
      }
      break;
    case Bytecodes::_invokedynamic:
    case Bytecodes::_invokehandle:
      if (YuhuPerformanceWarnings) {
        warning("JSR292 optimization not yet implemented in Yuhu");
      }
      set_trap(
        Deoptimization::make_trap_request(
          Deoptimization::Reason_unhandled,
          Deoptimization::Action_make_not_compilable), bci());
      return;
    }
  }

  // Trap if typeflow trapped (and we didn't before)
  if (ciblock()->has_trap()) {
    set_trap(
      Deoptimization::make_trap_request(
        Deoptimization::Reason_unloaded,
        Deoptimization::Action_reinterpret,
        ciblock()->trap_index()), ciblock()->trap_bci());
    return;
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
  tty->print_cr("=== Yuhu: add_incoming to block %d (bci=%d) ===", index(), start());
  tty->print_cr("  Predecessor block: %s", predecessor ? predecessor->getName().str().c_str() : "NULL");
  tty->print_cr("  Needs PHIs: %d", needs_phis());
  tty->flush();
  
  if (needs_phis()) {
    tty->print_cr("  Using YuhuPHIState::add_incoming");
    tty->flush();
    ((YuhuPHIState *) entry_state())->add_incoming(incoming_state);
  }
  else if (_entry_state == NULL) {
    tty->print_cr("  Setting entry_state directly (first incoming)");
    tty->flush();
    _entry_state = incoming_state;
  }
  else {
    tty->print_cr("  Verifying entry_state equality");
    tty->flush();
    assert(entry_state()->equal_to(incoming_state), "should be");
  }
  
  tty->print_cr("=== Yuhu: Finished add_incoming to block %d ===", index());
  tty->flush();
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

void YuhuTopLevelBlock::cache_after_Java_call(ciMethod *callee) {
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

    push(YuhuValue::create_generic(type, NULL, false));
  }
  YuhuJavaCallCacher(function(), callee).scan(current_state());
}

void YuhuTopLevelBlock::decache_for_VM_call() {
  YuhuVMCallDecacher(function(), bci()).scan(current_state());
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
  Value *exception_klass = builder()->CreateValueOfStructEntry(
    xstack(0)->jobject_value(),
    in_ByteSize(oopDesc::klass_offset_in_bytes()),
    YuhuType::klass_type(),
    "exception_klass");

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
    Value* callee = builder()->is_subtype_of();  // Get the is_subtype_of function
    builder()->CreateCondBr(
      builder()->CreateICmpNE(
        builder()->CreateCall(func_type, callee, args),
        LLVMValue::jbyte_constant(0)),
      handler_for_exception(i), not_subtype);

    builder()->SetInsertPoint(not_subtype);
  }
}

void YuhuTopLevelBlock::marshal_exception_slow(int num_options) {
  int *indexes = NEW_RESOURCE_ARRAY(int, num_options);
  for (int i = 0; i < num_options; i++)
    indexes[i] = exc_handler(i)->catch_klass_index();

  Value *index = call_vm(
    builder()->find_exception_handler(),
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

void YuhuTopLevelBlock::maybe_add_safepoint() {
  if (current_state()->has_safepointed())
    return;

  BasicBlock *orig_block = builder()->GetInsertBlock();
  YuhuState *orig_state = current_state()->copy();

  BasicBlock *do_safepoint = function()->CreateBlock("do_safepoint");
  BasicBlock *safepointed  = function()->CreateBlock("safepointed");

  Value *state = builder()->CreateLoad(
    YuhuType::jint_type(),  // Explicitly pass type for LLVM 20+
    builder()->CreateIntToPtr(
      LLVMValue::intptr_constant(
        (intptr_t) SafepointSynchronize::address_of_state()),
      PointerType::getUnqual(YuhuType::jint_type())),
    "state");

  builder()->CreateCondBr(
    builder()->CreateICmpEQ(
      state,
      LLVMValue::jint_constant(SafepointSynchronize::_synchronizing)),
    do_safepoint, safepointed);

  builder()->SetInsertPoint(do_safepoint);
  call_vm(builder()->safepoint(), EX_CHECK_FULL);
  BasicBlock *safepointed_block = builder()->GetInsertBlock();
  builder()->CreateBr(safepointed);

  builder()->SetInsertPoint(safepointed);
  current_state()->merge(orig_state, orig_block, safepointed_block);

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
  // LLVM 20+ uses opaque pointer types, reconstruct FunctionType from signature "Ti" -> "i"
  llvm::FunctionType* func_type = YuhuBuilder::make_ftype("Ti", "i");
  std::vector<Value*> args;
  args.push_back(thread());
  args.push_back(LLVMValue::jint_constant(trap_request));

  // Call uncommon_trap and then jump to unified exit block
  // Note: uncommon_trap may throw an exception and not return, but we need to handle the return case
  llvm::Value* call_result = builder()->CreateCall(func_type, builder()->uncommon_trap(), args);

  // CRITICAL: Jump to the unified exit block (contains epilogue marker and ret)
  // This ensures we only have ONE marker in the entire function
  builder()->CreateBr(function()->unified_exit_block());
}

void YuhuTopLevelBlock::call_register_finalizer(Value *receiver) {
  BasicBlock *orig_block = builder()->GetInsertBlock();
  YuhuState *orig_state = current_state()->copy();

  BasicBlock *do_call = function()->CreateBlock("has_finalizer");
  BasicBlock *done    = function()->CreateBlock("done");

  Value *klass = builder()->CreateValueOfStructEntry(
    receiver,
    in_ByteSize(oopDesc::klass_offset_in_bytes()),
    YuhuType::oop_type(),
    "klass");

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

  Value *result_addr = stack()->CreatePopFrame(type2size[type]);
  if (type != T_VOID) {
    builder()->CreateStore(
      pop_result(type)->generic_value(),
      builder()->CreateIntToPtr(
        result_addr,
        PointerType::getUnqual(YuhuType::to_stackType(type))));
  }

  // NOTE: We do NOT clear last_Java_sp here when returning from a Yuhu compiled method.
  //
  // The reason: last_Java_sp represents the "current Java frame" for stack traversal.
  // When a method returns, the caller becomes the "current frame", but the caller
  // should have already set last_Java_sp when it entered (if it's a compiled method)
  // or the interpreter maintains it (if it's an interpreted method).
  //
  // However, if we clear it here and the caller is another Yuhu compiled method that
  // hasn't set last_Java_sp yet (or if it's the top-level method), stack traversal
  // will fail because pd_last_frame() requires has_last_Java_frame() to be true.
  //
  // C2 clears last_Java_sp in its return stub, but C2 methods are typically called
  // from the interpreter or have special handling. For Yuhu, we keep last_Java_sp
  // set so that stack traversal can work correctly.
  //
  // If the caller is the interpreter, it will maintain last_Java_sp correctly.
  // If the caller is another compiled method, it should have set last_Java_sp when
  // it entered, so our value will be overwritten anyway.
  //
  // TODO: Re-evaluate this decision based on actual behavior and safepoint requirements.

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
      llvm::PointerType::getUnqual(YuhuType::oop_type()),
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

  // Debug: Print state info before add_incoming
  tty->print_cr("=== Yuhu: do_if_helper - adding incoming states ===");
  tty->print_cr("if_taken block: bci=%d, needs_phis=%d", 
                if_taken->start(), if_taken->needs_phis());
  tty->print_cr("not_taken block: bci=%d, needs_phis=%d", 
                not_taken->start(), not_taken->needs_phis());
  tty->flush();

  if_taken->add_incoming(if_taken_state);
  not_taken->add_incoming(not_taken_state);
  
  tty->print_cr("=== Yuhu: do_if_helper - finished adding incoming states ===");
  tty->flush();
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

Value *YuhuTopLevelBlock::get_direct_callee(ciMethod* method) {
  // Generate call to static call stub that returns the _from_compiled_entry directly
  // This stub loads the Method* and jumps to _from_compiled_entry, avoiding
  // the problematic field access in the generated LLVM IR
  // Pass both the target method and the current method being compiled
  address stub_addr = YuhuCompiler::compiler()->generate_static_call_stub(method, target());
  
  // Return the stub address as an integer constant
  // This will be used to access the compiled entry point
  return builder()->CreateIntToPtr(
    LLVMValue::intptr_constant((intptr_t)stub_addr),
    YuhuType::intptr_type(),
    "direct_callee_stub");
}

Value *YuhuTopLevelBlock::get_virtual_callee(YuhuValue* receiver,
                                              int vtable_index) {
  Value *klass = builder()->CreateValueOfStructEntry(
    receiver->jobject_value(),
    in_ByteSize(oopDesc::klass_offset_in_bytes()),
    YuhuType::oop_type(),
    "klass");

  return builder()->CreateLoad(
    YuhuType::Method_type(),  // Explicitly pass type for LLVM 20+
    builder()->CreateArrayAddress(
      klass,
      YuhuType::Method_type(),
      vtableEntry::size() * wordSize,
      in_ByteSize(InstanceKlass::vtable_start_offset() * wordSize),
      LLVMValue::intptr_constant(vtable_index)),
    "callee");
}

Value* YuhuTopLevelBlock::get_interface_callee(YuhuValue *receiver,
                                                ciMethod*   method) {
  BasicBlock *loop       = function()->CreateBlock("loop");
  BasicBlock *got_null   = function()->CreateBlock("got_null");
  BasicBlock *not_null   = function()->CreateBlock("not_null");
  BasicBlock *next       = function()->CreateBlock("next");
  BasicBlock *got_entry  = function()->CreateBlock("got_entry");

  // Locate the receiver's itable
  Value *object_klass = builder()->CreateValueOfStructEntry(
    receiver->jobject_value(), in_ByteSize(oopDesc::klass_offset_in_bytes()),
    YuhuType::klass_type(),
    "object_klass");

  Value *vtable_start = builder()->CreateAdd(
    builder()->CreatePtrToInt(object_klass, YuhuType::intptr_type()),
    LLVMValue::intptr_constant(
      InstanceKlass::vtable_start_offset() * HeapWordSize),
    "vtable_start");

  Value *vtable_length = builder()->CreateValueOfStructEntry(
    object_klass,
    in_ByteSize(InstanceKlass::vtable_length_offset() * HeapWordSize),
    YuhuType::jint_type(),
    "vtable_length");
  vtable_length =
    builder()->CreateIntCast(vtable_length, YuhuType::intptr_type(), false);

  bool needs_aligning = HeapWordsPerLong > 1;
  Value *itable_start = builder()->CreateAdd(
    vtable_start,
    builder()->CreateShl(
      vtable_length,
      LLVMValue::intptr_constant(exact_log2(vtableEntry::size() * wordSize))),
    needs_aligning ? "" : "itable_start");
  if (needs_aligning) {
    itable_start = builder()->CreateAnd(
      builder()->CreateAdd(
        itable_start, LLVMValue::intptr_constant(BytesPerLong - 1)),
      LLVMValue::intptr_constant(~(BytesPerLong - 1)),
      "itable_start");
  }

  // Locate this interface's entry in the table
  Value *iklass = builder()->CreateInlineMetadata(method->holder(), YuhuType::klass_type());
  BasicBlock *loop_entry = builder()->GetInsertBlock();
  builder()->CreateBr(loop);
  builder()->SetInsertPoint(loop);
  PHINode *itable_entry_addr = builder()->CreatePHI(
    YuhuType::intptr_type(), 0, "itable_entry_addr");
  itable_entry_addr->addIncoming(itable_start, loop_entry);

  Value *itable_entry = builder()->CreateIntToPtr(
    itable_entry_addr, YuhuType::itableOffsetEntry_type(), "itable_entry");

  Value *itable_iklass = builder()->CreateValueOfStructEntry(
    itable_entry,
    in_ByteSize(itableOffsetEntry::interface_offset_in_bytes()),
    YuhuType::klass_type(),
    "itable_iklass");

  builder()->CreateCondBr(
    builder()->CreateICmpEQ(itable_iklass, LLVMValue::nullKlass()),
    got_null, not_null);

  // A null entry means that the class doesn't implement the
  // interface, and wasn't the same as the class checked when
  // the interface was resolved.
  builder()->SetInsertPoint(got_null);
  builder()->CreateUnimplemented(__FILE__, __LINE__);
  builder()->CreateUnreachable();

  builder()->SetInsertPoint(not_null);
  builder()->CreateCondBr(
    builder()->CreateICmpEQ(itable_iklass, iklass),
    got_entry, next);

  builder()->SetInsertPoint(next);
  Value *next_entry = builder()->CreateAdd(
    itable_entry_addr,
    LLVMValue::intptr_constant(itableOffsetEntry::size() * wordSize));
  builder()->CreateBr(loop);
  itable_entry_addr->addIncoming(next_entry, next);

  // Locate the method pointer
  builder()->SetInsertPoint(got_entry);
  Value *offset = builder()->CreateValueOfStructEntry(
    itable_entry,
    in_ByteSize(itableOffsetEntry::offset_offset_in_bytes()),
    YuhuType::jint_type(),
    "offset");
  offset =
    builder()->CreateIntCast(offset, YuhuType::intptr_type(), false);

  return builder()->CreateLoad(
    YuhuType::Method_type(),  // Explicitly pass type for LLVM 20+
    builder()->CreateIntToPtr(
      builder()->CreateAdd(
        builder()->CreateAdd(
          builder()->CreateAdd(
            builder()->CreatePtrToInt(
              object_klass, YuhuType::intptr_type()),
            offset),
          LLVMValue::intptr_constant(
            method->itable_index() * itableMethodEntry::size() * wordSize)),
        LLVMValue::intptr_constant(
          itableMethodEntry::method_offset_in_bytes())),
      PointerType::getUnqual(YuhuType::Method_type())),
    "callee");
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
    if (YuhuInliner::attempt_inline(call_method, current_state())) {
      return;
    }
  }

  // Find the method we are calling
  Value *callee;
  if (call_is_virtual) {
    if (is_virtual || is_forced_virtual) {
      assert(klass->is_linked(), "scan_for_traps responsibility");
      int vtable_index = call_method->resolve_vtable_index(
        target()->holder(), klass);
      assert(vtable_index >= 0, "should be");
      callee = get_virtual_callee(receiver, vtable_index);
    }
    else {
      assert(is_interface, "should be");
      callee = get_interface_callee(receiver, call_method);
    }
  }
  else {
    // For direct calls (including optimized virtual calls), use get_direct_callee
    // which now returns the stub address that jumps to _from_compiled_entry
    callee = get_direct_callee(call_method);
  }

  Value *from_compiled_entry;
  if (call_is_virtual) {
    // For virtual/interface calls, load _from_compiled_entry from Method
    from_compiled_entry = builder()->CreateValueOfStructEntry(
      callee,
      Method::from_compiled_offset(),
      YuhuType::intptr_type(),
      "from_compiled_entry");
  } else {
    // For direct calls, callee is already the compiled entry address from stub
    from_compiled_entry = callee;
  }

  // IMPORTANT: Build argument list BEFORE decache_for_Java_call()!
  // decache will pop all arguments from the stack via xpop(),
  // so we must collect them first.
  std::vector<Value*> call_args;
  int arg_slots = call_method->arg_size();

  if (is_static) {
    // Static: first argument is NULL placeholder for receiver
    call_args.push_back(LLVMValue::intptr_constant(0));
    // Collect remaining Java arguments from stack in reverse order
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
    // Non-static: receiver is at position arg_size-1
    YuhuValue* recv_val = xstack(arg_slots - 1);
    // Explicit null check for method call receiver
    check_null(recv_val);
    call_args.push_back(recv_val->jobject_value());
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

  // Cast from_compiled_entry to a function pointer matching the callee's signature
  // We need to construct the callee's FunctionType based on its Java signature
  std::vector<llvm::Type*> param_types;
  
  // First parameter: receiver (for non-static) or NULL (for static)
  if (is_static) {
    param_types.push_back(YuhuType::intptr_type());  // void* null
  } else {
    param_types.push_back(YuhuType::oop_type());  // receiver
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

  Value *compiled_entry = builder()->CreateIntToPtr(
    from_compiled_entry,
    PointerType::getUnqual(compiled_ftype),
    "compiled_entry");

  // Call the compiled entry and get the actual return value
  // Note: decache_for_Java_call() was already called above (line 1566)
  Value* call_result = builder()->CreateCall(
    compiled_ftype, compiled_entry, call_args);

  // NOTE: Unlike Shark, we use the correct function return type instead of jint.
  // Shark uses a special entry point that returns jint (deoptimization count),
  // but Yuhu calls standard _from_compiled_entry which returns the actual method result.
  // Therefore, call_result contains the actual return value (e.g., object reference for array()).
  // Deoptimization is detected through check_pending_exception() below, not through the return value.
  // We ignore call_result here - cache_after_Java_call() will handle return values based on signature.

  // IMPORTANT: Create another OopMap at the return point
  // The decache_for_Java_call() above created an OopMap at the call site,
  // but we also need an OopMap at the return address because deoptimization
  // can happen when returning from the callee.
  // We use YuhuJavaCallDecacher again to create an OopMap at this return point.
  YuhuJavaCallDecacher(function(), bci(), call_method).scan(current_state());

  cache_after_Java_call(call_method);

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
  Value *object_klass = builder()->CreateValueOfStructEntry(
    object, in_ByteSize(oopDesc::klass_offset_in_bytes()),
    YuhuType::klass_type(),
    "object_klass");

  // Perform the check
  builder()->CreateCondBr(
    builder()->CreateICmpEQ(check_klass, object_klass),
    is_instance, subtype_check);

  builder()->SetInsertPoint(subtype_check);
  // LLVM 20+ requires FunctionType for CreateCall
  // LLVM 20+ uses opaque pointer types, reconstruct FunctionType from signature "KK" -> "c"
  Value* callee = builder()->is_subtype_of();
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
        old_top, YuhuType::oop_type(), "tlab_object");

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
      old_top, YuhuType::oop_type(), "heap_object");

    // LLVM 20+ requires alignment and success/failure ordering for CreateAtomicCmpXchg
#if LLVM_VERSION_MAJOR >= 20
    Value *check = builder()->CreateAtomicCmpXchg(
      top_addr, old_top, new_top,
      llvm::MaybeAlign(HeapWordSize),  // Alignment
      llvm::AtomicOrdering::SequentiallyConsistent,  // Success ordering
      llvm::AtomicOrdering::SequentiallyConsistent);  // Failure ordering
#else
    Value *check = builder()->CreateAtomicCmpXchg(top_addr, old_top, new_top, llvm::AtomicOrdering::SequentiallyConsistent);
#endif
    builder()->CreateCondBr(
      builder()->CreateICmpEQ(old_top, check),
      initialize, retry);

    // Initialize the object
    builder()->SetInsertPoint(initialize);
    if (tlab_object) {
      PHINode *phi = builder()->CreatePHI(
        YuhuType::oop_type(), 0, "fast_object");
      phi->addIncoming(tlab_object, got_tlab);
      phi->addIncoming(heap_object, got_heap);
      fast_object = phi;
    }
    else {
      fast_object = heap_object;
    }

    builder()->CreateMemset(
      builder()->CreateBitCast(
        fast_object, PointerType::getUnqual(YuhuType::jbyte_type())),
      LLVMValue::jbyte_constant(0),
      LLVMValue::jint_constant(size_in_bytes),
      LLVMValue::jint_constant(HeapWordSize));

    Value *mark_addr = builder()->CreateAddressOfStructEntry(
      fast_object, in_ByteSize(oopDesc::mark_offset_in_bytes()),
      PointerType::getUnqual(YuhuType::intptr_type()),
      "mark_addr");

    Value *klass_addr = builder()->CreateAddressOfStructEntry(
      fast_object, in_ByteSize(oopDesc::klass_offset_in_bytes()),
      PointerType::getUnqual(YuhuType::klass_type()),
      "klass_addr");

    // Set the mark
    intptr_t mark;
    if (UseBiasedLocking) {
      Unimplemented();
    }
    else {
      mark = (intptr_t) markOopDesc::prototype();
    }
    builder()->CreateStore(LLVMValue::intptr_constant(mark), mark_addr);

    // Set the class
    Value *rtklass = builder()->CreateInlineMetadata(klass, YuhuType::klass_type());
    builder()->CreateStore(rtklass, klass_addr);
    got_fast = builder()->GetInsertBlock();

    builder()->CreateBr(push_object);
    builder()->SetInsertPoint(slow_alloc_and_init);
    fast_state = current_state()->copy();
  }

  // The slow path
  call_vm(
    builder()->new_instance(),
    LLVMValue::jint_constant(iter()->get_klass_index()),
    EX_CHECK_FULL);
  slow_object = get_vm_result();
  got_slow = builder()->GetInsertBlock();

  // Push the object
  if (push_object) {
    builder()->CreateBr(push_object);
    builder()->SetInsertPoint(push_object);
  }
  if (fast_object) {
    PHINode *phi = builder()->CreatePHI(YuhuType::oop_type(), 0, "object");
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

  call_vm(
    builder()->anewarray(),
    LLVMValue::jint_constant(iter()->get_klass_index()),
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

  call_vm(
    builder()->multianewarray(),
    LLVMValue::jint_constant(iter()->get_klass_index()),
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

  Value *lockee = builder()->CreateLoad(YuhuType::oop_type(), monitor_object_addr);

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

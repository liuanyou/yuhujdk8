/*
 * Copyright (c) 1999, 2012, Oracle and/or its affiliates. All rights reserved.
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
#include "ci/ciType.hpp"
#include "ci/ciTypeFlow.hpp"
#include "memory/allocation.hpp"
#include "yuhu/yuhuBuilder.hpp"
#include "yuhu/yuhuCacheDecache.hpp"
#include "yuhu/yuhuState.hpp"
#include "yuhu/yuhuTopLevelBlock.hpp"
#include "yuhu/yuhuType.hpp"
#include "yuhu/yuhuValue.hpp"

using namespace llvm;

void YuhuState::initialize(const YuhuState *state) {
  _locals = NEW_RESOURCE_ARRAY(YuhuValue*, max_locals());
  _stack  = NEW_RESOURCE_ARRAY(YuhuValue*, max_stack());

  NOT_PRODUCT(memset(_locals, 23, max_locals() * sizeof(YuhuValue *)));
  NOT_PRODUCT(memset(_stack,  23, max_stack()  * sizeof(YuhuValue *)));
  _sp = _stack;

  if (state) {
    for (int i = 0; i < max_locals(); i++) {
      YuhuValue *value = state->local(i);
      if (value)
        value = value->clone();
      set_local(i, value);
    }

    for (int i = state->stack_depth() - 1; i >= 0; i--) {
      YuhuValue *value = state->stack(i);
      if (value)
        value = value->clone();
      push(value);
    }
  }

  set_num_monitors(state ? state->num_monitors() : 0);
}

bool YuhuState::equal_to(YuhuState *other) {
  if (target() != other->target())
    return false;

  if (method() != other->method())
    return false;

  if (oop_tmp() != other->oop_tmp())
    return false;

  if (max_locals() != other->max_locals())
    return false;

  if (stack_depth() != other->stack_depth())
    return false;

  if (num_monitors() != other->num_monitors())
    return false;

  if (has_safepointed() != other->has_safepointed())
    return false;

  // Local variables
  for (int i = 0; i < max_locals(); i++) {
    YuhuValue *value = local(i);
    YuhuValue *other_value = other->local(i);

    if (value == NULL) {
      if (other_value != NULL)
        return false;
    }
    else {
      if (other_value == NULL)
        return false;

      if (!value->equal_to(other_value))
        return false;
    }
  }

  // Expression stack
  for (int i = 0; i < stack_depth(); i++) {
    YuhuValue *value = stack(i);
    YuhuValue *other_value = other->stack(i);

    if (value == NULL) {
      if (other_value != NULL)
        return false;
    }
    else {
      if (other_value == NULL)
        return false;

      if (!value->equal_to(other_value))
        return false;
    }
  }

  return true;
}

void YuhuState::merge(YuhuState* other,
                       BasicBlock* other_block,
                       BasicBlock* this_block) {
  // Method
  Value *this_method = this->method();
  Value *other_method = other->method();
  if (this_method != other_method) {
    PHINode *phi = builder()->CreatePHI(YuhuType::Method_type(), 0, "method");
    phi->addIncoming(this_method, this_block);
    phi->addIncoming(other_method, other_block);
    set_method(phi);
  }

  // Temporary oop slot
  Value *this_oop_tmp = this->oop_tmp();
  Value *other_oop_tmp = other->oop_tmp();
  if (this_oop_tmp != other_oop_tmp) {
    assert(this_oop_tmp && other_oop_tmp, "can't merge NULL with non-NULL");
    PHINode *phi = builder()->CreatePHI(YuhuType::oop_type(), 0, "oop_tmp");
    phi->addIncoming(this_oop_tmp, this_block);
    phi->addIncoming(other_oop_tmp, other_block);
    set_oop_tmp(phi);
  }

  // Monitors
  assert(this->num_monitors() == other->num_monitors(), "should be");

  // Local variables
  assert(this->max_locals() == other->max_locals(), "should be");
  for (int i = 0; i < max_locals(); i++) {
    YuhuValue *this_value = this->local(i);
    YuhuValue *other_value = other->local(i);
    assert((this_value == NULL) == (other_value == NULL), "should be");
    if (this_value != NULL) {
      char name[18];
      snprintf(name, sizeof(name), "local_%d_", i);
      set_local(i, this_value->merge(
        builder(), other_value, other_block, this_block, name));
    }
  }

  // Expression stack
  assert(this->stack_depth() == other->stack_depth(), "should be");
  for (int i = 0; i < stack_depth(); i++) {
    YuhuValue *this_value = this->stack(i);
    YuhuValue *other_value = other->stack(i);
    assert((this_value == NULL) == (other_value == NULL), "should be");
    if (this_value != NULL) {
      char name[18];
      snprintf(name, sizeof(name), "stack_%d_", i);
      set_stack(i, this_value->merge(
        builder(), other_value, other_block, this_block, name));
    }
  }

  // Safepointed status
  set_has_safepointed(this->has_safepointed() && other->has_safepointed());
}

void YuhuState::replace_all(YuhuValue* old_value, YuhuValue* new_value) {
  // Local variables
  for (int i = 0; i < max_locals(); i++) {
    if (local(i) == old_value)
      set_local(i, new_value);
  }

  // Expression stack
  for (int i = 0; i < stack_depth(); i++) {
    if (stack(i) == old_value)
      set_stack(i, new_value);
  }
}

YuhuNormalEntryState::YuhuNormalEntryState(YuhuTopLevelBlock* block,
                                             Value*              method)
  : YuhuState(block) {
  assert(!block->stack_depth_at_entry(), "entry block shouldn't have stack");

  // Local variables
  for (int i = 0; i < max_locals(); i++) {
    ciType *type = block->local_type_at_entry(i);

    YuhuValue *value = NULL;
    switch (type->basic_type()) {
    case T_INT:
    case T_LONG:
    case T_FLOAT:
    case T_DOUBLE:
    case T_OBJECT:
    case T_ARRAY:
      if (i >= arg_size()) {
        ShouldNotReachHere();
      }
      value = YuhuValue::create_generic(type, NULL, i == 0 && !is_static());
      break;

    case ciTypeFlow::StateVector::T_NULL:
      value = YuhuValue::null();
      break;

    case ciTypeFlow::StateVector::T_BOTTOM:
      break;

    case ciTypeFlow::StateVector::T_LONG2:
    case ciTypeFlow::StateVector::T_DOUBLE2:
      break;

    default:
      ShouldNotReachHere();
    }
    set_local(i, value);
  }
  YuhuNormalEntryCacher(block->function(), method).scan(this);
}

YuhuOSREntryState::YuhuOSREntryState(YuhuTopLevelBlock* block,
                                       Value*              method,
                                       Value*              osr_buf)
  : YuhuState(block) {
  assert(block->stack_depth_at_entry() == 0, "entry block shouldn't have stack");
  set_num_monitors(block->ciblock()->monitor_count());

  // Local variables
  for (int i = 0; i < max_locals(); i++) {
    ciType *type = block->local_type_at_entry(i);

    YuhuValue *value = NULL;
    switch (type->basic_type()) {
    case T_INT:
    case T_LONG:
    case T_FLOAT:
    case T_DOUBLE:
    case T_OBJECT:
    case T_ARRAY:
      value = YuhuValue::create_generic(type, NULL, false);
      break;

    case ciTypeFlow::StateVector::T_NULL:
      value = YuhuValue::null();
      break;

    case ciTypeFlow::StateVector::T_BOTTOM:
      break;

    case ciTypeFlow::StateVector::T_LONG2:
    case ciTypeFlow::StateVector::T_DOUBLE2:
      break;

    default:
      ShouldNotReachHere();
    }
    set_local(i, value);
  }
  YuhuOSREntryCacher(block->function(), method, osr_buf).scan(this);
}

YuhuPHIState::YuhuPHIState(YuhuTopLevelBlock* block)
  : YuhuState(block), _block(block) {
  BasicBlock *saved_insert_point = builder()->GetInsertBlock();
  builder()->SetInsertPoint(block->entry_block());
  char name[18];

  // Method
  set_method(builder()->CreatePHI(YuhuType::Method_type(), 0, "method"));

  // Local variables
  for (int i = 0; i < max_locals(); i++) {
    ciType *type = block->local_type_at_entry(i);
    if (type->basic_type() == (BasicType) ciTypeFlow::StateVector::T_NULL) {
      // XXX we could do all kinds of clever stuff here
      type = ciType::make(T_OBJECT); // XXX what about T_ARRAY?
    }

    YuhuValue *value = NULL;
    switch (type->basic_type()) {
    case T_INT:
    case T_LONG:
    case T_FLOAT:
    case T_DOUBLE:
    case T_OBJECT:
    case T_ARRAY:
      snprintf(name, sizeof(name), "local_%d_", i);
      value = YuhuValue::create_phi(
        type, builder()->CreatePHI(YuhuType::to_stackType(type), 0, name));
      break;

    case T_ADDRESS:
      value = YuhuValue::address_constant(type->as_return_address()->bci());
      break;

    case ciTypeFlow::StateVector::T_BOTTOM:
      break;

    case ciTypeFlow::StateVector::T_LONG2:
    case ciTypeFlow::StateVector::T_DOUBLE2:
      break;

    default:
      ShouldNotReachHere();
    }
    set_local(i, value);
  }

  // Expression stack
  for (int i = 0; i < block->stack_depth_at_entry(); i++) {
    ciType *type = block->stack_type_at_entry(i);
    if (type->basic_type() == (BasicType) ciTypeFlow::StateVector::T_NULL) {
      // XXX we could do all kinds of clever stuff here
      type = ciType::make(T_OBJECT); // XXX what about T_ARRAY?
    }

    YuhuValue *value = NULL;
    switch (type->basic_type()) {
    case T_INT:
    case T_LONG:
    case T_FLOAT:
    case T_DOUBLE:
    case T_OBJECT:
    case T_ARRAY:
      snprintf(name, sizeof(name), "stack_%d_", i);
      value = YuhuValue::create_phi(
        type, builder()->CreatePHI(YuhuType::to_stackType(type), 0, name));
      break;

    case T_ADDRESS:
      value = YuhuValue::address_constant(type->as_return_address()->bci());
      break;

    case ciTypeFlow::StateVector::T_LONG2:
    case ciTypeFlow::StateVector::T_DOUBLE2:
      break;

    default:
      ShouldNotReachHere();
    }
    push(value);
  }

  // Monitors
  set_num_monitors(block->ciblock()->monitor_count());

  builder()->SetInsertPoint(saved_insert_point);
}

void YuhuPHIState::add_incoming(YuhuState* incoming_state) {
  BasicBlock *predecessor = builder()->GetInsertBlock();

  // Method
  ((PHINode *) method())->addIncoming(incoming_state->method(), predecessor);

  // Local variables
  for (int i = 0; i < max_locals(); i++) {
    if (local(i) != NULL)
      local(i)->addIncoming(incoming_state->local(i), predecessor);
  }

  // Expression stack
  int stack_depth = block()->stack_depth_at_entry();
  assert(stack_depth == incoming_state->stack_depth(), "should be");
  for (int i = 0; i < stack_depth; i++) {
    assert((stack(i) == NULL) == (incoming_state->stack(i) == NULL), "oops");
    if (stack(i))
      stack(i)->addIncoming(incoming_state->stack(i), predecessor);
  }

  // Monitors
  assert(num_monitors() == incoming_state->num_monitors(), "should be");

  // Temporary oop slot
  assert(oop_tmp() == incoming_state->oop_tmp(), "should be");
}

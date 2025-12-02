/*
 * Copyright (c) 1999, 2010, Oracle and/or its affiliates. All rights reserved.
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
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/llvmValue.hpp"
#include "yuhu/yuhuBuilder.hpp"
#include "yuhu/yuhuValue.hpp"

using namespace llvm;

// Cloning

YuhuValue* YuhuNormalValue::clone() const {
  return YuhuValue::create_generic(type(), generic_value(), zero_checked());
}
YuhuValue* YuhuPHIValue::clone() const {
  return YuhuValue::create_phi(type(), (PHINode *) generic_value(), this);
}
YuhuValue* YuhuAddressValue::clone() const {
  return YuhuValue::address_constant(address_value());
}

// Casting

bool YuhuValue::is_phi() const {
  return false;
}
bool YuhuPHIValue::is_phi() const {
  return true;
}
YuhuPHIValue* YuhuValue::as_phi() {
  ShouldNotCallThis();
  return NULL;  // Should never reach here
}
YuhuPHIValue* YuhuPHIValue::as_phi() {
  return this;
}

// Comparison

bool YuhuNormalValue::equal_to(YuhuValue *other) const {
  return (this->type()          == other->type() &&
          this->generic_value() == other->generic_value() &&
          this->zero_checked()  == other->zero_checked());
}
bool YuhuAddressValue::equal_to(YuhuValue *other) const {
  return (this->address_value() == other->address_value());
}

// Type access

ciType* YuhuValue::type() const {
  ShouldNotCallThis();
  return NULL;  // Should never reach here
}
ciType* YuhuNormalValue::type() const {
  return _type;
}

BasicType YuhuNormalValue::basic_type() const {
  return type()->basic_type();
}
BasicType YuhuAddressValue::basic_type() const {
  return T_ADDRESS;
}

int YuhuNormalValue::size() const {
  return type()->size();
}
int YuhuAddressValue::size() const {
  return 1;
}

bool YuhuValue::is_jint() const {
  return false;
}
bool YuhuValue::is_jlong() const {
  return false;
}
bool YuhuValue::is_jfloat() const {
  return false;
}
bool YuhuValue::is_jdouble() const {
  return false;
}
bool YuhuValue::is_jobject() const {
  return false;
}
bool YuhuValue::is_jarray() const {
  return false;
}
bool YuhuValue::is_address() const {
  return false;
}

bool YuhuNormalValue::is_jint() const {
  return llvm_value()->getType() == YuhuType::jint_type();
}
bool YuhuNormalValue::is_jlong() const {
  return llvm_value()->getType() == YuhuType::jlong_type();
}
bool YuhuNormalValue::is_jfloat() const {
  return llvm_value()->getType() == YuhuType::jfloat_type();
}
bool YuhuNormalValue::is_jdouble() const {
  return llvm_value()->getType() == YuhuType::jdouble_type();
}
bool YuhuNormalValue::is_jobject() const {
  return llvm_value()->getType() == YuhuType::oop_type();
}
bool YuhuNormalValue::is_jarray() const {
  return basic_type() == T_ARRAY;
}
bool YuhuAddressValue::is_address() const {
  return true;
}

// Typed conversions from YuhuValues

Value* YuhuValue::jint_value() const {
  ShouldNotCallThis();
  return NULL;  // Should never reach here
}
Value* YuhuValue::jlong_value() const {
  ShouldNotCallThis();
  return NULL;  // Should never reach here
}
Value* YuhuValue::jfloat_value() const {
  ShouldNotCallThis();
  return NULL;  // Should never reach here
}
Value* YuhuValue::jdouble_value() const {
  ShouldNotCallThis();
  return NULL;  // Should never reach here
}
Value* YuhuValue::jobject_value() const {
  ShouldNotCallThis();
  return NULL;  // Should never reach here
}
Value* YuhuValue::jarray_value() const {
  ShouldNotCallThis();
  return NULL;  // Should never reach here
}
int YuhuValue::address_value() const {
  ShouldNotCallThis();
  return 0;  // Should never reach here
}

Value* YuhuNormalValue::jint_value() const {
  assert(is_jint(), "should be");
  return llvm_value();
}
Value* YuhuNormalValue::jlong_value() const {
  assert(is_jlong(), "should be");
  return llvm_value();
}
Value* YuhuNormalValue::jfloat_value() const {
  assert(is_jfloat(), "should be");
  return llvm_value();
}
Value* YuhuNormalValue::jdouble_value() const {
  assert(is_jdouble(), "should be");
  return llvm_value();
}
Value* YuhuNormalValue::jobject_value() const {
  assert(is_jobject(), "should be");
  return llvm_value();
}
Value* YuhuNormalValue::jarray_value() const {
  // XXX assert(is_jarray(), "should be");
  // XXX http://icedtea.classpath.org/bugzilla/show_bug.cgi?id=324
  assert(is_jobject(), "should be");
  return llvm_value();
}
int YuhuAddressValue::address_value() const {
  return _bci;
}

// Type-losing conversions -- use with care!

Value* YuhuNormalValue::generic_value() const {
  return llvm_value();
}
Value* YuhuAddressValue::generic_value() const {
  return LLVMValue::intptr_constant(address_value());
}

Value* YuhuValue::intptr_value(YuhuBuilder* builder) const {
  ShouldNotCallThis();
  return NULL;  // Should never reach here
}
Value* YuhuNormalValue::intptr_value(YuhuBuilder* builder) const {
  return builder->CreatePtrToInt(jobject_value(), YuhuType::intptr_type());
}

// Phi-style stuff for YuhuPHIState::add_incoming

void YuhuValue::addIncoming(YuhuValue *value, BasicBlock* block) {
  ShouldNotCallThis();
}
void YuhuPHIValue::addIncoming(YuhuValue *value, BasicBlock* block) {
  assert(!is_clone(), "shouldn't be");
  ((llvm::PHINode *) generic_value())->addIncoming(
      value->generic_value(), block);
  if (!value->zero_checked())
    _all_incomers_zero_checked = false;
}
void YuhuAddressValue::addIncoming(YuhuValue *value, BasicBlock* block) {
  assert(this->equal_to(value), "should be");
}

// Phi-style stuff for YuhuState::merge

YuhuValue* YuhuNormalValue::merge(YuhuBuilder* builder,
                                    YuhuValue*   other,
                                    BasicBlock*   other_block,
                                    BasicBlock*   this_block,
                                    const char*   name) {
  assert(type() == other->type(), "should be");
  assert(zero_checked() == other->zero_checked(), "should be");

  PHINode *phi = builder->CreatePHI(YuhuType::to_stackType(type()), 0, name);
  phi->addIncoming(this->generic_value(), this_block);
  phi->addIncoming(other->generic_value(), other_block);
  return YuhuValue::create_generic(type(), phi, zero_checked());
}
YuhuValue* YuhuAddressValue::merge(YuhuBuilder* builder,
                                     YuhuValue*   other,
                                     BasicBlock*   other_block,
                                     BasicBlock*   this_block,
                                     const char*   name) {
  assert(this->equal_to(other), "should be");
  return this;
}

// Repeated null and divide-by-zero check removal

bool YuhuValue::zero_checked() const {
  ShouldNotCallThis();
  return false;  // Should never reach here
}
void YuhuValue::set_zero_checked(bool zero_checked) {
  ShouldNotCallThis();
}

bool YuhuNormalValue::zero_checked() const {
  return _zero_checked;
}
void YuhuNormalValue::set_zero_checked(bool zero_checked) {
  _zero_checked = zero_checked;
}

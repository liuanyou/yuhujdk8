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

#ifndef SHARE_VM_YUHU_YUHUVALUE_HPP
#define SHARE_VM_YUHU_YUHUVALUE_HPP

#include "ci/ciType.hpp"
#include "memory/allocation.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/llvmValue.hpp"
#include "yuhu/yuhuType.hpp"

// Items on the stack and in local variables are tracked using
// YuhuValue objects.
//
// All YuhuValues are one of two core types, YuhuNormalValue
// and YuhuAddressValue, but no code outside this file should
// ever refer to those directly.  The split is because of the
// way JSRs are handled: the typeflow pass expands them into
// multiple copies, so the return addresses pushed by jsr and
// popped by ret only exist at compile time.  Having separate
// classes for these allows us to check that our jsr handling
// is correct, via assertions.
//
// There is one more type, YuhuPHIValue, which is a subclass
// of YuhuNormalValue with a couple of extra methods.  Use of
// YuhuPHIValue outside of this file is acceptable, so long
// as it is obtained via YuhuValue::as_phi().

class YuhuBuilder;
class YuhuPHIValue;

class YuhuValue : public ResourceObj {
 protected:
  YuhuValue() {}

  // Cloning
 public:
  virtual YuhuValue* clone() const = 0;

  // Casting
 public:
  virtual bool           is_phi() const;
  virtual YuhuPHIValue* as_phi();

  // Comparison
 public:
  virtual bool equal_to(YuhuValue* other) const = 0;

  // Type access
 public:
  virtual BasicType basic_type() const = 0;
  virtual ciType*   type()       const;

  virtual bool is_jint()    const;
  virtual bool is_jlong()   const;
  virtual bool is_jfloat()  const;
  virtual bool is_jdouble() const;
  virtual bool is_jobject() const;
  virtual bool is_jarray()  const;
  virtual bool is_address() const;

  virtual int size() const = 0;

  bool is_one_word() const {
    return size() == 1;
  }
  bool is_two_word() const {
    return size() == 2;
  }

  // Typed conversion from YuhuValues
 public:
  virtual llvm::Value* jint_value()    const;
  virtual llvm::Value* jlong_value()   const;
  virtual llvm::Value* jfloat_value()  const;
  virtual llvm::Value* jdouble_value() const;
  virtual llvm::Value* jobject_value() const;
  virtual llvm::Value* jarray_value()  const;
  virtual int          address_value() const;

  // Typed conversion to YuhuValues
 public:
  static YuhuValue* create_jint(llvm::Value* value, bool zero_checked) {
    assert(value->getType() == YuhuType::jint_type(), "should be");
    return create_generic(ciType::make(T_INT), value, zero_checked);
  }
  static YuhuValue* create_jlong(llvm::Value* value, bool zero_checked) {
    assert(value->getType() == YuhuType::jlong_type(), "should be");
    return create_generic(ciType::make(T_LONG), value, zero_checked);
  }
  static YuhuValue* create_jfloat(llvm::Value* value) {
    assert(value->getType() == YuhuType::jfloat_type(), "should be");
    return create_generic(ciType::make(T_FLOAT), value, false);
  }
  static YuhuValue* create_jdouble(llvm::Value* value) {
    assert(value->getType() == YuhuType::jdouble_type(), "should be");
    return create_generic(ciType::make(T_DOUBLE), value, false);
  }
  static YuhuValue* create_jobject(llvm::Value* value, bool zero_checked) {
    assert(value->getType() == YuhuType::oop_type(), "should be");
    return create_generic(ciType::make(T_OBJECT), value, zero_checked);
  }

  // Typed conversion from constants of various types
 public:
  static YuhuValue* jint_constant(jint value) {
    return create_jint(LLVMValue::jint_constant(value), value != 0);
  }
  static YuhuValue* jlong_constant(jlong value) {
    return create_jlong(LLVMValue::jlong_constant(value), value != 0);
  }
  static YuhuValue* jfloat_constant(jfloat value) {
    return create_jfloat(LLVMValue::jfloat_constant(value));
  }
  static YuhuValue* jdouble_constant(jdouble value) {
    return create_jdouble(LLVMValue::jdouble_constant(value));
  }
  static YuhuValue* null() {
    return create_jobject(LLVMValue::null(), false);
  }
  static inline YuhuValue* address_constant(int bci);

  // Type-losing conversions -- use with care!
 public:
  virtual llvm::Value* generic_value() const = 0;
  virtual llvm::Value* intptr_value(YuhuBuilder* builder) const;

  static inline YuhuValue* create_generic(ciType*      type,
                                           llvm::Value* value,
                                           bool         zero_checked);
  static inline YuhuValue* create_phi(ciType*              type,
                                       llvm::PHINode*       phi,
                                       const YuhuPHIValue* parent = NULL);

  // Phi-style stuff
 public:
  virtual void addIncoming(YuhuValue* value, llvm::BasicBlock* block);
  virtual YuhuValue* merge(YuhuBuilder*     builder,
                            YuhuValue*       other,
                            llvm::BasicBlock* other_block,
                            llvm::BasicBlock* this_block,
                            const char*       name) = 0;

  // Repeated null and divide-by-zero check removal
 public:
  virtual bool zero_checked() const;
  virtual void set_zero_checked(bool zero_checked);
};

class YuhuNormalValue : public YuhuValue {
  friend class YuhuValue;

 protected:
  YuhuNormalValue(ciType* type, llvm::Value* value, bool zero_checked)
    : _type(type), _llvm_value(value), _zero_checked(zero_checked) {}

 private:
  ciType*      _type;
  llvm::Value* _llvm_value;
  bool         _zero_checked;

 private:
  llvm::Value* llvm_value() const {
    return _llvm_value;
  }

  // Cloning
 public:
  YuhuValue* clone() const;

  // Comparison
 public:
  bool equal_to(YuhuValue* other) const;

  // Type access
 public:
  ciType*   type()       const;
  BasicType basic_type() const;
  int       size()       const;

 public:
  bool is_jint()    const;
  bool is_jlong()   const;
  bool is_jfloat()  const;
  bool is_jdouble() const;
  bool is_jobject() const;
  bool is_jarray()  const;

  // Typed conversions to LLVM values
 public:
  llvm::Value* jint_value()    const;
  llvm::Value* jlong_value()   const;
  llvm::Value* jfloat_value()  const;
  llvm::Value* jdouble_value() const;
  llvm::Value* jobject_value() const;
  llvm::Value* jarray_value()  const;

  // Type-losing conversions, use with care
 public:
  llvm::Value* generic_value() const;
  llvm::Value* intptr_value(YuhuBuilder* builder) const;

  // Phi-style stuff
 public:
  YuhuValue* merge(YuhuBuilder*     builder,
                    YuhuValue*       other,
                    llvm::BasicBlock* other_block,
                    llvm::BasicBlock* this_block,
                    const char*       name);

  // Repeated null and divide-by-zero check removal
 public:
  bool zero_checked() const;
  void set_zero_checked(bool zero_checked);
};

class YuhuPHIValue : public YuhuNormalValue {
  friend class YuhuValue;

 protected:
  YuhuPHIValue(ciType* type, llvm::PHINode* phi, const YuhuPHIValue *parent)
    : YuhuNormalValue(type, phi, parent && parent->zero_checked()),
      _parent(parent),
      _all_incomers_zero_checked(true) {}

 private:
  const YuhuPHIValue* _parent;
  bool                 _all_incomers_zero_checked;

 private:
  const YuhuPHIValue* parent() const {
    return _parent;
  }
  bool is_clone() const {
    return parent() != NULL;
  }

 public:
  bool all_incomers_zero_checked() const {
    if (is_clone())
      return parent()->all_incomers_zero_checked();

    return _all_incomers_zero_checked;
  }

  // Cloning
 public:
  YuhuValue* clone() const;

  // Casting
 public:
  bool           is_phi() const;
  YuhuPHIValue* as_phi();

  // Phi-style stuff
 public:
  void addIncoming(YuhuValue *value, llvm::BasicBlock* block);
};

class YuhuAddressValue : public YuhuValue {
  friend class YuhuValue;

 protected:
  YuhuAddressValue(int bci)
    : _bci(bci) {}

 private:
  int _bci;

  // Cloning
 public:
  YuhuValue* clone() const;

  // Comparison
 public:
  bool equal_to(YuhuValue* other) const;

  // Type access
 public:
  BasicType basic_type() const;
  int       size()       const;
  bool      is_address() const;

  // Typed conversion from YuhuValues
 public:
  int address_value() const;

  // Type-losing conversion -- use with care!
 public:
  llvm::Value* generic_value() const;

  // Phi-style stuff
 public:
  void addIncoming(YuhuValue *value, llvm::BasicBlock* block);
  YuhuValue* merge(YuhuBuilder*     builder,
                    YuhuValue*       other,
                    llvm::BasicBlock* other_block,
                    llvm::BasicBlock* this_block,
                    const char*       name);
};

// YuhuValue methods that can't be declared above

inline YuhuValue* YuhuValue::create_generic(ciType*      type,
                                              llvm::Value* value,
                                              bool         zero_checked) {
  return new YuhuNormalValue(type, value, zero_checked);
}

inline YuhuValue* YuhuValue::create_phi(ciType*              type,
                                          llvm::PHINode*       phi,
                                          const YuhuPHIValue* parent) {
  return new YuhuPHIValue(type, phi, parent);
}

inline YuhuValue* YuhuValue::address_constant(int bci) {
  return new YuhuAddressValue(bci);
}

#endif // SHARE_VM_YUHU_YUHUVALUE_HPP

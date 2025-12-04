/*
 * Copyright (c) 1999, 2012, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2009, 2010 Red Hat, Inc.
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
#include "oops/arrayOop.hpp"
#include "oops/oop.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/yuhuContext.hpp"
#include "utilities/globalDefinitions.hpp"
#include "memory/allocation.hpp"

using namespace llvm;

YuhuContext::YuhuContext(const char* name)
  : LLVMContext(),
    _free_queue(NULL) {
  // Create a module to build our functions into
  _module = new Module(name, *this);
  
  // Set the target triple for AArch64
#ifdef TARGET_ARCH_aarch64
  _module->setTargetTriple("aarch64-apple-darwin");
#else
  // For other architectures, use the default or detect from host
  _module->setTargetTriple(llvm::sys::getDefaultTargetTriple());
#endif

  // Create basic types
  // Use llvm::Type explicitly to avoid conflict with HotSpot's Type class
  _void_type    = llvm::Type::getVoidTy(*this);
  _bit_type     = llvm::Type::getInt1Ty(*this);
  _jbyte_type   = llvm::Type::getInt8Ty(*this);
  _jshort_type  = llvm::Type::getInt16Ty(*this);
  _jint_type    = llvm::Type::getInt32Ty(*this);
  _jlong_type   = llvm::Type::getInt64Ty(*this);
  _jfloat_type  = llvm::Type::getFloatTy(*this);
  _jdouble_type = llvm::Type::getDoubleTy(*this);

  // Create compound types
  // Use llvm:: prefix to avoid conflict with HotSpot's ArrayType class
  _itableOffsetEntry_type = PointerType::getUnqual(
    llvm::ArrayType::get(jbyte_type(), itableOffsetEntry::size() * wordSize));

  // Use ::Metadata to refer to HotSpot's Metadata class, not llvm::Metadata
  _Metadata_type = PointerType::getUnqual(
    llvm::ArrayType::get(jbyte_type(), sizeof(::Metadata)));

  _klass_type = PointerType::getUnqual(
    llvm::ArrayType::get(jbyte_type(), sizeof(Klass)));

  _jniEnv_type = PointerType::getUnqual(
    llvm::ArrayType::get(jbyte_type(), sizeof(JNIEnv)));

  _jniHandleBlock_type = PointerType::getUnqual(
    llvm::ArrayType::get(jbyte_type(), sizeof(JNIHandleBlock)));

  _Method_type = PointerType::getUnqual(
    llvm::ArrayType::get(jbyte_type(), sizeof(Method)));

  _monitor_type = llvm::ArrayType::get(
    jbyte_type(), frame::interpreter_frame_monitor_size() * wordSize);

  _oop_type = PointerType::getUnqual(
    llvm::ArrayType::get(jbyte_type(), sizeof(oopDesc)));

  _thread_type = PointerType::getUnqual(
    llvm::ArrayType::get(jbyte_type(), sizeof(JavaThread)));

  // Note: AArch64 uses standard ABI stack, no ZeroStack needed
  // _zeroStack_type is not used for AArch64

  std::vector<llvm::Type*> params;
  params.push_back(Method_type());
  params.push_back(intptr_type());
  params.push_back(thread_type());
  _entry_point_type = FunctionType::get(jint_type(), params, false);

  params.clear();
  params.push_back(Method_type());
  params.push_back(PointerType::getUnqual(jbyte_type()));
  params.push_back(intptr_type());
  params.push_back(thread_type());
  _osr_entry_point_type = FunctionType::get(jint_type(), params, false);

  // Create mappings
  for (int i = 0; i < T_CONFLICT; i++) {
    switch (i) {
    case T_BOOLEAN:
      _to_stackType[i] = jint_type();
      _to_arrayType[i] = jbyte_type();
      break;

    case T_BYTE:
      _to_stackType[i] = jint_type();
      _to_arrayType[i] = jbyte_type();
      break;

    case T_CHAR:
      _to_stackType[i] = jint_type();
      _to_arrayType[i] = jshort_type();
      break;

    case T_SHORT:
      _to_stackType[i] = jint_type();
      _to_arrayType[i] = jshort_type();
      break;

    case T_INT:
      _to_stackType[i] = jint_type();
      _to_arrayType[i] = jint_type();
      break;

    case T_LONG:
      _to_stackType[i] = jlong_type();
      _to_arrayType[i] = jlong_type();
      break;

    case T_FLOAT:
      _to_stackType[i] = jfloat_type();
      _to_arrayType[i] = jfloat_type();
      break;

    case T_DOUBLE:
      _to_stackType[i] = jdouble_type();
      _to_arrayType[i] = jdouble_type();
      break;

    case T_OBJECT:
    case T_ARRAY:
      _to_stackType[i] = oop_type();
      _to_arrayType[i] = oop_type();
      break;

    case T_ADDRESS:
      _to_stackType[i] = intptr_type();
      _to_arrayType[i] = NULL;
      break;

    default:
      _to_stackType[i] = NULL;
      _to_arrayType[i] = NULL;
    }
  }
}

class YuhuFreeQueueItem : public CHeapObj<mtNone> {
 public:
  YuhuFreeQueueItem(llvm::Function* function, YuhuFreeQueueItem *next)
    : _function(function), _next(next) {}

 private:
  llvm::Function*     _function;
  YuhuFreeQueueItem* _next;

 public:
  llvm::Function* function() const {
    return _function;
  }
  YuhuFreeQueueItem* next() const {
    return _next;
  }
};

void YuhuContext::push_to_free_queue(Function* function) {
  _free_queue = new YuhuFreeQueueItem(function, _free_queue);
}

Function* YuhuContext::pop_from_free_queue() {
  if (_free_queue == NULL)
    return NULL;

  YuhuFreeQueueItem *item = _free_queue;
  Function *function = item->function();
  _free_queue = item->next();
  delete item;
  return function;
}

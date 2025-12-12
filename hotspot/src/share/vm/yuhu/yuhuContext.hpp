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

#ifndef SHARE_VM_YUHU_YUHUCONTEXT_HPP
#define SHARE_VM_YUHU_YUHUCONTEXT_HPP

#include "yuhu/llvmHeaders.hpp"
#include "yuhu/yuhuCompiler.hpp"

// The LLVMContext class allows multiple instances of LLVM to operate
// independently of each other in a multithreaded context.  We extend
// this here to store things in Yuhu that are LLVMContext-specific.

class YuhuFreeQueueItem;

class YuhuContext : public llvm::LLVMContext {
 public:
  YuhuContext(const char* name);

 private:
  llvm::Module* _module;

 public:
  llvm::Module* module() const {
    return _module;
  }

  // Get this thread's YuhuContext
 public:
  static YuhuContext& current() {
    return *YuhuCompiler::compiler()->context();
  }

  // Module accessors
 public:
  void add_function(llvm::Function* function) const {
    // Check if function is already in the module
    // If it is, don't add it again (to avoid issues with push_back)
    llvm::Module* mod = module();
    if (function->getParent() == mod) {
      // Function is already in the module, check if it's in the function list
      bool found = false;
      for (llvm::Module::iterator I = mod->begin(), E = mod->end(); I != E; ++I) {
        if (&*I == function) {
          found = true;
          break;
        }
      }
      if (!found) {
        // Function is in module but not in list, add it
        mod->getFunctionList().push_back(function);
      }
      // If found, do nothing - function is already in the list
    } else {
      // Function is not in this module, add it
      mod->getFunctionList().push_back(function);
    }
  }
  llvm::Constant* get_external(const char*               name,
                               llvm::FunctionType* sig) {
    // LLVM 9+ changed getOrInsertFunction() to return FunctionCallee instead of Constant*
#if LLVM_VERSION_MAJOR >= 9
    llvm::FunctionCallee callee = module()->getOrInsertFunction(name, sig);
    // getCallee() returns Value*, but we need Constant*
    // In LLVM, Function is a Constant, so we can cast it
    return llvm::cast<llvm::Constant>(callee.getCallee());
#else
    return module()->getOrInsertFunction(name, sig);
#endif
  }

  // Basic types
 private:
  llvm::Type*        _void_type;
  llvm::IntegerType* _bit_type;
  llvm::IntegerType* _jbyte_type;
  llvm::IntegerType* _jshort_type;
  llvm::IntegerType* _jint_type;
  llvm::IntegerType* _jlong_type;
  llvm::Type*        _jfloat_type;
  llvm::Type*        _jdouble_type;

 public:
  llvm::Type* void_type() const {
    return _void_type;
  }
  llvm::IntegerType* bit_type() const {
    return _bit_type;
  }
  llvm::IntegerType* jbyte_type() const {
    return _jbyte_type;
  }
  llvm::IntegerType* jshort_type() const {
    return _jshort_type;
  }
  llvm::IntegerType* jint_type() const {
    return _jint_type;
  }
  llvm::IntegerType* jlong_type() const {
    return _jlong_type;
  }
  llvm::Type* jfloat_type() const {
    return _jfloat_type;
  }
  llvm::Type* jdouble_type() const {
    return _jdouble_type;
  }
  llvm::IntegerType* intptr_type() const {
    return LP64_ONLY(jlong_type()) NOT_LP64(jint_type());
  }

  // Compound types
 private:
  llvm::PointerType*  _itableOffsetEntry_type;
  llvm::PointerType*  _jniEnv_type;
  llvm::PointerType*  _jniHandleBlock_type;
  llvm::PointerType*  _Metadata_type;
  llvm::PointerType*  _klass_type;
  llvm::PointerType*  _Method_type;
  llvm::ArrayType*    _monitor_type;
  llvm::PointerType*  _oop_type;
  llvm::PointerType*  _thread_type;
  // Note: AArch64 uses standard ABI stack, no ZeroStack needed
  // _zeroStack_type removed for AArch64
  llvm::FunctionType* _entry_point_type;
  llvm::FunctionType* _osr_entry_point_type;

 public:
  llvm::PointerType* itableOffsetEntry_type() const {
    return _itableOffsetEntry_type;
  }
  llvm::PointerType* jniEnv_type() const {
    return _jniEnv_type;
  }
  llvm::PointerType* jniHandleBlock_type() const {
    return _jniHandleBlock_type;
  }
  llvm::PointerType* Metadata_type() const {
    return _Metadata_type;
  }
  llvm::PointerType* klass_type() const {
    return _klass_type;
  }
  llvm::PointerType* Method_type() const {
    return _Method_type;
  }
  llvm::ArrayType* monitor_type() const {
    return _monitor_type;
  }
  llvm::PointerType* oop_type() const {
    return _oop_type;
  }
  llvm::PointerType* thread_type() const {
    return _thread_type;
  }
  // Note: zeroStack_type() removed for AArch64 - uses standard ABI stack
  llvm::FunctionType* entry_point_type() const {
    return _entry_point_type;
  }
  llvm::FunctionType* osr_entry_point_type() const {
    return _osr_entry_point_type;
  }

  // Mappings
 private:
  llvm::Type* _to_stackType[T_CONFLICT];
  llvm::Type* _to_arrayType[T_CONFLICT];

 private:
  llvm::Type* map_type(llvm::Type* const* table,
                             BasicType                type) const {
    assert(type >= 0 && type < T_CONFLICT, "unhandled type");
    llvm::Type* result = table[type];
    assert(result != NULL, "unhandled type");
    return result;
  }

 public:
  llvm::Type* to_stackType(BasicType type) const {
    return map_type(_to_stackType, type);
  }
  llvm::Type* to_arrayType(BasicType type) const {
    return map_type(_to_arrayType, type);
  }

  // Functions queued for freeing
 private:
  YuhuFreeQueueItem* _free_queue;

 public:
  void push_to_free_queue(llvm::Function* function);
  llvm::Function* pop_from_free_queue();
};

#endif // SHARE_VM_YUHU_YUHUCONTEXT_HPP

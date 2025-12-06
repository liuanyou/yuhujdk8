/*
 * Copyright (c) 1999, 2013, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2008, 2009, 2010, 2011 Red Hat, Inc.
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

#ifndef SHARE_VM_YUHU_YUHU_COMPILER_HPP
#define SHARE_VM_YUHU_YUHU_COMPILER_HPP

#include "ci/ciEnv.hpp"
#include "ci/ciMethod.hpp"
#include "compiler/abstractCompiler.hpp"
#include "compiler/compileBroker.hpp"
// Forward declarations to avoid including LLVM headers in header file
// This allows compileBroker.cpp (C++11) to include this header without C++17
// LLVM headers are only included in .cpp files which can use C++17
namespace llvm {
  class ExecutionEngine;
  class Function;
}
class YuhuContext;
class YuhuMemoryManager;
class YuhuEntry;

class YuhuCompiler : public AbstractCompiler {
 public:
  // Creation
  YuhuCompiler();

  // Name of this compiler
  const char *name()     { return "yuhu"; }

  // Compiler identification
  bool is_yuhu()         { return true; }
  bool is_c1()           { return false; }
  bool is_c2()           { return false; }
  bool is_shark()        { return false; }

  // Missing feature tests
  bool supports_native() { return true; }
  bool supports_osr()    { return true; }
  bool can_compile_method(methodHandle method)  {
    return ! (method->is_method_handle_intrinsic() || method->is_compiled_lambda_form());
  }

  // Initialization
  void initialize();

  // Compile a normal (bytecode) method and install it in the VM
  void compile_method(ciEnv* env, ciMethod* target, int entry_bci);

  // Generate a wrapper for a native (JNI) method
  nmethod* generate_native_wrapper(MacroAssembler* masm,
                                   methodHandle    target,
                                   int             compile_id,
                                   BasicType*      arg_types,
                                   BasicType       return_type);

  // Free compiled methods (and native wrappers)
  void free_compiled_method(address code);

  // Each thread generating IR needs its own context.  The normal
  // context is used for bytecode methods, and is protected from
  // multiple simultaneous accesses by being restricted to the
  // compiler thread.  The native context is used for JNI methods,
  // and is protected from multiple simultaneous accesses by the
  // adapter handler library lock.
 private:
  YuhuContext* _normal_context;
  YuhuContext* _native_context;

 public:
  YuhuContext* context() const {
    if (JavaThread::current()->is_Compiler_thread()) {
      return _normal_context;
    }
    else {
      assert(AdapterHandlerLibrary_lock->owned_by_self(), "should be");
      return _native_context;
    }
  }

  // The LLVM execution engine is the JIT we use to generate native
  // code.  It is thread safe, but we need to protect it with a lock
  // of our own because otherwise LLVM's lock and HotSpot's locks
  // interleave and deadlock.  The YuhuMemoryManager is not thread
  // safe, and is protected by the same lock as the execution engine.
 private:
  Monitor*               _execution_engine_lock;
  YuhuMemoryManager*    _memory_manager;
  llvm::ExecutionEngine* _execution_engine;

 private:
  Monitor* execution_engine_lock() const {
    return _execution_engine_lock;
  }
  YuhuMemoryManager* memory_manager() const {
    assert(execution_engine_lock()->owned_by_self(), "should be");
    return _memory_manager;
  }
  llvm::ExecutionEngine* execution_engine() const {
    assert(execution_engine_lock()->owned_by_self(), "should be");
    return _execution_engine;
  }

  // Global access
 public:
  static YuhuCompiler* compiler() {
    AbstractCompiler *compiler =
      CompileBroker::compiler(CompLevel_yuhu_optimized);
    assert(compiler != NULL, "Yuhu compiler should be initialized");
    assert(compiler->is_yuhu() && compiler->is_initialized(), "should be");
    return (YuhuCompiler *) compiler;
  }

  // Helpers
 private:
  static const char* methodname(const char* klass, const char* method);
  void generate_native_code(YuhuEntry*     entry,
                            llvm::Function* function,
                            const char*     name);
  void free_queued_methods();
};

#endif // SHARE_VM_YUHU_YUHUCOMPILER_HPP

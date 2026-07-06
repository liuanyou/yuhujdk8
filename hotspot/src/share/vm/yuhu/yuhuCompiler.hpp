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
#include <memory>  // For std::unique_ptr
// Forward declarations to avoid including LLVM headers in header file
// This allows compileBroker.cpp (C++11) to include this header without C++17
// LLVM headers are only included in .cpp files which can use C++17
namespace llvm {
  class ExecutionEngine;
  class Function;
  namespace orc {
    class LLJIT;
  }
}
class YuhuBuilder;
class YuhuContext;
class YuhuEntry;
class YuhuFunction;
class YuhuRuntime;

class YuhuCompiler : public AbstractCompiler {
 public:
  // Creation
  YuhuCompiler();
  // Destructor must be declared here but defined in .cpp file
  // because std::unique_ptr<llvm::orc::LLJIT> needs complete type for destruction
  ~YuhuCompiler();

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

  static bool contains_incomplete_state_analysis(ciTypeFlow* flow);

  // Compile a normal (bytecode) method and install it in the VM
  void compile_method(ciEnv* env, ciMethod* target, int entry_bci);

  static bool need_stack_bang(int frame_size_in_bytes);
  static int measure_normal_adapter_size(int frame_size_in_bytes);
  static int measure_exception_handler_size();
  static int measure_deopt_handler_size();
  static int measure_unwind_handler_size(int frame_size_in_bytes);

  int generate_normal_adapter_into(CodeBuffer& cb, address* verified_entry_point, int frame_size_in_bytes);
  // Exception and deoptimization handler generation
  int generate_exception_handler(CodeBuffer& cb, int handler_size);
  int generate_deopt_handler(CodeBuffer& cb, int handler_size);
  int generate_unwind_handler(CodeBuffer& cb, address code_start, int frame_size_in_bytes, size_t adapter_size, uint64_t unified_exit_block_start_pco);

  // Generate a wrapper for a native (JNI) method
  nmethod* generate_native_wrapper(MacroAssembler* masm,
                                   methodHandle    target,
                                   int             compile_id,
                                   BasicType*      arg_types,
                                   BasicType       return_type);

  // Free compiled methods (and native wrappers)
  void free_compiled_method(address code);

  // Track stubs that need patching for x28 restoration
 private:
    struct Impl;
    std::unique_ptr<Impl> _p_impl;

 public:
  YuhuContext* context() const;

 private:
//  Monitor* execution_engine_lock() const;
//  llvm::orc::LLJIT* jit() const;

  // Release last code blob without requiring lock (safe after nmethod installation)
  void release_last_code_blob_unlocked();

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
//  void generate_native_code(YuhuEntry*     entry,
//                            llvm::Function* function,
//                            const char*     name);
//  void free_queued_methods();
};

#endif // SHARE_VM_YUHU_YUHUCOMPILER_HPP

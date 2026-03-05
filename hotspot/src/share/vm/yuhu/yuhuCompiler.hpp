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
class YuhuContext;
class YuhuMemoryManager;
class YuhuEntry;

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

  // Compile a normal (bytecode) method and install it in the VM
  void compile_method(ciEnv* env, ciMethod* target, int entry_bci);

  static int measure_normal_adapter_size();
  static int measure_exception_handler_size();
  static int measure_deopt_handler_size();
  static int measure_unwind_handler_size(int frame_size_in_bytes);

  int generate_normal_adapter_into(CodeBuffer& cb, address llvm_entry);
  // Exception and deoptimization handler generation
  int generate_exception_handler(CodeBuffer& cb, int handler_size);
  int generate_deopt_handler(CodeBuffer& cb, int handler_size);
  int generate_unwind_handler(CodeBuffer& cb, int frame_size_in_bytes);

  // Generate a wrapper for a native (JNI) method
  nmethod* generate_native_wrapper(MacroAssembler* masm,
                                   methodHandle    target,
                                   int             compile_id,
                                   BasicType*      arg_types,
                                   BasicType       return_type);

  // Generate static call stub for direct method calls
  address generate_static_call_stub(ciMethod* target_method, ciMethod* current_method);

  // Patch the placeholder instruction in the static call stub with the correct x28 offset
  void patch_x28_restoration_stub(address stub_addr, int x28_offset_from_x29);

  // Free compiled methods (and native wrappers)
  void free_compiled_method(address code);

  // Track stubs that need patching for x28 restoration
 private:
  // Map from ciMethod to list of stub addresses that call it
  // Key: ciMethod* (the callee being called)
  // Value: GrowableArray of stub addresses that need patching when callee is compiled
  GrowableArray<ciMethod*>* _stub_patch_methods;
  GrowableArray<GrowableArray<address>*>* _stub_patch_addresses;
  
  // Add a stub that needs patching when the target method is compiled
  void register_stub_for_patching(ciMethod* target_method, address stub_addr);
  
  // Patch all stubs that call the given method
  void patch_stubs_for_method(ciMethod* target_method, int x28_offset);
  
 public:

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
  // interleave and deadlock.
  // Note: We use ORC JIT (LLVM 11+), which requires LLVM 11 or later.
  // LLVM 20 is recommended and fully supported.
 private:
  Monitor*               _execution_engine_lock;
  // ORC JIT (LLVM 11+) - recommended for LLVM 20
  std::unique_ptr<llvm::orc::LLJIT> _jit;
  // MemoryManager 由 ORC JIT 内部管理，或通过自定义 MemoryMapper
  // TODO: In stage 2, integrate CodeCache via MemoryMapper

 private:
  Monitor* execution_engine_lock() const {
    return _execution_engine_lock;
  }
  llvm::orc::LLJIT* jit() const {
    assert(execution_engine_lock()->owned_by_self(), "should be");
    return _jit.get();
  }
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
  void generate_native_code(YuhuEntry*     entry,
                            llvm::Function* function,
                            const char*     name);
  void free_queued_methods();
};

#endif // SHARE_VM_YUHU_YUHUCOMPILER_HPP

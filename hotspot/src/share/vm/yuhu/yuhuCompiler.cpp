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

#include "precompiled.hpp"
#include "ci/ciEnv.hpp"
#include "ci/ciMethod.hpp"
#include "code/debugInfoRec.hpp"
#include "code/dependencies.hpp"
#include "code/exceptionHandlerTable.hpp"
#include "code/oopRecorder.hpp"
#include "compiler/abstractCompiler.hpp"
#include "compiler/oopMap.hpp"
#include "yuhu/llvmHeaders.hpp"
#include <functional>  // For std::function used in unique_ptr deleter
#include "yuhu/yuhuBuilder.hpp"
#include "yuhu/yuhuCodeBuffer.hpp"
#include "yuhu/yuhuCompiler.hpp"
#include "yuhu/yuhuContext.hpp"
#include "yuhu/yuhuEntry.hpp"
#include "yuhu/yuhuFunction.hpp"
#include "yuhu/yuhuMemoryManager.hpp"
#include "yuhu/yuhuNativeWrapper.hpp"
#include "yuhu/yuhu_globals.hpp"
#include "utilities/debug.hpp"

#include <fnmatch.h>

using namespace llvm;

namespace {
  cl::opt<std::string>
  MCPU("mcpu");

  cl::list<std::string>
  MAttrs("mattr",
         cl::CommaSeparated);
}

YuhuCompiler::YuhuCompiler()
  : AbstractCompiler() {
  // Create the lock to protect the memory manager and execution engine
  // CRITICAL: This lock must have a rank higher than CompileThread_lock (nonleaf+5)
  // to avoid deadlock detection errors. The lock is acquired in generate_native_code()
  // which is called from compile_method() that runs in a compiler thread that may
  // already hold CompileThread_lock. Therefore, this lock must be acquired AFTER
  // CompileThread_lock in the lock order, meaning it needs a higher rank.
  _execution_engine_lock = new Monitor(Mutex::nonleaf + 6, "YuhuExecutionEngineLock");
  MutexLocker locker(execution_engine_lock());

  // Make LLVM safe for multithreading
  // LLVM 20+ removed llvm_start_multithreaded() - LLVM is always multithreaded
  // No need to call it anymore
#if LLVM_VERSION_MAJOR < 20
  if (!llvm_start_multithreaded())
    fatal("llvm_start_multithreaded() failed");
#endif

  // Initialize the native target (AArch64)
  // CRITICAL: Must initialize targets before creating ExecutionEngine
#ifdef TARGET_ARCH_aarch64
  // LLVM 20+ uses LLVMInitialize*Target() instead of Initialize*Target()
#if LLVM_VERSION_MAJOR >= 20
  // Explicitly initialize AArch64 target components
  LLVMInitializeAArch64Target();
  LLVMInitializeAArch64TargetInfo();
  LLVMInitializeAArch64TargetMC();
  LLVMInitializeAArch64AsmPrinter();  // Note: No "Target" in the middle
  LLVMInitializeAArch64AsmParser();
  LLVMInitializeAArch64Disassembler();
  
  // Also initialize native target (may be needed for some components)
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();
#else
  InitializeAArch64Target();
  InitializeAArch64TargetInfo();
  InitializeAArch64TargetMC();
  InitializeAArch64TargetAsmPrinter();
  InitializeAArch64AsmParser();
  InitializeAArch64Disassembler();
  
  // Also initialize native target
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();
#endif
  
  // CRITICAL: Force link MCJIT and Interpreter to ensure they are available
  // This is required when using static libraries or to ensure proper linking
  LLVMLinkInMCJIT();
  LLVMLinkInInterpreter();
#else
  InitializeNativeTarget();
  InitializeNativeTargetInfo();
  InitializeNativeTargetMC();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();
  InitializeNativeTargetDisassembler();
  
  // Force link MCJIT and Interpreter
  LLVMLinkInMCJIT();
  LLVMLinkInInterpreter();
#endif

  // Create the two contexts which we'll use
  _normal_context = new YuhuContext("normal");
  _native_context = new YuhuContext("native");

  // Create the memory manager
  _memory_manager = new YuhuMemoryManager();

  // Finetune LLVM for the current host CPU.
  // LLVM 15+ changed getHostCPUFeatures() signature: it now returns StringMap instead of taking a reference
#if LLVM_VERSION_MAJOR >= 15
  StringMap<bool> Features = llvm::sys::getHostCPUFeatures();
  bool gotCpuFeatures = !Features.empty();
  std::string cpu("-mcpu=" + llvm::sys::getHostCPUName().str());
#else
  StringMap<bool> Features;
  bool gotCpuFeatures = llvm::sys::getHostCPUFeatures(Features);
  std::string cpu("-mcpu=" + llvm::sys::getHostCPUName());
#endif

  std::vector<const char*> args;
  args.push_back(""); // program name
  args.push_back(cpu.c_str());

  std::string mattr("-mattr=");
  if(gotCpuFeatures){
    for(StringMap<bool>::iterator I = Features.begin(),
      E = Features.end(); I != E; ++I){
      if(I->second){
        std::string attr(I->first());
        mattr+="+"+attr+",";
      }
    }
    args.push_back(mattr.c_str());
  }

  args.push_back(0);  // terminator
  cl::ParseCommandLineOptions(args.size() - 1, (char **) &args[0]);

  // Create the JIT
  std::string ErrorMsg;

  // LLVM 20+ requires std::unique_ptr<Module> for EngineBuilder
  // EngineBuilder will take ownership of the Module, but YuhuContext still needs access
  // We'll create a new unique_ptr with default deleter, but we need to ensure
  // the Module is not deleted when the unique_ptr is destroyed
  // Note: ExecutionEngine will own the Module, so we can safely pass ownership
#if LLVM_VERSION_MAJOR >= 20
  // Create a unique_ptr with default deleter, but use a no-op deleter wrapper
  // We'll manually manage the lifetime to avoid double deletion
  llvm::Module* module = _normal_context->module();
  // Create a unique_ptr that will transfer ownership to EngineBuilder
  // The ExecutionEngine will own the Module, so we don't need to worry about deletion
  std::unique_ptr<llvm::Module> module_ptr(module);
  EngineBuilder builder(std::move(module_ptr));
  // Note: After this, ExecutionEngine owns the Module, but YuhuContext still has a pointer
  // This is safe because ExecutionEngine will keep the Module alive
#else
  EngineBuilder builder(_normal_context->module());
#endif
  
  // Note: Target triple is already set in YuhuContext when creating the Module
  // No need to set it again in EngineBuilder
  
  builder.setMCPU(MCPU);
  builder.setMAttrs(MAttrs);
  // LLVM 20+ uses setMCJITMemoryManager with std::unique_ptr
#if LLVM_VERSION_MAJOR >= 20
  // Create a unique_ptr with default deleter
  // ExecutionEngine will own the MemoryManager, so we can safely pass ownership
  RTDyldMemoryManager* mm = memory_manager();
  std::unique_ptr<RTDyldMemoryManager> mm_ptr(mm);
  builder.setMCJITMemoryManager(std::move(mm_ptr));
  // Note: After this, ExecutionEngine owns the MemoryManager
  // We still have the pointer in _memory_manager, but ExecutionEngine manages lifetime
#else
  builder.setJITMemoryManager(memory_manager());
#endif
  builder.setEngineKind(EngineKind::JIT);
  builder.setErrorStr(&ErrorMsg);
  if (! fnmatch(YuhuOptimizationLevel, "None", 0)) {
    tty->print_cr("Yuhu optimization level set to: None");
#if LLVM_VERSION_MAJOR >= 20
    builder.setOptLevel(llvm::CodeGenOptLevel::None);
#else
    builder.setOptLevel(llvm::CodeGenOpt::None);
#endif
  } else if (! fnmatch(YuhuOptimizationLevel, "Less", 0)) {
    tty->print_cr("Yuhu optimization level set to: Less");
#if LLVM_VERSION_MAJOR >= 20
    builder.setOptLevel(llvm::CodeGenOptLevel::Less);
#else
    builder.setOptLevel(llvm::CodeGenOpt::Less);
#endif
  } else if (! fnmatch(YuhuOptimizationLevel, "Aggressive", 0)) {
    tty->print_cr("Yuhu optimization level set to: Aggressive");
#if LLVM_VERSION_MAJOR >= 20
    builder.setOptLevel(llvm::CodeGenOptLevel::Aggressive);
#else
    builder.setOptLevel(llvm::CodeGenOpt::Aggressive);
#endif
  } // else Default is selected by, well, default :-)
  _execution_engine = builder.create();

  if (!execution_engine()) {
    if (!ErrorMsg.empty())
      printf("Error while creating Yuhu JIT: %s\n",ErrorMsg.c_str());
    else
      printf("Unknown error while creating Yuhu JIT\n");
    exit(1);
  }

  // LLVM 20+: Set DataLayout for modules from ExecutionEngine's TargetMachine
  // This is critical to avoid SIGSEGV in getABITypeAlign
#if LLVM_VERSION_MAJOR >= 20
  if (execution_engine()) {
    // Get the TargetMachine from ExecutionEngine
    llvm::TargetMachine* TM = execution_engine()->getTargetMachine();
    if (TM) {
      const llvm::DataLayout& DL = TM->createDataLayout();
      // Set DataLayout for normal context module
      // Note: ExecutionEngine owns the module now, but we can still set DataLayout
      // The module pointer in YuhuContext is still valid (ExecutionEngine keeps it alive)
      if (_normal_context->module()) {
        _normal_context->module()->setDataLayout(DL);
      }
      // Set DataLayout for native context module (will be added later)
      if (_native_context->module()) {
        _native_context->module()->setDataLayout(DL);
      }
    }
  }
#endif

  // LLVM 20+ requires std::unique_ptr<Module> for addModule
#if LLVM_VERSION_MAJOR >= 20
  std::unique_ptr<llvm::Module> native_module_ptr(_native_context->module());
  execution_engine()->addModule(std::move(native_module_ptr));
#else
  execution_engine()->addModule(_native_context->module());
#endif

  // All done
  set_state(initialized);
}

void YuhuCompiler::initialize() {
  ShouldNotCallThis();
}

void YuhuCompiler::compile_method(ciEnv*    env,
                                   ciMethod* target,
                                   int       entry_bci) {
  assert(is_initialized(), "should be");
  ResourceMark rm;
  const char *name = methodname(
    target->holder()->name()->as_utf8(), target->name()->as_utf8());

  // Do the typeflow analysis
  ciTypeFlow *flow;
  if (entry_bci == InvocationEntryBci)
    flow = target->get_flow_analysis();
  else
    flow = target->get_osr_flow_analysis(entry_bci);
  if (flow->failing())
    return;
  if (YuhuPrintTypeflowOf != NULL) {
    if (!fnmatch(YuhuPrintTypeflowOf, name, 0))
      flow->print_on(tty);
  }

  // Create the recorders
  Arena arena;
  env->set_oop_recorder(new OopRecorder(&arena));
  OopMapSet oopmaps;
  env->set_debug_info(new DebugInformationRecorder(env->oop_recorder()));
  env->debug_info()->set_oopmaps(&oopmaps);
  env->set_dependencies(new Dependencies(env));

  // Create the code buffer and builder
  CodeBuffer hscb("Yuhu", 256 * K, 64 * K);
  hscb.initialize_oop_recorder(env->oop_recorder());
  YuhuMacroAssembler masm(&hscb);
  YuhuCodeBuffer cb(masm);
  YuhuBuilder builder(&cb);

  // Emit the entry point
  YuhuEntry *entry = (YuhuEntry *) cb.malloc(sizeof(YuhuEntry));

  // Build the LLVM IR for the method
  Function *function = YuhuFunction::build(env, &builder, flow, name);
  if (env->failing()) {
    return;
  }

  // Generate native code.  It's unpleasant that we have to drop into
  // the VM to do this -- it blocks safepoints -- but I can't see any
  // other way to handle the locking.
  {
    ThreadInVMfromNative tiv(JavaThread::current());
    generate_native_code(entry, function, name);
  }

  // Install the method into the VM
  CodeOffsets offsets;
  offsets.set_value(CodeOffsets::Deopt, 0);
  offsets.set_value(CodeOffsets::Exceptions, 0);
  offsets.set_value(CodeOffsets::Verified_Entry,
                    target->is_static() ? 0 : wordSize);

  ExceptionHandlerTable handler_table;
  ImplicitExceptionTable inc_table;

  env->register_method(target,
                       entry_bci,
                       &offsets,
                       0,
                       &hscb,
                       0,
                       &oopmaps,
                       &handler_table,
                       &inc_table,
                       this,
                       env->comp_level(),
                       false,
                       false);
}

nmethod* YuhuCompiler::generate_native_wrapper(MacroAssembler* masm,
                                                methodHandle    target,
                                                int             compile_id,
                                                BasicType*      arg_types,
                                                BasicType       return_type) {
  assert(is_initialized(), "should be");
  ResourceMark rm;
  const char *name = methodname(
    target->klass_name()->as_utf8(), target->name()->as_utf8());

  // Create the code buffer and builder
  // Note: masm is MacroAssembler*, but YuhuCodeBuffer requires YuhuMacroAssembler&
  // Since YuhuMacroAssembler inherits from MacroAssembler, we can safely cast
  YuhuMacroAssembler* yuhu_masm = static_cast<YuhuMacroAssembler*>(masm);
  assert(yuhu_masm != NULL, "masm must be a YuhuMacroAssembler");
  YuhuCodeBuffer cb(*yuhu_masm);
  YuhuBuilder builder(&cb);

  // Emit the entry point
  YuhuEntry *entry = (YuhuEntry *) cb.malloc(sizeof(YuhuEntry));

  // Build the LLVM IR for the method
  YuhuNativeWrapper *wrapper = YuhuNativeWrapper::build(
    &builder, target, name, arg_types, return_type);

  // Generate native code
  generate_native_code(entry, wrapper->function(), name);

  // Return the nmethod for installation in the VM
  return nmethod::new_native_nmethod(target,
                                     compile_id,
                                     masm->code(),
                                     0,
                                     0,
                                     wrapper->frame_size(),
                                     wrapper->receiver_offset(),
                                     wrapper->lock_offset(),
                                     wrapper->oop_maps());
}

void YuhuCompiler::generate_native_code(YuhuEntry* entry,
                                         Function*   function,
                                         const char* name) {
  // Print the LLVM bitcode, if requested
  if (YuhuPrintBitcodeOf != NULL) {
    if (!fnmatch(YuhuPrintBitcodeOf, name, 0)) {
      // LLVM 20: dump() may not be available in all builds
      // Use print() with llvm::errs() instead
      // Note: dump() is a convenience method that calls print(errs())
      // If dump() is not available, we can use print() directly
      llvm::raw_ostream &OS = llvm::errs();
      function->print(OS);
      OS.flush();
    }
  }

  if (YuhuVerifyFunction != NULL) {
    if (!fnmatch(YuhuVerifyFunction, name, 0)) {
      verifyFunction(*function);
    }
  }

  // Compile to native code
  address code = NULL;
  context()->add_function(function);
  {
    MutexLocker locker(execution_engine_lock());
    free_queued_methods();

#ifndef NDEBUG
    if (YuhuPrintAsmOf != NULL) {
      if (!fnmatch(YuhuPrintAsmOf, name, 0)) {
        // LLVM 20+ uses lowercase setCurrentDebugType
        llvm::setCurrentDebugType(X86_ONLY("x86-emitter") NOT_X86("jit"));
        llvm::DebugFlag = true;
      }
      else {
        llvm::setCurrentDebugType("");
        llvm::DebugFlag = false;
      }
    }
#endif // !NDEBUG
    memory_manager()->set_entry_for_function(function, entry);
    code = (address) execution_engine()->getPointerToFunction(function);
  }
  assert(code != NULL, "code must be != NULL");
  entry->set_entry_point(code);
  entry->set_function(function);
  entry->set_context(context());
  address code_start = entry->code_start();
  address code_limit = entry->code_limit();

  // Register generated code for profiling, etc
  if (JvmtiExport::should_post_dynamic_code_generated())
    JvmtiExport::post_dynamic_code_generated(name, code_start, code_limit);

  // Print debug information, if requested
  if (YuhuTraceInstalls) {
    tty->print_cr(
      " [%p-%p): %s (%d bytes code)",
      code_start, code_limit, name, code_limit - code_start);
  }
}

void YuhuCompiler::free_compiled_method(address code) {
  // This method may only be called when the VM is at a safepoint.
  // All _thread_in_vm threads will be waiting for the safepoint to
  // finish with the exception of the VM thread, so we can consider
  // ourself the owner of the execution engine lock even though we
  // can't actually acquire it at this time.
  assert(Thread::current()->is_Compiler_thread(), "must be called by compiler thread");
  assert_locked_or_safepoint(CodeCache_lock);

  YuhuEntry *entry = (YuhuEntry *) code;
  entry->context()->push_to_free_queue(entry->function());
}

void YuhuCompiler::free_queued_methods() {
  // The free queue is protected by the execution engine lock
  assert(execution_engine_lock()->owned_by_self(), "should be");

  while (true) {
    Function *function = context()->pop_from_free_queue();
    if (function == NULL)
      break;

    // LLVM 20+ removed freeMachineCodeForFunction
    // The function will be freed when the module is removed
#if LLVM_VERSION_MAJOR < 20
    execution_engine()->freeMachineCodeForFunction(function);
#endif
    function->eraseFromParent();
  }
}

const char* YuhuCompiler::methodname(const char* klass, const char* method) {
  char *buf = NEW_RESOURCE_ARRAY(char, strlen(klass) + 2 + strlen(method) + 1);

  char *dst = buf;
  for (const char *c = klass; *c; c++) {
    if (*c == '/')
      *(dst++) = '.';
    else
      *(dst++) = *c;
  }
  *(dst++) = ':';
  *(dst++) = ':';
  for (const char *c = method; *c; c++) {
    *(dst++) = *c;
  }
  *(dst++) = '\0';
  return buf;
}

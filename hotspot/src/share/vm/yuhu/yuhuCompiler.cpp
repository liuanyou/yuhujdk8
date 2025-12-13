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
#include <cstring>

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

  // ORC JIT: MemoryManager is handled internally or via MemoryMapper
  // We'll create it later if needed for CodeCache integration (stage 2)
  // For now (stage 1), we use ORC JIT's default memory management

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

  // LLVM 20+: Set DataLayout for modules BEFORE creating ExecutionEngine
  // This is critical to avoid SIGSEGV in getABITypeAlign
  // We need to create a TargetMachine to get the DataLayout
#if LLVM_VERSION_MAJOR >= 20
  {
    // Get target triple from module
    std::string TripleStr = _normal_context->module()->getTargetTriple();
    if (TripleStr.empty()) {
      TripleStr = llvm::sys::getDefaultTargetTriple();
    }
    
    // Look up target
    std::string Error;
    const llvm::Target* Target = llvm::TargetRegistry::lookupTarget(TripleStr, Error);
    if (!Target) {
      fatal(err_msg("Failed to lookup target: %s", Error.c_str()));
    }
    
    // Create TargetMachine to get DataLayout
    llvm::TargetOptions Options;
    llvm::Reloc::Model RM = llvm::Reloc::Model::PIC_;
    // Extract mattr string (remove "-mattr=" prefix if present)
    std::string mattrStr;
    if (gotCpuFeatures && !mattr.empty() && mattr != "-mattr=") {
      mattrStr = mattr.substr(7);  // Remove "-mattr=" prefix
    }
    std::unique_ptr<llvm::TargetMachine> TM(
      Target->createTargetMachine(TripleStr, MCPU, mattrStr, Options, RM));
    
    if (TM) {
      // Get DataLayout string from TargetMachine
      const llvm::DataLayout& DL = TM->createDataLayout();
      std::string DLStr = DL.getStringRepresentation();
      
      // DEBUG: Print DataLayout string
      tty->print_cr("Yuhu: Setting DataLayout: %s", DLStr.c_str());
      
      // Set DataLayout for both modules BEFORE creating ExecutionEngine
      _normal_context->module()->setDataLayout(DLStr);
      _native_context->module()->setDataLayout(DLStr);
      
      // DEBUG: Verify DataLayout was set
      std::string verify1 = _normal_context->module()->getDataLayout().getStringRepresentation();
      std::string verify2 = _native_context->module()->getDataLayout().getStringRepresentation();
      tty->print_cr("Yuhu: Normal module DataLayout verified: %s", verify1.c_str());
      tty->print_cr("Yuhu: Native module DataLayout verified: %s", verify2.c_str());
      if (verify1 != DLStr || verify2 != DLStr) {
        fatal(err_msg("DataLayout mismatch! Expected: %s, Got normal: %s, Got native: %s", 
                      DLStr.c_str(), verify1.c_str(), verify2.c_str()));
      }
    }
  }
#endif

  // ========== ORC JIT 初始化 (LLVM 11+) ==========
  // Note: ORC JIT requires LLVM 11+. LLVM 20 is recommended.
  // Get target triple from module
  std::string TripleStr = _normal_context->module()->getTargetTriple();
  if (TripleStr.empty()) {
    TripleStr = llvm::sys::getDefaultTargetTriple();
  }
  
  // Create LLJIT with target machine builder
  auto JIT = llvm::orc::LLJITBuilder()
    .setJITTargetMachineBuilder(
      llvm::orc::JITTargetMachineBuilder(llvm::Triple(TripleStr))
        .setCPU(MCPU)
        .addFeatures(MAttrs))
    .create();
  
  if (!JIT) {
    std::string ErrMsg;
    llvm::handleAllErrors(JIT.takeError(), [&](const llvm::ErrorInfoBase &EIB) {
      ErrMsg = EIB.message();
    });
    fatal(err_msg("Failed to create LLJIT: %s", ErrMsg.c_str()));
  }
  
  _jit = std::move(*JIT);
  tty->print_cr("Yuhu: ORC JIT initialized successfully");
  
  // Add normal module to JITDylib
  // Note: ORC JIT uses ThreadSafeModule, which requires ThreadSafeContext
  // For now, we'll create a new ThreadSafeContext for each module
  // TODO: Consider reusing ThreadSafeContext if possible
  {
    auto TSCtx = std::make_unique<llvm::orc::ThreadSafeContext>(
      std::make_unique<llvm::LLVMContext>());
    
    // Create ThreadSafeModule from normal module
    // Note: We need to clone the module because ThreadSafeModule takes ownership
    std::unique_ptr<llvm::Module> normal_module_clone(
      llvm::CloneModule(*_normal_context->module()).release());
    auto TSM = llvm::orc::ThreadSafeModule(
      std::move(normal_module_clone), *TSCtx);
    
    auto &JD = _jit->getMainJITDylib();
    if (auto Err = _jit->addIRModule(JD, std::move(TSM))) {
      std::string ErrMsg;
      llvm::handleAllErrors(std::move(Err), [&](const llvm::ErrorInfoBase &EIB) {
        ErrMsg = EIB.message();
      });
      fatal(err_msg("Failed to add normal IR module: %s", ErrMsg.c_str()));
    }
  }
  
  // Add native module to JITDylib
  {
    auto TSCtx = std::make_unique<llvm::orc::ThreadSafeContext>(
      std::make_unique<llvm::LLVMContext>());
    
    std::unique_ptr<llvm::Module> native_module_clone(
      llvm::CloneModule(*_native_context->module()).release());
    auto TSM = llvm::orc::ThreadSafeModule(
      std::move(native_module_clone), *TSCtx);
    
    auto &JD = _jit->getMainJITDylib();
    if (auto Err = _jit->addIRModule(JD, std::move(TSM))) {
      std::string ErrMsg;
      llvm::handleAllErrors(std::move(Err), [&](const llvm::ErrorInfoBase &EIB) {
        ErrMsg = EIB.message();
      });
      fatal(err_msg("Failed to add native IR module: %s", ErrMsg.c_str()));
    }
  }

  // All done
  set_state(initialized);
}

YuhuCompiler::~YuhuCompiler() {
  // std::unique_ptr<llvm::orc::LLJIT> will automatically clean up
  // No explicit cleanup needed
}

void YuhuCompiler::initialize() {
  ShouldNotCallThis();
}

// Emit OSR adapter into the given CodeBuffer using YuhuMacroAssembler.
// Arguments expected by LLVM function: (Method*, osr_buf, base_pc, thread)
// Incoming from interpreter OSR jump: x0 = osr_buf
// If llvm_label is non-NULL, emit a branch to the label (to be patched later).
// Otherwise, emit an absolute jump to llvm_entry.
static int generate_osr_adapter_into(CodeBuffer& cb,
                                     Method* method,
                                     address base_pc,
                                     YuhuLabel* llvm_label,
                                     address llvm_entry) {
  YuhuMacroAssembler masm(&cb);
  address start = masm.current_pc();

  // Mark current PC for later use (avoid using lr).
  YuhuLabel adapter_label;
  masm.pin_label(adapter_label);
  masm.write_inst_adr(YuhuMacroAssembler::x16, adapter_label); // x16 = current PC

  // Save incoming osr_buf
  masm.write_inst_mov_reg(YuhuMacroAssembler::x17, YuhuMacroAssembler::x0); // x17 = osr_buf

  // Load Method* (literal)
  masm.write_insts_mov_imm64(YuhuMacroAssembler::x0, (uint64_t)method);

  // Restore osr_buf into x1
  masm.write_inst_mov_reg(YuhuMacroAssembler::x1, YuhuMacroAssembler::x17);

  // Load base_pc (literal)
  masm.write_insts_mov_imm64(YuhuMacroAssembler::x2, (uint64_t)base_pc);

  // Load thread into x3
  masm.write_insts_get_thread(YuhuMacroAssembler::x3);

  // Jump to LLVM entry
  if (llvm_label != NULL) {
    masm.write_inst_b(*llvm_label);   // Placeholder, patched when label is bound
  } else {
    masm.write_insts_mov_imm64(YuhuMacroAssembler::x16, (uint64_t)llvm_entry);
    masm.write_inst_br(YuhuMacroAssembler::x16);
  }

  address end = masm.current_pc();
  return (int)(end - start);
}

void YuhuCompiler::compile_method(ciEnv*    env,
                                   ciMethod* target,
                                   int       entry_bci) {
  assert(is_initialized(), "should be");
  ResourceMark rm;
  const char *base_name = methodname(
    target->holder()->name()->as_utf8(), target->name()->as_utf8());

  // Use base name directly for both OSR and normal entries.
  // ORC JIT uses JITDylib isolation, so we don't need to distinguish by function name.
  // Removing the ".osr.<entry_bci>" suffix that was added for MCJIT compatibility.
  const char *func_name = base_name;

  // Do the typeflow analysis
  ciTypeFlow *flow;
  if (entry_bci == InvocationEntryBci)
    flow = target->get_flow_analysis();
  else
    flow = target->get_osr_flow_analysis(entry_bci);
  if (flow->failing())
    return;
  if (YuhuPrintTypeflowOf != NULL) {
    if (!fnmatch(YuhuPrintTypeflowOf, base_name, 0))
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

  // Remove any existing functions with the same name in both modules to avoid
  // duplicated symbols (e.g., LLVM auto-suffix ".1") and potential EE failures.
  {
    const std::string func_name_str(func_name);
    llvm::Module* mod_normal = _normal_context->module();
    llvm::Module* mod_native = _native_context->module();
    if (mod_normal != NULL) {
      if (llvm::Function* oldf = mod_normal->getFunction(func_name_str)) {
        oldf->eraseFromParent();
      }
    }
    if (mod_native != NULL) {
      if (llvm::Function* oldf = mod_native->getFunction(func_name_str)) {
        oldf->eraseFromParent();
      }
    }
  }

  // Emit the entry point
  YuhuEntry *entry = (YuhuEntry *) cb.malloc(sizeof(YuhuEntry));

  // Build the LLVM IR for the method
  Function *function = YuhuFunction::build(env, &builder, flow, func_name);
  if (env->failing()) {
    tty->print_cr("Yuhu: compile failing during IR build for %s (func_name=%s) entry_bci=%d comp_level=%d",
                  base_name, func_name, entry_bci, env->comp_level());
    return;
  }

  // Generate native code.  It's unpleasant that we have to drop into
  // the VM to do this -- it blocks safepoints -- but I can't see any
  // other way to handle the locking.
  {
    ThreadInVMfromNative tiv(JavaThread::current());
    // Diagnostic: Check state before generate_native_code
    tty->print_cr("Yuhu: Before generate_native_code for %s (entry_bci=%d)", func_name, entry_bci);
    // Note: ORC JIT uses default memory management in stage 1
    // MemoryManager state will be available in stage 2 after CodeCache integration
    // Note: Compiler threads are already in WXWrite mode, so no need to manage WX state
    generate_native_code(entry, function, func_name);
  }

  bool is_osr = (entry_bci != InvocationEntryBci);
  int adapter_size = 0;

  // Install the method into the VM
  CodeOffsets offsets;
  offsets.set_value(CodeOffsets::Deopt, 0);
  offsets.set_value(CodeOffsets::Exceptions, 0);

  ExceptionHandlerTable handler_table;
  ImplicitExceptionTable inc_table;

  // Wrap LLVM-emitted code (already in CodeCache via YuhuMemoryManager) into a CodeBuffer
  size_t llvm_code_size = (size_t)(entry->code_limit() - entry->code_start());
  tty->print_cr("Yuhu: compile_method - entry_bci=%d, is_osr=%d, code_start=%p, code_limit=%p, llvm_code_size=%zu", 
                entry_bci, is_osr, entry->code_start(), entry->code_limit(), llvm_code_size);

  if (!is_osr) {
    // Normal method: behavior unchanged
    CodeBuffer llvm_cb(entry->code_start(), (CodeBuffer::csize_t)llvm_code_size);
    llvm_cb.insts()->set_end(entry->code_limit());
    llvm_cb.initialize_oop_recorder(env->oop_recorder());

    offsets.set_value(CodeOffsets::Verified_Entry,
                      target->is_static() ? 0 : wordSize);

    env->register_method(target,
                         entry_bci,
                         &offsets,
                         0,
                         &llvm_cb,
                         0,
                         &oopmaps,
                         &handler_table,
                         &inc_table,
                         this,
                         env->comp_level(),
                         false,
                         false);
  } else {
    // OSR method: build adapter + LLVM code into a combined CodeCache blob.

    // First, measure adapter size using a temporary CodeBuffer.
    const int kAdapterBufSize = 512;
    char adapter_buf[kAdapterBufSize];
    CodeBuffer temp_cb((address)adapter_buf, (CodeBuffer::csize_t)kAdapterBufSize);
    YuhuLabel dummy_label;
    adapter_size = generate_osr_adapter_into(temp_cb,
                                             target->get_Method(),
                                             /*base_pc*/ (address)0,  // placeholder
                                             &dummy_label,
                                             /*llvm_entry*/ (address)0);
    assert(adapter_size > 0 && adapter_size < kAdapterBufSize, "adapter size sanity");

    size_t combined_size = adapter_size + llvm_code_size;

    // Allocate new BufferBlob from CodeCache to hold adapter + llvm code.
    BufferBlob* combined_blob = BufferBlob::create("yuhu-osr-combined", combined_size);
    if (combined_blob == NULL) {
      fatal(err_msg("YuhuCompiler::compile_method: failed to allocate combined BufferBlob (size=%zu)", combined_size));
    }

    address combined_base = (address)combined_blob->content_begin();
    CodeBuffer combined_cb(combined_base, (CodeBuffer::csize_t)combined_size);

    // Emit adapter into combined buffer with correct addresses.
    YuhuLabel llvm_entry_label;
    int emitted_adapter = generate_osr_adapter_into(combined_cb,
                                                    target->get_Method(),
                                                    /*base_pc*/ combined_base,
                                                    &llvm_entry_label,
                                                    /*llvm_entry*/ combined_base + adapter_size);
    assert(emitted_adapter == adapter_size, "adapter size mismatch");

    // Copy LLVM code after adapter.
    memcpy(combined_base + adapter_size, entry->code_start(), llvm_code_size);

    // Extend instruction section to cover adapter + LLVM code, then patch jump.
    combined_cb.insts()->set_end(combined_base + combined_size);
    {
      YuhuMacroAssembler patch_masm(&combined_cb);
      // Move the pc to LLVM entry for binding
      combined_cb.insts()->set_end(combined_base + adapter_size);
      patch_masm.pin_label(llvm_entry_label);
      // Restore end to full size
      combined_cb.insts()->set_end(combined_base + combined_size);
    }

    // Update entry points to the combined blob.
    entry->set_entry_point(combined_base);
    entry->set_code_limit(combined_base + combined_size);

    // Prepare CodeBuffer for register_method.
    CodeBuffer final_cb(combined_base, (CodeBuffer::csize_t)combined_size);
    final_cb.insts()->set_end(combined_base + combined_size);
    final_cb.initialize_oop_recorder(env->oop_recorder());

    // For OSR nmethods, entry/verified_entry should be identical (static check),
    // and execution enters via OSR_Entry. Keep both at 0 to satisfy nmethod asserts.
    offsets.set_value(CodeOffsets::Entry, 0);
    offsets.set_value(CodeOffsets::Verified_Entry, 0);
    offsets.set_value(CodeOffsets::OSR_Entry, 0);           // adapter at start

    env->register_method(target,
                         entry_bci,
                         &offsets,
                         0,
                         &final_cb,
                         0,
                         &oopmaps,
                         &handler_table,
                         &inc_table,
                         this,
                         env->comp_level(),
                         false,
                         false);
  }

#if LLVM_VERSION_MAJOR >= 4
  // Free the temporary BufferBlob allocated in YuhuMemoryManager once nmethod is installed.
  // Safe to call without lock since nmethod has already copied the code.
  
  // Diagnostic: Check ORC JIT and Module state after compilation
  // Note: Must use ThreadInVMfromNative because execution_engine_lock() requires _thread_in_vm state
  {
    ThreadInVMfromNative tiv(JavaThread::current());
    MutexLocker locker(execution_engine_lock());
    tty->print_cr("Yuhu: After compilation (entry_bci=%d) - checking ORC JIT state:", entry_bci);
    if (jit() != NULL) {
      tty->print_cr("  ORC JIT: %p", jit());
      // Check if we can query ORC JIT state
    }
    llvm::Module* mod = _normal_context->module();
    if (mod != NULL) {
      tty->print_cr("  Module: %p, name: %s", mod, mod->getName().str().c_str());
      int func_count = 0;
      for (llvm::Module::iterator I = mod->begin(), E = mod->end(); I != E; ++I) {
        func_count++;
      }
      tty->print_cr("  Total functions in Module: %d", func_count);
    }
  }
  
  release_last_code_blob_unlocked();
#endif
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
  
  // ========== Debug: Verify Function and Module state (from 006_getpointertofunction_returns_null.md) ==========
  // Debug 1-2, 4: 锁外的检查（不需要 execution_engine()）
  tty->print_cr("=== Yuhu: generate_native_code for %s ===", name);
  
  // Check Module function count before add_function
  llvm::Module* mod_before = function->getParent();
  int func_count_before = 0;
  if (mod_before != NULL) {
    for (llvm::Module::iterator I = mod_before->begin(), E = mod_before->end(); I != E; ++I) {
      func_count_before++;
    }
    tty->print_cr("Yuhu: Module function count BEFORE add_function: %d", func_count_before);
  }
  
  // Try explicit add_function call (even though Function is already in Module)
  // This may trigger ExecutionEngine's "notification" mechanism
  // See: 013_executionengine_api_analysis.md - 方案 A
  context()->add_function(function);
  
  // Check Module function count after add_function
  llvm::Module* mod_after = function->getParent();
  int func_count_after = 0;
  if (mod_after != NULL) {
    for (llvm::Module::iterator I = mod_after->begin(), E = mod_after->end(); I != E; ++I) {
      func_count_after++;
    }
    tty->print_cr("Yuhu: Module function count AFTER add_function: %d", func_count_after);
    if (func_count_after > func_count_before) {
      tty->print_cr("Yuhu: WARNING - Function may have been added twice! (before=%d, after=%d)", 
                    func_count_before, func_count_after);
    } else if (func_count_after == func_count_before) {
      tty->print_cr("Yuhu: Function count unchanged - LLVM may have ignored duplicate add");
    }
  }
  
  // Debug 1: Verify Function is in Module
  llvm::Module* func_mod = function->getParent();
  if (func_mod == NULL) {
    fatal(err_msg("Function %s has no parent Module!", name));
  }
  tty->print_cr("Function name: %s, address: %p", function->getName().str().c_str(), function);
  tty->print_cr("Function's Module: %p, name: %s", func_mod, func_mod->getName().str().c_str());
  tty->print_cr("Module TargetTriple: %s", func_mod->getTargetTriple().c_str());
  tty->print_cr("Module DataLayout: %s", func_mod->getDataLayout().getStringRepresentation().c_str());
  
  // Debug 2: Verify Module pointer validity
  llvm::Module* normal_mod = _normal_context->module();
  llvm::Module* native_mod = _native_context->module();
  tty->print_cr("_normal_context->module(): %p", normal_mod);
  tty->print_cr("_native_context->module(): %p", native_mod);
  if (func_mod != normal_mod && func_mod != native_mod) {
    tty->print_cr("WARNING: Function's Module is neither normal nor native context!");
    tty->print_cr("  Function Module: %p", func_mod);
    tty->print_cr("  Normal Module: %p", normal_mod);
    tty->print_cr("  Native Module: %p", native_mod);
  } else {
    tty->print_cr("Function's Module matches context: %s", 
                   (func_mod == normal_mod) ? "normal" : "native");
  }
  
  // Debug 4: Verify IR correctness (不需要锁)
  tty->print_cr("Verifying Function IR...");
  if (llvm::verifyFunction(*function, &llvm::errs())) {
    fatal(err_msg("Function %s failed IR verification!", name));
  }
  tty->print_cr("IR verification passed");
  
  // Check if Function is in Module's function list (不需要锁)
  bool found_in_module = false;
  for (llvm::Module::iterator I = func_mod->begin(), E = func_mod->end(); I != E; ++I) {
    if (&*I == function) {
      found_in_module = true;
      break;
    }
  }
  tty->print_cr("Function found in Module function list: %s", found_in_module ? "YES" : "NO");
  
  // Print Function details (不需要锁)
  tty->print_cr("Function linkage: %d", (int)function->getLinkage());
  tty->print_cr("Function isDeclaration: %s", function->isDeclaration() ? "YES" : "NO");
  tty->print_cr("Function hasBody: %s", function->empty() ? "NO" : "YES");
  if (!function->empty()) {
    tty->print_cr("Function basic blocks: %d", (int)function->size());
  }
  // ========== End of 锁外调试代码 ==========
  
  // Declare variables outside the lock for use after lock is released
  uint8_t* mm_base = NULL;
  uintptr_t mm_size = 0;
  
  {
    MutexLocker locker(execution_engine_lock());
    
    // ========== Debug 3: 锁内的检查（需要访问 jit()）==========
    // ORC JIT: Check JIT state
    if (!jit()) {
      fatal(err_msg("ORC JIT is NULL!"));
    }
    tty->print_cr("ORC JIT: %p", jit());
    
    // Diagnostic: ORC JIT uses default memory management (stage 1)
    // MemoryManager state will be available in stage 2 after CodeCache integration
    
    // Diagnostic: Check Module state
    llvm::Module* func_mod = function->getParent();
    if (func_mod != NULL) {
      tty->print_cr("Yuhu: Module state check:");
      tty->print_cr("  Module pointer: %p", func_mod);
      tty->print_cr("  Module name: %s", func_mod->getName().str().c_str());
      // Count functions in module
      int func_count = 0;
      for (llvm::Module::iterator I = func_mod->begin(), E = func_mod->end(); I != E; ++I) {
        func_count++;
      }
      tty->print_cr("  Total functions in Module: %d", func_count);
      // List all function names
      tty->print_cr("  Functions in Module:");
      for (llvm::Module::iterator I = func_mod->begin(), E = func_mod->end(); I != E; ++I) {
        tty->print_cr("    - %s (ptr=%p)", I->getName().str().c_str(), &*I);
      }
    }
    // ========== End of 锁内调试代码 ==========
    
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
    
    // ========== ORC JIT: 查找函数符号 ==========
    // ORC JIT: Add the module containing this function to JITDylib if not already added
    // Note: For ORC JIT, we need to add the module each time a new function is compiled
    // This is different from MCJIT where modules are added once at initialization
    // Note: Compiler threads are already in WXWrite mode, so no need to manage WX state
    
    // Get the module containing this function (func_mod already defined above in lock)
    if (func_mod == NULL) {
      fatal(err_msg("Function %s has no parent Module!", name));
    }
    
    // For ORC JIT, we need to add the module to JITDylib
    // Since modules are already added at initialization, we just need to lookup the function
    // However, if this is a new function added to an existing module, we may need to re-add the module
    // For now, we'll try lookup first, and if it fails, we'll add the module
    
    std::string func_name = function->getName().str();
    tty->print_cr("ORC JIT: Looking up function: %s (linkage=%d)", 
                   func_name.c_str(), (int)function->getLinkage());
    
    // Debug: Try to get mangled name
    auto MangledName = jit()->mangle(func_name);
    tty->print_cr("ORC JIT: Mangled name: %s", MangledName.c_str());
    
    // Try to lookup the function
    auto Sym = jit()->lookup(func_name);
    if (!Sym) {
      // Function not found - may need to add module
      tty->print_cr("ORC JIT: Function not found, adding module to JITDylib...");
      
      // Create ThreadSafeModule from the module containing this function
      auto TSCtx = std::make_unique<llvm::orc::ThreadSafeContext>(
        std::make_unique<llvm::LLVMContext>());
      
      // Clone the module (ORC JIT takes ownership)
      std::unique_ptr<llvm::Module> module_clone(
        llvm::CloneModule(*func_mod).release());
      
      // Debug: Check if cloned module contains the function
      llvm::Function* cloned_func = module_clone->getFunction(func_name);
      if (cloned_func == NULL) {
        tty->print_cr("ERROR: Cloned module does not contain function %s!", func_name.c_str());
        fatal(err_msg("Cloned module missing function %s", name));
      } else {
        tty->print_cr("Cloned module contains function: %s (linkage=%d)", 
                     func_name.c_str(), (int)cloned_func->getLinkage());
      }
      
      auto TSM = llvm::orc::ThreadSafeModule(
        std::move(module_clone), *TSCtx);
      
      // Add to JITDylib
      auto &JD = jit()->getMainJITDylib();
      tty->print_cr("ORC JIT: Adding module to JITDylib (function: %s)", func_name.c_str());
      if (auto Err = jit()->addIRModule(JD, std::move(TSM))) {
        std::string ErrMsg;
        llvm::handleAllErrors(std::move(Err), [&](const llvm::ErrorInfoBase &EIB) {
          ErrMsg = EIB.message();
        });
        fatal(err_msg("Failed to add IR module for function %s: %s", name, ErrMsg.c_str()));
      }
      tty->print_cr("ORC JIT: Module added successfully");
      
      // Try lookup again
      Sym = jit()->lookup(func_name);
      if (!Sym) {
        std::string ErrMsg;
        llvm::handleAllErrors(Sym.takeError(), [&](const llvm::ErrorInfoBase &EIB) {
          ErrMsg = EIB.message();
        });
        fatal(err_msg("Failed to lookup function %s after adding module: %s", name, ErrMsg.c_str()));
      }
    }
    
    // In LLVM 20, lookup returns Expected<ExecutorAddr>
    // ExecutorAddr can be directly converted to address (it's essentially uint64_t)
    code = (address) Sym->getValue();
    tty->print_cr("ORC JIT: lookup returned: %p", code);
    
    // For ORC JIT, we don't have YuhuMemoryManager yet (stage 1: use default memory management)
    // So we'll set mm_base and mm_size to NULL/0 for now
    mm_base = NULL;
    mm_size = 0;
    tty->print_cr("ORC JIT: Using default memory management (mm_base=NULL, mm_size=0)");
  }
  
  if (code == NULL) {
    fatal(err_msg("ORC JIT lookup returned NULL for %s. Check debug output above for details.", name));
  }
  // ORC JIT: For stage 1, we use default memory management
  // mm_base and mm_size are NULL/0, so we use code address directly
  // TODO: In stage 2, integrate CodeCache via MemoryMapper
  tty->print_cr("Yuhu: setting entry code range - mm_base=%p, mm_size=%lu, code=%p", 
                mm_base, (unsigned long)mm_size, code);
  if (mm_base != NULL && mm_size > 0) {
    entry->set_entry_point((address)mm_base);
    entry->set_code_limit((address)(mm_base + mm_size));
    tty->print_cr("ORC JIT: using MemoryManager range: code_start=%p, code_limit=%p, size=%lu", 
                  (address)mm_base, (address)(mm_base + mm_size), (unsigned long)mm_size);
  } else {
    // For ORC JIT stage 1, we don't have CodeCache integration yet
    // Use code address directly (will be fixed in stage 2)
    tty->print_cr("ORC JIT: Using code address directly (stage 1 - no CodeCache integration yet)");
    tty->print_cr("ORC JIT: code=%p (size will be determined later)", code);
    entry->set_entry_point(code);
    
    // Estimate code size based on function structure (temporary solution for stage 1)
    // Use basic block count to estimate size
    // In LLVM 20, use size() method instead of getBasicBlockList().size()
    size_t bb_count = function->size();  // size() returns the number of basic blocks
    size_t estimated_size = bb_count * 128;  // Estimate ~128 bytes per basic block
    if (estimated_size < 512) estimated_size = 512;  // Minimum 512 bytes
    if (estimated_size > 65536) estimated_size = 65536;  // Maximum 64KB (safety limit)
    
    entry->set_code_limit((address)(code + estimated_size));
    tty->print_cr("ORC JIT: Estimated code size: %zu bytes (%zu basic blocks)", 
                  estimated_size, bb_count);
  }
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

void YuhuCompiler::release_last_code_blob_unlocked() {
    // Note: ORC JIT uses default memory management in stage 1
    // MemoryManager integration will be added in stage 2
    // For now, this is a no-op
    tty->print_cr("Yuhu: release_last_code_blob_unlocked - ORC JIT stage 1 (no-op)");
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
    // Note: ORC JIT manages code lifecycle automatically
    // No need to explicitly free machine code
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

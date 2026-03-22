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
#include "yuhu/yuhuPrologueAnalyzer.hpp"
#include "yuhu/yuhuDebugInfo.hpp"
#include "yuhu/yuhu_globals.hpp"
#include "utilities/debug.hpp"
#include "asm/yuhu/yuhu_macroAssembler.hpp"
#include "code/codeCache.hpp"
#include "oops/method.hpp"
#include "c1/c1_Runtime1.hpp"
#include "runtime/sharedRuntime.hpp"

#include <fnmatch.h>
#include <cstring>
#include <set>

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
  
  // Initialize stub patching tracking structures
  _stub_patch_methods = new (ResourceObj::C_HEAP, mtCompiler) GrowableArray<ciMethod*>(100, true);
  _stub_patch_addresses = new (ResourceObj::C_HEAP, mtCompiler) GrowableArray<GrowableArray<address>*>(100, true);
  
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
  auto JTMB = llvm::orc::JITTargetMachineBuilder(llvm::Triple(TripleStr))
    .setCPU(MCPU)
    .addFeatures(MAttrs);

  JTMB.addFeatures({"+reserve-x28", "+reserve-x21", "+reserve-x26", "+reserve-x22", "+reserve-x24"});

  // CRITICAL: Reserve x28 (Thread*) and x12 (Method*) registers
  // This prevents LLVM from using these registers in generated code
  // Method 1: Try to set reserved registers through TargetMachine options
  llvm::TargetOptions Options;
  // TODO: Set Options to reserve specific registers if API is available

  auto JIT = llvm::orc::LLJITBuilder()
    .setJITTargetMachineBuilder(JTMB)
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
// NOTE: base_pc parameter is ignored - we use PC-relative address calculation instead.
static int generate_osr_adapter_into(CodeBuffer& cb,
                                     Method* method,
                                     address base_pc,  // Unused - kept for compatibility
                                     YuhuLabel* llvm_label,
                                     address llvm_entry) {
  YuhuMacroAssembler masm(&cb);
  address start = masm.current_pc();

  // Create a label at the start of the adapter to calculate base_pc
  // This ensures base_pc points to the actual CodeCache address after register_method
  YuhuLabel base_pc_label;
  masm.pin_label(base_pc_label);

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

  // Load base_pc using PC-relative address calculation
  // This ensures base_pc is correct even after code relocation in nmethod::new_nmethod
  masm.write_inst_adr(YuhuMacroAssembler::x2, base_pc_label); // x2 = base_pc (adapter start)

  // Load thread into x3
  masm.write_insts_get_thread(YuhuMacroAssembler::x3);

  // Jump to LLVM entry using PC-relative jump
  // This ensures the jump address is correct even after code relocation in nmethod::new_nmethod
  if (llvm_label != NULL) {
    masm.write_inst_b(*llvm_label);   // Placeholder, patched when label is bound
  } else {
    // Use relative jump instead of absolute address
    // b instruction range is ±128MB, which is sufficient for adapter_size
    masm.write_inst_b(llvm_entry);
  }

  address end = masm.current_pc();
  return (int)(end - start);
}

// Helper function: Scan LLVM code from end to find actual code size (excluding trailing udf #0)
// AArch64 'udf #0' encoding: 0x00000000 (all zeros)
// Returns the number of bytes from code_start to the last non-udf instruction
static size_t calculate_effective_code_size(address code_start, size_t total_size) {
  if (total_size == 0) return 0;
  
  // Scan forward from code_start to find effective code size
  // Look for consecutive udf #0 instructions (encoding: 0x00000000)
  // When we find 20 consecutive udf #0 (80 bytes), assume code ends there
  address scan_end = code_start + total_size;
  address pc = code_start;
  size_t zero_count = 0;
  address last_non_zero = code_start;
  const size_t CONTINUOUS_ZERO_THRESHOLD = 80;  // 20 instructions * 4 bytes
  
  while (pc + 4 <= scan_end) {
    uint32_t inst = *(uint32_t*)pc;
    
    if (inst == 0x00000000) {
      zero_count += 4;
      if (zero_count >= CONTINUOUS_ZERO_THRESHOLD) {
        // Found enough consecutive zeros, code likely ends at last non-zero instruction
        return (last_non_zero + 4) - code_start;
      }
    } else {
      zero_count = 0;
      last_non_zero = pc;
    }
    
    pc += 4;
  }
  
  // If no sufficient continuous zeros found, return total_size
  return total_size;
}

// Measure adapter size for normal (non-OSR) method compilation.
// Returns the exact byte size needed for the parameter adapter stub.
int YuhuCompiler::measure_normal_adapter_size() {
    // First, measure adapter size using a temporary CodeBuffer.
    // Use the same jump method (direct jump with NULL label) as actual generation
    // to ensure size consistency.
    // The b instruction size is fixed (4 bytes) as long as offset is within ±128MB range,
    // so using a reasonable placeholder address is safe.
    const int kAdapterBufSize = 512;
    char adapter_buf[kAdapterBufSize];
    CodeBuffer temp_cb((address)adapter_buf, (CodeBuffer::csize_t)kAdapterBufSize);
    // Use a placeholder address that's within b instruction range (±128MB)
    // The exact value doesn't matter as long as the offset is encodable (within ±128MB)
    // Using adapter_buf + 80 assumes adapter is roughly 80 bytes, giving ~80 byte offset
    address placeholder_entry = (address)adapter_buf + 80;

    YuhuMacroAssembler masm(&temp_cb);
    address start = masm.current_pc();

    // CRITICAL: Adapter must NOT allocate any stack frame
    // If adapter allocates stack frame, current_sp in LLVM function will be wrong
    // This will cause incorrect stack frame traversal during safepoints

    // Simply jump to LLVM function
    // x0-x7 are already Java method parameters (from i2c adapter)
    // For static methods, x0 is NULL (passed by i2c adapter)
    // Method* (x12) and Thread* (x28) are HotSpot global registers, preserved by i2c adapter
    // Stack arguments (> 8) are in caller's stack frame, accessible via x20 (esp)

    // Use relative jump instead of absolute address
    // b instruction range is ±128MB, which is sufficient for adapter_size
    masm.write_inst_b(placeholder_entry);

    address end = masm.current_pc();
    return (int)(end - start);
}

int YuhuCompiler::generate_normal_adapter_into(CodeBuffer& cb, address llvm_entry) {
  YuhuMacroAssembler masm(&cb);
  address start = masm.current_pc();

  // CRITICAL: Adapter must NOT allocate any stack frame
  // If adapter allocates stack frame, current_sp in LLVM function will be wrong
  // This will cause incorrect stack frame traversal during safepoints
  
  // Simply jump to LLVM function
  // x0-x7 are already Java method parameters (from i2c adapter)
  // For static methods, x0 is NULL (passed by i2c adapter)
  // Method* (x12) and Thread* (x28) are HotSpot global registers, preserved by i2c adapter
  // Stack arguments (> 8) are in caller's stack frame, accessible via x20 (esp)

  // Use relative jump instead of absolute address
  // b instruction range is ±128MB, which is sufficient for adapter_size
  masm.write_inst_b(llvm_entry);

  address end = masm.current_pc();
  return (int)(end - start);
}

void YuhuCompiler::compile_method(ciEnv*    env,
                                   ciMethod* target,
                                   int       entry_bci) {
  assert(is_initialized(), "should be");
  ResourceMark rm;
  
  // ========== 临时测试：只测试普通编译 ==========
  // TODO: 测试完成后删除此代码块
  if (entry_bci != InvocationEntryBci) {
    // 跳过 OSR 编译，只测试普通编译
    // 调用 record_failure 来标记编译失败，这样 compileBroker 就不会
    // 调用 record_method_not_compilable，从而避免断言失败。
    // 注意：record_failure 不会标记方法为不可编译，只是记录失败原因。
    tty->print_cr("Yuhu: SKIPPING OSR compilation for %s (entry_bci=%d) - normal-only test mode",
                  methodname(target->holder()->name()->as_utf8(), target->name()->as_utf8()),
                  entry_bci);
    env->record_failure("normal-only test mode: skipping OSR compilation");
    return;
  }
//  if (strcmp(target->holder()->name()->as_utf8(), "sun/nio/cs/UTF_8$Encoder") == 0
//    && strcmp(target->name()->as_utf8(), "encodeArrayLoop") == 0
//    && strcmp(target->signature()->as_symbol()->as_utf8(), "(Ljava/nio/CharBuffer;Ljava/nio/ByteBuffer;)Ljava/nio/charset/CoderResult;") == 0) {
//      tty->print_cr("Yuhu: SKIPPING %s", methodname(target->holder()->name()->as_utf8(), target->name()->as_utf8()));
//      env->record_failure("normal-only test mode: skipping encodeArrayLoop compilation");
//      return;
//  }
//    if (strcmp(target->holder()->name()->as_utf8(), "java/nio/charset/CharsetEncoder") == 0
//        && strcmp(target->name()->as_utf8(), "encode") == 0
//        && strcmp(target->signature()->as_symbol()->as_utf8(), "(Ljava/nio/CharBuffer;Ljava/nio/ByteBuffer;Z)Ljava/nio/charset/CoderResult;") == 0) {
//        tty->print_cr("Yuhu: SKIPPING %s", methodname(target->holder()->name()->as_utf8(), target->name()->as_utf8()));
//        env->record_failure("normal-only test mode: skipping encode compilation");
//        return;
//    }
//    if (strcmp(target->holder()->name()->as_utf8(), "sun/nio/cs/StreamEncoder") == 0
//        && strcmp(target->name()->as_utf8(), "implWrite") == 0
//        && strcmp(target->signature()->as_symbol()->as_utf8(), "([CII)V") == 0) {
//        tty->print_cr("Yuhu: SKIPPING %s", methodname(target->holder()->name()->as_utf8(), target->name()->as_utf8()));
//        env->record_failure("normal-only test mode: skipping implWrite compilation");
//        return;
//    }
  // ========== 临时测试代码结束 ==========
  
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
  if (flow->failing()) {
      env->record_failure("flow analysis has encountered an error");
      return;
  }
  // Bail out if any block has a trap (unloaded class at compile time).
  // Yuhu does not support deoptimization, so such methods cannot be compiled.
  for (int i = 0; i < flow->block_count(); i++) {
    if (flow->pre_order_at(i)->has_trap()) {
      env->record_failure("block has trap (unloaded class)");
      return;
    }
  }
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
  YuhuBuilder builder(&cb, NULL);  // function will be set later by YuhuFunction

  // Remove any existing functions with the same name in both modules to avoid
  // duplicated symbols (e.g., LLVM auto-suffix ".1") and potential EE failures.
  // Also clear all other functions from the module to avoid conflicts from
  // previously compiled methods (e.g., Matrix::multiply appearing in encodeArrayLoop IR)
  {
    llvm::Module* mod_normal = _normal_context->module();
    llvm::Module* mod_native = _native_context->module();
    
    // Clear all functions from normal module
    if (mod_normal != NULL) {
      std::vector<llvm::Function*> to_erase;
      for (auto& func : mod_normal->getFunctionList()) {
        to_erase.push_back(&func);
      }
      for (llvm::Function* func : to_erase) {
        func->eraseFromParent();
      }
    }
    
    // Clear all functions from native module
    if (mod_native != NULL) {
      std::vector<llvm::Function*> to_erase;
      for (auto& func : mod_native->getFunctionList()) {
        to_erase.push_back(&func);
      }
      for (llvm::Function* func : to_erase) {
        func->eraseFromParent();
      }
    }
  }

  // Emit the entry point
  YuhuEntry *entry = (YuhuEntry *) cb.malloc(sizeof(YuhuEntry));
  size_t llvm_code_size = 0;
  size_t effective_code_size = 0;

  // Build the LLVM IR for the method and get the debug information recorder
  YuhuDebugInformationRecorder* debug_info_recorder = NULL;
  Function *function = YuhuFunction::build(env, &builder, flow, func_name, &debug_info_recorder);
  if (env->failing()) {
    tty->print_cr("Yuhu: compile failing during IR build for %s (func_name=%s) entry_bci=%d comp_level=%d",
                  base_name, func_name, entry_bci, env->comp_level());
    return;
  }

  // Output LLVM IR immediately after build for debugging PHI node type mismatch
  tty->print_cr("=== Yuhu: LLVM IR for %s (after build, before verification) ===", func_name);
  llvm::raw_ostream &OS = llvm::errs();
  function->print(OS);
  OS.flush();
  tty->print_cr("=== End of LLVM IR for %s ===", func_name);
  tty->flush();

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

    address code_start = entry->code_start();
    llvm_code_size = entry->code_limit() - code_start;
    // Calculate effective code size by scanning for trailing udf #0 instructions
    effective_code_size = calculate_effective_code_size(entry->code_start(), llvm_code_size);
    
    // After native code generation, scan the generated machine code for offset markers
    // and update the offset mapper with the actual PC offsets
    YuhuOffsetMapper* offset_mapper = cb.offset_mapper();
    if (offset_mapper != NULL) {
      tty->print_cr("Yuhu: Scanning generated code for offset markers...");
      builder.scan_and_update_offset_markers(code_start, effective_code_size, offset_mapper);

      // After scanning and updating the mapper, relocate OopMaps
//      builder.relocate_oopmaps(offset_mapper, env);
//      tty->print_cr("Yuhu: OopMap relocation completed with %d mappings", offset_mapper->num_mappings());
    } else {
      tty->print_cr("Yuhu: WARNING - No offset mapper available for OopMap relocation");
    }
  }

  bool is_osr = (entry_bci != InvocationEntryBci);
  int adapter_size = 0;

  // Calculate frame_size for nmethod registration
  // frame_size is in words (intptr_t units), not bytes
  // According to YuhuStack::initialize:
  //   frame_words = header_words + monitor_words + stack_words
  //   header_words = 6
  //   extra_locals = max_locals - size_of_parameters
  //   frame_size (words) = frame_words + extra_locals
  int header_words = yuhu_frame_header_words;
  
  // Calculate max_monitors using flow analysis (similar to YuhuTargetInvariants::count_monitors)
  int max_monitors = 0;
  if (target->is_synchronized() || target->has_monitor_bytecodes()) {
    for (int i = 0; i < flow->block_count(); i++) {
      max_monitors = MAX2(max_monitors, flow->pre_order_at(i)->monitor_count());
    }
  }
  
  int monitor_words = max_monitors * frame::interpreter_frame_monitor_size();
  int stack_words = target->max_stack();
  int locals_words = target->max_locals();
  int arg_size = target->arg_size();  // Use arg_size() instead of size_of_parameters()
  int extra_locals = locals_words - arg_size;
  int frame_words = header_words + monitor_words + stack_words + yuhu_llvm_spill_slots;
  
  // Step 1: Analyze LLVM prologue to get actual stack space used
  address llvm_code_start = entry->code_start();
  int actual_prologue_bytes = YuhuPrologueAnalyzer::analyze_prologue_stack_bytes(llvm_code_start);
  int actual_prologue_words = (actual_prologue_bytes + wordSize - 1) / wordSize;  // Round up
  
  tty->print_cr("Yuhu: Prologue analysis - code_start=%p, prologue_bytes=%d, prologue_words=%d",
                llvm_code_start, actual_prologue_bytes, actual_prologue_words);

  // Step 3: Calculate final frame_size using actual prologue size
  int frame_size = frame_words + locals_words + actual_prologue_words;
  // CRITICAL: Align frame_size to 2 words (16 bytes) to match yuhuStack.cpp
  // yuhuStack.cpp uses align_size_up(frame_size_bytes, 16), so we must align here too
  frame_size = align_size_up(frame_size, 2);

  // Install the method into the VM
  CodeOffsets offsets;
  offsets.set_value(CodeOffsets::Deopt,       0);  // will be updated per path below
  offsets.set_value(CodeOffsets::Exceptions,  0);  // will be updated per path below

  ExceptionHandlerTable handler_table;
  ImplicitExceptionTable inc_table;
  
  tty->print_cr("Yuhu: compile_method - entry_bci=%d, is_osr=%d, code_start=%p, code_limit=%p", 
                entry_bci, is_osr, entry->code_start(), entry->code_limit());
  tty->print_cr("Yuhu: LLVM code - total_size=%zu, effective_size=%zu, saved=%zu bytes (%.1f%%)",
                llvm_code_size, effective_code_size, 
                llvm_code_size - effective_code_size,
                (llvm_code_size > 0) ? (100.0 * (llvm_code_size - effective_code_size) / llvm_code_size) : 0.0);

  if (!is_osr) {
    // Normal method: build adapter + LLVM code into a combined CodeCache blob.
    // The adapter rearranges parameters from i2c adapter format to Yuhu's expected format.
    adapter_size = measure_normal_adapter_size();
    assert(adapter_size > 0 && adapter_size < 512, "adapter size sanity");

      // Measure exception handler and deopt handler sizes first
    int unwind_handler_size = measure_unwind_handler_size(frame_size * wordSize);
    int exc_handler_size  = measure_exception_handler_size();
    int deopt_handler_size = measure_deopt_handler_size();

    size_t combined_size = adapter_size + effective_code_size + unwind_handler_size;

    // Create CodeBuffer that manages its own BufferBlob internally
    CodeBuffer combined_cb("yuhu-normal-combined", (int)combined_size, 0);
    if (combined_cb.blob() == NULL) {
      fatal(err_msg("YuhuCompiler::compile_method: failed to allocate combined CodeBuffer (size=%zu)", combined_size));
    }
    combined_cb.initialize_stubs_size( exc_handler_size + deopt_handler_size);

    address combined_base = combined_cb.insts_begin();

    // Emit adapter into combined buffer with correct addresses.
    // Use direct jump (pass NULL for llvm_label) to avoid patching complexity.
    address llvm_entry = combined_base + adapter_size;
    tty->print_cr("Yuhu: Normal adapter - combined_base=%p, adapter_size=%d, llvm_entry=%p",
                  combined_base, adapter_size, llvm_entry);
    int emitted_adapter = generate_normal_adapter_into(combined_cb, llvm_entry);
    assert(emitted_adapter == adapter_size, "adapter size mismatch");
    tty->print_cr("Yuhu: Normal adapter - emitted_adapter=%d, actual LLVM code starts at %p",
                  emitted_adapter, combined_base + emitted_adapter);

    // Copy LLVM code after adapter.
    // Only copy effective code (excluding trailing udf #0 padding)
    memcpy(combined_base + adapter_size, entry->code_start(), effective_code_size);

    // Convert virtual offsets to real offsets and add to the debug information recorder
    if (debug_info_recorder != NULL) {
      DebugInformationRecorder* real_debug_info = env->debug_info();
      YuhuOffsetMapper* offset_mapper = cb.offset_mapper();
      if (real_debug_info != NULL && offset_mapper != NULL) {
        tty->print_cr("Yuhu: Converting virtual offsets to real offsets and adding to debug info recorder");
        debug_info_recorder->convert_and_add_to_real_recorder(real_debug_info, target, offset_mapper, adapter_size);
      }
    }

      // Generate unwind handler (always needed - JVM requires it for exception propagation)
      // This is different from exception handler - unwind handler propagates exceptions upward.
      // frame_size_in_bytes is the complete yuhu frame size in bytes, used to restore SP
      // before jumping to unwind_exception_id.
      combined_cb.insts()->set_end(combined_base + adapter_size + effective_code_size);
      generate_unwind_handler(combined_cb, frame_size * wordSize);

    // adjust oopmaps offset
//    builder.adjust_oopmaps_pc_offset(env, adapter_size);

      // CRITICAL: Fix up epilogue markers (0xcafebabe -> sub sp, x29, #imm)
      // This is needed to restore SP from x29 before returning from the function.
      // See doc/yuhu/activities/039_epilogue_sp_restoration.md for details.
      builder.fixup_prologue_epilogue_markers(combined_base, combined_size);

    // Extend instruction section to cover adapter + LLVM code.
    combined_cb.insts()->set_end(combined_base + combined_size);

      // Generate exception handler stub only if there might be landing pads
      // TODO: Check LLVM IR for landing pads instead of always generating
      if (exc_handler_size >= 0) {
          generate_exception_handler(combined_cb, exc_handler_size);
          // TODO: Properly handle exception handlers for each landing pad
          // For now, DO NOT register any exception handlers to avoid infinite loop
          tty->print_cr("Yuhu: Skipping exception handler registration (not yet implemented for multiple landing pads)");
      }

    // Generate deopt handler (always needed)
    generate_deopt_handler(combined_cb, deopt_handler_size);

    combined_cb.initialize_oop_recorder(env->oop_recorder());
    // The label should already be bound to the jump instruction location.
    // We need to patch the jump instruction to point to llvm_entry.
    // Since we used write_inst_b(*llvm_label), the label should be bound when we call pin_label.
    // But we need to patch it after we know the actual llvm_entry address.
    // Let's use a direct jump instead of a label-based jump for simplicity.
    // Actually, we already passed llvm_entry to generate_normal_adapter_into, so it should use direct jump.
    // But if we passed a label, we need to patch it. Let's check the implementation.
    // For now, let's use direct jump (pass NULL for llvm_label) to avoid patching complexity.

    // Set entry point offsets according to HotSpot's requirements:
    // - For static methods: Entry == Verified_Entry (both point to adapter)
    // - For non-static methods: Entry != Verified_Entry
    //   (Entry points to adapter for class check, Verified_Entry points to LLVM code)
    if (target->is_static()) {
      // Static methods: both entry points point to adapter (no class check needed)
      offsets.set_value(CodeOffsets::Entry, 0);
      offsets.set_value(CodeOffsets::Verified_Entry, 0);
    } else {
      // Non-static methods: Entry points to adapter (for class check),
      // Verified_Entry points to LLVM code (skip class check)
      offsets.set_value(CodeOffsets::Entry, 0);
      offsets.set_value(CodeOffsets::Verified_Entry, adapter_size);
    }
    offsets.set_value(CodeOffsets::UnwindHandler, adapter_size + effective_code_size);
    offsets.set_value(CodeOffsets::Exceptions, 0);
    offsets.set_value(CodeOffsets::Deopt,       exc_handler_size);
    // UnwindHandler is already set in generate_unwind_handler()

    tty->print_cr("Yuhu: Registering method - combined_base=%p (disassemble this address, not entry->code_start()=%p)",
                  combined_base, entry->code_start());
    tty->print_cr("Yuhu: frame_size=%d words (header=%d, monitor=%d, stack=%d, extra_locals=%d)",
                  frame_size, header_words, monitor_words, stack_words, extra_locals);
    tty->print_cr("Yuhu: CodeBuffer range: code_begin=%p, code_end=%p, size=%zu",
                  combined_cb.insts()->start(), combined_cb.insts()->end(),
                  combined_cb.insts()->end() - combined_cb.insts()->start());
    
    // Generate minimal scope descriptor for deoptimization support
    // This allows the VM to handle deoptimization when called methods deopt
    DebugInformationRecorder* debug_info = env->debug_info();
    if (debug_info != NULL) {
      tty->print_cr("Yuhu: Generating minimal scope descriptor for deoptimization support");
//      YuhuDebugInfo::generate_minimal_debug_info(debug_info, target, frame_size);
    }
    
    env->register_method(target,
                         entry_bci,
                         &offsets,
                         0,
                         &combined_cb,
                         frame_size,  // Pass calculated frame_size in words
                         &oopmaps,
                         &handler_table,
                         &inc_table,
                         this,
                         env->comp_level(),
                         false,
                         false);
    
    // Verify nmethod was installed correctly
    address test_pc = combined_base + adapter_size + 16;  // Some PC in LLVM code
    CodeBlob* found_blob = CodeCache::find_blob(test_pc);
    tty->print_cr("Yuhu: Post-registration check - test_pc=%p, found_blob=%p",
                  test_pc, found_blob);
    if (found_blob != NULL) {
      tty->print_cr("Yuhu: Found blob - code_begin=%p, code_end=%p",
                    found_blob->code_begin(), found_blob->code_end());
    } else {
      tty->print_cr("Yuhu: WARNING - CodeCache::find_blob returned NULL!");
    }
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

    tty->print_cr("Yuhu: Registering OSR method - combined_base=%p, frame_size=%d words",
                  combined_base, frame_size);
    
    // Generate minimal scope descriptor for deoptimization support
    DebugInformationRecorder* debug_info = env->debug_info();
    if (debug_info != NULL) {
      tty->print_cr("Yuhu: Generating minimal scope descriptor for OSR method");
      YuhuDebugInfo::generate_minimal_debug_info(debug_info, target, frame_size);
    }
    
    env->register_method(target,
                         entry_bci,
                         &offsets,
                         0,
                         &final_cb,
                         frame_size,  // Pass calculated frame_size in words
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
  // Note: register_method already handles thread state transitions via VM_ENTRY_MARK,
  // so we don't need additional ThreadInVMfromNative here.
  // Removing the diagnostic block to avoid WX state issues on AArch64.
  
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
  YuhuBuilder builder(&cb, NULL);  // no function context for native wrapper

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
      tty->print_cr("=== YUHU DEBUG: LLVM IR for function %s ===", name);
      // LLVM 20: dump() may not be available in all builds
      // Use print() with llvm::errs() instead
      llvm::raw_ostream &OS = llvm::errs();
      function->print(OS);
      OS.flush();
      tty->print_cr("=== END LLVM IR ===");
      tty->flush();
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
  
  // Debug: Collect all instructions and verify they are in basic blocks
  tty->print_cr("=== Yuhu: Collecting all instructions in function %s ===", name);
  std::set<llvm::Instruction*> all_instructions_in_blocks;
  int total_instructions = 0;
  int total_basic_blocks = 0;
  
  // Collect all instructions from all basic blocks
  for (llvm::Function::iterator BB = function->begin(), E = function->end(); BB != E; ++BB) {
    total_basic_blocks++;
    for (llvm::BasicBlock::iterator I = BB->begin(), IE = BB->end(); I != IE; ++I) {
      all_instructions_in_blocks.insert(&*I);
      total_instructions++;
    }
  }
  
  tty->print_cr("  Total basic blocks: %d", total_basic_blocks);
  tty->print_cr("  Total instructions in blocks: %d", total_instructions);
  tty->flush();
  
  // Check for orphaned instructions (instructions not in any basic block)
  // This is done by checking all values in the function and seeing if they are instructions
  // without a parent basic block
  int orphaned_count = 0;
  for (llvm::Function::iterator BB = function->begin(), E = function->end(); BB != E; ++BB) {
    for (llvm::BasicBlock::iterator I = BB->begin(), IE = BB->end(); I != IE; ++I) {
      llvm::Instruction* inst = &*I;
      
      // Check if instruction has a valid parent
      llvm::BasicBlock* parent = inst->getParent();
      if (parent == NULL) {
        tty->print_cr("  ERROR: Instruction %p has NULL parent!", inst);
        tty->print_cr("    Instruction type: %s", inst->getOpcodeName());
        if (inst->hasName()) {
          tty->print_cr("    Instruction name: %s", inst->getName().str().c_str());
        }
        orphaned_count++;
        continue;
      }
      
      // Check if instruction's parent matches the basic block we're iterating
      if (parent != &*BB) {
        tty->print_cr("  WARNING: Instruction %p parent mismatch!", inst);
        tty->print_cr("    Expected parent: %p (%s)", &*BB, BB->getName().str().c_str());
        tty->print_cr("    Actual parent: %p (%s)", parent, parent->getName().str().c_str());
        tty->print_cr("    Instruction type: %s", inst->getOpcodeName());
        if (inst->hasName()) {
          tty->print_cr("    Instruction name: %s", inst->getName().str().c_str());
        }
      }
      
      // Check if instruction's getFunction() returns valid value
      // Only check if parent is valid (getFunction() requires valid parent)
      if (parent != NULL) {
        // getFunction() is implemented as: getParent()->getParent()
        // So if parent is valid, we can safely call getFunction()
        llvm::Function* func = parent->getParent();
        if (func == NULL) {
          tty->print_cr("  ERROR: Instruction %p parent's getParent() returns NULL!", inst);
          tty->print_cr("    Instruction type: %s", inst->getOpcodeName());
          if (inst->hasName()) {
            tty->print_cr("    Instruction name: %s", inst->getName().str().c_str());
          }
          tty->print_cr("    Parent basic block: %p (%s)", parent, parent->getName().str().c_str());
          orphaned_count++;
        } else if (func != function) {
          tty->print_cr("  ERROR: Instruction %p parent's getParent() returns wrong function!", inst);
          tty->print_cr("    Expected function: %p (%s)", function, function->getName().str().c_str());
          tty->print_cr("    Actual function: %p (%s)", func, func->getName().str().c_str());
          tty->print_cr("    Instruction type: %s", inst->getOpcodeName());
          if (inst->hasName()) {
            tty->print_cr("    Instruction name: %s", inst->getName().str().c_str());
          }
          tty->print_cr("    Parent basic block: %p (%s)", parent, parent->getName().str().c_str());
          orphaned_count++;
        } else {
          // Also try calling inst->getFunction() directly to see if it matches
          // This is the actual method that verifier calls, and it might crash if parent is invalid
          llvm::Function* direct_func = NULL;
          bool getFunction_succeeded = false;
          // We can't use try-catch for LLVM internal methods, so we'll just call it
          // If it crashes, we'll see it in the crash log
          direct_func = inst->getFunction();
          getFunction_succeeded = true;
          if (direct_func != function) {
            tty->print_cr("  WARNING: Instruction %p getFunction() returns different value!", inst);
            tty->print_cr("    Via parent: %p (%s)", func, func->getName().str().c_str());
            tty->print_cr("    Direct: %p (%s)", direct_func, direct_func ? direct_func->getName().str().c_str() : "NULL");
            tty->print_cr("    Expected: %p (%s)", function, function->getName().str().c_str());
          }
        }
      }
    }
  }
  
  if (orphaned_count > 0) {
    tty->print_cr("  ERROR: Found %d orphaned or invalid instructions!", orphaned_count);
    tty->flush();
    // Don't fatal here, let verifyFunction catch it
  } else {
    tty->print_cr("  All instructions are properly in basic blocks");
  }
  tty->flush();
  
  // Output IR to file for analysis (before verification)
  // This allows using LLVM tools like 'opt -verify' to analyze the IR
  std::string ir_filename = std::string("/tmp/yuhu_ir_") + std::string(name) + ".ll";
  // Replace invalid filename characters
  for (size_t i = 0; i < ir_filename.length(); i++) {
    if (ir_filename[i] == ':' || ir_filename[i] == '/' || ir_filename[i] == ' ') {
      ir_filename[i] = '_';
    }
  }
  
  std::error_code EC;
  llvm::raw_fd_ostream ir_file(ir_filename, EC, llvm::sys::fs::OF_Text);
  if (!EC) {
    // Print the entire module (not just the function) to include metadata definitions
    // This ensures metadata nodes used by llvm.read_register are properly serialized
    llvm::Module* mod = function->getParent();
    if (mod != NULL) {
      mod->print(ir_file, nullptr);
    } else {
      // Fallback: print just the function if module is not available
      function->print(ir_file);
    }
    ir_file.flush();
    tty->print_cr("Yuhu: IR written to %s (use 'opt -verify %s' to analyze)", 
                  ir_filename.c_str(), ir_filename.c_str());
    tty->flush();
  } else {
    tty->print_cr("Yuhu: Failed to write IR to %s: %s", 
                  ir_filename.c_str(), EC.message().c_str());
    tty->flush();
  }
  
  // Debug 4: Verify IR correctness (不需要锁)
  tty->print_cr("Verifying Function IR...");
  if (llvm::verifyFunction(*function, &llvm::errs())) {
    tty->print_cr("Yuhu: IR verification failed! See %s for the IR", ir_filename.c_str());
    tty->print_cr("Yuhu: Run 'opt -verify %s' to get detailed error messages", ir_filename.c_str());
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
      // CRITICAL: Use the SAME LLVMContext as the original module to preserve metadata
      // Creating a new context causes metadata to be lost, leading to "use of undefined metadata" errors
      // Also, cloning with a new context may cause register name validation issues
      // Instead, we should use the module's existing context
      llvm::LLVMContext* module_ctx = &func_mod->getContext();
      
      // Clone the module using the SAME context (not a new one)
      // This preserves metadata nodes and register name information
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
      
      // CRITICAL: Create ThreadSafeContext using the SAME LLVMContext as the original module
      // This ensures metadata is preserved and register names are validated correctly
      // However, we cannot move from module_ctx (it's a reference to the module's context)
      // Instead, we need to share the context or use a different approach
      // For now, create a new context but ensure the cloned module uses the original context
      // The cloned module should already have the correct context from CloneModule
      auto TSCtx = std::make_unique<llvm::orc::ThreadSafeContext>(
        std::make_unique<llvm::LLVMContext>());
      
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

// Generate a static call stub for direct method calls
// This stub loads the Method* and jumps to _from_compiled_entry
// Following C1/C2 approach to avoid slot addressing issues
// IMPORTANT: Preserve Method* in x12 (x_method_in_constant_pool) register
address YuhuCompiler::generate_static_call_stub(ciMethod* target_method, ciMethod* current_method) {
  // Allocate permanent stub in Code Cache
  // Stub size: approximately 32 bytes for AArch64
  // - adr x9, method_literal  (4 bytes) - Get address of literal
  // - ldr x9, [x9]            (4 bytes) - Load Method* from literal
  // - mov x12, x9               (4 bytes) - Move Method* to x12 (x_method_in_constant_pool)
  // - ldr x9, [x9, #72]       (4 bytes) - Load _from_compiled_entry
  // - br x9                   (4 bytes) - Jump to entry
  // - .align 8                (padding)
  // - .quad <Method*>         (8 bytes) - Method* constant
  const int stub_size = 64;  // Generous size to account for alignment
  
  ResourceMark rm;
  
  // Allocate buffer space in Code Cache for the stub
  BufferBlob* stub_blob = BufferBlob::create("yuhu_static_call_stub", stub_size);
  if (stub_blob == NULL) {
    fatal("CodeCache is full - cannot allocate static call stub");
    return NULL;
  }
  
  CodeBuffer stub_buffer(stub_blob);
  YuhuMacroAssembler masm(&stub_buffer);
  
  // Get the Method* address (target method)
  Method* method_ptr = target_method->get_Method();
  
  // AArch64 stub code using YuhuMacroAssembler:
  // We need to:
  // 1. Load Method* from a PC-relative location
  // 2. Move Method* to x12 register (x_method_in_constant_pool) for c2i adapter
  // 3. Load _from_compiled_entry from Method*
  // 4. Jump to the entry point
  
  // Use x9 as temporary register
  YuhuLabel method_literal;
  
  // Get address of the Method* literal
  masm.write_inst_adr(YuhuMacroAssembler::x9, method_literal);
  
  // Load Method* value from the literal
  masm.write_inst_ldr(YuhuMacroAssembler::x9, YuhuAddress(YuhuMacroAssembler::x9, 0));
  
  // Move Method* to x12 register (x_method_in_constant_pool) for c2i adapter
  // This is CRITICAL: c2i adapter expects Method* in x12
  masm.write_inst_mov_reg(YuhuMacroAssembler::x12, YuhuMacroAssembler::x9);
  
  // Load _from_compiled_entry field from Method*
  masm.write_inst_ldr(YuhuMacroAssembler::x9, 
                      YuhuAddress(YuhuMacroAssembler::x9, Method::from_compiled_offset()));
  
  // Jump to the compiled entry
  masm.write_inst_br(YuhuMacroAssembler::x9);
  
  // Align to 8-byte boundary for the literal
  while ((masm.current_pc() - stub_buffer.insts_begin()) % 8 != 0) {
    masm.write_inst("nop");
  }
  
  // Bind the label and emit the Method* literal
  masm.pin_label(method_literal);
  // Use emit_int64 to properly emit the 64-bit address
  stub_buffer.insts()->emit_int64((intptr_t)method_ptr);
  
  // The stub is now permanently in the Code Cache via BufferBlob
  address stub_addr = stub_buffer.insts_begin();
  
  // Register this stub for patching when the current method (caller) is compiled
  // When the current method is compiled, we'll know its x28 offset and can patch this stub
  register_stub_for_patching(current_method, stub_addr);
  
  if (YuhuTraceInstalls) {
    tty->print_cr("Yuhu: Generated static call stub at " PTR_FORMAT " for target method %s (from caller %s)",
                  p2i(stub_addr), target_method->name()->as_utf8(), current_method->name()->as_utf8());
    tty->print_cr("  Method* = " PTR_FORMAT ", _from_compiled_offset = %d",
                  p2i(method_ptr), Method::from_compiled_offset());
  }
  
  return stub_addr;
}

// Register a stub that needs patching when the target method is compiled
void YuhuCompiler::register_stub_for_patching(ciMethod* target_method, address stub_addr) {
  // Find if we already have entries for this method
  int index = -1;
  for (int i = 0; i < _stub_patch_methods->length(); i++) {
    if (_stub_patch_methods->at(i) == target_method) {
      index = i;
      break;
    }
  }
  
  if (index == -1) {
    // First stub for this method, create new entry
    _stub_patch_methods->append(target_method);
    GrowableArray<address>* stubs = new (ResourceObj::C_HEAP, mtCompiler) GrowableArray<address>(10, true);
    stubs->append(stub_addr);
    _stub_patch_addresses->append(stubs);
  } else {
    // Add to existing entry
    _stub_patch_addresses->at(index)->append(stub_addr);
  }
  
  if (YuhuTraceInstalls) {
    tty->print_cr("Yuhu: Registered stub " PTR_FORMAT " for patching when %s is compiled",
                  p2i(stub_addr), target_method->name()->as_utf8());
  }
}

// Measure exception handler size (without actually generating code).
// Returns the exact byte size needed for the exception handler.
int YuhuCompiler::measure_exception_handler_size() {
  // Use a temporary buffer on the stack to measure
  const int kTempBufSize = 256;
  char temp_buf[kTempBufSize];
  CodeBuffer temp_cb((address)temp_buf, (CodeBuffer::csize_t)kTempBufSize);
  YuhuMacroAssembler masm(&temp_cb);
  address start = masm.current_pc();

  // nop to ensure the return address points into the code area (same reason as C1)
  masm.write_inst("nop");

  // Load the runtime entry address into a scratch register and call it.
  // Runtime1::handle_exception_from_callee_id expects:
  //   r0 = exception oop, r3 = throwing pc
  address runtime_entry = Runtime1::entry_for(Runtime1::handle_exception_from_callee_id);
  masm.write_insts_mov_imm64(YuhuMacroAssembler::x16, (uint64_t)(uintptr_t)runtime_entry);
  masm.write_inst_blr(YuhuMacroAssembler::x16);

  address end = masm.current_pc();
  return (int)(end - start);
}

int YuhuCompiler::generate_exception_handler(CodeBuffer& cb, int handler_size) {
  tty->print_cr("Yuhu: Generating exception handler stub");

  YuhuMacroAssembler masm(&cb);

  address handler_base = masm.start_a_stub(handler_size);
  assert(handler_base != NULL, "Not enough space for exception handler");

  // Record offset BEFORE the nop -- this is where the handler starts
  address start = masm.current_pc();

  // nop to ensure the return address points into the code area (same reason as C1)
  masm.write_inst("nop");

  // Load the runtime entry address into a scratch register and call it.
  // Runtime1::handle_exception_from_callee_id expects:
  //   r0 = exception oop, r3 = throwing pc
  address runtime_entry = Runtime1::entry_for(Runtime1::handle_exception_from_callee_id);
  masm.write_insts_mov_imm64(YuhuMacroAssembler::x16, (uint64_t)(uintptr_t)runtime_entry);
  masm.write_inst_blr(YuhuMacroAssembler::x16);

  masm.end_a_stub();

  address end = masm.current_pc();
  return (int)(end - start);
}

// Generate unwind handler for propagating exceptions to callers
// This is required by JVM for all compiled methods, even those without try-catch
int YuhuCompiler::measure_unwind_handler_size(int frame_size_in_bytes) {
    // Use a temporary buffer on the stack to measure.
    // frame_offset_bytes is NOT known at measure time (it depends on the LLVM prologue of
    // the specific method being compiled).  We use the worst-case SUB instruction here to
    // ensure we reserve enough stub space; the actual value is patched at
    // generate_unwind_handler() time.
    //
    // Synchronized methods: monitor exit requires C1-style MonitorExitStub infrastructure
    // that yuhu does not yet implement.  Synchronized methods are therefore not supported
    // through this path; they will fall back to the interpreter or trigger an assert at
    // compile time before reaching this code.
    const int kTempBufSize = 256;
    char temp_buf[kTempBufSize];
    CodeBuffer temp_cb((address)temp_buf, (CodeBuffer::csize_t)kTempBufSize);
    YuhuMacroAssembler masm(&temp_cb);
    address start = masm.current_pc();

    // 1. Preserve exception oop in x19 across the frame teardown.
    //    (x19 is a callee-saved register; the caller already saved it, so we can use it here.)
    masm.write_inst("mov x19, x0");

    // 2. Load exception oop from JavaThread and clear TLS fields.
    masm.write_inst_ldr(YuhuMacroAssembler::x0, YuhuAddress(YuhuMacroAssembler::x28, JavaThread::exception_oop_offset()));
    masm.write_inst_str(YuhuMacroAssembler::xzr, YuhuAddress(YuhuMacroAssembler::x28, JavaThread::exception_oop_offset()));
    masm.write_inst_str(YuhuMacroAssembler::xzr, YuhuAddress(YuhuMacroAssembler::x28, JavaThread::exception_pc_offset()));

    // 3. Remove frame: restore SP
    //    then pop x29/x30 (ldp, 4 bytes).
    //    Actual imm is filled in by generate_unwind_handler().
    masm.write_insts_remove_frame(frame_size_in_bytes);

    // 4. Jump to Runtime1::unwind_exception_id.
    //    At that point: r0 = exception oop, lr = caller's return address.
    address runtime_entry = Runtime1::entry_for(Runtime1::unwind_exception_id);
    masm.write_insts_mov_imm64(YuhuMacroAssembler::x16, (uint64_t)(uintptr_t)runtime_entry);
    masm.write_inst_br(YuhuMacroAssembler::x16);

    address end = masm.current_pc();
    return (int)(end - start);
}

// Generate unwind handler for propagating exceptions to callers.
// This is required by the JVM for all compiled methods, even those without try-catch.
//
// frame_size_in_bytes: the complete yuhu frame size in bytes (frame_size * wordSize).
//
// Register protocol on entry (set by exception_handler_for_pc or trap dispatch):
//   x28 (rthread) = JavaThread* with exception_oop / exception_pc filled in
//
// Register protocol on exit to Runtime1::unwind_exception_id:
//   x0 = exception oop, lr = caller's return address (used to find the caller's handler)
int YuhuCompiler::generate_unwind_handler(CodeBuffer& cb, int frame_size_in_bytes) {
    tty->print_cr("Yuhu: Generating unwind handler (frame_size_in_bytes=%d)", frame_size_in_bytes);

    YuhuMacroAssembler masm(&cb);

    address start = masm.current_pc();

    // 1. Preserve exception oop in x19 across the frame teardown.
    //    x19 is callee-saved so it is safe to clobber here — the caller-saved copy
    //    lives on the stack (the LLVM prologue spilled it), but we are about to drop the
    //    frame anyway, so there is nothing left to restore.
    masm.write_inst("mov x19, x0");

    // 2. Load exception oop from JavaThread and clear TLS fields.
    //    Runtime1::generate_unwind_exception asserts these are empty on entry.
    masm.write_inst_ldr(YuhuMacroAssembler::x0, YuhuAddress(YuhuMacroAssembler::x28, JavaThread::exception_oop_offset()));
    masm.write_inst_str(YuhuMacroAssembler::xzr, YuhuAddress(YuhuMacroAssembler::x28, JavaThread::exception_oop_offset()));
    masm.write_inst_str(YuhuMacroAssembler::xzr, YuhuAddress(YuhuMacroAssembler::x28, JavaThread::exception_pc_offset()));

    // Synchronized methods: monitor exit (unlock_object) is not yet implemented in yuhu.
    // Synchronized methods must not reach this handler; they are rejected by
    // YuhuCompiler::can_compile_method() or will hit an assertion earlier.

    // 3. Remove frame: mirror the LLVM epilogue sequence.
    masm.write_insts_remove_frame(frame_size_in_bytes);

    // 4. Jump to Runtime1::unwind_exception_id.
    //    That stub uses lr to call SharedRuntime::exception_handler_for_return_address,
    //    which walks the caller's frame to locate its exception handler.
    //    On return it does: br <handler_addr>  with  x0=exception_oop, x3=throwing_pc.
    address runtime_entry = Runtime1::entry_for(Runtime1::unwind_exception_id);
    masm.write_insts_mov_imm64(YuhuMacroAssembler::x16, (uint64_t)(uintptr_t)runtime_entry);
    masm.write_inst_br(YuhuMacroAssembler::x16);

    address end = masm.current_pc();

    return (int)(end - start);
}

// Measure deopt handler size (without actually generating code).
// Returns the exact byte size needed for the deopt handler.
int YuhuCompiler::measure_deopt_handler_size() {
  // Use a temporary buffer on the stack to measure
  const int kTempBufSize = 256;
  char temp_buf[kTempBufSize];
  CodeBuffer temp_cb((address)temp_buf, (CodeBuffer::csize_t)kTempBufSize);
  YuhuMacroAssembler masm(&temp_cb);
  address start = masm.current_pc();

  // nop to ensure the return address points into the code area (same reason as C1)
  masm.write_inst("nop");

  // adr lr, . -- set lr to the current PC (this is the "return address" for deopt)
  masm.write_inst_adr(YuhuMacroAssembler::lr, masm.current_pc());

  // Load deopt unpack address and jump
  address deopt_unpack = SharedRuntime::deopt_blob()->unpack();
  masm.write_insts_mov_imm64(YuhuMacroAssembler::x16, (uint64_t)(uintptr_t)deopt_unpack);
  masm.write_inst_br(YuhuMacroAssembler::x16);

  address end = masm.current_pc();
  return (int)(end - start);
}

int YuhuCompiler::generate_deopt_handler(CodeBuffer& cb, int handler_size) {
  tty->print_cr("Yuhu: Generating deopt handler stub");

  YuhuMacroAssembler masm(&cb);

  address handler_base = masm.start_a_stub(handler_size);
  assert(handler_base != NULL, "Not enough space for deopt handler");

  // Record offset BEFORE the nop -- this is where the handler starts
  address start = masm.current_pc();

  // nop to ensure the return address points into the code area (same reason as C1)
  masm.write_inst("nop");

  // adr lr, . -- set lr to the current PC (this is the "return address" for deopt)
  masm.write_inst_adr(YuhuMacroAssembler::lr, masm.current_pc());

  // Load deopt unpack address and jump
  address deopt_unpack = SharedRuntime::deopt_blob()->unpack();
  masm.write_insts_mov_imm64(YuhuMacroAssembler::x16, (uint64_t)(uintptr_t)deopt_unpack);
  masm.write_inst_br(YuhuMacroAssembler::x16);

  masm.end_a_stub();

  address end = masm.current_pc();
  return (int)(end - start);
}

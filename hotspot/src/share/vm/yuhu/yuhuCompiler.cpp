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
#include "yuhu/yuhuORCPlugins.hpp"
#include "yuhu/yuhuTracingIRCompiler.hpp"
#include "yuhu/yuhuBuilder.hpp"
#include "yuhu/yuhuCodeBuffer.hpp"
#include "yuhu/yuhuCompiler.hpp"
#include "yuhu/yuhuContext.hpp"
#include "yuhu/yuhuEntry.hpp"
#include "yuhu/yuhuFunction.hpp"
#include "yuhu/yuhuDebugInformationRecorder.hpp"
#include "yuhu/yuhuNativeWrapper.hpp"
#include "yuhu/yuhuPrologueAnalyzer.hpp"
#include "yuhu/yuhu_globals.hpp"
#include "utilities/debug.hpp"

// Forward declaration of gc_safepoint_poll from yuhuRuntime.cpp
extern "C" void gc_safepoint_poll();
extern "C" void handle_deoptimization();
#include "asm/yuhu/yuhu_macroAssembler.hpp"
#include "code/codeCache.hpp"
#include "oops/method.hpp"
#include "c1/c1_Runtime1.hpp"
#include "runtime/sharedRuntime.hpp"
#include "yuhu/yuhuIRTransformer.hpp"
#include "yuhu/yuhuRuntime.hpp"

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

struct YuhuCompiler::Impl {
    // Each thread generating IR needs its own context.  The normal
    // context is used for bytecode methods, and is protected from
    // multiple simultaneous accesses by being restricted to the
    // compiler thread.  The native context is used for JNI methods,
    // and is protected from multiple simultaneous accesses by the
    // adapter handler library lock.
private:
    YuhuContext* _normal_context;
    YuhuContext* _native_context;
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

public:
    Impl() {
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

                // Set DataLayout for both modules BEFORE creating ExecutionEngine
                _normal_context->module()->setDataLayout(DLStr);
                _native_context->module()->setDataLayout(DLStr);

                // DEBUG: Verify DataLayout was set
                std::string verify1 = _normal_context->module()->getDataLayout().getStringRepresentation();
                std::string verify2 = _native_context->module()->getDataLayout().getStringRepresentation();
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

        JTMB.addFeatures({"+reserve-x19", "+reserve-x20", "+reserve-x21", "+reserve-x22", "+reserve-x23",
                          "+reserve-x24", "+reserve-x25", "+reserve-x26", "+reserve-x27", "+reserve-x28"});

        // CRITICAL: Reserve x28 (Thread*) and x12 (Method*) registers
        // This prevents LLVM from using these registers in generated code
        // Method 1: Try to set reserved registers through TargetMachine options
        llvm::TargetOptions Options;
        // TODO: Set Options to reserve specific registers if API is available

        // Create custom ObjectLinkingLayer with plugins
        auto CreateObjectLinkingLayer = [&](orc::ExecutionSession &ES, const llvm::Triple &TT) {
            auto MemMgr = std::make_unique<jitlink::InProcessMemoryManager>(sysconf(_SC_PAGESIZE));
            auto Layer = std::make_unique<orc::ObjectLinkingLayer>(ES, std::move(MemMgr));

            // Add GOTAndPLTHandlerPlugin to fix edge before we patch
//    Layer->addPlugin(std::make_unique<GOTAndPLTHandlerPlugin>());

            // Add CallSiteExtractorPlugin to scan for virtual address placeholders and update return pc addresses
            Layer->addPlugin(std::make_unique<CallSiteExtractorPlugin>());

            // Add MachineCodePrinterPlugin to trace generated machine code
//    Layer->addPlugin(std::make_unique<MachineCodePrinterPlugin>());

            return Layer;
        };

        // Configure ORC JIT with custom ObjectLinkingLayer and TracingIRCompiler
        auto JIT = llvm::orc::LLJITBuilder()
                .setJITTargetMachineBuilder(JTMB)
                .setObjectLinkingLayerCreator(CreateObjectLinkingLayer)
                .setCompileFunctionCreator(
                        [](llvm::orc::JITTargetMachineBuilder JTMB)
                                -> llvm::Expected<std::unique_ptr<llvm::orc::IRCompileLayer::IRCompiler>> {
                            // 获取 mangling options
                            auto MO = llvm::orc::irManglingOptionsFromTargetOptions(JTMB.getOptions());

                            // 1. 创建默认的 ConcurrentIRCompiler
                            auto DefaultCompiler = std::make_unique<llvm::orc::ConcurrentIRCompiler>(std::move(JTMB));

                            // 2. 用 TracingIRCompiler 包装它
                            return std::make_unique<TracingIRCompiler>(std::move(DefaultCompiler), std::move(MO));
                        })
                .create();

        if (!JIT) {
            std::string ErrMsg;
            llvm::handleAllErrors(JIT.takeError(), [&](const llvm::ErrorInfoBase &EIB) {
                ErrMsg = EIB.message();
            });
            fatal(err_msg("Failed to create LLJIT: %s", ErrMsg.c_str()));
        }
        (*JIT)->getIRTransformLayer().setTransform(YuhuIRTransformer::runGCPasses);

        _jit = std::move(*JIT);

        // Add DynamicLibrarySearchGenerator so ORC JIT can resolve symbols from
        // the current process (e.g. yuhu_resolve_static_field in libjvm.dylib)
        auto &MainJD = _jit->getMainJITDylib();
        auto DLSGOrErr = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
                _jit->getDataLayout().getGlobalPrefix());
        if (!DLSGOrErr) {
            std::string ErrMsg;
            llvm::handleAllErrors(DLSGOrErr.takeError(), [&](const llvm::ErrorInfoBase &EIB) {
                ErrMsg = EIB.message();
            });
            fatal(err_msg("Failed to create DynamicLibrarySearchGenerator: %s", ErrMsg.c_str()));
        }
        MainJD.addGenerator(std::move(*DLSGOrErr));

        llvm::orc::SymbolMap SymMap;
        auto& ES = _jit->getExecutionSession();
        SymMap[ES.intern("_gc.safepoint_poll")] =
                llvm::orc::ExecutorSymbolDef(
                        llvm::orc::ExecutorAddr::fromPtr(&gc_safepoint_poll),
                        llvm::JITSymbolFlags::Callable);
        SymMap[ES.intern("___llvm_deoptimize")] =
                llvm::orc::ExecutorSymbolDef(
                        llvm::orc::ExecutorAddr::fromPtr(&handle_deoptimization),
                        llvm::JITSymbolFlags::Callable);
        auto symErr = MainJD.define(llvm::orc::absoluteSymbols(std::move(SymMap)));
        if (symErr) {
            // handle symErr
            llvm::handleAllErrors(std::move(symErr), [](const llvm::ErrorInfoBase& EIB) {
                fatal(err_msg("Error defining symbol: %s", EIB.message().c_str()));
            });
        }

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
    }

    YuhuContext* context() const {
        if (JavaThread::current()->is_Compiler_thread()) {
            return _normal_context;
        }
        else {
            assert(AdapterHandlerLibrary_lock->owned_by_self(), "should be");
            return _native_context;
        }
    }

    Monitor* execution_engine_lock() const {
        return _execution_engine_lock;
    }

    llvm::orc::LLJIT* jit() const {
        assert(execution_engine_lock()->owned_by_self(), "should be");
        return _jit.get();
    }

    llvm::orc::LLJIT* jit_without_lock() const {
        return _jit.get();
    }

    void free_queued_methods() {
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

    void generate_native_code(llvm::orc::ResourceTrackerSP RT, YuhuEntry* entry,
                              Function*   function,
                              const char* name) {
        // No need to set field table - runtime helper uses CP index directly.

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

        // Check Module function count before add_function
        llvm::Module* mod_before = function->getParent();
        int func_count_before = 0;
        if (mod_before != NULL) {
            for (llvm::Module::iterator I = mod_before->begin(), E = mod_before->end(); I != E; ++I) {
                func_count_before++;
            }
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
        }

        // Debug 1: Verify Function is in Module
        llvm::Module* func_mod = function->getParent();
        if (func_mod == NULL) {
            fatal(err_msg("Function %s has no parent Module!", name));
        }

        // Debug 2: Verify Module pointer validity
        llvm::Module* normal_mod = _normal_context->module();
        llvm::Module* native_mod = _native_context->module();

        // Debug: Collect all instructions and verify they are in basic blocks
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
                    orphaned_count++;
                    continue;
                }

                // Check if instruction's getFunction() returns valid value
                // Only check if parent is valid (getFunction() requires valid parent)
                if (parent != NULL) {
                    // getFunction() is implemented as: getParent()->getParent()
                    // So if parent is valid, we can safely call getFunction()
                    llvm::Function* func = parent->getParent();
                    if (func == NULL) {
                        orphaned_count++;
                    } else if (func != function) {
                        orphaned_count++;
                    } else {
                        // Also try calling inst->getFunction() directly to see if it matches
                        // This is the actual method that verifier calls, and it might crash if parent is invalid
                        llvm::Function* direct_func = NULL;
                        bool getFunction_succeeded = false;
                        // We can't use try-catch for LLVM internal methods, so we'll just call it
                        // If it crashes, we'll see it in the crash log
                        direct_func = inst->getFunction();
                        if (direct_func != function && YuhuTraceFunction) {
                            tty->print_cr("  WARNING: Instruction %p getFunction() returns different value!", inst);
                            tty->print_cr("    Via parent: %p (%s)", func, func->getName().str().c_str());
                            tty->print_cr("    Direct: %p (%s)", direct_func, direct_func ? direct_func->getName().str().c_str() : "NULL");
                            tty->print_cr("    Expected: %p (%s)", function, function->getName().str().c_str());
                        }
                    }
                }
            }
        }

        if (orphaned_count > 0 && YuhuTraceInstalls) {
            tty->print_cr("  ERROR: Found %d orphaned or invalid instructions!", orphaned_count);
            tty->flush();
            // Don't fatal here, let verifyFunction catch it
        } else if (YuhuTraceInstalls) {
            tty->print_cr("  All instructions are properly in basic blocks");
        }
        tty->flush();

        // Output IR to file for analysis (before verification)
        // This allows using LLVM tools like 'opt -verify' to analyze the IR
        std::string ir_filename = std::string("/tmp/yuhu_ir_") + std::string(name) + ".ll";
        if (YuhuDumpIRToFile) {
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
        }

        // Debug 4: Verify IR correctness (不需要锁)
        if (llvm::verifyFunction(*function, &llvm::errs())) {
            tty->print_cr("Yuhu: IR verification failed! See %s for the IR", ir_filename.c_str());
            tty->print_cr("Yuhu: Run 'opt -verify %s' to get detailed error messages", ir_filename.c_str());
            fatal(err_msg("Function %s failed IR verification!", name));
        }

        // ========== End of 锁外调试代码 ==========

        {
            MutexLocker locker(execution_engine_lock());

            // ========== Debug 3: 锁内的检查（需要访问 jit()）==========
            // ORC JIT: Check JIT state
            if (!jit()) {
                fatal(err_msg("ORC JIT is NULL!"));
            }

            // Diagnostic: ORC JIT uses default memory management (stage 1)
            // MemoryManager state will be available in stage 2 after CodeCache integration
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

            // Debug: Try to get mangled name
            auto MangledName = jit()->mangle(func_name);

            YuhuDebugInformationRecorder::get()->set_mangled_func_name(MangledName);

            // Try to lookup the function
            auto Sym = jit()->lookup(func_name);
            if (!Sym) {
                // Function not found - may need to add module

                // Clone the module using the SAME context (not a new one)
                // This preserves metadata nodes and register name information
                std::unique_ptr<llvm::Module> module_clone(
                        llvm::CloneModule(*func_mod).release());

                // Debug: Check if cloned module contains the function
                llvm::Function* cloned_func = module_clone->getFunction(func_name);
                if (cloned_func == NULL) {
                    fatal(err_msg("Cloned module missing function %s", name));
                }

                // CRITICAL: Create ThreadSafeContext using the SAME LLVMContext as the original module
                // This ensures metadata is preserved and register names are validated correctly
                // However, we cannot move from module_ctx (it's a reference to the module's context)
                // Instead, we need to share the context or use a different approach
                // For now, create a new context but ensure the cloned module uses the original context
                // The cloned module should already have the correct context from CloneModule
                auto TSCtx = std::make_unique<llvm::orc::ThreadSafeContext>(
                        std::make_unique<llvm::LLVMContext>());

                // No need to patch anything - runtime helper will resolve fields dynamically.

                auto TSM = llvm::orc::ThreadSafeModule(
                        std::move(module_clone), *TSCtx);

                // Add to JITDylib
                auto &JD = jit()->getMainJITDylib();

                if (auto Err = jit()->addIRModule(RT, std::move(TSM))) {
                    std::string ErrMsg;
                    llvm::handleAllErrors(std::move(Err), [&](const llvm::ErrorInfoBase &EIB) {
                        ErrMsg = EIB.message();
                    });
                    fatal(err_msg("Failed to add IR module for function %s: %s", name, ErrMsg.c_str()));
                }

                // oop resolution already done above (before addIRModule).

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
        }

        if (code == NULL) {
            fatal(err_msg("ORC JIT lookup returned NULL for %s. Check debug output above for details.", name));
        }

        // For ORC JIT stage 1, we don't have CodeCache integration yet
        // Use code address directly (will be fixed in stage 2)
        size_t code_size = YuhuDebugInformationRecorder::get()->get_func_size();
        if (YuhuTraceInstalls) {
            tty->print_cr("ORC JIT: Using code address directly (stage 1 - no CodeCache integration yet)");
            tty->print_cr("ORC JIT: code=%p (size will be determined later)", code);
            tty->print_cr("ORC JIT: code_size=%d", code_size);
        }
        entry->set_entry_point(code);
        // use func size to calculate code limit
        entry->set_code_limit((address)(code + code_size));

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

    void eraseFunctions() {
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
    }
};

YuhuCompiler::YuhuCompiler()
  : AbstractCompiler(), _p_impl(std::make_unique<Impl>()) {

  // Initialize VM call RuntimeStubs
  YuhuRuntime::initialize_vm_stubs();

  // All done
  set_state(initialized);
}

YuhuCompiler::~YuhuCompiler() {
  // std::unique_ptr<llvm::orc::LLJIT> will automatically clean up
  // No explicit cleanup needed
}

YuhuContext* YuhuCompiler::context() const {
    return _p_impl->context();
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

bool YuhuCompiler::need_stack_bang(int frame_size_in_bytes) {
    // For Yuhu:
    // 1. stub_function() is always NULL (Yuhu doesn't generate stubs)
    // 2. has_java_calls() = any java_call in call site entries
    // 3. frame_size > page_size/8 = frame larger than 512 bytes (assuming 4KB page)
    bool has_java_calls = YuhuDebugInformationRecorder::get()->has_java_call_sites();

    return (UseStackBanging &&
            (has_java_calls || frame_size_in_bytes > os::vm_page_size() >> 3));
}

// Measure adapter size for normal (non-OSR) method compilation.
// Returns the exact byte size needed for the parameter adapter stub.
int YuhuCompiler::measure_normal_adapter_size(int frame_size_in_bytes) {
    const int kAdapterBufSize = 64;
    char adapter_buf[kAdapterBufSize];
    CodeBuffer temp_cb((address)adapter_buf, (CodeBuffer::csize_t)kAdapterBufSize);

    YuhuMacroAssembler masm(&temp_cb);
    address start = masm.current_pc();

    YuhuLabel label;

    masm.write_inst_b(label);

    masm.pin_label(label);

    if (need_stack_bang(frame_size_in_bytes)) {
        masm.write_insts_generate_stack_overflow_check(frame_size_in_bytes);
    }

    // padding instruction in case it doesn't generate stack overflow check instructions
    masm.write_inst("nop");

    address end = masm.current_pc();
    return (int)(end - start);
}

int YuhuCompiler::generate_normal_adapter_into(CodeBuffer& cb, address* verified_entry_point, int frame_size_in_bytes) {
  YuhuMacroAssembler masm(&cb);
  address start = masm.current_pc();

  YuhuLabel label;

  masm.write_inst_b(label);

  masm.pin_label(label);

  *verified_entry_point = masm.current_pc();

  if (need_stack_bang(frame_size_in_bytes)) {
      masm.write_insts_generate_stack_overflow_check(frame_size_in_bytes);
  }

  // padding instruction in case it doesn't generate stack overflow check instructions
  masm.write_inst("nop");

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
    if (YuhuTraceOsrCompilation) {
        tty->print_cr("Yuhu: SKIPPING OSR compilation for %s (entry_bci=%d) - normal-only test mode",
                      methodname(target->holder()->name()->as_utf8(), target->name()->as_utf8()),
                      entry_bci);
    }
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
    if (strcmp(target->holder()->name()->as_utf8(), "java/lang/Math") == 0
        && strcmp(target->name()->as_utf8(), "min") == 0
        && strcmp(target->signature()->as_symbol()->as_utf8(), "(II)I") == 0) {
        assert(true, "just checking");
    }
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
        if (strcmp(target->holder()->name()->as_utf8(), "sun/nio/cs/UTF_8$Encoder") == 0
            && strcmp(target->name()->as_utf8(), "encodeArrayLoop") == 0
            && strcmp(target->signature()->as_symbol()->as_utf8(), "(Ljava/nio/CharBuffer;Ljava/nio/ByteBuffer;)Ljava/nio/charset/CoderResult;") == 0) {
          break;
        }
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

    _p_impl->eraseFunctions();

  // Emit the entry point
  YuhuEntry *entry = (YuhuEntry *) cb.malloc(sizeof(YuhuEntry));
  size_t llvm_code_size = 0;
  size_t effective_code_size = 0;

    // Initialize thread-local debug information recorder
    YuhuDebugInformationRecorder::initialize_tls();
    YuhuDebugInformationRecorder* recorder = YuhuDebugInformationRecorder::get();
    // Set the LLVM module reference for embedding metadata later
    recorder->set_module(YuhuContext::current().module());

  Function *function = YuhuFunction::build(env, &builder, flow, func_name);
  if (env->failing()) {
    tty->print_cr("Yuhu: compile failing during IR build for %s (func_name=%s) entry_bci=%d comp_level=%d",
                  base_name, func_name, entry_bci, env->comp_level());
    return;
  }
  
  // NEW: Embed call site mappings as metadata before compilation
  // This allows the JITLink plugin to extract virtual address mappings
  YuhuDebugInformationRecorder::get()->embed_call_site_metadata();

  llvm::orc::ResourceTrackerSP RT = _p_impl->jit_without_lock()->getMainJITDylib().createResourceTracker();

  // Generate native code.  It's unpleasant that we have to drop into
  // the VM to do this -- it blocks safepoints -- but I can't see any
  // other way to handle the locking.
  {
    ThreadInVMfromNative tiv(JavaThread::current());
    _p_impl->generate_native_code(RT, entry, function, func_name);

    address code_start = entry->code_start();
    llvm_code_size = entry->code_limit() - code_start;
    // the code size is exactly calculated in ORC plugins
    effective_code_size = llvm_code_size;
  }

  bool is_osr = (entry_bci != InvocationEntryBci);
  int adapter_size = 0;
  
  // Step 1: Analyze LLVM prologue to get actual stack space used
  address llvm_code_start = entry->code_start();
  int actual_prologue_bytes = YuhuPrologueAnalyzer::analyze_prologue_stack_bytes(llvm_code_start);
  int actual_prologue_words = actual_prologue_bytes / wordSize;

  // Step 3: Calculate final frame_size using actual prologue size
//  int frame_size = frame_words + locals_words + actual_prologue_words;
  int frame_size = actual_prologue_words;
  // CRITICAL: Align frame_size to 2 words (16 bytes) to match yuhuStack.cpp
  // yuhuStack.cpp uses align_size_up(frame_size_bytes, 16), so we must align here too
  frame_size = align_size_up(frame_size, 2);

  // Install the method into the VM
  CodeOffsets offsets;
  offsets.set_value(CodeOffsets::Deopt,       0);  // will be updated per path below
  offsets.set_value(CodeOffsets::Exceptions,  0);  // will be updated per path below

  ExceptionHandlerTable handler_table;
  ImplicitExceptionTable inc_table;

  if (!is_osr) {
    // Normal method: build adapter + LLVM code into a combined CodeCache blob.
    // The adapter rearranges parameters from i2c adapter format to Yuhu's expected format.
    adapter_size = measure_normal_adapter_size(actual_prologue_bytes);
    assert(adapter_size > 0 && adapter_size < 512, "adapter size sanity");

      // Measure exception handler and deopt handler sizes first
    int unwind_handler_size = measure_unwind_handler_size(frame_size * wordSize);
    int exc_handler_size  = measure_exception_handler_size();
    int deopt_handler_size = measure_deopt_handler_size();

    size_t combined_size = adapter_size + effective_code_size + unwind_handler_size;

    // Create CodeBuffer that manages its own BufferBlob internally
    CodeBuffer combined_cb("yuhu-normal-combined", (int)combined_size, (int)(combined_size * 0.15));
    if (combined_cb.blob() == NULL) {
      fatal(err_msg("YuhuCompiler::compile_method: failed to allocate combined CodeBuffer (size=%zu)", combined_size));
    }
    combined_cb.initialize_stubs_size( exc_handler_size + deopt_handler_size);

    address combined_base = combined_cb.insts_begin();

    // Emit adapter into combined buffer with correct addresses.
    // Use direct jump (pass NULL for llvm_label) to avoid patching complexity.
    address verified_entry_point;
    int emitted_adapter = generate_normal_adapter_into(combined_cb, &verified_entry_point, actual_prologue_bytes);
    assert(emitted_adapter == adapter_size, "adapter size mismatch");
    assert(verified_entry_point != NULL, "verified entry point must have valid address");

    // Copy LLVM code after adapter.
    // Only copy effective code (excluding trailing udf #0 padding)
    memcpy(combined_base + adapter_size, entry->code_start(), effective_code_size);

      // Generate unwind handler (always needed - JVM requires it for exception propagation)
      // This is different from exception handler - unwind handler propagates exceptions upward.
      // frame_size_in_bytes is the complete yuhu frame size in bytes, used to restore SP
      // before jumping to unwind_exception_id.
      combined_cb.insts()->set_end(combined_base + adapter_size + effective_code_size);
      generate_unwind_handler(combined_cb, frame_size * wordSize);

      // CRITICAL: Fix up epilogue markers (0xcafebabe -> sub sp, x29, #imm)
      // This is needed to restore SP from x29 before returning from the function.
      // See doc/yuhu/activities/039_epilogue_sp_restoration.md for details.
//      builder.fixup_prologue_epilogue_markers(combined_base, combined_size);
      
      // Scan for oop markers and generate relocation records
      // This must be done AFTER fixup_prologue_epilogue_markers because both operations
      // scan the machine code and modify it. The oop marker scanning generates HotSpot
      // relocation records that will be processed during register_method.
      combined_cb.initialize_oop_recorder(env->oop_recorder());
//      builder.scan_for_oop_markers_and_generate_relocation(&combined_cb, combined_base, combined_size);
      builder.scan_and_generate_all_relocations(entry->code_start(), effective_code_size, &combined_cb, combined_base, adapter_size);

    // Extend instruction section to cover adapter + LLVM code.
    combined_cb.insts()->set_end(combined_base + combined_size);

      // Generate exception handler stub only if there might be landing pads
      // TODO: Check LLVM IR for landing pads instead of always generating
      if (exc_handler_size >= 0) {
          generate_exception_handler(combined_cb, exc_handler_size);
          // TODO: Properly handle exception handlers for each landing pad
          // For now, DO NOT register any exception handlers to avoid infinite loop
      }

    // Generate deopt handler (always needed)
    generate_deopt_handler(combined_cb, deopt_handler_size);

      if (target->is_static()) {
          // Static methods: both entry points point to adapter (no class check needed)
          offsets.set_value(CodeOffsets::Entry, 0);
          offsets.set_value(CodeOffsets::Verified_Entry, 0);
      } else {
          // Non-static methods: Entry points to adapter (for class check),
          // Verified_Entry points to LLVM code (skip class check)
          offsets.set_value(CodeOffsets::Entry, 0);
          offsets.set_value(CodeOffsets::Verified_Entry, (int)(verified_entry_point - combined_base));
      }

    offsets.set_value(CodeOffsets::UnwindHandler, adapter_size + effective_code_size);
    offsets.set_value(CodeOffsets::Exceptions, 0);
    offsets.set_value(CodeOffsets::Deopt,       exc_handler_size);

    // Register oop map
    YuhuDebugInformationRecorder::get()->convert_and_add_to_real_recorder(env->debug_info(), target, adapter_size, frame_size);
    
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

    if (YuhuTraceInstalls) {
        tty->print_cr("Yuhu: Register method %s successfully, nmethod: code_begin=%p, code_end=%p", func_name,
                      target->get_Method()->code()->code_begin(), target->get_Method()->code()->code_end());
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
//      YuhuDebugInfo::generate_minimal_debug_info(debug_info, target, frame_size);
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
    // Release thread-local debug information recorder after compilation is complete
    // This frees the C heap memory and clears the TLS slot
    YuhuDebugInformationRecorder::release();
    if (auto Err = RT->remove()) {
        llvm::consumeError(std::move(Err));
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

    llvm::orc::ResourceTrackerSP  RT = _p_impl->jit_without_lock()->getMainJITDylib().createResourceTracker();

    // Generate native code
    _p_impl->generate_native_code(RT, entry, wrapper->function(), name);

    if (auto Err = RT->remove()) {
        llvm::consumeError(std::move(Err));
    }

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

void YuhuCompiler::release_last_code_blob_unlocked() {
    // Note: ORC JIT uses default memory management in stage 1
    // MemoryManager integration will be added in stage 2
    // For now, this is a no-op
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

// Measure exception handler size (without actually generating code).
// Returns the exact byte size needed for the exception handler.
int YuhuCompiler::measure_exception_handler_size() {
  // Use a temporary buffer on the stack to measure
  const int kTempBufSize = 64;
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
    if (YuhuTraceInstalls) {
        tty->print_cr("Yuhu: Generating exception handler stub");
    }

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
    const int kTempBufSize = 64;
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
    if (YuhuTraceInstalls) {
        tty->print_cr("Yuhu: Generating unwind handler (frame_size_in_bytes=%d)", frame_size_in_bytes);
    }

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
  const int kTempBufSize = 64;
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
    if (YuhuTraceInstalls) {
        tty->print_cr("Yuhu: Generating deopt handler stub");
    }

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

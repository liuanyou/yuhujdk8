/*
 * Copyright (c) 2024, Yuhu Compiler Project
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only.
 */

#include "precompiled.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/yuhuIRTransformer.hpp"
#include "yuhu/yuhuRewriteStatepointsForGC.hpp"
#include "yuhu/yuhu_globals.hpp"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Transforms/Scalar/PlaceSafepoints.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

void declareGCSafepointPoll(Module& M) {
    LLVMContext& Ctx = M.getContext();
    FunctionType* PollType = FunctionType::get(
            llvm::Type::getVoidTy(Ctx),  // 返回类型 void
            false                  // 不是可变参数
    );

    // 如果不存在，则声明
    if (!M.getFunction("gc.safepoint_poll")) {
        Function::Create(PollType, Function::ExternalLinkage,
                         "gc.safepoint_poll", &M);
    }
}

llvm::Expected<llvm::orc::ThreadSafeModule> YuhuIRTransformer::runGCPasses(llvm::orc::ThreadSafeModule TSM,
                                       llvm::orc::MaterializationResponsibility &MR) {
    TSM.withModuleDo([](Module &M) {
        errs() << "=== Running GC Passes on module: " << M.getName() << " ===\n";

        declareGCSafepointPoll(M);

        // 1. 创建 PassBuilder 和 StandardInstrumentations
        PassInstrumentationCallbacks PIC;
        StandardInstrumentations SI(M.getContext(), true);
        SI.registerCallbacks(PIC);

        PassBuilder PB;

        // 2. 创建各种分析管理器（LLVM 20 的正确方式）
        LoopAnalysisManager LAM;
        FunctionAnalysisManager FAM;
        CGSCCAnalysisManager CGAM;
        ModuleAnalysisManager MAM;

        // 3. 注册分析管理器
        PB.registerLoopAnalyses(LAM);
        PB.registerFunctionAnalyses(FAM);
        PB.registerCGSCCAnalyses(CGAM);
        PB.registerModuleAnalyses(MAM);
        PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

        // 4. 创建 ModulePassManager
        ModulePassManager MPM;

        // 5. 添加 GC 相关的 Pass

        // 可选：添加清理 Pass
        FunctionPassManager FPM;
        FPM.addPass(InstCombinePass());
        FPM.addPass(SimplifyCFGPass());
        // PlaceSafepointsPass inserts gc.safepoint_poll for loops,
        // then RewriteStatepointsForGC converts to gc.statepoint
        FPM.addPass(PlaceSafepointsPass());

        MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));

        MPM.addPass(YuhuRewriteStatepointsForGC());

        // 6. 运行 Pass Manager
        MPM.run(M, MAM);

        // 7. 验证 IR
        if (verifyModule(M, &errs())) {
            errs() << "Module verification failed after GC passes!\n";
        }

        errs() << "=== GC passes completed ===\n";
    });

    return std::move(TSM);
}

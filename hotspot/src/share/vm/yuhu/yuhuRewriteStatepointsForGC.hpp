/*
 * Copyright (c) 2024, Yuhu Compiler Project
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only.
 *
 * YuhuRewriteStatepointsForGC - Custom LLVM pass that wraps function calls
 * with gc.statepoint intrinsics for GC support, while safely skipping
 * inline assembly calls that RS4GC cannot handle.
 */

#ifndef SHARE_VM_YUHU_YUHUREWRITESTATEPOINTSFORGC_HPP
#define SHARE_VM_YUHU_YUHUREWRITESTATEPOINTSFORGC_HPP

#include "yuhu/llvmHeaders.hpp"

/**
 * YuhuRewriteStatepointsForGC
 * 
 * Custom implementation of LLVM's RewriteStatepointsForGC pass that:
 * 1. Processes function calls like the standard RS4GC pass
 * 2. Skips inline assembly calls (which have no callable address)
 * 3. Wraps only "real" function calls with gc.statepoint intrinsics
 * 4. Maintains full GC oop tracking for relocating collectors
 * 
 * Why this is needed:
 * - LLVM's RS4GC crashes on inline asm: "Cannot take the address of an inline asm!"
 * - Yuhu uses inline asm for register access, PC/SP operations, and markers
 * - These inline asm calls don't involve GC pointers and don't need statepoints
 * - By skipping them, we enable full GC support while preserving inline asm
 * 
 * Architecture:
 * - Callee-saved register save/restore moved to stubs (not in LLVM IR)
 * - All remaining inline asm is GC-safe (no heap objects involved)
 * - Regular function calls (to stubs/runtime) get full statepoint wrapping
 * 
 * Usage:
 *   ModulePassManager MPM;
 *   MPM.addPass(YuhuRewriteStatepointsForGC());
 */
//===- RewriteStatepointsForGC.h - ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides interface to "Rewrite Statepoints for GC" pass.
//
// This passe rewrites call/invoke instructions so as to make potential
// relocations performed by the garbage collector explicit in the IR.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/PassManager.h"

namespace llvm {

    class DominatorTree;

    class Function;

    class Module;

    class TargetTransformInfo;

    class TargetLibraryInfo;
}

class YuhuRewriteStatepointsForGC : public llvm::PassInfoMixin<YuhuRewriteStatepointsForGC> {
public:
    llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);

    bool runOnFunction(llvm::Function &F, llvm::DominatorTree &, llvm::TargetTransformInfo &,
                       const llvm::TargetLibraryInfo &);
};

#endif // SHARE_VM_YUHU_YUHUREWRITESTATEPOINTSFORGC_HPP

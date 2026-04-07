/*
 * Copyright (c) 2024, Yuhu Compiler Project
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only.
 */

#ifndef SHARE_VM_YUHU_YUHUTRACINGIRCOMPILER_HPP
#define SHARE_VM_YUHU_YUHUTRACINGIRCOMPILER_HPP

#include "yuhu/llvmHeaders.hpp"

// TracingIRCompiler - Wrapper around IRCompiler to trace IR to object file compilation
// This class prints the input IR and disassembles the output object file
class TracingIRCompiler : public llvm::orc::IRCompileLayer::IRCompiler {
public:
    TracingIRCompiler(std::unique_ptr<llvm::orc::IRCompileLayer::IRCompiler> WrappedCompiler,
                      llvm::orc::IRSymbolMapper::ManglingOptions MO);

    llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> operator()(llvm::Module &M) override;

private:
    std::unique_ptr<llvm::orc::IRCompileLayer::IRCompiler> WrappedCompiler;

    llvm::Error disassembleObjectFile(llvm::MemoryBuffer &ObjBuffer);
};

#endif // SHARE_VM_YUHU_YUHUTRACINGIRCOMPILER_HPP

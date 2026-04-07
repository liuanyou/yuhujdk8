/*
 * Copyright (c) 2024, Yuhu Compiler Project
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only.
 */

#include "precompiled.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/yuhuTracingIRCompiler.hpp"

using namespace llvm;

// TracingIRCompiler implementation
// This class wraps the default IRCompiler to trace compilation

TracingIRCompiler::TracingIRCompiler(std::unique_ptr<orc::IRCompileLayer::IRCompiler> WrappedCompiler,
                                     orc::IRSymbolMapper::ManglingOptions MO)
        : orc::IRCompileLayer::IRCompiler(std::move(MO)),
          WrappedCompiler(std::move(WrappedCompiler)) {}

Expected<std::unique_ptr<MemoryBuffer>> TracingIRCompiler::operator()(Module &M) {
    // 1. 编译前：打印 IR
    errs() << "\n=== TracingIRCompiler: Before Compilation ===\n";
    errs() << "Module: " << M.getName() << "\n";
    M.print(errs(), nullptr);
    errs() << "=== End of IR ===\n\n";

    // 专门查找 sub 指令
    for (auto &F : M) {
        for (auto &BB : F) {
            for (auto &I : BB) {
                if (I.getOpcode() == llvm::Instruction::Sub) {
                    errs() << "Found sub in IR: ";
                    I.print(errs());
                    errs() << "\n";
                    errs() << "  Operand 0: ";
                    I.getOperand(0)->print(errs());
                    errs() << "\n  Operand 1: ";
                    I.getOperand(1)->print(errs());
                    errs() << "\n";
                }
            }
        }
    }

    // 2. 调用真正的编译器（ConcurrentIRCompiler）
    auto ObjBuffer = (*WrappedCompiler)(M);

    if (!ObjBuffer) {
        errs() << "❌ Compilation failed\n";
        return ObjBuffer;
    }

    errs() << "✅ Compiled successfully, size: "
           << (*ObjBuffer)->getBufferSize() << " bytes\n";

    // 可选：反汇编目标文件，检查 sub 指令
    if (auto Err = disassembleObjectFile(**ObjBuffer)) {
        errs() << "Warning: Failed to disassemble\n";
    }

    return ObjBuffer;
}

Error TracingIRCompiler::disassembleObjectFile(MemoryBuffer &ObjBuffer) {
    // 创建 ObjectFile
    auto Obj = object::ObjectFile::createObjectFile(ObjBuffer.getMemBufferRef());
    if (!Obj) {
        return Obj.takeError();
    }

    errs() << "\n=== Object File Disassembly ===\n";

    for (auto &Section : (*Obj)->sections()) {
        if (Section.isText()) {
            auto Name = Section.getName();
            if (Name) {
                errs() << "Section: " << *Name << "\n";
            }

            auto Contents = Section.getContents();
            if (Contents) {
                auto Data = Contents->data();
                auto Size = Contents->size();

                errs() << "Size: " << Size << " bytes\n";
                errs() << "First 64 bytes: ";
                for (size_t i = 0; i < std::min(static_cast<uint64_t>(Size), static_cast<uint64_t>(64)); ++i) {
                    errs() << format_hex_no_prefix(static_cast<unsigned int>(Data[i] & 0xFF), 2);
                    if ((i + 1) % 4 == 0) errs() << " ";
                }
                errs() << "\n";

                errs() << "  Instructions (32-bit):\n";
                for (size_t i = 0; i + 4 <= Size; i += 4) {
                    // 正确读取小端序的32位指令
                    uint32_t instr = 0;
                    for (size_t j = 0; j < 4; j++) {
                        instr |= (static_cast<uint32_t>(Data[i + j] & 0xFF)) << (j * 8);
                    }

                    errs() << "    " << format_hex(instr, 8) << ": ";

                    // 反汇编常见指令
                    if (instr == 0xd65f03c0 || instr == 0xc0035fd6) {
                        errs() << "ret";
                    } else if ((instr & 0xff000000) == 0xcb000000) {
                        // sub 指令
                        uint32_t rd = (instr >> 0) & 0x1f;
                        uint32_t rn = (instr >> 5) & 0x1f;
                        uint32_t rm = (instr >> 16) & 0x1f;
                        errs() << "sub x" << rd << ", x" << rn << ", x" << rm;
                    } else if ((instr & 0xff000000) == 0xaa000000) {
                        // mov (orr) 指令
                        uint32_t rd = (instr >> 0) & 0x1f;
                        uint32_t rn = (instr >> 5) & 0x1f;
                        errs() << "mov x" << rd << ", x" << rn;
                    } else if ((instr & 0xffc00000) == 0xf9400000) {
                        // ldr 指令
                        uint32_t rt = (instr >> 0) & 0x1f;
                        uint32_t rn = (instr >> 5) & 0x1f;
                        uint32_t imm = (instr >> 10) & 0xfff;
                        errs() << "ldr x" << rt << ", [x" << rn << ", #" << imm << "]";
                    } else {
                        errs() << "unknown";
                    }
                    errs() << "\n";
                }
                errs() << "\n";
            }
        }
    }
    errs() << "=== End of Disassembly ===\n\n";

    return Error::success();
}

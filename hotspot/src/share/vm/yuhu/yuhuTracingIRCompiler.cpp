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
#include "yuhu/yuhu_globals.hpp"
#include "yuhu/yuhuDebugInformationRecorder.hpp"
#include "llvm/Object/StackMapParser.h"

using namespace llvm;

// TracingIRCompiler implementation
// This class wraps the default IRCompiler to trace compilation

TracingIRCompiler::TracingIRCompiler(std::unique_ptr<orc::IRCompileLayer::IRCompiler> WrappedCompiler,
                                     orc::IRSymbolMapper::ManglingOptions MO)
        : orc::IRCompileLayer::IRCompiler(std::move(MO)),
          WrappedCompiler(std::move(WrappedCompiler)) {}

Expected<std::unique_ptr<MemoryBuffer>> TracingIRCompiler::operator()(Module &M) {
    // 使用专用开关 YuhuTraceIRCompilation
    bool shouldTrace = YuhuTraceIRCompilation;

    // 1. 编译前：打印 IR（仅在匹配时）
    if (shouldTrace) {
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
    }

    // 2. 调用真正的编译器（ConcurrentIRCompiler）
    auto ObjBuffer = (*WrappedCompiler)(M);

    if (!ObjBuffer) {
        if (shouldTrace) {
            errs() << "❌ Compilation failed\n";
        }
        return ObjBuffer;
    }

    if (shouldTrace) {
        errs() << "✅ Compiled successfully, size: "
               << (*ObjBuffer)->getBufferSize() << " bytes\n";

        // 可选：反汇编目标文件，检查 sub 指令
        if (auto Err = disassembleObjectFile(**ObjBuffer)) {
            errs() << "Warning: Failed to disassemble\n";
        }
    }

    // ObjBuffer is a std::unique_ptr<MemoryBuffer>
    // You can parse it as an object file!

    // Use LLVM's ObjectFile API:
    auto ObjFile = llvm::object::ObjectFile::createObjectFile(
            (*ObjBuffer)->getMemBufferRef());

    if (!ObjFile) {
        return ObjBuffer;
    }

    // 1. 找到 __LLVM_StackMaps 段
    for (auto &Section : (*ObjFile)->sections()) {
        auto NameOrErr = Section.getName();
        if (!NameOrErr) {
            continue;
        }
        errs() << "Section: " << *NameOrErr << "\n";
        // Section.getName() returns in format of segment,section eg
        // Section: __TEXT,__text
        // Section: $__GOT
        // Section: __LD,__compact_unwind
        // Section: __LLVM_STACKMAPS,__llvm_stackmaps
        // Section: __TEXT,__lcl_macho_hdr
        // Section: __TEXT,__unwind_info
        if (!Section.getName()->ends_with("__llvm_stackmaps")) continue;
        errs() << "Size: " << Section.getSize() << "\n";
        auto ContentOrErr = Section.getContents();
        if (!ContentOrErr) {
            continue;
        }
        auto Content = *ContentOrErr;
        const uint8_t* Data = reinterpret_cast<const uint8_t*>(Content.data());
        size_t Size = Content.size();

        if (Size < 8) continue;

        // 2. 使用 LLVM 的 StackMapParser
        // 注意：使用 llvm::support::endianness::little 或 big
        using StackMapParser = llvm::StackMapParser<endianness::little>;

        StackMapParser Parser(llvm::ArrayRef<uint8_t>(Data, Size));

        // 4. 遍历该函数中的所有 statepoint 记录
        for (auto StatepointRecord : Parser.records()) {
            uint64_t StatepointID = StatepointRecord.getID();
            uint32_t InstructionOffset = StatepointRecord.getInstructionOffset();
            errs() << "[StackMap] ID: " << StatepointID << " , InstructionOffset: " << InstructionOffset
                    << ", Locations: " << StatepointRecord.getNumLocations() << " , Liveouts: " << StatepointRecord.getNumLiveOuts() << "\n";
            if (YuhuTraceMachineCode) {
                errs() << "[StackMap]   Statepoint at offset: " << InstructionOffset << " , ID: " << StatepointID << "\n";
            }

            // 5. 解析 locations（栈上的 GC 根）
            for (auto LocationRecord : StatepointRecord.locations()) {
                auto Kind = LocationRecord.getKind();
                if (Kind == StackMapParser::LocationKind::Direct) {
                    uint32_t DwarfRegNum = LocationRecord.getDwarfRegNum();
                    int32_t Offset = LocationRecord.getOffset();
                    errs() << "[StackMap] Direct: " << DwarfRegNum << " , Offset: " << Offset << "\n";
                    if (YuhuTraceMachineCode) {
                        errs() << "[StackMap]     GC Root at stack offset: " << Offset << " , Direct: " << DwarfRegNum << "\n";
                    }
                    YuhuDebugInformationRecorder::get()->register_stack_map(InstructionOffset, static_cast<uint8_t>(Kind), DwarfRegNum, Offset);
                } else if (Kind == StackMapParser::LocationKind::Register) {
                    uint32_t DwarfRegNum = LocationRecord.getDwarfRegNum();
                    errs() << "[StackMap] Register: " << DwarfRegNum << "\n";
                    if (YuhuTraceMachineCode) {
                        errs() << "[StackMap]     GC Root in register: " << (int)DwarfRegNum << "\n";
                    }
                    YuhuDebugInformationRecorder::get()->register_stack_map(InstructionOffset, static_cast<uint8_t>(Kind), DwarfRegNum, 0);
                } else if (Kind == StackMapParser::LocationKind::Indirect) {
                    uint32_t DwarfRegNum = LocationRecord.getDwarfRegNum();
                    int32_t Offset = LocationRecord.getOffset();
                    errs() << "[StackMap] Indirect: " << DwarfRegNum << " , Offset: " << Offset << "\n";
                    if (YuhuTraceMachineCode) {
                        errs() << "[StackMap]     GC Root at stack offset: " << Offset << " , Indirect: " << DwarfRegNum << "\n";
                    }
                    YuhuDebugInformationRecorder::get()->register_stack_map(InstructionOffset, static_cast<uint8_t>(Kind), DwarfRegNum, Offset);
                } else if (Kind == StackMapParser::LocationKind::Constant) {
                    errs() << "[StackMap] Constant: " << LocationRecord.getSmallConstant() << "\n";
                    if (YuhuTraceMachineCode) {
                        errs() << "[StackMap] Ignore Constant" << "\n";
                    }
                } else if (Kind == StackMapParser::LocationKind::ConstantIndex) {
                    errs() << "[StackMap] ConstantIndex: " << LocationRecord.getConstantIndex() << "\n";
                    if (YuhuTraceMachineCode) {
                        errs() << "[StackMap] Ignore ConstantIndex" << "\n";
                    }
                }
            }
        }

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

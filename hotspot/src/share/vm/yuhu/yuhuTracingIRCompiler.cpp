/*
 * Copyright (c) 2024, Yuhu Compiler Project
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only.
 */

#include "precompiled.hpp"
#include "yuhu/yuhuTracingIRCompiler.hpp"
#include "yuhu/yuhu_globals.hpp"

#pragma push_macro("assert")
#ifdef assert
#undef assert
#endif

#include "yuhu/llvmHeaders.hpp"
#include "llvm/Object/StackMapParser.h"

#pragma pop_macro("assert")

#include "yuhu/yuhuDebugInformationRecorder.hpp"

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
    }

    // ObjBuffer is a std::unique_ptr<MemoryBuffer>
    // You can parse it as an object file!

    // Use LLVM's ObjectFile API:
    auto ObjFile = llvm::object::ObjectFile::createObjectFile(
            (*ObjBuffer)->getMemBufferRef());

    if (!ObjFile) {
        return ObjBuffer;
    }

    parseStackMap(ObjFile);

    return ObjBuffer;
}

void TracingIRCompiler::parseStackMap(llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>> &ObjFile) {
    // 1. 找到 __LLVM_StackMaps 段
    for (auto &Section : (*ObjFile)->sections()) {
        auto NameOrErr = Section.getName();
        if (!NameOrErr) {
            continue;
        }
        if (YuhuTraceMachineCode) {
            errs() << "Section: " << *NameOrErr << "\n";
        }
        // Section.getName() returns in format of segment,section eg
        // Section: __TEXT,__text
        // Section: $__GOT
        // Section: __LD,__compact_unwind
        // Section: __LLVM_STACKMAPS,__llvm_stackmaps
        // Section: __TEXT,__lcl_macho_hdr
        // Section: __TEXT,__unwind_info
        if (!Section.getName()->ends_with("__llvm_stackmaps")) continue;
        if (YuhuTraceMachineCode) {
            errs() << "Size: " << Section.getSize() << "\n";
        }
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
            if (YuhuTraceMachineCode) {
                errs() << "[StackMap] ID: " << StatepointID << " , InstructionOffset: " << InstructionOffset
                       << ", Locations: " << StatepointRecord.getNumLocations() << " , Liveouts: " << StatepointRecord.getNumLiveOuts() << "\n";
            }

            if (DEOPT_STATEPOINT_ID == StatepointID) {
                // process stack map for deopt bundle
                // last 4 locations is bci, num of locals, num of expression stacks, num of monitors
                GrowableArray<StackMapLocation> locations;

                for (auto LocationRecord : StatepointRecord.locations()) {
                    StackMapLocation location{};
                    auto Kind = LocationRecord.getKind();
                    location.kind = static_cast<uint8_t>(LocationRecord.getKind());
                    switch (Kind) {
                        case StackMapParser::LocationKind::Direct:
                            location.reg_num = LocationRecord.getDwarfRegNum();
                            location.offset = LocationRecord.getOffset();
                            if (YuhuTraceMachineCode) {
                                errs() << "[StackMap]     Deopt Bundle operand at stack offset: " << location.offset << " , Direct: "
                                       << location.reg_num << "\n";
                            }
                            break;
                        case StackMapParser::LocationKind::Register:
                            location.reg_num = LocationRecord.getDwarfRegNum();
                            if (YuhuTraceMachineCode) {
                                errs() << "[StackMap]     Deopt Bundle operand in register: " << (int) location.reg_num << "\n";
                            }
                            break;
                        case StackMapParser::LocationKind::Indirect:
                            location.reg_num = LocationRecord.getDwarfRegNum();
                            location.offset = LocationRecord.getOffset();
                            if (YuhuTraceMachineCode) {
                                errs() << "[StackMap]     Deopt Bundle operand at stack offset: " << location.offset << " , Indirect: "
                                       << location.reg_num << "\n";
                            }
                            break;
                        case StackMapParser::LocationKind::Constant:
                            location.constant = LocationRecord.getSmallConstant();
                            if (YuhuTraceMachineCode) {
                                errs() << "[StackMap]     Deopt Bundle operand at Constant: " << location.constant << "\n";
                            }
                            break;
                        case StackMapParser::LocationKind::ConstantIndex:
                            uint32_t constantIndex = LocationRecord.getConstantIndex();
                            if (YuhuTraceMachineCode) {
                                errs() << "[StackMap] Ignore ConstantIndex: " << constantIndex << "\n";
                            }
                            break;
                    }
                    locations.append(location);
                }

                assert(locations.length() >= 4, "locations should contain bci, num of locals, num of expression stacks, num of monitors");
                // get bci
                StackMapLocation bci_loc = locations.at(locations.length() - 4);
                assert(bci_loc.kind == static_cast<uint8_t>(StackMapParser::LocationKind::Constant), "bci should be constant");
                uint32_t bci = bci_loc.constant;
                // get num of locals
                StackMapLocation num_of_locals_loc = locations.at(locations.length() - 3);
                assert(num_of_locals_loc.kind == static_cast<uint8_t>(StackMapParser::LocationKind::Constant), "num of locals should be constant");
                uint32_t num_of_locals = num_of_locals_loc.constant;
                // get num of expression stacks
                StackMapLocation num_of_expression_stacks_loc = locations.at(locations.length() - 2);
                assert(num_of_expression_stacks_loc.kind == static_cast<uint8_t>(StackMapParser::LocationKind::Constant), "num of expression stacks should be constant");
                uint32_t num_of_expression_stacks = num_of_expression_stacks_loc.constant;
                // get num of monitors
                StackMapLocation num_of_monitors_loc = locations.at(locations.length() - 1);
                assert(num_of_monitors_loc.kind == static_cast<uint8_t>(StackMapParser::LocationKind::Constant), "num of monitors should be constant");
                uint32_t num_of_monitors = num_of_monitors_loc.constant;

                assert(locations.length() >= (int)(4 + num_of_locals * 2 + num_of_expression_stacks * 2 + num_of_monitors * 2), 
                       "locations should have enough locals, expression stacks, and monitors (each with type metadata)");

                YuhuDebugInformationRecorder::get()->register_deopt_bundle(InstructionOffset, bci);
                
                // Each value is now preceded by its type metadata
                // Layout: [type0, val0, type1, val1, ...]
                // Total entries per section = count * 2
                int locals_total = num_of_locals * 2;
                int stacks_total = num_of_expression_stacks * 2;
                int monitors_total = num_of_monitors * 2;
                
                int metadata_end = locations.length() - 4;
                int monitors_start = metadata_end - monitors_total;
                int stacks_start = monitors_start - stacks_total;
                int locals_start = stacks_start - locals_total;
                
                // register locals (every other entry starting from locals_start)
                for (int i = locals_start; i < stacks_start; i += 2) {
                    // i is type, i+1 is value
                    StackMapLocation* type_loc = &locations.at(i);
                    StackMapLocation* val_loc = &locations.at(i + 1);
                    
                    uint8_t basic_type = (type_loc->kind == static_cast<uint8_t>(StackMapParser::LocationKind::Constant)) 
                                         ? (uint8_t)type_loc->constant : T_VOID;
                    
                    YuhuDebugInformationRecorder::get()->register_deopt_bundle_local_data(InstructionOffset,
                                                                                          val_loc->kind,
                                                                                          val_loc->reg_num,
                                                                                          val_loc->offset,
                                                                                          val_loc->constant,
                                                                                          basic_type);
                }
                // register expression stacks (every other entry starting from stacks_start)
                for (int i = stacks_start; i < monitors_start; i += 2) {
                    StackMapLocation* type_loc = &locations.at(i);
                    StackMapLocation* val_loc = &locations.at(i + 1);
                    
                    uint8_t basic_type = (type_loc->kind == static_cast<uint8_t>(StackMapParser::LocationKind::Constant)) 
                                         ? (uint8_t)type_loc->constant : T_VOID;
                    
                    YuhuDebugInformationRecorder::get()->register_deopt_bundle_expression_stack_data(InstructionOffset,
                                                                                                     val_loc->kind,
                                                                                                     val_loc->reg_num,
                                                                                                     val_loc->offset,
                                                                                                     val_loc->constant,
                                                                                                     basic_type);
                }
                // register monitors (every other entry starting from monitors_start)
                for (int i = monitors_start; i < metadata_end; i += 2) {
                    StackMapLocation* type_loc = &locations.at(i);
                    StackMapLocation* val_loc = &locations.at(i + 1);
                    
                    uint8_t basic_type = (type_loc->kind == static_cast<uint8_t>(StackMapParser::LocationKind::Constant)) 
                                         ? (uint8_t)type_loc->constant : T_OBJECT; // Monitors should be T_OBJECT
                    
                    YuhuDebugInformationRecorder::get()->register_deopt_bundle_monitor_data(InstructionOffset,
                                                                                            val_loc->kind,
                                                                                            val_loc->reg_num,
                                                                                            val_loc->offset,
                                                                                            val_loc->constant,
                                                                                            basic_type);
                }
            } else {
                // 5. 解析 locations（栈上的 GC 根）
                for (auto LocationRecord : StatepointRecord.locations()) {
                    auto Kind = LocationRecord.getKind();
                    if (Kind == StackMapParser::LocationKind::Direct) {
                        uint32_t DwarfRegNum = LocationRecord.getDwarfRegNum();
                        int32_t Offset = LocationRecord.getOffset();
                        if (YuhuTraceMachineCode) {
                            errs() << "[StackMap]     GC Root at stack offset: " << Offset << " , Direct: "
                                   << DwarfRegNum << "\n";
                        }
                        YuhuDebugInformationRecorder::get()->register_stack_map(InstructionOffset,
                                                                                static_cast<uint8_t>(Kind),
                                                                                DwarfRegNum,
                                                                                Offset);
                    } else if (Kind == StackMapParser::LocationKind::Register) {
                        uint32_t DwarfRegNum = LocationRecord.getDwarfRegNum();
                        if (YuhuTraceMachineCode) {
                            errs() << "[StackMap]     GC Root in register: " << (int) DwarfRegNum << "\n";
                        }
                        YuhuDebugInformationRecorder::get()->register_stack_map(InstructionOffset,
                                                                                static_cast<uint8_t>(Kind),
                                                                                DwarfRegNum,
                                                                                0);
                    } else if (Kind == StackMapParser::LocationKind::Indirect) {
                        uint32_t DwarfRegNum = LocationRecord.getDwarfRegNum();
                        int32_t Offset = LocationRecord.getOffset();
                        if (YuhuTraceMachineCode) {
                            errs() << "[StackMap]     GC Root at stack offset: " << Offset << " , Indirect: "
                                   << DwarfRegNum << "\n";
                        }
                        YuhuDebugInformationRecorder::get()->register_stack_map(InstructionOffset,
                                                                                static_cast<uint8_t>(Kind),
                                                                                DwarfRegNum,
                                                                                Offset);
                    } else if (Kind == StackMapParser::LocationKind::Constant) {
                        uint32_t constant = LocationRecord.getSmallConstant();
                        if (YuhuTraceMachineCode) {
                            errs() << "[StackMap] Ignore Constant: " << constant << "\n";
                        }
                        // record every location, otherwise call site may not find corresponding stack map
                        YuhuDebugInformationRecorder::get()->register_stack_map(InstructionOffset,
                                                                                static_cast<uint8_t>(Kind),
                                                                                0,
                                                                                0,
                                                                                constant);
                    } else if (Kind == StackMapParser::LocationKind::ConstantIndex) {
                        uint32_t constantIndex = LocationRecord.getConstantIndex();
                        if (YuhuTraceMachineCode) {
                            errs() << "[StackMap] Ignore ConstantIndex: " << constantIndex << "\n";
                        }
                        // record every location, otherwise call site may not find corresponding stack map
                        YuhuDebugInformationRecorder::get()->register_stack_map(InstructionOffset,
                                                                                static_cast<uint8_t>(Kind),
                                                                                0,
                                                                                0);
                    }
                }
            }
        }

    }
}

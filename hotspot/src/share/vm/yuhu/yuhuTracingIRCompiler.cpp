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

    parseStackMap(ObjFile, M);

    return ObjBuffer;
}

void TracingIRCompiler::parseStackMap(llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>> &ObjFile, llvm::Module &M) {
    // Iterate through all functions in the module
    for (auto &F : M.functions()) {
        if (F.isDeclaration()) continue;  // Skip declarations

        // Get the function name
        std::string func_name = F.getName().str();

        if (YuhuTraceMachineCode) {
            if (YuhuStackMapFile != NULL) {
                FILE *f = fopen(YuhuStackMapFile, "a");
                fileStream fs(f, true);
                fs.print_cr("[StackMap] Function Name: %s", func_name.c_str());
                fs.flush();
            } else {
                // This is the unmangled name, e.g., "java.lang.String::indexOf"
                errs() << "[StackMap] Function Name: " << func_name << "\n";
            }
        }
        break;
    }

    // 1. 找到 __LLVM_StackMaps 段
    for (auto &Section : (*ObjFile)->sections()) {
        auto NameOrErr = Section.getName();
        if (!NameOrErr) {
            continue;
        }
        if (YuhuTraceMachineCode) {
            if (YuhuStackMapFile != NULL) {
                FILE *f = fopen(YuhuStackMapFile, "a");
                fileStream fs(f, true);
                fs.print_cr("[StackMap] Section: %s", (*NameOrErr).str().c_str());
                fs.flush();
            } else {
                errs() << "[StackMap] Section: " << *NameOrErr << "\n";
            }
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
            if (YuhuStackMapFile != NULL) {
                FILE *f = fopen(YuhuStackMapFile, "a");
                fileStream fs(f, true);
                fs.print_cr("[StackMap] Section: %d", Section.getSize());
                fs.flush();
            } else {
                errs() << "[StackMap] Size: " << Section.getSize() << "\n";
            }
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
                if (YuhuStackMapFile != NULL) {
                    FILE *f = fopen(YuhuStackMapFile, "a");
                    fileStream fs(f, true);
                    fs.print_cr("[StackMap] ID: %llu , InstructionOffset: %u , Locations: %d , Liveouts: %d",
                                StatepointID, InstructionOffset, StatepointRecord.getNumLocations(),
                                StatepointRecord.getNumLiveOuts());
                    fs.flush();
                } else {
                    errs() << "[StackMap] ID: " << StatepointID << " , InstructionOffset: " << InstructionOffset
                           << " , Locations: " << StatepointRecord.getNumLocations() << " , Liveouts: " << StatepointRecord.getNumLiveOuts() << "\n";
                }
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
                                if (YuhuStackMapFile != NULL) {
                                    FILE *f = fopen(YuhuStackMapFile, "a");
                                    fileStream fs(f, true);
                                    fs.print_cr("[StackMap]     Deopt Bundle operand at stack offset: %d , Direct: %d",
                                                location.offset, location.reg_num);
                                    fs.flush();
                                } else {
                                    errs() << "[StackMap]     Deopt Bundle operand at stack offset: " << location.offset << " , Direct: "
                                           << location.reg_num << "\n";
                                }
                            }
                            break;
                        case StackMapParser::LocationKind::Register:
                            location.reg_num = LocationRecord.getDwarfRegNum();
                            if (YuhuTraceMachineCode) {
                                if (YuhuStackMapFile != NULL) {
                                    FILE *f = fopen(YuhuStackMapFile, "a");
                                    fileStream fs(f, true);
                                    fs.print_cr("[StackMap]     Deopt Bundle operand in register: %d",
                                                (int) location.reg_num);
                                    fs.flush();
                                } else {
                                    errs() << "[StackMap]     Deopt Bundle operand in register: " << (int) location.reg_num << "\n";
                                }
                            }
                            break;
                        case StackMapParser::LocationKind::Indirect:
                            location.reg_num = LocationRecord.getDwarfRegNum();
                            location.offset = LocationRecord.getOffset();
                            if (YuhuTraceMachineCode) {
                                if (YuhuStackMapFile != NULL) {
                                    FILE *f = fopen(YuhuStackMapFile, "a");
                                    fileStream fs(f, true);
                                    fs.print_cr(
                                            "[StackMap]     Deopt Bundle operand at stack offset: %d , Indirect: %d",
                                            location.offset, location.reg_num);
                                    fs.flush();
                                } else {
                                    errs() << "[StackMap]     Deopt Bundle operand at stack offset: " << location.offset << " , Indirect: "
                                           << location.reg_num << "\n";
                                }
                            }
                            break;
                        case StackMapParser::LocationKind::Constant:
                            location.constant = LocationRecord.getSmallConstant();
                            if (YuhuTraceMachineCode) {
                                if (YuhuStackMapFile != NULL) {
                                    FILE *f = fopen(YuhuStackMapFile, "a");
                                    fileStream fs(f, true);
                                    fs.print_cr("[StackMap]     Deopt Bundle operand at Constant: %llu",
                                                location.constant);
                                    fs.flush();
                                } else {
                                    errs() << "[StackMap]     Deopt Bundle operand at Constant: " << location.constant << "\n";
                                }
                            }
                            break;
                        case StackMapParser::LocationKind::ConstantIndex:
                            uint32_t constantIndex = LocationRecord.getConstantIndex();
                            location.constant = Parser.getConstant(constantIndex).getValue();
                            if (YuhuTraceMachineCode) {
                                if (YuhuStackMapFile != NULL) {
                                    FILE *f = fopen(YuhuStackMapFile, "a");
                                    fileStream fs(f, true);
                                    fs.print_cr("[StackMap]     Deopt Bundle operand at ConstantIndex: %d , value: %llu", constantIndex, location.constant);
                                    fs.flush();
                                } else {
                                    errs() << "[StackMap]     Deopt Bundle operand at ConstantIndex: " << constantIndex << " , value: " << location.constant << "\n";
                                }
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

                assert(locations.length() >= (int)(4 + num_of_locals + num_of_expression_stacks),
                       "locations should have basic type of locals and expression stacks");

                YuhuDebugInformationRecorder::get()->register_deopt_bundle(InstructionOffset, bci);
                
                // Each section only has its type metadata
                // Layout: [type0, type1, ...]
                // Total entries per section = count * 1
                int locals_total = num_of_locals;
                int stacks_total = num_of_expression_stacks;
                
                int metadata_end = locations.length() - 4;
                int stacks_start = metadata_end - stacks_total;
                int locals_start = stacks_start - locals_total;
                
                // register locals (every other entry starting from locals_start)
                for (int i = locals_start; i < stacks_start; i++) {
                    // i is type
                    StackMapLocation* type_loc = &locations.at(i);
                    assert(type_loc->kind == static_cast<uint8_t>(StackMapParser::LocationKind::Constant), "basic_type must be constant");
                    
                    uint8_t basic_type = (uint8_t)type_loc->constant;
                    
                    YuhuDebugInformationRecorder::get()->register_deopt_bundle_local_data(InstructionOffset, basic_type);
                }
                // register expression stacks (every other entry starting from stacks_start)
                for (int i = stacks_start; i < metadata_end; i++) {
                    StackMapLocation* type_loc = &locations.at(i);
                    assert(type_loc->kind == static_cast<uint8_t>(StackMapParser::LocationKind::Constant), "basic_type must be constant");

                    uint8_t basic_type = (uint8_t)type_loc->constant;
                    
                    YuhuDebugInformationRecorder::get()->register_deopt_bundle_expression_stack_data(InstructionOffset, basic_type);
                }
                YuhuDebugInformationRecorder::get()->register_deopt_bundle_monitor_data(InstructionOffset, num_of_monitors);
            } else if (EXTENDED_SP_ALLOCA_STATEPOINT_ID == StatepointID) {
                bool found_offset = false;
                for (auto LocationRecord : StatepointRecord.locations()) {
                    auto Kind = LocationRecord.getKind();
                    if (Kind == StackMapParser::LocationKind::Direct || Kind == StackMapParser::LocationKind::Indirect) {
                        found_offset = true;
                        uint32_t DwarfRegNum = LocationRecord.getDwarfRegNum();
                        int32_t Offset = LocationRecord.getOffset();
                        if (YuhuTraceMachineCode) {
                            if (YuhuStackMapFile != NULL) {
                                FILE *f = fopen(YuhuStackMapFile, "a");
                                fileStream fs(f, true);
                                fs.print_cr("[StackMap]     Extended sp alloca at stack offset: %d , DwarfRegNum: %d , Kind: %d", Offset,
                                            DwarfRegNum, static_cast<uint32_t>(Kind));
                                fs.flush();
                            } else {
                                errs() << "[StackMap]     Extended sp alloca at stack offset: " << Offset << " , DwarfRegNum: "
                                       << DwarfRegNum << " , Kind: " << static_cast<uint32_t>(Kind) << "\n";
                            }
                        }
                        YuhuDebugInformationRecorder::get()->register_frame_layout_info_with_stack_map_fields(DwarfRegNum, static_cast<uint8_t>(Kind), Offset);
                        break;
                    }
                }
                assert(found_offset, "extended sp alloca should have offset");
            } else {
                // 5. 解析 locations（栈上的 GC 根）
                for (auto LocationRecord : StatepointRecord.locations()) {
                    auto Kind = LocationRecord.getKind();
                    if (Kind == StackMapParser::LocationKind::Direct) {
                        uint32_t DwarfRegNum = LocationRecord.getDwarfRegNum();
                        int32_t Offset = LocationRecord.getOffset();
                        if (YuhuTraceMachineCode) {
                            if (YuhuStackMapFile != NULL) {
                                FILE *f = fopen(YuhuStackMapFile, "a");
                                fileStream fs(f, true);
                                fs.print_cr("[StackMap]     GC Root at stack offset: %d , Direct: %d", Offset,
                                            DwarfRegNum);
                                fs.flush();
                            } else {
                                errs() << "[StackMap]     GC Root at stack offset: " << Offset << " , Direct: "
                                       << DwarfRegNum << "\n";
                            }
                        }
                        YuhuDebugInformationRecorder::get()->register_stack_map(InstructionOffset,
                                                                                static_cast<uint8_t>(Kind),
                                                                                DwarfRegNum,
                                                                                Offset);
                    } else if (Kind == StackMapParser::LocationKind::Register) {
                        uint32_t DwarfRegNum = LocationRecord.getDwarfRegNum();
                        if (YuhuTraceMachineCode) {
                            if (YuhuStackMapFile != NULL) {
                                FILE *f = fopen(YuhuStackMapFile, "a");
                                fileStream fs(f, true);
                                fs.print_cr("[StackMap]     GC Root in register: %d", (int) DwarfRegNum);
                                fs.flush();
                            } else {
                                errs() << "[StackMap]     GC Root in register: " << (int) DwarfRegNum << "\n";
                            }
                        }
                        YuhuDebugInformationRecorder::get()->register_stack_map(InstructionOffset,
                                                                                static_cast<uint8_t>(Kind),
                                                                                DwarfRegNum,
                                                                                0);
                    } else if (Kind == StackMapParser::LocationKind::Indirect) {
                        uint32_t DwarfRegNum = LocationRecord.getDwarfRegNum();
                        int32_t Offset = LocationRecord.getOffset();
                        if (YuhuTraceMachineCode) {
                            if (YuhuStackMapFile != NULL) {
                                FILE *f = fopen(YuhuStackMapFile, "a");
                                fileStream fs(f, true);
                                fs.print_cr("[StackMap]     GC Root at stack offset: %d , Indirect: %d", Offset,
                                            DwarfRegNum);
                                fs.flush();
                            } else {
                                errs() << "[StackMap]     GC Root at stack offset: " << Offset << " , Indirect: "
                                       << DwarfRegNum << "\n";
                            }
                        }
                        YuhuDebugInformationRecorder::get()->register_stack_map(InstructionOffset,
                                                                                static_cast<uint8_t>(Kind),
                                                                                DwarfRegNum,
                                                                                Offset);
                    } else if (Kind == StackMapParser::LocationKind::Constant) {
                        uint32_t constant = LocationRecord.getSmallConstant();
                        if (YuhuTraceMachineCode) {
                            if (YuhuStackMapFile != NULL) {
                                FILE *f = fopen(YuhuStackMapFile, "a");
                                fileStream fs(f, true);
                                fs.print_cr("[StackMap] Ignore Constant: %d", constant);
                                fs.flush();
                            } else {
                                errs() << "[StackMap] Ignore Constant: " << constant << "\n";
                            }
                        }
                        // record every location, otherwise call site may not find corresponding stack map
                        YuhuDebugInformationRecorder::get()->register_stack_map(InstructionOffset,
                                                                                static_cast<uint8_t>(Kind),
                                                                                0,
                                                                                0,
                                                                                constant);
                    } else if (Kind == StackMapParser::LocationKind::ConstantIndex) {
                        uint32_t constantIndex = LocationRecord.getConstantIndex();
                        uint64_t constant = Parser.getConstant(constantIndex).getValue();
                        if (YuhuTraceMachineCode) {
                            if (YuhuStackMapFile != NULL) {
                                FILE *f = fopen(YuhuStackMapFile, "a");
                                fileStream fs(f, true);
                                fs.print_cr("[StackMap] Ignore ConstantIndex: %d , value: %llu", constantIndex, constant);
                                fs.flush();
                            } else {
                                errs() << "[StackMap] Ignore ConstantIndex: " << constantIndex << " , value: " << constant << "\n";
                            }
                        }
                        // record every location, otherwise call site may not find corresponding stack map
                        YuhuDebugInformationRecorder::get()->register_stack_map(InstructionOffset,
                                                                                static_cast<uint8_t>(Kind),
                                                                                0,
                                                                                0,
                                                                                constant);
                    }
                }
            }
        }

    }
}

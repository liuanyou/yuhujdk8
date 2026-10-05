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
#include "llvm/IR/Statepoint.h"

#pragma pop_macro("assert")

#include "yuhu/yuhuDebugInformationRecorder.hpp"

using namespace llvm;

// File-based logging for debugging
static FILE* yuhu_stack_map_log = NULL;
static void yuhu_stack_map_log_init() {
    if (yuhu_stack_map_log == NULL) {
        yuhu_stack_map_log = fopen(YuhuStackMapFile, "a");
    }
}
#define YUHU_STACK_MAP_LOG(fmt, ...) \
    do { \
        yuhu_stack_map_log_init(); \
        if (yuhu_stack_map_log) { \
            fprintf(yuhu_stack_map_log, fmt "\n", ##__VA_ARGS__); \
            fflush(yuhu_stack_map_log); \
        } \
    } while(0)

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
                YUHU_STACK_MAP_LOG("[StackMap] Function Name: %s", func_name.c_str());
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
                YUHU_STACK_MAP_LOG("[StackMap] Section: %s", (*NameOrErr).str().c_str());
            } else {
                errs() << "[StackMap] Section: " << *NameOrErr << "\n";
            }
        }
        // Section.getName() returns in format of segment,section eg
        // Section: __TEXT,__text
        // Section: $__GOT
        // Section: __LD,__compact_unwind
        // Section: __TEXT,__const
        // Section: __LLVM_STACKMAPS,__llvm_stackmaps
        // Section: __TEXT,__lcl_macho_hdr
        // Section: __TEXT,__unwind_info
        if (!Section.getName()->ends_with("__llvm_stackmaps")) continue;
        if (YuhuTraceMachineCode) {
            if (YuhuStackMapFile != NULL) {
                YUHU_STACK_MAP_LOG("[StackMap] Section: %d", Section.getSize());
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

        YuhuDebugInformationRecorder* recorder = YuhuDebugInformationRecorder::get();

        // 4. 遍历该函数中的所有 statepoint 记录
        for (auto StatepointRecord : Parser.records()) {
            uint64_t StatepointID = StatepointRecord.getID();
            uint32_t InstructionOffset = StatepointRecord.getInstructionOffset();

            if (YuhuTraceMachineCode) {
                if (YuhuStackMapFile != NULL) {
                    YUHU_STACK_MAP_LOG("[StackMap] ID: %llu , InstructionOffset: %u , Locations: %d , Liveouts: %d",
                                       StatepointID, InstructionOffset, StatepointRecord.getNumLocations(),
                                       StatepointRecord.getNumLiveOuts());
                } else {
                    errs() << "[StackMap] ID: " << StatepointID << " , InstructionOffset: " << InstructionOffset
                           << " , Locations: " << StatepointRecord.getNumLocations() << " , Liveouts: "
                           << StatepointRecord.getNumLiveOuts() << "\n";
                }
            }

            if (EXTENDED_SP_ALLOCA_STATEPOINT_ID == StatepointID) {
                bool found_offset = false;
                for (auto LocationRecord: StatepointRecord.locations()) {
                    auto Kind = LocationRecord.getKind();
                    if (Kind == StackMapParser::LocationKind::Direct ||
                        Kind == StackMapParser::LocationKind::Indirect) {
                        found_offset = true;
                        uint32_t DwarfRegNum = LocationRecord.getDwarfRegNum();
                        int32_t Offset = LocationRecord.getOffset();
                        if (YuhuTraceMachineCode) {
                            if (YuhuStackMapFile != NULL) {
                                YUHU_STACK_MAP_LOG(
                                        "[StackMap]     Extended sp alloca at stack offset: %d , DwarfRegNum: %d , Kind: %d",
                                        Offset, DwarfRegNum, static_cast<uint32_t>(Kind));
                            } else {
                                errs() << "[StackMap]     Extended sp alloca at stack offset: " << Offset
                                       << " , DwarfRegNum: "
                                       << DwarfRegNum << " , Kind: " << static_cast<uint32_t>(Kind) << "\n";
                            }
                        }
                        recorder->register_frame_layout_info_with_stack_map_fields(DwarfRegNum, static_cast<uint8_t>(Kind), Offset);
                        break;
                    }
                }
                assert(found_offset, "extended sp alloca should have offset");
            } else if (X0_SP_ALLOCA_STATEPOINT_ID == StatepointID) {
                bool found_offset = false;
                for (auto LocationRecord: StatepointRecord.locations()) {
                    auto Kind = LocationRecord.getKind();
                    if (Kind == StackMapParser::LocationKind::Direct ||
                        Kind == StackMapParser::LocationKind::Indirect) {
                        found_offset = true;
                        uint32_t DwarfRegNum = LocationRecord.getDwarfRegNum();
                        int32_t Offset = LocationRecord.getOffset();
                        if (YuhuTraceMachineCode) {
                            if (YuhuStackMapFile != NULL) {
                                YUHU_STACK_MAP_LOG(
                                        "[StackMap]     X0 sp alloca at stack offset: %d , DwarfRegNum: %d , Kind: %d",
                                        Offset, DwarfRegNum, static_cast<uint32_t>(Kind));
                            } else {
                                errs() << "[StackMap]     X0 sp alloca at stack offset: " << Offset
                                       << " , DwarfRegNum: "
                                       << DwarfRegNum << " , Kind: " << static_cast<uint32_t>(Kind) << "\n";
                            }
                        }
                        break;
                    }
                }
                assert(found_offset, "extended sp alloca should have offset");
            } else {
                CallSiteType call_site_type = recorder->get_call_site_type_by_statepoint_id(StatepointID);
                if (YuhuTraceMachineCode) {
                    if (YuhuStackMapFile != NULL) {
                        YUHU_STACK_MAP_LOG("[StackMap] ID: %llu , call site type: %d", StatepointID, call_site_type);
                    } else {
                        errs() << "[StackMap] ID: " << StatepointID << " , call site type: " << static_cast<uint8_t>(call_site_type) << "\n";
                    }
                }

                // either call site or patch point comes to here
                recorder->register_stack_map(StatepointID, InstructionOffset);
                for (auto LocationRecord: StatepointRecord.locations()) {
                    auto Kind = LocationRecord.getKind();
                    if (Kind == StackMapParser::LocationKind::Direct) {
                        uint32_t DwarfRegNum = LocationRecord.getDwarfRegNum();
                        int32_t Offset = LocationRecord.getOffset();
                        if (YuhuTraceMachineCode) {
                            if (YuhuStackMapFile != NULL) {
                                YUHU_STACK_MAP_LOG("[StackMap]     GC Root at stack offset: %d , Direct: %d",
                                                   Offset, DwarfRegNum);
                            } else {
                                errs() << "[StackMap]     GC Root at stack offset: " << Offset << " , Direct: "
                                       << DwarfRegNum << "\n";
                            }
                        }
                        recorder->register_stack_map_location_data(StatepointID,
                                                                   InstructionOffset,
                                                                   static_cast<uint8_t>(Kind),
                                                                   DwarfRegNum,
                                                                   Offset);
                    } else if (Kind == StackMapParser::LocationKind::Register) {
                        uint32_t DwarfRegNum = LocationRecord.getDwarfRegNum();
                        if (YuhuTraceMachineCode) {
                            if (YuhuStackMapFile != NULL) {
                                YUHU_STACK_MAP_LOG("[StackMap]     GC Root in register: %d", (int) DwarfRegNum);
                            } else {
                                errs() << "[StackMap]     GC Root in register: " << (int) DwarfRegNum << "\n";
                            }
                        }
                        recorder->register_stack_map_location_data(StatepointID,
                                                                   InstructionOffset,
                                                                   static_cast<uint8_t>(Kind),
                                                                   DwarfRegNum,
                                                                   0);
                    } else if (Kind == StackMapParser::LocationKind::Indirect) {
                        uint32_t DwarfRegNum = LocationRecord.getDwarfRegNum();
                        int32_t Offset = LocationRecord.getOffset();
                        if (YuhuTraceMachineCode) {
                            if (YuhuStackMapFile != NULL) {
                                YUHU_STACK_MAP_LOG("[StackMap]     GC Root at stack offset: %d , Indirect: %d",
                                                   Offset, DwarfRegNum);
                            } else {
                                errs() << "[StackMap]     GC Root at stack offset: " << Offset << " , Indirect: "
                                       << DwarfRegNum << "\n";
                            }
                        }
                        recorder->register_stack_map_location_data(StatepointID,
                                                                   InstructionOffset,
                                                                   static_cast<uint8_t>(Kind),
                                                                   DwarfRegNum,
                                                                   Offset);
                    } else if (Kind == StackMapParser::LocationKind::Constant) {
                        uint32_t constant = LocationRecord.getSmallConstant();
                        if (YuhuTraceMachineCode) {
                            if (YuhuStackMapFile != NULL) {
                                YUHU_STACK_MAP_LOG("[StackMap]     Constant: %d", constant);
                            } else {
                                errs() << "[StackMap]     Constant: " << constant << "\n";
                            }
                        }
                        // record every location, otherwise call site may not find corresponding stack map
                        recorder->register_stack_map_location_data(StatepointID,
                                                                   InstructionOffset,
                                                                   static_cast<uint8_t>(Kind),
                                                                   0,
                                                                   0,
                                                                   constant);
                    } else if (Kind == StackMapParser::LocationKind::ConstantIndex) {
                        uint32_t constantIndex = LocationRecord.getConstantIndex();
                        uint64_t constant = Parser.getConstant(constantIndex).getValue();
                        if (YuhuTraceMachineCode) {
                            if (YuhuStackMapFile != NULL) {
                                YUHU_STACK_MAP_LOG("[StackMap]     ConstantIndex: %d , value: %llu",
                                                   constantIndex, constant);
                            } else {
                                errs() << "[StackMap]     ConstantIndex: " << constantIndex << " , value: "
                                       << constant << "\n";
                            }
                        }
                        // record every location, otherwise call site may not find corresponding stack map
                        recorder->register_stack_map_location_data(StatepointID,
                                                                   InstructionOffset,
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

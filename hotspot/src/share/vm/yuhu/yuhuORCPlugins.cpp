/*
 * Copyright (c) 2024, Yuhu Compiler Project
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only.
 */

#include "precompiled.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/yuhuORCPlugins.hpp"
#include "yuhu/yuhuVirtualAddressPatcher.hpp"
#include "yuhu/yuhuFunction.hpp"
#include "yuhu/yuhuCompiler.hpp"
#include "yuhu/yuhu_globals.hpp"
#include "code/debugInfoRec.hpp"
#include "compiler/oopMap.hpp"
#include <unistd.h>  // For sysconf

using namespace llvm;

// MachineCodePrinterPlugin implementation
// This plugin prints all generated machine code for debugging purposes

void MachineCodePrinterPlugin::notifyLoaded(llvm::orc::MaterializationResponsibility &MR) {
    if (YuhuTraceMachineCode) {
        errs() << "\n=== ObjectLinkingLayer: Loaded Object ===\n";
        errs() << "Materializing symbols: ";
        for (auto &Sym: MR.getSymbols()) {
            errs() << Sym << " ";
        }
        errs() << "\n";
    }
}

llvm::Error MachineCodePrinterPlugin::notifyEmitted(llvm::orc::MaterializationResponsibility &MR) {
    return Error::success();

}

llvm::Error MachineCodePrinterPlugin::notifyFailed(llvm::orc::MaterializationResponsibility &MR) {
    return Error::success();

}

llvm::Error MachineCodePrinterPlugin::notifyRemovingResources(llvm::orc::JITDylib &JD, llvm::orc::ResourceKey K) {
    return Error::success();
}

void MachineCodePrinterPlugin::notifyTransferringResources(llvm::orc::JITDylib &JD, llvm::orc::ResourceKey DstKey,
                                                           llvm::orc::ResourceKey SrcKey) {
}

void MachineCodePrinterPlugin::modifyPassConfig(llvm::orc::MaterializationResponsibility &MR,
                                                llvm::jitlink::LinkGraph &LG,
                                                llvm::jitlink::PassConfiguration &PassConfig) {
// 在链接完成后打印机器码
    PassConfig.PostFixupPasses.push_back(
            [this](
                    jitlink::LinkGraph &G
            ) -> Error {
                return
                        dumpMachineCode(G);
            });
}

llvm::Error MachineCodePrinterPlugin::dumpMachineCode(llvm::jitlink::LinkGraph &G) {
    // 使用专用开关 YuhuTraceMachineCode
    if (!YuhuTraceMachineCode) {
        return Error::success();
    }

    // 输出机器码
    errs() << "\n=== Machine Code from LinkGraph ===\n";
    errs() << "Graph: " << G.getName() << "\n";

    for (auto Sym: G.defined_symbols()) {
        if (!Sym->hasName()) continue;

        errs() << "Symbol: " << Sym->getName() << "\n";
        errs() << "  Address: " << Sym->getAddress() << "\n";

        auto &Block = Sym->getBlock();
        auto Size = Block.getSize();
        auto Content = Block.getContent();

        errs() << "  Size: " << Size << " bytes\n";

        // 方法1：按字节打印（原始格式）
        errs() << "  Raw bytes: ";
        for (size_t i = 0; i < std::min(static_cast<uint64_t>(Size), static_cast<uint64_t>(32)); ++i) {
            errs() << format_hex_no_prefix(static_cast<unsigned int>(Content[i] & 0xFF), 2);
            if ((i + 1) % 4 == 0) errs() << " ";
        }
        errs() << "\n";

        // 方法2：按32位指令打印（正确的小端序格式）
        errs() << "  Instructions (32-bit):\n";
        for (size_t i = 0; i + 4 <= Size; i += 4) {
            // 正确读取小端序的32位指令
            uint32_t instr = 0;
            for (size_t j = 0; j < 4; j++) {
                instr |= (static_cast<uint32_t>(Content[i + j] & 0xFF)) << (j * 8);
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
    return Error::success();
}

void OopMapExtractorPlugin::notifyLoaded(llvm::orc::MaterializationResponsibility &MR) {
}

llvm::Error OopMapExtractorPlugin::notifyEmitted(llvm::orc::MaterializationResponsibility &MR) {
    return Error::success();

}

llvm::Error OopMapExtractorPlugin::notifyFailed(llvm::orc::MaterializationResponsibility &MR) {
    return Error::success();

}

llvm::Error OopMapExtractorPlugin::notifyRemovingResources(llvm::orc::JITDylib &JD, llvm::orc::ResourceKey K) {
    return Error::success();
}

void OopMapExtractorPlugin::notifyTransferringResources(llvm::orc::JITDylib &JD, llvm::orc::ResourceKey DstKey,
                                                           llvm::orc::ResourceKey SrcKey) {
}

// OopMapExtractorPlugin implementation
// Scans for movz/movk/blr patterns and extracts VM call site information

void OopMapExtractorPlugin::modifyPassConfig(llvm::orc::MaterializationResponsibility &MR,
                                              llvm::jitlink::LinkGraph &LG,
                                              llvm::jitlink::PassConfiguration &PassConfig) {
    // Add pass after all relocations are resolved
    PassConfig.PostFixupPasses.push_back(
        [this, &MR](
            jitlink::LinkGraph &G
        ) -> Error {
            return extractCallSites(G, MR);
        });
}

llvm::Error OopMapExtractorPlugin::extractCallSites(llvm::jitlink::LinkGraph &G,
                                                      llvm::orc::MaterializationResponsibility &MR) {
    // Always run this pass (not just when tracing)
    
    // Get the current function from YuhuCompiler
    YuhuFunction* function = YuhuCompiler::current_compiling_function();
    if (function == NULL) {
        if (YuhuTraceMachineCode) {
            errs() << "[OopMap Extractor] Warning: No current function set, skipping OopMap extraction\n";
        }
        return Error::success();
    }
    
    // Iterate over all sections looking for code sections
    for (auto &Section : G.sections()) {
        // Only process executable sections (code)
        if (Section.getName().starts_with(".text") || Section.getName() == "__text") {
            // Iterate over all blocks in this section
            for (auto *Block : Section.blocks()) {
                // Get mutable content so we can patch it
                auto Content = Block->getMutableContent(G);
                uint64_t BaseAddr = Block->getAddress().getValue();
                size_t Size = Block->getSize();
                
                if (Size < 20) continue;  // Need at least 5 instructions for dual placeholders
                
                uint8_t* CodeData = reinterpret_cast<uint8_t*>(Content.data());
                
                // TODO: Parse __llvm_stackmaps section to get statepoint information
                // For now, scan for call instructions (blr) and look backwards for placeholders
                
                // Simple approach: scan for blr instructions and look backwards
                for (size_t offset = 12; offset + 4 <= Size; offset += 4) {
                    uint32_t inst = *(uint32_t*)(CodeData + offset);
                    
                    // Check if this is a blr instruction: 0xD63F0000 | rn
                    if ((inst & 0xFFFFFC1F) == 0xD63F0000) {
                        // Found a blr instruction, scan backwards for placeholders
                        VirtualAddressMatch match;
                        
                        bool found = YuhuVirtualAddressScanner::scan_backwards_for_placeholders(
                            CodeData,
                            offset,
                            200,  // Max scan distance: 200 bytes (~50 instructions)
                            match
                        );
                        
                        if (found) {
                            // Validate 1-1-1 relationship
                            if (match.last_java_pc_va == 0 || match.call_target_va == 0) {
                                if (YuhuTraceMachineCode) {
                                    errs() << "[OopMap Extractor] ERROR: Missing placeholders at offset " 
                                           << format_hex(offset, 8) << "\n";
                                }
                                continue;
                            }
                            
                            // Extract virtual_offset and validate
                            uint64_t ljpc_offset = YuhuVirtualAddressScanner::extract_virtual_offset(match.last_java_pc_va);
                            uint64_t ct_offset = YuhuVirtualAddressScanner::extract_virtual_offset(match.call_target_va);
                            
                            if (ljpc_offset != ct_offset) {
                                if (YuhuTraceMachineCode) {
                                    errs() << "[OopMap Extractor] ERROR: Mismatched virtual_offsets at offset " 
                                           << format_hex(offset, 8) << "\n";
                                    errs() << "  last_Java_pc offset: " << ljpc_offset << "\n";
                                    errs() << "  call_target offset: " << ct_offset << "\n";
                                }
                                continue;
                            }
                            
                            uint64_t virtual_offset = ljpc_offset;
                            
                            if (YuhuTraceMachineCode) {
                                errs() << "[OopMap Extractor] Found dual placeholders:\n";
                                errs() << "  Statepoint call offset: " << format_hex(offset, 8) << "\n";
                                errs() << "  Virtual offset: " << virtual_offset << "\n";
                                errs() << "  last_Java_pc VA: " << format_hex(match.last_java_pc_va, 16) 
                                       << " at " << format_hex(match.last_java_pc_placeholder_offset, 8) << "\n";
                                errs() << "  call_target VA: " << format_hex(match.call_target_va, 16) 
                                       << " at " << format_hex(match.call_target_placeholder_offset, 8) << "\n";
                            }
                            
                            // Look up OopMap in deferred registry
                            OopMap* oopmap = function->get_deferred_oopmap(virtual_offset);
                            
                            if (oopmap == NULL) {
                                if (YuhuTraceMachineCode) {
                                    errs() << "[OopMap Extractor] ERROR: No deferred OopMap for virtual_offset " 
                                           << virtual_offset << "\n";
                                }
                                continue;
                            }
                            
                            // Calculate actual offsets for patching
                            // The return address is the instruction AFTER the bl
                            uint64_t return_pc_offset = offset + 4;
                            
                            // Look up the actual helper address from virtual_offset mapping
                            uint64_t helper_addr = function->get_helper_address_by_offset((int)virtual_offset);
                            if (helper_addr == 0) {
                                if (YuhuTraceMachineCode) {
                                    errs() << "[OopMap Extractor] ERROR: No helper address for virtual_offset " 
                                           << virtual_offset << "\n";
                                }
                                continue;
                            }
                            
                            // Find the marker pattern with matching virtual_offset
                            uint64_t marker_offset = YuhuVirtualAddressScanner::scan_for_marker_with_offset(
                                CodeData,
                                offset,  // Search from bl instruction backwards
                                100,     // Max scan distance
                                virtual_offset  // Look for THIS specific virtual_offset
                            );
                            
                            if (marker_offset == UINT64_MAX) {
                                if (YuhuTraceMachineCode) {
                                    errs() << "[OopMap Extractor] ERROR: Could not find marker with virtual_offset "
                                           << virtual_offset << " for statepoint at offset " << offset << "\n";
                                }
                                continue;
                            }
                            
                            // The adr instruction is at marker_offset + 8 (2 instructions after marker start)
                            uint64_t adr_offset = marker_offset + 8;
                            
                            // Calculate absolute addresses for patching
                            // BaseAddr is the load address of this section (from JITLink)
                            uint64_t adr_absolute_address = BaseAddr + adr_offset;
                            uint64_t return_absolute_address = BaseAddr + return_pc_offset;
                            
                            // Patch the adr instruction with correct PC-relative offset
                            YuhuVirtualAddressScanner::patch_adr_instruction(
                                CodeData,
                                adr_offset,
                                adr_absolute_address,      // Address of adr instruction
                                return_absolute_address    // Target address (return after bl)
                            );
                            
                            // Register OopMap at the return PC offset
                            YuhuDebugInformationRecorder* debug_info = function->debug_info_recorder();
                            if (debug_info != NULL) {
                                // Calculate the actual code offset (relative to method start)
                                int code_offset = (int)return_pc_offset;
                                
                                // Register the safepoint with the OopMap
                                debug_info->add_safepoint(code_offset, oopmap);
                                
                                if (YuhuTraceMachineCode) {
                                    errs() << "  [OK] Registered OopMap at code offset: " << code_offset << "\n";
                                }
                            } else {
                                if (YuhuTraceMachineCode) {
                                    errs() << "  [WARNING] No DebugInformationRecorder, OopMap not registered\n";
                                }
                            }
                            
                            if (YuhuTraceMachineCode) {
                                errs() << "  [OK] Patched adr instruction at offset " << adr_offset 
                                       << " to point to return address at offset " << return_pc_offset << "\n";
                                errs() << "  [OK] Patched call_target with helper: " << format_hex(helper_addr, 16) << "\n";
                            }
                            
                            // Skip past this blr instruction
                            // (don't skip, as there might be multiple blr instructions in sequence)
                        }
                    }
                }
            }
        }
    }
    
    return Error::success();
}

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
#include "yuhu/yuhuDebugInformationRecorder.hpp"
#include "yuhu/yuhuCompiler.hpp"
#include "yuhu/yuhu_globals.hpp"
#include "code/debugInfoRec.hpp"
#include "compiler/oopMap.hpp"
#include "llvm/Object/StackMapParser.h"
#include "llvm/Support/Endian.h"
#include <unistd.h>  // For sysconf

using namespace llvm;

extern "C" void gc_safepoint_poll();

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

void CallSiteExtractorPlugin::notifyLoaded(llvm::orc::MaterializationResponsibility &MR) {
}

llvm::Error CallSiteExtractorPlugin::notifyEmitted(llvm::orc::MaterializationResponsibility &MR) {
    return Error::success();

}

llvm::Error CallSiteExtractorPlugin::notifyFailed(llvm::orc::MaterializationResponsibility &MR) {
    return Error::success();

}

llvm::Error CallSiteExtractorPlugin::notifyRemovingResources(llvm::orc::JITDylib &JD, llvm::orc::ResourceKey K) {
    return Error::success();
}

void CallSiteExtractorPlugin::notifyTransferringResources(llvm::orc::JITDylib &JD, llvm::orc::ResourceKey DstKey,
                                                           llvm::orc::ResourceKey SrcKey) {
}

// CallSiteExtractorPlugin implementation
// Scans for movz/movk/blr patterns and extracts VM call site information
void CallSiteExtractorPlugin::modifyPassConfig(llvm::orc::MaterializationResponsibility &MR,
                                              llvm::jitlink::LinkGraph &LG,
                                              llvm::jitlink::PassConfiguration &PassConfig) {
    // After dead code is emitted, and after relocation happens, as we don't need to take care of edge update
    PassConfig.PostFixupPasses.push_back(
        [this, &MR](
            jitlink::LinkGraph &G
        ) -> Error {
            return extractCallSites(G, MR);
        });
}

llvm::Error CallSiteExtractorPlugin::extractCallSites(llvm::jitlink::LinkGraph &G,
                                                      llvm::orc::MaterializationResponsibility &MR) {
    // Iterate over all sections looking for code sections
    for (auto &Section : G.sections()) {
        // Only process executable sections (code)
        // Section.getName() returns in format of segment,section
        if (!Section.getName().ends_with("__text"))
            continue;

        // Search function symbol
        llvm::jitlink::Symbol* found_func;
        for (auto *Sym : Section.symbols()) {
            if (!Sym->hasName()) {
                continue;
            }
            if (YuhuDebugInformationRecorder::get()->get_mangled_func_name() == ((*(Sym->getName())).str())) {
                found_func = Sym;
                if (YuhuTraceMachineCode) {
                    errs() << "[CallSite Extractor] Sym: " << *(Sym->getName())
                           << ", address: " << Sym->getAddress().getValue()
                           << ", start: " << Sym->getRange().Start.getValue()
                           << ", end: " << Sym->getRange().End.getValue() << "\n";
                }
                // Populate func size
                YuhuDebugInformationRecorder::get()->set_func_size(Sym->getRange().End.getValue() - Sym->getRange().Start.getValue());
                break;
            }
        }
        if (!found_func) {
            continue;
        }

        // Iterate over all blocks in this section
        for (auto *Block : Section.blocks()) {
            // Get mutable content so we can patch it
            auto Content = Block->getMutableContent(G);
            uint64_t BaseAddr = Block->getAddress().getValue();
            size_t Size = Block->getSize();

            if (Size < 32) continue;  // Need at least 8 instructions for dual placeholders, 4 for last java pc and 4 for call target

            if (!(BaseAddr >= found_func->getRange().Start.getValue() && (BaseAddr + Block->getSize()) <= found_func->getRange().End.getValue())) {
                // Skip if block doesn't fall into the function code range
                continue;
            }

            size_t block_offset = BaseAddr - found_func->getRange().Start.getValue();

            uint8_t* CodeData = reinterpret_cast<uint8_t*>(Content.data());

            // For now, scan for call instructions (blr) and look backwards for placeholders
            for (size_t offset = 0; offset + 4 <= Size; offset += 4) {

                // Let's scan forward. Coz sometime llvm uses b instruction to jump to common machine code in order to share machine code for multiple call targets,
                // and blr instruction is not always right after movz/movk sequences.
                // Need 2 instructions at least.
                if (offset + 8 <= Size && YuhuVirtualAddressScanner::is_placeholder_pc_pattern((uint32_t*)(CodeData + offset))) {
                    VirtualAddressMatch match;

                    bool found = YuhuVirtualAddressScanner::scan_forwards_for_call_targets(
                            CodeData,
                            offset,
                            offset + 200 <= Size ? 200 : Size - offset,
                            match);

                    if (found && !match.safepoint_poll_call) {
                        // Validate 1-1-1 relationship
                        if (match.last_java_pc_va == 0 || match.call_target_va == 0) {
                            if (YuhuTraceMachineCode) {
                                errs() << "[CallSite Extractor] ERROR: Missing placeholders at offset "
                                       << format_hex(offset, 8) << "\n";
                            }
                            continue;
                        }

                        // Extract virtual_offset and validate
                        uint64_t ljpc_offset = YuhuVirtualAddressScanner::extract_virtual_offset_from_virtual_last_java_pc(match.last_java_pc_va);
                        uint64_t ct_offset = YuhuVirtualAddressScanner::extract_virtual_offset_from_virtual_call_target(match.call_target_va);

                        if (ljpc_offset != ct_offset) {
                            if (YuhuTraceMachineCode) {
                                errs() << "[CallSite Extractor] ERROR: Mismatched virtual_offsets at offset "
                                       << format_hex(offset, 8) << "\n";
                                errs() << "  last_Java_pc offset: " << ljpc_offset << "\n";
                                errs() << "  call_target offset: " << ct_offset << "\n";
                            }
                            continue;
                        }

                        uint64_t virtual_offset = ljpc_offset;

                        if (YuhuTraceMachineCode) {
                            errs() << "[CallSite Extractor] Found dual placeholders:\n";
                            errs() << "  call offset: " << format_hex(offset, 8) << "\n";
                            errs() << "  Virtual offset: " << virtual_offset << "\n";
                            errs() << "  last_Java_pc VA: " << format_hex(match.last_java_pc_va, 16)
                                   << " at " << format_hex(match.last_java_pc_placeholder_offset, 8) << "\n";
                            errs() << "  call_target VA: " << format_hex(match.call_target_va, 16)
                                   << " at " << format_hex(match.call_target_placeholder_offset, 8) << "\n";
                        }

                        // Calculate actual offsets for patching
                        // The return address is the instruction AFTER the bl
                        uint64_t return_pc_offset = match.call_target_blr_offset + 4;

                        // Look up the actual helper address from virtual_offset mapping
                        uint64_t helper_addr = YuhuDebugInformationRecorder::get()->get_call_site_helper_address_by_offset(virtual_offset);
                        if (helper_addr == 0) {
                            if (YuhuTraceMachineCode) {
                                errs() << "[CallSite Extractor] ERROR: No helper address for virtual_offset "
                                       << virtual_offset << "\n";
                            }
                            continue;
                        }

                        // update return_pc_offset by virtual_offsets
                        YuhuDebugInformationRecorder::get()->update_call_site_return_pc_offset(virtual_offset, block_offset + return_pc_offset);

                        if (YuhuDebugInformationRecorder::get()->get_call_site_type_by_offset(virtual_offset) == CallSiteType::vm_call) {
                            // The adr instruction is at marker_offset + 8 (2 instructions after marker start)
                            uint64_t adr_offset = match.last_java_pc_placeholder_offset + 8;

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
                            if (YuhuTraceMachineCode) {
                                errs() << "  [OK] Patched adr instruction at offset " << adr_offset
                                       << " to point to return address at offset " << return_pc_offset << "\n";
                            }
                        }

                        YuhuVirtualAddressScanner::patch_call_target_instructions(CodeData, match.call_target_placeholder_offset,
                                                                                  YuhuDebugInformationRecorder::get()->get_call_site_helper_address_by_offset(virtual_offset));

                        if (YuhuTraceMachineCode) {
                            errs() << "  [OK] Patched call_target with helper: " << format_hex(helper_addr, 16) << "\n";
                        }
                    } else if (found && match.safepoint_poll_call) {
                        if (match.last_java_pc_va == 0) {
                            if (YuhuTraceMachineCode) {
                                errs() << "[CallSite Extractor] ERROR: Missing placeholders at offset "
                                       << format_hex(offset, 8) << "\n";
                            }
                            continue;
                        }
                        uint64_t ljpc_offset = YuhuVirtualAddressScanner::extract_virtual_offset_from_virtual_last_java_pc(match.last_java_pc_va);
                        uint64_t virtual_offset = ljpc_offset;
                        // Calculate actual offsets for patching
                        // The return address is the instruction AFTER the bl
                        uint64_t return_pc_offset = match.call_target_blr_offset + 4;

                        // Look up the actual helper address from virtual_offset mapping
                        uint64_t helper_addr = YuhuDebugInformationRecorder::get()->get_call_site_helper_address_by_offset((int)virtual_offset);
                        if (helper_addr == 0 || helper_addr != (uint64_t)&gc_safepoint_poll) {
                            if (YuhuTraceMachineCode) {
                                errs() << "[CallSite Extractor] ERROR: No helper address or not safepoint poll call for virtual_offset "
                                       << virtual_offset << "\n";
                            }
                            continue;
                        }
                        // update return_pc_offset by virtual_offsets
                        YuhuDebugInformationRecorder::get()->update_call_site_return_pc_offset((int)virtual_offset, block_offset + return_pc_offset);
                    }
                }
            }
        }
    }
    
    return Error::success();
}

void GOTAndPLTHandlerPlugin::notifyLoaded(llvm::orc::MaterializationResponsibility &MR) {
}

llvm::Error GOTAndPLTHandlerPlugin::notifyEmitted(llvm::orc::MaterializationResponsibility &MR) {
    return Error::success();

}

llvm::Error GOTAndPLTHandlerPlugin::notifyFailed(llvm::orc::MaterializationResponsibility &MR) {
    return Error::success();

}

llvm::Error GOTAndPLTHandlerPlugin::notifyRemovingResources(llvm::orc::JITDylib &JD, llvm::orc::ResourceKey K) {
    return Error::success();
}

void GOTAndPLTHandlerPlugin::notifyTransferringResources(llvm::orc::JITDylib &JD, llvm::orc::ResourceKey DstKey,
                                                          llvm::orc::ResourceKey SrcKey) {
}

void GOTAndPLTHandlerPlugin::modifyPassConfig(llvm::orc::MaterializationResponsibility &MR,
                                               llvm::jitlink::LinkGraph &LG,
                                               llvm::jitlink::PassConfiguration &PassConfig) {
    // The correct phase order:
    // PrePrunePasses - mark live/discard symbols
    // Dead stripping happens
    // PostPrunePasses ← GOT/PLT building belongs here
    // Memory allocation + address assignment
    // PreFixupPasses - late optimizations
    // Fixups applied (relocations resolved)
    // PostFixupPasses - testing/validatio
    PassConfig.PostPrunePasses.push_back(
            [this, &MR](
                    jitlink::LinkGraph &G
            ) -> Error {
//                jitlink::aarch64::GOTTableManager GOT(G);
//                jitlink::aarch64::PLTTableManager PLT(G, GOT);
//                jitlink::visitExistingEdges(G, GOT, PLT);
                return visitEdges(G, MR);
            });
}

llvm::Error GOTAndPLTHandlerPlugin::visitEdges(llvm::jitlink::LinkGraph &G,
                                                      llvm::orc::MaterializationResponsibility &MR) {
    jitlink::aarch64::GOTTableManager GOT(G);
    jitlink::aarch64::PLTTableManager PLT(G, GOT);

    // Iterate all blocks
    for (auto *Block : G.blocks()) {
        for (auto &Edge : Block->edges()) {
            auto Kind = Edge.getKind();
            errs() << "Before fixing " << G.getEdgeKindName(Kind) << " edge at "
                   << Block->getFixupAddress(Edge) << " (" << Block->getAddress() << " + "
                   << formatv("{0:x}", Edge.getOffset()) << ")\n";
            if (Kind >= llvm::jitlink::Edge::GenericEdgeKind::FirstRelocation &&
                (Kind == llvm::jitlink::aarch64::Page21 ||
                 Kind == llvm::jitlink::aarch64::PageOffset12 ||
                 Kind == llvm::jitlink::aarch64::GotPageOffset15 ||
                 Kind == llvm::jitlink::aarch64::Delta32 ||
                 (Kind == llvm::jitlink::aarch64::Branch26PCRel && !Edge.getTarget().isDefined()))) {
                // skip already processed Edges
                errs() << "Skip already processed Edges" << "\n";
                continue;
            }
            if (GOT.visitEdge(G, Block, Edge)) {
                errs() << "GOT fixing " << G.getEdgeKindName(Kind) << " edge at "
                       << Block->getFixupAddress(Edge) << " (" << Block->getAddress() << " + "
                       << formatv("{0:x}", Edge.getOffset()) << ")\n";
            } else if (PLT.visitEdge(G, Block, Edge)) {
                errs() << "PLT fixing " << G.getEdgeKindName(Kind) << " edge at "
                       << Block->getFixupAddress(Edge) << " (" << Block->getAddress() << " + "
                       << formatv("{0:x}", Edge.getOffset()) << ")\n";
            }
            errs() << "After fixing " << G.getEdgeKindName(Kind) << " edge at "
                   << Block->getFixupAddress(Edge) << " (" << Block->getAddress() << " + "
                   << formatv("{0:x}", Edge.getOffset()) << ")\n";
        }
    }
    return Error::success();
}
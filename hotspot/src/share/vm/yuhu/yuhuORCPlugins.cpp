/*
 * Copyright (c) 2024, Yuhu Compiler Project
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only.
 */

#include "precompiled.hpp"
#include "yuhu/yuhuORCPlugins.hpp"
#include "yuhu/yuhuVirtualAddressPatcher.hpp"
#include "yuhu/yuhuFunction.hpp"
#include "yuhu/yuhu_globals.hpp"
#include "llvm/Support/Endian.h"

#pragma push_macro("assert")
#ifdef assert
#undef assert
#endif

#include "yuhu/llvmHeaders.hpp"
#include "llvm/Object/StackMapParser.h"

#pragma pop_macro("assert")

#include "yuhu/yuhuDebugInformationRecorder.hpp"

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
    auto recorder = YuhuDebugInformationRecorder::get();
    for (auto &Section : G.sections()) {
        if (!Section.getName().ends_with("__const"))
            continue;
        for (auto *Sym : Section.symbols()) {
            assert(Sym->getAddress().getValue() == Sym->getRange().Start.getValue(), "addr should match to range start");
            recorder->register_const_symbol(Sym->getAddress().getValue(), Sym->getRange().Start.getValue(), Sym->getRange().End.getValue());
            if (YuhuTraceMachineCode) {
                errs() << "[CallSite Extractor] const, address: "
                        << Sym->getAddress().getValue()
                        << ", start: " << Sym->getRange().Start.getValue()
                        << ", end: " << Sym->getRange().End.getValue()
                        << ", section name: " << Section.getName() << "\n";
            }
        }
    }

    GrowableArray<SymbolEntry> got_symbol_entries;

    for (auto &Section : G.sections()) {
        if (!Section.getName().ends_with("$__GOT"))
            continue;
        for (auto *Sym : Section.symbols()) {
            assert(Sym->getAddress().getValue() == Sym->getRange().Start.getValue(), "addr should match to range start");
            uint64_t target_addr = Sym->getAddress().getValue();
            uint64_t function_address = *(uint64_t*)target_addr;
            assert(function_address != 0, "function address should be valid");
            assert(recorder->contains_call_target(function_address) || recorder->get_const_symbol_by_range_addr(function_address),
                   "addr should be call target or const address");

            SymbolEntry entry{};
            entry.addr = target_addr;
            entry.start = Sym->getRange().Start.getValue();
            entry.end = Sym->getRange().End.getValue();
            got_symbol_entries.append(entry);
            if (YuhuTraceMachineCode) {
                errs() << "[CallSite Extractor] got, address: "
                       << Sym->getAddress().getValue()
                       << ", start: " << Sym->getRange().Start.getValue()
                       << ", end: " << Sym->getRange().End.getValue()
                       << ", section name: " << Section.getName() << "\n";
            }
        }
    }

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
            if (recorder->get_mangled_func_name() == ((*(Sym->getName())).str())) {
                found_func = Sym;
                if (YuhuTraceMachineCode) {
                    errs() << "[CallSite Extractor] Sym: " << *(Sym->getName())
                           << ", address: " << Sym->getAddress().getValue()
                           << ", start: " << Sym->getRange().Start.getValue()
                           << ", end: " << Sym->getRange().End.getValue()
                           << ", section name: " << Section.getName() << "\n";;

                    Disassembler::decode((address)Sym->getRange().Start.getValue(), (address)Sym->getRange().End.getValue(), tty);
                }
                // Populate func size
                recorder->set_func_size(Sym->getRange().End.getValue() - Sym->getRange().Start.getValue());
                break;
            }
        }
        if (!found_func) {
            continue;
        }

        ResourceMark rm;
        auto patchpoints = recorder->get_patchpoint_stack_maps();
        for (int i = 0; i < patchpoints->length(); ++i) {
            auto patchpoint = patchpoints->at(i);
            uint32_t offset_in_func = patchpoint->stack_map_entry->instruction_offset;
            uint64_t patchpoint_addr = found_func->getRange().Start.getValue() + offset_in_func;
            for (auto *Block : Section.blocks()) {
                uint64_t BaseAddr = Block->getAddress().getValue();
                size_t Size = Block->getSize();
                if (!(BaseAddr <= patchpoint_addr && patchpoint_addr < (BaseAddr + Size))) {
                    // Skip if patchpoint doesn't fall into the block code range
                    continue;
                }
                assert((patchpoint_addr + patchpoint->patchpoint_entry->reserved_bytes) <= (BaseAddr + Size), "Block should cover reserved bytes");
                if (patchpoint->patchpoint_entry->call_site_statepoint_id != 0) {
                    CallSiteType call_site_type = recorder->get_call_site_type_by_statepoint_id(patchpoint->patchpoint_entry->call_site_statepoint_id);
                    assert(call_site_type != CallSiteType::none, "call site type should not be none");
                    if (call_site_type == CallSiteType::unwind_call) {
                        // patch to unwind handler, should do it in YuhuBuilder::scan_and_generate_all_relocations
                    }
                }
            }
        }

        // Iterate over all blocks in this section
        for (auto *Block : Section.blocks()) {
            uint64_t BaseAddr = Block->getAddress().getValue();
            size_t Size = Block->getSize();
            if (!(BaseAddr >= found_func->getRange().Start.getValue() && (BaseAddr + Size) <= found_func->getRange().End.getValue())) {
                // Skip if block doesn't fall into the function code range
                continue;
            }
            size_t block_offset = BaseAddr - found_func->getRange().Start.getValue();
            for (auto &Edge : Block->edges()) {
                auto Kind = Edge.getKind();
                uint64_t target_addr = Edge.getTarget().getAddress().getValue();
                auto section_name = Edge.getTarget().getSection().getName();
                errs() << "Getting " << G.getEdgeKindName(Kind) << ", " << target_addr << ", " << section_name << " edge at "
                       << Block->getFixupAddress(Edge) << " (" << Block->getAddress() << " + "
                       << formatv("{0:x}", Edge.getOffset()) << ")\n";

                int index = got_symbol_entries.find(&target_addr, [](void* token, const SymbolEntry entry) -> bool {
                    return *((uint64_t*)token) == entry.addr;
                });
                if (index != -1) {
                    // if it is GOT symbol
                    recorder->register_edge(block_offset + Edge.getOffset(), EdgeTargetType::got_symbol, target_addr);
                } else if (recorder->get_const_symbol_by_range_addr(target_addr)) {
                    // if it is const symbol
                    recorder->register_edge(block_offset + Edge.getOffset(), EdgeTargetType::const_symbol, target_addr);
                } else if (Kind != llvm::jitlink::Edge::GenericEdgeKind::KeepAlive) {
                    ShouldNotReachHere();
                }
            }
        }
    }

    YuhuDebugInformationRecorder::get()->clean_eliminated_call_sites();
    
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
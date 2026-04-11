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
#include "yuhu/yuhu_globals.hpp"
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

bool OopMapExtractorPlugin::isMovzMovkBlrSequence(const uint32_t* inst, uint64_t& call_target) {
    // Pattern: movz (lsl #32) -> movk (lsl #16) -> movk (lsl #0) -> blr
    // This loads a 48-bit address and makes an indirect call
    
    uint32_t rd = inst[0] & 0x1F;
    
    // Check movz with lsl #32: 0xD2800000 | (imm << 5) | rd | (2 << 21)
    bool is_movz_32 = (inst[0] & 0xFF800000) == 0xD2800000 &&
                      ((inst[0] >> 21) & 0x3) == 2 &&
                      (inst[0] & 0x1F) == rd;
    
    if (!is_movz_32) return false;
    
    // Check movk with lsl #16: 0xF2800000 | (imm << 5) | rd | (1 << 21)
    bool is_movk_16 = (inst[1] & 0xFF800000) == 0xF2800000 &&
                      ((inst[1] >> 21) & 0x3) == 1 &&
                      (inst[1] & 0x1F) == rd;
    
    // Check movk with lsl #0: 0xF2800000 | (imm << 5) | rd | (0 << 21)
    bool is_movk_0 = (inst[2] & 0xFF800000) == 0xF2800000 &&
                     ((inst[2] >> 21) & 0x3) == 0 &&
                     (inst[2] & 0x1F) == rd;
    
    // Check blr: 0xD63F0000 | rn
    bool is_blr = (inst[3] & 0xFFFFFC1F) == 0xD63F0000 &&
                  (inst[3] & 0x1F) == rd;
    
    if (is_movk_16 && is_movk_0 && is_blr) {
        // Reconstruct the 48-bit address
        uint64_t addr = 0;
        addr |= ((uint64_t)((inst[0] >> 5) & 0xFFFF)) << 32;  // movz
        addr |= ((uint64_t)((inst[1] >> 5) & 0xFFFF)) << 16;  // movk #16
        addr |= ((uint64_t)((inst[2] >> 5) & 0xFFFF)) << 0;   // movk #0
        
        call_target = addr;
        return true;
    }
    
    return false;
}

llvm::Error OopMapExtractorPlugin::extractCallSites(llvm::jitlink::LinkGraph &G,
                                                      llvm::orc::MaterializationResponsibility &MR) {
    // Always run this pass (not just when tracing)
    
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
                
                if (Size < 16) continue;  // Need at least 4 instructions
                
                uint8_t* CodeData = reinterpret_cast<uint8_t*>(Content.data());
                
                // Scan for movz/movk/blr pattern
                for (size_t offset = 0; offset + 16 <= Size; offset += 4) {
                    uint32_t* inst = reinterpret_cast<uint32_t*>(CodeData + offset);
                    
                    uint64_t loaded_address = 0;
                    if (isMovzMovkBlrSequence(inst, loaded_address)) {
                        // Check if this is a virtual address (0xDEADxxxxxxxxxx)
                        if ((loaded_address >> 48) == 0xDEAD) {
                            // Extract virtual_offset
                            int virtual_offset = (loaded_address >> 16) & 0xFFFFFFFF;
                            
                            if (YuhuTraceMachineCode) {
                                errs() << "[OopMap Extractor] Found virtual address call site:\n";
                                errs() << "  Virtual offset: " << virtual_offset << "\n";
                                errs() << "  Virtual address: " << format_hex(loaded_address, 16) << "\n";
                                errs() << "  Location: " << format_hex(BaseAddr + offset, 16) << "\n";
                            }
                            
                            // TODO: Look up actual helper address from YuhuFunction
                            // For now, we need to access the function's call site registry
                            // This requires passing the function pointer to the plugin
                            // For the first iteration, we'll just log and skip patching
                            
                            if (YuhuTraceMachineCode) {
                                errs() << "  [TODO] Patch with actual helper address\n";
                                errs() << "  [TODO] Record OopMap at return PC\n";
                            }
                            
                            // Skip past this sequence
                            offset += 12;
                        }
                    }
                }
            }
        }
    }
    
    return Error::success();
}

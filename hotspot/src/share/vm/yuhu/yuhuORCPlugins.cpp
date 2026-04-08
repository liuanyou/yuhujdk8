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

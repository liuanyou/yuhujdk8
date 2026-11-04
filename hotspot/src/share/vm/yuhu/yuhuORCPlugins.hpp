/*
 * Copyright (c) 2024, Yuhu Compiler Project
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only.
 */

#ifndef SHARE_VM_YUHU_YUHUORCPLUGINS_HPP
#define SHARE_VM_YUHU_YUHUORCPLUGINS_HPP

#include "yuhu/llvmHeaders.hpp"

// Forward declarations
class YuhuFunction;

// ORC JIT Plugin declarations
// All ObjectLinkingLayer::Plugin implementations should be defined here

namespace llvm {

    namespace orc {

        // Forward declarations
        class ObjectLinkingLayer;

        class MaterializationResponsibility;

        class JITDylib;

    } // namespace orc

    namespace jitlink {
        class LinkGraph;

        class PassConfiguration;
    } // namespace jitlink
} // namespace llvm

// MachineCodePrinterPlugin - Print machine code after JIT compilation
// This plugin traces all generated machine instructions for debugging
class MachineCodePrinterPlugin : public llvm::orc::ObjectLinkingLayer::Plugin {
public:
    void notifyLoaded(llvm::orc::MaterializationResponsibility &MR) override;

    llvm::Error notifyEmitted(llvm::orc::MaterializationResponsibility &MR) override;

    llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility &MR) override;

    llvm::Error notifyRemovingResources(llvm::orc::JITDylib &JD, llvm::orc::ResourceKey K) override;

    void notifyTransferringResources(llvm::orc::JITDylib &JD, llvm::orc::ResourceKey DstKey,
                                     llvm::orc::ResourceKey SrcKey) override;

    void modifyPassConfig(llvm::orc::MaterializationResponsibility &MR,
                          llvm::jitlink::LinkGraph &LG,
                          llvm::jitlink::PassConfiguration &PassConfig) override;
private:
    llvm::Error dumpMachineCode(llvm::jitlink::LinkGraph &G);
};

// CallSiteExtractorPlugin - Harvest the finalized LinkGraph for the nmethod builder.
// It registers a post-fixup pass, so every address and edge value it reads is the
// linked one.  It patches no machine code; it records into the calling compiler
// thread's YuhuDebugInformationRecorder:
//   1. __DATA,__const / __DATA,__data symbol ranges (jump tables, oop/metadata slots)
//   2. interior fixups - pointer words living inside those symbols, classified by
//      comparing the in-memory value against the resolved edge, because the Mach-O
//      arm64 pointer edge kinds are not exposed by LLVM's public headers
//      (absolute64 / relative64 / relative32)
//   3. $__GOT entries, validated to reference a known call target or const/data symbol
//   4. the compiled function's __text edges, tagged got_symbol or const_symbol and
//      stored as function-relative offsets (an ADRP + LDR pair yields two edges)
// The nmethod builder then copies the referenced symbols into cb->consts(), re-points
// the ADRP immediates at the copies and registers the relocations that keep the
// references - the code-side hop and the pointer words inside the copied tables -
// valid once the code leaves the LLVM arena.
class CallSiteExtractorPlugin : public llvm::orc::ObjectLinkingLayer::Plugin {
public:
    void notifyLoaded(llvm::orc::MaterializationResponsibility &MR) override;

    llvm::Error notifyEmitted(llvm::orc::MaterializationResponsibility &MR) override;

    llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility &MR) override;

    llvm::Error notifyRemovingResources(llvm::orc::JITDylib &JD, llvm::orc::ResourceKey K) override;

    void notifyTransferringResources(llvm::orc::JITDylib &JD, llvm::orc::ResourceKey DstKey,
                                     llvm::orc::ResourceKey SrcKey) override;

    void modifyPassConfig(llvm::orc::MaterializationResponsibility &MR,
                          llvm::jitlink::LinkGraph &LG,
                          llvm::jitlink::PassConfiguration &PassConfig) override;

private:
    llvm::Error extractCallSites(llvm::jitlink::LinkGraph &G,
                                 llvm::orc::MaterializationResponsibility &MR);
};

// GOTAndPLTHandlerPlugin - handle edge
// 1. LLVM generates edges like RequestGOTAndTransformToPage21 and RequestGOTAndTransformToPageOffset12
// 2. GOTTableManager::visitEdge() transforms these edges to point to GOT entries
// 3. GOTTableManager::createEntry() creates GOT entries populated with the resolved symbol address
class GOTAndPLTHandlerPlugin : public llvm::orc::ObjectLinkingLayer::Plugin {
public:
    void notifyLoaded(llvm::orc::MaterializationResponsibility &MR) override;

    llvm::Error notifyEmitted(llvm::orc::MaterializationResponsibility &MR) override;

    llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility &MR) override;

    llvm::Error notifyRemovingResources(llvm::orc::JITDylib &JD, llvm::orc::ResourceKey K) override;

    void notifyTransferringResources(llvm::orc::JITDylib &JD, llvm::orc::ResourceKey DstKey,
                                     llvm::orc::ResourceKey SrcKey) override;

    void modifyPassConfig(llvm::orc::MaterializationResponsibility &MR,
                          llvm::jitlink::LinkGraph &LG,
                          llvm::jitlink::PassConfiguration &PassConfig) override;
private:
private:
    llvm::Error visitEdges(llvm::jitlink::LinkGraph &G,
                                 llvm::orc::MaterializationResponsibility &MR);
};

#endif // SHARE_VM_YUHU_YUHUORCPLUGINS_HPP

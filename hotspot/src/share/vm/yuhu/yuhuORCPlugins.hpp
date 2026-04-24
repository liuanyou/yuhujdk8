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

// CallSiteExtractorPlugin - Extract call site information from compiled machine code
// This plugin scans for dual virtual address placeholders (0xDEADxxxx and 0xBEEFBEEFxxxx)
// and patches them with actual values, then update call site with correct return pc addresses
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

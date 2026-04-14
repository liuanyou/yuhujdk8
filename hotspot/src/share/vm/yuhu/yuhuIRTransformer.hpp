/*
 * Copyright (c) 2024, Yuhu Compiler Project
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only.
 */

#ifndef SHARE_VM_YUHU_YUHUIRTRANSFORMER_HPP
#define SHARE_VM_YUHU_YUHUIRTRANSFORMER_HPP

#include "yuhu/llvmHeaders.hpp"

class YuhuIRTransformer {
public:
    // 你的 IR 转换函数
    static llvm::Expected<llvm::orc::ThreadSafeModule> runGCPasses(llvm::orc::ThreadSafeModule TSM,
                                           llvm::orc::MaterializationResponsibility &MR);
};

#endif // SHARE_VM_YUHU_YUHUIRTRANSFORMER_HPP

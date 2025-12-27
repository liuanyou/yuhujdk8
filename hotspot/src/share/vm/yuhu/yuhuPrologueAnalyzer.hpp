/*
 * Copyright (c) 2024. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 */

#ifndef SHARE_VM_YUHU_YUHU_PROLOGUE_ANALYZER_HPP
#define SHARE_VM_YUHU_YUHU_PROLOGUE_ANALYZER_HPP

#include "memory/allocation.hpp"

// Analyze AArch64 prologue to determine actual stack space used by callee-saved register spills.
class YuhuPrologueAnalyzer : public AllStatic {
public:
  // Analyze the prologue of an AArch64 function starting at code_start.
  // Returns the total stack bytes allocated by the prologue (from stp instructions).
  static int analyze_prologue_stack_bytes(address code_start);

private:
  // Check if instruction is a pre-indexed stp (e.g., stp x29, x30, [sp, #-16]!)
  static bool is_stp_pre_index(uint32_t inst);
  
  // Extract the immediate offset from stp instruction (in bytes)
  static int extract_stp_immediate(uint32_t inst);
  
  // Check if instruction is a regular stack frame allocation (e.g., sub sp, sp, #N)
  static bool is_sub_sp_imm(uint32_t inst);
};

#endif // SHARE_VM_YUHU_YUHU_PROLOGUE_ANALYZER_HPP

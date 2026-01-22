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

  // Find the offset of x28's saved value relative to x29 (frame pointer)
  // Returns the offset as a positive number for use in: ldr x8, [x29, #offset]
  // Returns -1 if x28 is not found in the prologue
  static int find_x28_offset_from_x29(address code_start);

  // Check if instruction is sub sp, sp, #imm (indicates end of prologue, start of Yuhu frame alloc)
  static bool is_sub_sp_imm(uint32_t inst);

private:
  // Check if instruction is a pre-indexed stp (e.g., stp x29, x30, [sp, #-16]!)
  static bool is_stp_pre_index(uint32_t inst);
  
  // Extract the immediate offset from stp instruction (in bytes)
  static int extract_stp_immediate(uint32_t inst);
  
  // Check if instruction is stp x28 pre-indexed
  static bool is_stp_x28_pre_index(uint32_t inst);
  
  // Check if instruction is stp x28 post-indexed
  static bool is_stp_x28_post_index(uint32_t inst);
  
  // Check if instruction is stp x28 regular
  static bool is_stp_x28_regular(uint32_t inst);
  
  // Check if instruction is add x29, sp, #imm
  static bool is_add_x29_sp_imm(uint32_t inst);
  
  // Extract immediate from add instruction
  static int extract_add_immediate(uint32_t inst);
  
  // Extract Rt from stp instruction
  static int extract_stp_rt(uint32_t inst);
  
  // Extract Rt2 from stp instruction
  static int extract_stp_rt2(uint32_t inst);
};

#endif // SHARE_VM_YUHU_YUHU_PROLOGUE_ANALYZER_HPP

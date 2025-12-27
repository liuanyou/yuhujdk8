/*
 * Copyright (c) 2024. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 */

#include "precompiled.hpp"
#include "yuhu/yuhuPrologueAnalyzer.hpp"
#include "utilities/globalDefinitions.hpp"

// Analyze AArch64 prologue to determine stack space used by callee-saved register spills.
int YuhuPrologueAnalyzer::analyze_prologue_stack_bytes(address code_start) {
  if (code_start == NULL) {
    return 0;
  }

  int total_prologue_bytes = 0;
  unsigned char* pc = (unsigned char*)code_start;

  // Scan the first 10 instructions (prologue is typically 3-6 instructions)
  for (int i = 0; i < 10; i++) {
    uint32_t inst = *(uint32_t*)pc;

    // Check for pre-indexed stp (e.g., stp x29, x30, [sp, #-16]!)
    if (is_stp_pre_index(inst)) {
      int imm = extract_stp_immediate(inst);
      // imm is negative (e.g., -16, -32)
      total_prologue_bytes += (-imm);
    }
    // If we hit a sub sp, sp, #N instruction, that's Yuhu's frame allocation, not prologue
    else if (is_sub_sp_imm(inst)) {
      break;  // End of LLVM prologue
    }

    pc += 4;  // AArch64 instructions are 4 bytes
  }

  return total_prologue_bytes;
}

// AArch64 stp (pre-indexed) encoding:
// 31 30 29 28 27 26 25 24 23 22 21 20 19 18 17 16 15 14 13 12 11 10 09 08 07 06 05 04 03 02 01 00
// |opc| 1  0  1  0  0  1| V| 1  1|  imm7       |  Rt2     |  Rn      |  Rt      |
// For 64-bit GP regs: opc = 10, V = 0
// Pre-indexed: bit 24 = 1, bit 23 = 1 (writeback mode)
// Rn = 31 (sp)
bool YuhuPrologueAnalyzer::is_stp_pre_index(uint32_t inst) {
  // Check key fields:
  // - Bits [31:30] = 10 (opc = 64-bit)
  // - Bits [29:25] = 10100 (stp/ldp family)
  // - Bit 26 (V) = 0 (general-purpose registers)
  // - Bits [24:23] = 11 (pre-indexed writeback)
  // - Bit 22 = 0 (store, not load)
  // - Bits [9:5] = 11111 (Rn = sp)
  
  // First check: is this stp family? (bits [31:22] = 1010 0100 11)
  uint32_t stp_family_mask = 0xFFC00000;  // Mask bits [31:22]
  uint32_t stp_family_bits = 0xA9800000;  // stp pre-indexed pattern
  
  if ((inst & stp_family_mask) != stp_family_bits) {
    return false;
  }
  
  // Second check: Rn must be sp (31)
  uint32_t rn = (inst >> 5) & 0x1F;
  return (rn == 31);
}

// Extract the signed 7-bit immediate from stp instruction (scaled by 8 for 64-bit regs)
int YuhuPrologueAnalyzer::extract_stp_immediate(uint32_t inst) {
  // imm7 is bits [21:15]
  int imm7 = (inst >> 15) & 0x7F;
  
  // Sign-extend from 7 bits to 32 bits
  if (imm7 & 0x40) {
    imm7 |= 0xFFFFFF80;  // Sign extend
  }
  
  // Scale by 8 (since we're storing 64-bit registers)
  return imm7 * 8;
}

// Check if instruction is sub sp, sp, #imm (indicates end of prologue, start of Yuhu frame alloc)
// Encoding: sub sp, sp, #imm
// 31 30 29 28 27 26 25 24 23 22 21 20 ... 05 04 03 02 01 00
// |1  1  0  1  0  0  0  1  0  0|shift|      imm12        | Rn   | Rd   |
// Rn = Rd = 31 (sp)
bool YuhuPrologueAnalyzer::is_sub_sp_imm(uint32_t inst) {
  // sub sp, sp, #imm: bits [31:21] = 11010010000 or 11010010010 (with shift)
  // bits [9:5] = 11111 (Rn = sp), bits [4:0] = 11111 (Rd = sp)
  uint32_t mask = 0xFFC003FF;
  uint32_t expected = 0xD10003FF;  // sub sp, sp, #imm (shift = 0)
  
  if ((inst & mask) == expected) {
    return true;
  }
  
  // Also check with shift = 1 (lsl #12)
  uint32_t expected_shift = 0xD14003FF;
  return (inst & mask) == expected_shift;
}

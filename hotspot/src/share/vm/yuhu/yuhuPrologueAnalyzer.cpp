/*
 * Copyright (c) 2024. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 */

#include "precompiled.hpp"
#include "yuhu/yuhuPrologueAnalyzer.hpp"
#include "utilities/globalDefinitions.hpp"

// Analyze AArch64 prologue to determine stack space used by callee-saved register spills.
// NOTE: This includes both callee-saved register saves AND spill space allocated by LLVM.
// LLVM allocates spill space (sub sp, sp, #N) AFTER setting up the frame pointer,
// and we need to count it as part of the prologue for correct frame_size calculation.
int YuhuPrologueAnalyzer::analyze_prologue_stack_bytes(address code_start) {
  if (code_start == NULL) {
    return 0;
  }

  int total_prologue_bytes = 0;
  unsigned char* pc = (unsigned char*)code_start;
  bool found_frame_setup = false;

  // Scan the first 10 instructions (prologue is typically 6-10 instructions)
  for (int i = 0; i < 10; i++) {
    uint32_t inst = *(uint32_t*)pc;

    // Check for pre-indexed stp (e.g., stp x29, x30, [sp, #-16]!)
    if (is_stp_pre_index(inst)) {
      int imm = extract_stp_immediate(inst);
      // imm is negative (e.g., -16, -32)
      total_prologue_bytes += (-imm);
    }
    // Check for add x29, sp, #imm (frame pointer setup)
    else if (is_add_x29_sp_imm(inst)) {
      found_frame_setup = true;
    }
    // Check for sub sp, sp, #imm (LLVM spill space allocation)
    // IMPORTANT: We MUST count this! LLVM generates this after setting FP
    // to allocate space for spilled registers. This IS part of the prologue.
    else if (is_sub_sp_imm(inst)) {
      int imm = extract_sub_sp_immediate(inst);
      // imm is positive (e.g., 64 for sub sp, sp, #0x40)
      total_prologue_bytes += imm;  // 先累加！
      // After FP is set up and spill space is allocated, prologue is done
      if (found_frame_setup) {
        break;  // 累加完再 break
      }
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

// Extract the immediate value from sub sp, sp, #imm instruction
int YuhuPrologueAnalyzer::extract_sub_sp_immediate(uint32_t inst) {
  // imm12 is bits [21:10], scaled by shift amount
  int imm12 = (inst >> 10) & 0xFFF;

  // Shift is bits [23:22]
  int shift_val = (inst >> 22) & 0x3;

  // Apply scaling based on shift
  switch (shift_val) {
    case 0:  // LSL #0
      return imm12;
    case 1:  // LSL #12
      return imm12 << 12;
    default:  // Invalid for SUB instruction
      return imm12;
  }
}

// Analyze AArch64 prologue to find where x28 is saved relative to x29
int YuhuPrologueAnalyzer::find_x28_offset_from_x29(address code_start) {
  if (code_start == NULL) {
    return -1;
  }

  unsigned char* pc = (unsigned char*)code_start;
  int sp_offset_from_x29 = 0;  // Track the relationship between sp and x29
  int x28_saved_sp_offset = -1;  // Track where x28 is saved on stack
  bool found_yuhu_frame_alloc = false;  // Track if we've seen Yuhu's frame allocation

  // Scan the first 10 instructions (prologue is typically 6-10 instructions)
  for (int i = 0; i < 10; i++) {
    uint32_t inst = *(uint32_t*)pc;

    // Check for add x29, sp, #imm (this establishes x29 = sp + imm)
    if (is_add_x29_sp_imm(inst)) {
      sp_offset_from_x29 = extract_add_immediate(inst);
      // Continue scanning to find x28 if not found yet
      if (x28_saved_sp_offset != -1) {
        break;  // Found both x28 and x29, stop searching
      }
    }
    // Check for stp instructions that might save x28 (NEW PROLOGUE: regular offset mode)
    else if (is_stp_x28_regular(inst)) {
      int imm = extract_stp_immediate(inst);
      // For regular: [sp, #imm], the store happens at (current_sp + imm)
      x28_saved_sp_offset = imm;
      // Continue scanning to find x29 if not found yet
      if (sp_offset_from_x29 != 0) {
        break;  // Found both x28 and x29, stop searching
      }
    }
    // Check for pre-indexed stp (OLD PROLOGUE: stp x29, x30, [sp, #-16]!)
    else if (is_stp_x28_pre_index(inst)) {
      int imm = extract_stp_immediate(inst);
      // For pre-indexed: [sp, #imm]!, the store happens at (sp + imm) before sp is updated
      x28_saved_sp_offset = imm;
      // Continue scanning to find x29 if not found yet
      if (sp_offset_from_x29 != 0) {
        break;  // Found both x28 and x29, stop searching
      }
    }
    else if (is_stp_x28_post_index(inst)) {
      // For post-indexed: [sp], #imm, the store happens at current sp, then sp is updated
      x28_saved_sp_offset = 0;
      // Continue scanning to find x29 if not found yet
      if (sp_offset_from_x29 != 0) {
        break;  // Found both x28 and x29, stop searching
      }
    }
    // Mark when we see the Yuhu frame allocation (sub sp, sp, #large_imm)
    // But continue scanning because we might have found x28 already
    else if (is_sub_sp_imm(inst)) {
      found_yuhu_frame_alloc = true;
      // Don't break yet - we might have already found x28 and x29
      if (x28_saved_sp_offset != -1 && sp_offset_from_x29 != 0) {
        break;  // Found everything, stop searching
      }
      // If we haven't found x28 or x29 yet, keep scanning for a few more instructions
      // because they might come after the sub (in the new prologue order)
    }

    pc += 4;  // AArch64 instructions are 4 bytes

    // Safety: if we've gone too far and haven't found what we need, stop
    if (found_yuhu_frame_alloc && i > 10) {
      break;
    }
  }

  if (x28_saved_sp_offset == -1) {
    return -1;  // x28 not found in prologue
  }

  // Calculate x28's offset from x29
  //
  // NEW PROLOGUE (with callee-saved registers):
  //   sub    sp, sp, #0x70           -> sp = sp_original - 112
  //   stp    x28, x27, [sp, #0x10]   -> x28 at [sp + 16] = sp_original - 112 + 16 = sp_original - 96
  //   add    x29, sp, #0x60          -> x29 = sp + 96 = (sp_original - 112) + 96 = sp_original - 16
  //
  // So:
  //   x28 is at: sp_original - 96
  //   x29 is at: sp_original - 16
  //   x28_offset_from_x29 = (sp_original - 96) - (sp_original - 16) = -80
  //
  // Formula: x28_offset_from_x29 = x28_saved_sp_offset - sp_offset_from_x29
  //   where x28_saved_sp_offset is the offset from current SP (16)
  //   and sp_offset_from_x29 is the offset added to SP to get x29 (96)
  //
  // OLD PROLOGUE (pre-indexed stp):
  //   stp    x29, x30, [sp, #-16]!   -> x29 at [sp_original - 16], sp becomes sp_original - 16
  //   add    x29, sp, #0x0            -> x29 = sp = sp_original - 16
  //   (x28 is not saved in old prologue, it's reserved throughout)
  //
  // The formula works for both cases:
  //   NEW: 16 - 96 = -80  ✓
  //   OLD: -16 - 0 = -16 ✓
  int x28_offset_from_x29 = x28_saved_sp_offset - sp_offset_from_x29;

  return x28_offset_from_x29;
}

// Extract the immediate value from "add x29, sp, #imm" instruction in prologue
// This is needed to restore SP before returning from the function.
// Returns the immediate value (positive number), or 0 if not found.
//
// NOTE: In the new LLVM prologue (with alloca), the structure is:
//   1. sub sp, sp, #0x70         (allocate all stack space at once)
//   2. stp x28, x27, [sp, #0x10] (save callee-saved regs)
//   3. stp x26, x25, [sp, #0x20]
//   4. ...
//   5. add x29, sp, #0x60        (set up frame pointer) <-- we need this!
//
// So "add x29, sp, #imm" comes AFTER the initial "sub sp, sp, #large_imm",
// which means we should NOT break early when we see sub sp, sp, #imm.
int YuhuPrologueAnalyzer::extract_add_x29_sp_imm(address code_start) {
  if (code_start == NULL) {
    return 0;
  }

  unsigned char* pc = (unsigned char*)code_start;

  // Scan the first 10 instructions looking for "add x29, sp, #imm"
  for (int i = 0; i < 10; i++) {
    uint32_t inst = *(uint32_t*)pc;

    if (is_add_x29_sp_imm(inst)) {
      int imm = extract_add_immediate(inst);
      return imm;  // Found it!
    }

    pc += 4;  // AArch64 instructions are 4 bytes
  }

  return 0;  // Not found, return 0 as fallback
}

// AArch64 stp (store pair) encoding for 64-bit registers:
// 31 30 29 28 27 26 25 24 23 22 21 20 19 18 17 16 15 14 13 12 11 10 09 08 07 06 05 04 03 02 01 00
// |opc| 1  0  1  0  0  1| V| 1  1|  imm7       |  Rt2     |  Rn      |  Rt      |
// For 64-bit GP regs: opc = 10, V = 0
// For pre-indexed: bit 24 = 1, bit 23 = 1 (writeback mode)
// For post-indexed: bit 24 = 0, bit 23 = 1 (writeback mode)
// For regular: bit 24 = 0, bit 23 = 0 (no writeback)

bool YuhuPrologueAnalyzer::is_stp_x28_pre_index(uint32_t inst) {
  // Check if this is stp instruction with pre-indexed writeback
  // bits [31:22] = 1010010011 (stp pre-indexed)
  uint32_t stp_pre_idx_mask = 0xFFC00000;  // Mask bits [31:22]
  uint32_t stp_pre_idx_bits = 0xA9800000;  // stp pre-indexed pattern
  
  if ((inst & stp_pre_idx_mask) != stp_pre_idx_bits) {
    return false;
  }
  
  // Check if Rt or Rt2 is x28 (register 28)
  int rt = extract_stp_rt(inst);
  int rt2 = extract_stp_rt2(inst);
  
  return (rt == 28 || rt2 == 28);
}

bool YuhuPrologueAnalyzer::is_stp_x28_post_index(uint32_t inst) {
  // Check if this is stp instruction with post-indexed writeback
  // bits [31:22] = 1010010001 (stp post-indexed)
  uint32_t stp_post_idx_mask = 0xFFC00000;  // Mask bits [31:22]
  uint32_t stp_post_idx_bits = 0xA8C00000;  // stp post-indexed pattern
  
  if ((inst & stp_post_idx_mask) != stp_post_idx_bits) {
    return false;
  }
  
  // Check if Rt or Rt2 is x28 (Register 28)
  int rt = extract_stp_rt(inst);
  int rt2 = extract_stp_rt2(inst);
  
  return (rt == 28 || rt2 == 28);
}

bool YuhuPrologueAnalyzer::is_stp_x28_regular(uint32_t inst) {
  // Check if this is stp instruction without writeback
  // bits [31:22] = 1010010000 (stp regular)
  uint32_t stp_reg_mask = 0xFFC00000;  // Mask bits [31:22]
  uint32_t stp_reg_bits = 0xA9000000;  // stp regular pattern
  
  if ((inst & stp_reg_mask) != stp_reg_bits) {
    return false;
  }
  
  // Check if Rt or Rt2 is x28 (Register 28)
  int rt = extract_stp_rt(inst);
  int rt2 = extract_stp_rt2(inst);
  
  return (rt == 28 || rt2 == 28);
}

int YuhuPrologueAnalyzer::extract_stp_rt(uint32_t inst) {
  // Rt is bits [4:0]
  return inst & 0x1F;
}

int YuhuPrologueAnalyzer::extract_stp_rt2(uint32_t inst) {
  // Rt2 is bits [14:10]
  return (inst >> 10) & 0x1F;
}

// Check if instruction is add x29, sp, #imm
// Encoding: add x29, sp, #imm
// 31 30 29 28 27 26 25 24 23 22 21 20 ... 05 04 03 02 01 00
// |1  0  0  1  0  0  0  1  0  1|shift|      imm12        | Rn   | Rd   |
// Rn = 31 (sp), Rd = 29 (x29)
bool YuhuPrologueAnalyzer::is_add_x29_sp_imm(uint32_t inst) {
  // Check opcode: bits [31:22] = 1001000101 (add immediate)
  uint32_t mask = 0xFFC00000;
  uint32_t expected_op = 0x91000000;
  if ((inst & mask) != expected_op) {
    return false;
  }
  
  // Check Rn = 31 (sp) and Rd = 29 (x29)
  uint32_t rn = (inst >> 5) & 0x1F;  // bits [9:5]
  uint32_t rd = inst & 0x1F;         // bits [4:0]
  
  return (rn == 31 && rd == 29);
}

int YuhuPrologueAnalyzer::extract_add_immediate(uint32_t inst) {
  // imm12 is bits [21:10], scaled by shift amount
  int imm12 = (inst >> 10) & 0xFFF;
  
  // Shift is bits [23:22]
  int shift_val = (inst >> 22) & 0x3;
  
  // Apply scaling based on shift
  switch (shift_val) {
    case 0:  // LSL #0
      return imm12;
    case 1:  // LSL #12
      return imm12 << 12;
    default:  // Invalid for ADD instruction
      return imm12;
  }
}

/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_VM_YUHU_YUHUVIRTUALADDRESSPATCHER_HPP
#define SHARE_VM_YUHU_YUHUVIRTUALADDRESSPATCHER_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"
#include "yuhu/yuhu_globals.hpp"

// AArch64 instruction encodings
// movz (move with zero): 0xD2800000 | (imm16 << 5) | rd | (shift << 21)
//   shift: 0=lsl #0, 1=lsl #16, 2=lsl #32, 3=lsl #48
// movk (move and keep):  0xF2800000 | (imm16 << 5) | rd | (shift << 21)
// these mask and pattern are not that exact, still needs to check shift,
// so put them in specific file instead of yuhu_globals.hpp
static const uint32_t MOVZ_MASK = 0xFF800000;
static const uint32_t MOVZ_PATTERN_64 = 0xD2800000;
static const uint32_t MOVZ_PATTERN_32 = 0x52800000;

static const uint32_t MOVK_MASK = 0xFF800000;
static const uint32_t MOVK_PATTERN_64 = 0xF2800000;
static const uint32_t MOVK_PATTERN_32 = 0x72800000;

// MOV (immediate) is encoded as ORR (immediate)
static const uint32_t MOV_IMM_MASK = 0xFF800000;
static const uint32_t MOV_IMM_PATTERN_32 = 0x2A000000;
static const uint32_t MOV_IMM_PATTERN_64 = 0xAA000000;

enum class CallTargetType : uint8_t {
    none = 0,
    safepoint_poll = 1,
    vm = 2,
    java = 3,
    deopt = 4,
    unwind = 5,
    leaf = 6
};

// Information about matched placeholders for a single statepoint
class VirtualAddressMatch : public ResourceObj {
public:
  uint64_t virtual_offset;              // The shared virtual offset (e.g., 0x1000)
  
  uint64_t last_java_pc_va;             // Last Java PC placeholder (e.g., 0xDEAD1000)
  uint64_t last_java_pc_placeholder_offset;  // Offset of movz instruction for last_Java_pc
  
  uint64_t call_target_va;              // Call target placeholder (e.g., 0xBEEF1000)
  uint64_t call_target_placeholder_offset;   // Offset of movz instruction for call target
  uint64_t call_target_blr_offset; // Offset of blr instruction

  CallTargetType call_target_type;
};

// Scanner for finding dual virtual address placeholders in machine code
class YuhuVirtualAddressScanner : public AllStatic {
 public:
    // Scan forwards from statepoint call to find both placeholders
    // Returns true if both placeholders are found and share the same virtual_offset
    static bool scan_forwards_for_call_targets(
            const uint8_t* code_buffer,
            uint64_t statepoint_call_offset,
            uint64_t max_scan_distance,
            VirtualAddressMatch& out_match
    );
  
  // Patch call target movz/movk instructions with a new 64-bit value
  // Handles 3-instruction pattern: movz (lsl #48) + movk (lsl #16) + movk (no shift)
  static void patch_call_target_instructions(
    uint8_t* code_buffer,
    uint64_t movz_offset,
    uint64_t new_value
  );
  
  // Validate that a virtual address has the correct magic number
  static bool is_last_java_pc_placeholder(uint64_t va) {
    return (va & 0xFFFF0000) == LAST_JAVA_PC_MAGIC;
  }
  
  // Extract virtual_offset from a virtual address
  static uint64_t extract_virtual_offset_from_virtual_last_java_pc(uint64_t va) {
    return va & 0x0000FFFF;
  }

  static uint64_t extract_virtual_offset_from_virtual_call_target(uint64_t va) {
    return (va & 0x0000FFFF0000) >> 16;
  }

  static bool is_placeholder_pc_pattern(uint32_t* instr) {
      if (instr == NULL) {
          return false;
      }

      uint32_t inst = instr[0];

      if (is_mov_32_or_movz_32(inst)) {
          uint32_t low16 = (inst >> 5) & 0xFFFF;
          if (low16 == 0xBEEF) {
              return false;
          }
          // Check next instruction for movk
          uint32_t next_inst = instr[1];
          if ((next_inst & MOVK_MASK) == MOVK_PATTERN_32) {
              uint32_t next_shift = (next_inst >> 21) & 0x3;
              if (next_shift == 1) {
                  uint32_t mid16_31 = (next_inst >> 5) & 0xFFFF;
                  // Check virtual address for last java pc
                  if (mid16_31 == 0xDEAD) {
                      return true;
                  }
              }
          }
      }
      return false;
  }

  static bool is_placeholder_call_target_pattern(uint32_t* instr) {
      if (instr == NULL) {
          return false;
      }

      uint32_t inst = instr[0];
      if (is_mov_64_or_movz_64(inst)) {
          uint32_t low16 = (inst >> 5) & 0xFFFF;

          // Check next instruction for movk
          uint32_t next_inst = instr[1];
          if (low16 == 0xBEEF && (next_inst & MOVK_MASK) == MOVK_PATTERN_64) {
              uint32_t next_shift = (next_inst >> 21) & 0x3;
              if (next_shift == 1) {
                  uint32_t mid16_31 = (next_inst >> 5) & 0xFFFF;

                  uint32_t next_next_inst = instr[2];
                  // Check another movk instruction
                  if ((next_next_inst & MOVK_MASK) == MOVK_PATTERN_64) {
                      uint32_t next_next_shift = (next_next_inst >> 21) & 0x3;
                      if (next_next_shift == 2) {
                          uint32_t mid32_47 = (next_next_inst >> 5) & 0xFFFF;
                          if (mid32_47 == mid16_31) {
                              return true;
                          }
                      }
                  }
              }
          }
      }
      return false;
  }

  /**
   * extract target address from b instruction
   *
   * @param pc
   * @param inst
   * @return
   */
    static uint64_t decode_b_target(uint64_t pc, uint32_t inst) {
        // Extract 26-bit signed offset
        int32_t offset = inst & 0x03FFFFFF;  // bits [25:0]

        // Sign-extend from 26 bits to 32 bits
        if (offset & 0x02000000) {            // Check bit 25 (sign bit)
            offset |= 0xFC000000;               // Sign extend
        }

        // Target = PC + (offset * 4)
        uint64_t target = pc + ((int64_t)offset * 4);

        return target;
    };

    static void scan_from_b_target(uint32_t* instr, uint32_t inst, const uint8_t* code_buffer, VirtualAddressMatch* out_match, bool* found_blr) {
        uint64_t target_address = decode_b_target((uint64_t) instr, inst);

        // Calculate offset within CodeData
        uint64_t target_offset = target_address - (uint64_t)code_buffer;

        // scan another 25 instructions to find blr instruction
        for (uint64_t b_offset = 0; b_offset + 4 <= 100; b_offset += 4) {
            uint32_t* b_instr = (uint32_t*)(code_buffer + target_offset + b_offset);
            uint32_t b_inst = b_instr[0];
            if (is_blr_pattern(b_instr)) {
                // Always use first blr instruction as blr offset
                out_match->call_target_blr_offset = target_offset + b_offset;
                *found_blr = true;
                break;
            } else if ((b_inst & B_MASK) == B_PATTERN) {
                scan_from_b_target(b_instr, b_inst, code_buffer, out_match, found_blr);
                break;
            }
        }
    };

    /**
     * extract page offset from adrp instruction
     *
     * @param instr
     * @return
     */
    static int64_t extract_page_offset(uint32_t* instr) {
        // Extract the 21-bit immediate and shift left by 12
        int64_t imm = ((instr[0] >> 29) & 0x3) |           // immlo (2 bits)
                      (((instr[0] >> 5) & 0x7FFFF) << 2); // immhi (19 bits)

        // Sign-extend from 21 bits to 64 bits
        if (imm & (1ULL << 20)) {
            imm |= ~((1ULL << 21) - 1);  // Sign extend
        }

        // Multiply by page size (4096)
        return imm << 12;
    };

    /**
     * check if it is adrp instruction sequences.
     *
     * @param instr
     * @return
     */
    static bool is_adrp_pattern(uint32_t* instr) {
        // Check instruction encoding (little-endian):
        // adrp   x8, 3
        // ldr    x8, [x8, #0x718]
        if ((instr[0] & 0x9F000000) == 0x90000000 &&
            (instr[1] & 0xFFC00000) == 0xF9400000) {
            return true;
        }
        return false;
    };

    static bool is_blr_pattern(uint32_t* instr) {
        uint32_t inst = instr[0];
        return (inst & BLR_MASK) == BLR_PATTERN;
    }

    // oop_relocation related functions
    // Helper function to check if instruction sequence matches marker pattern
    static bool is_oop_marker_pattern(uint32_t* instr) {
        // Check instruction encoding (little-endian):
        // [0] mov w19, #0xCAFE       → 0x52995FD3
        // [1] movk w19, #0xBABE, lsl #16 → 0x72B757D3
        // [2] mov w20, #imm16        → 0x528xxxxxB4 (bits 5-20 contain imm16)
        // [3] nop                    → 0xD503201F
        // [4] nop                    → 0xD503201F

        if (instr[0] == 0x52995FD3 &&  // mov w19, #0xCAFE
            instr[1] == 0x72B757D3 &&  // movk w19, #0xBABE, lsl #16
            (instr[3] & 0xFFFFFFF0) == 0xD5032010 &&  // nop (allow low 4 bits variation)
            (instr[4] & 0xFFFFFFF0) == 0xD5032010) {  // nop
            return true;
        }
        return false;
    };

    // metadata_relocation marker pattern
    // Same shape as oop marker but with low immediate 0xDEAD (vs 0xCAFE).
    //   [0] mov  w19, #0xDEAD            → 0x529BD5B3
    //   [1] movk w19, #0xBABE, lsl #16   → 0x72B757D3
    //   [2] mov  w20, #metadata_id       → 0x528xxxxxB4 (bits 5-20 = metadata_id)
    //   [3] nop, [4] nop
    // Distinguished from last_Java_pc marker (which has 0xDEAD in the HIGH 16 bits).
    static bool is_metadata_marker_pattern(uint32_t* instr) {
        if (instr[0] == 0x529BD5B3 &&  // mov w19, #0xDEAD
            instr[1] == 0x72B757D3 &&  // movk w19, #0xBABE, lsl #16
            (instr[3] & 0xFFFFFFF0) == 0xD5032010 &&  // nop
            (instr[4] & 0xFFFFFFF0) == 0xD5032010) {  // nop
            return true;
        }
        return false;
    };

    static bool is_call_site_with_call_target_marker_pattern(uint32_t* instr) {
        return is_placeholder_pc_pattern(instr) && (instr[3] & 0xFFFFFFF0) == 0xD5032010 && (instr[4] & 0xFFFFFFF0) == 0xD5032010;
    }

    static bool is_call_site_without_call_target_marker_pattern(uint32_t* instr) {
        return is_placeholder_pc_pattern(instr) && (instr[2] & 0xFFFFFFF0) == 0xD5032010 && (instr[3] & 0xFFFFFFF0) == 0xD5032010;
    }

    // Helper function to check if 3 instructions form a mov/movk sequence
    static bool is_mov_movk_sequence(uint32_t* instr) {
        // Check for mov/movk sequence (C1 compatible format):
        // [0] mov xN, #imm16         → 0xD28xxxxx (bit 31-23 = 0b110100101)
        // [1] movk xN, #imm16, lsl #16 → 0xF2Axxxxx (bit 31-23 = 0b1111001010)
        // [2] movk xN, #imm16, lsl #32 → 0xF2Cxxxxx (bit 31-23 = 0b1111001011)

        // All must be AArch64 mov/movk immediate instructions
        if ((instr[0] & 0xFF800000) != 0xD2800000 ||  // mov xN, #imm16
            (instr[1] & 0xFFE00000) != 0xF2A00000 ||  // movk xN, #imm16, lsl #16
            (instr[2] & 0xFFE00000) != 0xF2C00000) {  // movk xN, #imm16, lsl #32
            return false;
        }

        // Verify all 3 instructions use the same destination register
        int rd0 = instr[0] & 0x1F;  // bits 4-0
        int rd1 = instr[1] & 0x1F;
        int rd2 = instr[2] & 0x1F;

        return (rd0 == rd1 && rd1 == rd2);
    };

    // Helper function to extract oop_id from marker
    static int extract_mov_imm16(uint32_t* instr) {
        // Extract imm16 from: mov w19, #imm16
        // Encoding: 0x528xxxxxB4, where bits 5-20 contain imm16
        uint32_t mov_instr = instr[2];
        uint16_t imm16 = (mov_instr >> 5) & 0xFFFF;
        return (int)imm16;
    };

    // Helper function to extract 64-bit value from mov/movk sequence
    static uint64_t extract_from_movk_sequence(uint32_t* instr) {
        uint16_t imm0 = (instr[0] >> 5) & 0xFFFF;   // bits [15:0]
        uint16_t imm1 = (instr[1] >> 5) & 0xFFFF;   // bits [31:16]
        uint16_t imm2 = (instr[2] >> 5) & 0xFFFF;   // bits [47:32]

        return ((uint64_t)imm2 << 32) | ((uint64_t)imm1 << 16) | (uint64_t)imm0;
    };

    static bool is_mov_32_or_movz_32(uint32_t inst) {
        // expected instruction sequences for last java pc
        // mov    w19, #0x20
        // movk   w19, #0xdead, lsl #16
        bool is_mov_32 = ((inst & MOV_IMM_MASK) == MOV_IMM_PATTERN_32);
        bool is_movz_32 = ((inst & MOVZ_MASK) == MOVZ_PATTERN_32) && ((inst >> 21) & 0x3) == 0;
        return is_mov_32 || is_movz_32;
    };

    static bool is_mov_64_or_movz_64(uint32_t inst) {
        // expected instruction sequences for call target, usually llvm uses movz, just handle mov for safe case
        // movz   x8, #0xbeef, lsl #0
        // movk   x8, #0x20, lsl #16
        // movk   x8, #0x20, lsl #32
        // or
        // mov    x8, #0xbeef
        // movk   x8, #0x20, lsl #16
        // movk   x8, #0x20, lsl #32
        bool is_mov_64 = ((inst & MOV_IMM_MASK) == MOV_IMM_PATTERN_64);
        bool is_movz_64 = ((inst & MOVZ_MASK) == MOVZ_PATTERN_64) && ((inst >> 21) & 0x3) == 0;
        return is_mov_64 || is_movz_64;
    };
};

#endif // SHARE_VM_YUHU_YUHUVIRTUALADDRESSPATCHER_HPP

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

// Information about matched placeholders for a single statepoint
struct VirtualAddressMatch {
  uint64_t virtual_offset;              // The shared virtual offset (e.g., 0x1000)
  
  uint64_t last_java_pc_va;             // Last Java PC placeholder (e.g., 0xDEAD1000)
  uint64_t last_java_pc_placeholder_offset;  // Offset of movz instruction for last_Java_pc
  
  uint64_t call_target_va;              // Call target placeholder (e.g., 0xBEEF1000)
  uint64_t call_target_placeholder_offset;   // Offset of movz instruction for call target
  uint64_t call_target_blr_offset; // Offset of blr instruction
  bool safepoint_poll_call; // true means it is safepoint poll call, otherwise, normal java calls, VM calls, and helper calls
};

// Scanner for finding dual virtual address placeholders in machine code
class YuhuVirtualAddressScanner : public AllStatic {
 public:
  // Scan backwards from statepoint call to find both placeholders
  // Returns true if both placeholders are found and share the same virtual_offset
  static bool scan_backwards_for_placeholders(
    const uint8_t* code_buffer,
    uint64_t statepoint_call_offset,
    uint64_t max_scan_distance,
    VirtualAddressMatch& out_match
  );

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
  
  // Patch adr instruction with new PC-relative offset
  // adr_address: absolute address of the adr instruction
  // target_address: absolute address that adr should point to (return address after bl)
  static void patch_adr_instruction(
    uint8_t* code_buffer,
    uint64_t adr_offset,
    uint64_t adr_address,  // Absolute address of adr instruction
    uint64_t target_address  // Absolute address of target (return address after bl)
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
      auto is_mov_32_or_movz_32 = [](uint32_t inst) -> bool {
          // expected instruction sequences for last java pc
          // mov    w19, #0x20
          // movk   w19, #0xdead, lsl #16
          bool is_mov_32 = ((inst & MOV_IMM_MASK) == MOV_IMM_PATTERN_32);
          bool is_movz_32 = ((inst & MOVZ_MASK) == MOVZ_PATTERN_32) && ((inst >> 21) & 0x3) == 0;
          return is_mov_32 || is_movz_32;
      };

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
            if ((b_inst & BLR_MASK) == BLR_PATTERN) {
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
};

#endif // SHARE_VM_YUHU_YUHUVIRTUALADDRESSPATCHER_HPP

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

#include "precompiled.hpp"
#include "yuhu/yuhuVirtualAddressPatcher.hpp"
#include "utilities/debug.hpp"
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

extern "C" void gc_safepoint_poll();

bool YuhuVirtualAddressScanner::scan_backwards_for_placeholders(
  const uint8_t* code_buffer,
  uint64_t statepoint_call_offset,
  uint64_t max_scan_distance,
  VirtualAddressMatch& out_match
) {
  uint64_t scan_start = statepoint_call_offset;
  uint64_t scan_end = (statepoint_call_offset > max_scan_distance) 
                      ? statepoint_call_offset - max_scan_distance : 0;
  
  // Initialize match structure
  out_match.last_java_pc_va = 0;
  out_match.last_java_pc_placeholder_offset = 0;
  out_match.last_java_pc_adr_offset = 0;
  out_match.call_target_va = 0;
  out_match.call_target_placeholder_offset = 0;
  out_match.virtual_offset = 0;
  out_match.safepoint_poll_call = false;
  
  bool found_ljpc = false;
  bool found_call_target = false;
  bool found_safepoint_poll_call = false;

  auto is_mov_32_or_movz_32 = [](uint32_t inst) -> bool {
      // expected instruction sequences for last java pc
      // mov    w19, #0x20
      // movk   w19, #0xdead, lsl #16
      bool is_mov_32 = ((inst & MOV_IMM_MASK) == MOV_IMM_PATTERN_32);
      bool is_movz_32 = ((inst & MOVZ_MASK) == MOVZ_PATTERN_32) && ((inst >> 21) & 0x3) == 0;
      return is_mov_32 || is_movz_32;
  };

  auto is_mov_64_or_movz_64 = [](uint32_t inst) -> bool {
      // expected instruction sequences for call target, usually llvm uses movz, just handle mov for safe case
      // movz   x8, #0x20, lsl #0
      // movk   x8, #0xbeef, lsl #16
      // movk   x8, #0xbeef, lsl #32
      // or
      // mov    x8, #0x20
      // movk   x8, #0xbeef, lsl #16
      // movk   x8, #0xbeef, lsl #32
      bool is_mov_64 = ((inst & MOV_IMM_MASK) == MOV_IMM_PATTERN_64);
      bool is_movz_64 = ((inst & MOVZ_MASK) == MOVZ_PATTERN_64) && ((inst >> 21) & 0x3) == 0;
      return is_mov_64 || is_movz_64;
  };

    auto is_adrp_pattern = [](uint32_t* instr) -> bool {
        // Check instruction encoding (little-endian):
        // adrp   x8, 3
        // ldr    x8, [x8, #0x718]
        // blr    x8
        if ((instr[0] & 0x9F000000) == 0x90000000 &&
            (instr[1] & 0xFFC00000) == 0xF9400000 &&
            (instr[2] & 0xFFFFFC1F) == 0xD63F0000) {
            return true;
        }
        return false;
    };

    auto extract_page_offset = [](uint32_t* instr) -> int64_t {
        // Extract the 21-bit immediate and shift left by 12
        int64_t imm = ((instr[0] >> 29) & 0x3) |           // immlo (2 bits)
                      (((int64_t)instr[0] >> 5) & 0x7FFFF); // immhi (19 bits)

        // Sign-extend from 21 bits to 64 bits
        if (imm & (1ULL << 20)) {
            imm |= ~((1ULL << 21) - 1);  // Sign extend
        }

        // Multiply by page size (4096)
        return imm << 12;
    };
  
  // Scan backwards from statepoint call
  for (uint64_t offset = scan_start; offset + 12 >= scan_end && offset >= 12; offset -= 4) {
    // Read instruction at current offset
    uint32_t* instr = (uint32_t*)(code_buffer + offset);
    uint32_t inst = instr[0];

    // Immediate stop at another blr instruction
    if (offset < scan_start && (inst & BLR_MASK) == BLR_PATTERN) {
        break;
    }

    // handle for last java pc
    if (is_mov_32_or_movz_32(inst)) {
        uint32_t low16 = (inst >> 5) & 0xFFFF;

        // Check next instruction for movk
        uint32_t next_inst = instr[1];
        if ((next_inst & MOVK_MASK) == MOVK_PATTERN_32) {
            uint32_t next_shift = (next_inst >> 21) & 0x3;
            if (next_shift == 1) {
                uint32_t mid16_31 = (next_inst >> 5) & 0xFFFF;
                // Check virtual address for last java pc
                if (mid16_31 == 0xDEAD) {
                    // Found last_Java_pc placeholder
                    out_match.last_java_pc_va = ((uint64_t)mid16_31 << 16) | low16;
                    out_match.last_java_pc_placeholder_offset = offset;
                    out_match.last_java_pc_adr_offset = offset + 8; // if it is safepoint poll call, there is no adr instruction, so last_java_pc_adr_offset is not used

                    // Verify same virtual_offset
                    if (out_match.virtual_offset == 0) {
                        out_match.virtual_offset = low16;
                    } else if (out_match.virtual_offset != low16) {
                        // Mismatched virtual_offsets - this is a critical error
                        assert(out_match.virtual_offset == low16, "Mismatched virtual_offsets - placeholders don't belong to same call site");
                        return false;  // Early exit in all builds
                    }
                    found_ljpc = true;
                }
            }
        }
    } else if (is_mov_64_or_movz_64(inst)) {
        uint32_t low16 = (inst >> 5) & 0xFFFF;

        // Check next instruction for movk
        uint32_t next_inst = instr[1];
        if ((next_inst & MOVK_MASK) == MOVK_PATTERN_64) {
            uint32_t next_shift = (next_inst >> 21) & 0x3;
            if (next_shift == 1) {
                uint32_t mid16_31 = (next_inst >> 5) & 0xFFFF;
                // Check virtual address for call target
                if (mid16_31 == 0xBEEF) {
                    uint32_t next_next_inst = instr[2];
                    // Check another movk instruction
                    if ((next_next_inst & MOVK_MASK) == MOVK_PATTERN_64) {
                        uint32_t next_next_shift = (next_next_inst >> 21) & 0x3;
                        if (next_next_shift == 2) {
                            uint32_t mid32_47 = (next_next_inst >> 5) & 0xFFFF;
                            if (mid32_47 == 0xBEEF) {
                                // Found call target placeholder
                                out_match.call_target_va = ((uint64_t)mid32_47 << 32) | ((uint64_t)mid16_31 << 16) | low16;
                                out_match.call_target_placeholder_offset = offset;

                                // Verify same virtual_offset
                                if (out_match.virtual_offset == 0) {
                                    out_match.virtual_offset = low16;
                                } else if (out_match.virtual_offset != low16) {
                                    // Mismatched virtual_offsets - this is a critical error
                                    assert(out_match.virtual_offset == low16, "Mismatched virtual_offsets - placeholders don't belong to same call site");
                                    return false;  // Early exit in all builds
                                }
                                found_call_target = true;
                            }
                        }
                    }
                }
            }
        }
    } else if (is_adrp_pattern(instr)) {
        // Locate target page
        int64_t page_offset = extract_page_offset(instr);
        uint64_t pc_page = ((uint64_t)instr) & ~0xFFFULL;
        uint64_t target_page = pc_page + page_offset;

        uint32_t imm12 = (instr[1] >> 10) & 0xFFF;
        uint64_t offset = imm12 << 3;

        uint64_t target_address = target_page + offset;

        uint64_t function_address = *(uint64_t*)target_address;
        if (function_address == (uint64_t)&gc_safepoint_poll) {
            out_match.safepoint_poll_call = true;
            found_safepoint_poll_call = true;
        }
    }
    
    // Stop if we found both placeholders
    if ((found_ljpc && found_call_target) || (found_ljpc && found_safepoint_poll_call)) {
      return true;
    }
  }
  
  // Did not find both placeholders
  return false;
}

void YuhuVirtualAddressScanner::patch_call_target_instructions(
  uint8_t* code_buffer,
  uint64_t movz_offset,
  uint64_t new_value
) {
  // Extract 16-bit chunks from new_value
  uint16_t imm0 = (new_value >> 0) & 0xFFFF;   // Bits 15-0
  uint16_t imm1 = (new_value >> 16) & 0xFFFF;  // Bits 31-16
  uint16_t imm2 = (new_value >> 32) & 0xFFFF;  // Bits 47-32

    // expected instruction sequences for call target, usually llvm uses movz, just handle mov for safe case
    // movz   x8, #0x20, lsl #0
    // movk   x8, #0xbeef, lsl #16
    // movk   x8, #0xbeef, lsl #32
    // or
    // mov    x8, #0x20
    // movk   x8, #0xbeef, lsl #16
    // movk   x8, #0xbeef, lsl #32
  
  uint32_t* instructions = (uint32_t*)(code_buffer + movz_offset);
  
  // Patch movz (bits 15-0, lsl #0)
  // MOVZ 64-bit: 0xD2800000 | (imm16 << 5) | (shift << 21) | rd
  // shift #0 = 0b00 = 0
  uint32_t movz_inst = instructions[0];
  // Verify this is a movz with lsl #0
  assert((movz_inst & MOVZ_MASK) == MOVZ_PATTERN_64 && ((movz_inst >> 21) & 0x3) == 0,
         "Expected movz instruction with lsl #0");
  movz_inst = (movz_inst & ~(0xFFFF << 5)) | (imm0 << 5);  // Replace imm16
  instructions[0] = movz_inst;
  
  // Patch first movk (bits 31-16, lsl #16)
  // MOVK 64-bit: 0xF2800000 | (imm16 << 5) | (shift << 21) | rd
  // shift #16 = 0b01 = 1
  uint32_t movk_inst1 = instructions[1];
  assert((movk_inst1 & MOVK_MASK) == MOVK_PATTERN_64 && ((movk_inst1 >> 21) & 0x3) == 1,
         "Expected movk instruction with lsl #16");
  movk_inst1 = (movk_inst1 & ~(0xFFFF << 5)) | (imm1 << 5);  // Replace imm16
  instructions[1] = movk_inst1;

  // Patch second movk (bits 47-32, lsl #32)
  // MOVK 64-bit: 0xF2800000 | (imm16 << 5) | (shift << 21) | rd
  // shift #32 = 0b10 = 2
  uint32_t movk_inst2 = instructions[2];
  assert((movk_inst2 & MOVK_MASK) == MOVK_PATTERN_64 && ((movk_inst2 >> 21) & 0x3) == 2,
         "Expected movk instruction with lsl #32");
  movk_inst2 = (movk_inst2 & ~(0xFFFF << 5)) | (imm2 << 5);  // Replace imm16
  instructions[2] = movk_inst2;
}

void YuhuVirtualAddressScanner::patch_adr_instruction(
  uint8_t* code_buffer,
  uint64_t adr_offset,
  uint64_t adr_address,  // Absolute address of adr instruction
  uint64_t target_address  // Absolute address of target (return address after bl)
) {
  // Calculate PC-relative offset
  // ADR calculates: target = current_PC + offset
  // current_PC is the address of the adr instruction itself
  int64_t pc_offset = (int64_t)(target_address - adr_address);
  
  // Verify offset fits in 21-bit signed range (-1MB to +1MB)
  assert(pc_offset >= -0x100000 && pc_offset < 0x100000, "ADR offset out of range");
  
  // AArch64 ADR instruction encoding:
  // adr xd, label
  // = 0001 0000 | immlo (2 bits) | 00000 | immhi (19 bits) | rd (5 bits)
  // 
  // Bits:
  // 30-29: immlo (bits 0-1 of offset)
  // 28-24: 00010 (opcode)
  // 23-5: immhi (bits 2-20 of offset)
  // 4-0: rd (destination register, x20 = 20)
  
  uint32_t* adr_instr = (uint32_t*)(code_buffer + adr_offset);
  
  // Build new ADR instruction
  uint32_t new_adr = 0x10000000;  // ADR opcode base (bits 28-24 = 00010)
  
  // Encode immlo (bits 0-1 of offset) into bits 29-30
  new_adr |= ((pc_offset & 0x3) << 29);
  
  // Encode immhi (bits 2-20 of offset) into bits 5-23
  new_adr |= (((pc_offset >> 2) & 0x7FFFF) << 5);
  
  // Set destination register to x20 (rd = 20)
  new_adr |= 20;
  
  // Patch the instruction
  *adr_instr = new_adr;
}

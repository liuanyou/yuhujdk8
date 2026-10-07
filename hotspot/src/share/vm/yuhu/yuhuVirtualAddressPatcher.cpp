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

#include "yuhu/yuhuVirtualAddressPatcher.hpp"
#include "utilities/debug.hpp"

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
    // movz   x8, #0xbeef, lsl #0
    // movk   x8, #0x20, lsl #16
    // movk   x8, #0x20, lsl #32
    // or
    // mov    x8, #0xbeef
    // movk   x8, #0x20, lsl #16
    // movk   x8, #0x20, lsl #32
  
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

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

// AArch64 instruction encodings
// movz (move with zero): 0xD2800000 | (imm16 << 5) | rd | (shift << 21)
//   shift: 0=lsl #0, 1=lsl #16, 2=lsl #32, 3=lsl #48
// movk (move and keep):  0xF2800000 | (imm16 << 5) | rd | (shift << 21)

static const uint32_t MOVZ_MASK = 0xFF800000;
static const uint32_t MOVZ_PATTERN = 0xD2800000;
static const uint32_t MOVK_MASK = 0xFF800000;
static const uint32_t MOVK_PATTERN = 0xF2800000;

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
  out_match.call_target_va = 0;
  out_match.call_target_placeholder_offset = 0;
  out_match.virtual_offset = 0;
  
  bool found_ljpc = false;
  bool found_call_target = false;
  
  // Scan backwards from statepoint call
  for (uint64_t offset = scan_start; offset + 8 >= scan_end && offset >= 8; offset -= 4) {
    // Read instruction at current offset
    uint32_t inst = *(uint32_t*)(code_buffer + offset);
    
    // Check for movz with lsl #32 (shift = 2)
    if ((inst & MOVZ_MASK) == MOVZ_PATTERN) {
      uint32_t imm16 = (inst >> 5) & 0xFFFF;
      uint32_t shift = (inst >> 21) & 0x3;
      
      if (shift == 2) {  // lsl #32
        // Check if there's a following movk instruction
        if (offset + 4 < scan_start + 100) {  // Sanity check
          uint32_t next_inst = *(uint32_t*)(code_buffer + offset + 4);
          
          if ((next_inst & MOVK_MASK) == MOVK_PATTERN) {
            uint32_t next_shift = (next_inst >> 21) & 0x3;
            
            // movk should have no shift (shift = 0) for low 16 bits
            if (next_shift == 0) {
              uint32_t low16 = (next_inst >> 5) & 0xFFFF;
              uint64_t virtual_address = ((uint64_t)imm16 << 32) | low16;
              
              // Check magic number
              if (imm16 == 0xDEAD) {
                // Found last_Java_pc placeholder
                out_match.last_java_pc_va = virtual_address;
                out_match.last_java_pc_placeholder_offset = offset;
                out_match.virtual_offset = low16;
                found_ljpc = true;
              } else if (imm16 == 0xBEEF) {
                // Found call target placeholder
                out_match.call_target_va = virtual_address;
                out_match.call_target_placeholder_offset = offset;
                
                // Verify same virtual_offset
                if (out_match.virtual_offset == 0) {
                  out_match.virtual_offset = low16;
                } else if (out_match.virtual_offset != low16) {
                  // Mismatched virtual_offsets - this is an error
                  return false;
                }
                found_call_target = true;
              }
            }
          }
        }
      }
    }
    
    // Stop if we found both placeholders
    if (found_ljpc && found_call_target) {
      return true;
    }
  }
  
  // Did not find both placeholders
  return false;
}

uint64_t YuhuVirtualAddressScanner::extract_virtual_address_from_movz_movk(
  const uint8_t* code_buffer,
  uint64_t movz_offset
) {
  uint32_t movz_inst = *(uint32_t*)(code_buffer + movz_offset);
  uint32_t movk_inst = *(uint32_t*)(code_buffer + movz_offset + 4);
  
  uint32_t high16 = (movz_inst >> 5) & 0xFFFF;
  uint32_t low16 = (movk_inst >> 5) & 0xFFFF;
  
  return ((uint64_t)high16 << 32) | low16;
}

void YuhuVirtualAddressScanner::patch_movz_movk_instructions(
  uint8_t* code_buffer,
  uint64_t movz_offset,
  uint64_t new_value
) {
  // Extract 16-bit chunks from new_value
  uint16_t imm0 = (new_value >> 0) & 0xFFFF;   // Bits 15-0
  uint16_t imm1 = (new_value >> 16) & 0xFFFF;  // Bits 31-16
  uint16_t imm2 = (new_value >> 32) & 0xFFFF;  // Bits 47-32
  uint16_t imm3 = (new_value >> 48) & 0xFFFF;  // Bits 63-48
  
  // For AArch64, we need to patch the existing movz/movk pair
  // Current pattern: movz (lsl #32) + movk (no shift)
  // This covers bits 47-32 and 15-0
  // We need to add movk for bits 31-16 and 63-48 if they're non-zero
  
  uint32_t* instructions = (uint32_t*)(code_buffer + movz_offset);
  
  // Patch movz (bits 47-32, lsl #32)
  uint32_t movz_inst = instructions[0];
  movz_inst = (movz_inst & ~(0xFFFF << 5)) | (imm2 << 5);  // Replace imm16
  instructions[0] = movz_inst;
  
  // Patch movk (bits 15-0, no shift)
  uint32_t movk_inst = instructions[1];
  movk_inst = (movk_inst & ~(0xFFFF << 5)) | (imm0 << 5);  // Replace imm16
  instructions[1] = movk_inst;
  
  // If bits 31-16 or 63-48 are non-zero, we need to insert additional movk instructions
  // This is more complex and would require code relocation
  // For now, we assume virtual addresses fit in 48 bits (imm1 and imm3 are zero)
  assert(imm1 == 0 && imm3 == 0, "Virtual addresses must fit in 48 bits for simple patching");
}

uint64_t YuhuVirtualAddressScanner::scan_for_marker(
  const uint8_t* code_buffer,
  uint64_t search_start_offset,
  uint64_t max_scan_distance
) {
  // Scan for any marker with 0xDEAD magic (regardless of virtual_offset)
  const uint32_t MOV_W19_IMM_MASK = 0xFFE0001F;
  const uint32_t MOV_W19_IMM_PATTERN = 0x52800013;  // movz w19, #imm16
  
  const uint32_t MOVK_W19_MASK = 0xFFE0001F;
  
  // Scan backwards from search_start_offset
  uint64_t scan_end = (search_start_offset > max_scan_distance) 
                      ? search_start_offset - max_scan_distance : 0;
  
  for (uint64_t offset = search_start_offset; offset + 8 >= scan_end && offset >= 8; offset -= 4) {
    // Read two consecutive instructions
    uint32_t inst1 = *(uint32_t*)(code_buffer + offset);
    uint32_t inst2 = *(uint32_t*)(code_buffer + offset + 4);
    
    // Check first instruction: movz w19, #0xDEAD
    if ((inst1 & MOV_W19_IMM_MASK) == MOV_W19_IMM_PATTERN) {
      uint32_t imm16 = (inst1 >> 5) & 0xFFFF;
      
      // Check if it's the marker magic (0xDEAD)
      if (imm16 == 0xDEAD) {
        // Check second instruction: movk w19, #virtual_offset, lsl #16
        if ((inst2 & MOVK_W19_MASK) == 0xF2A00013) {  // movk with lsl #16
          uint32_t shift = (inst2 >> 21) & 0x3;
          
          // Check if shift is lsl #16
          if (shift == 1) {
            // Found a marker!
            return offset;
          }
        }
      }
    }
  }
  
  // Marker not found
  return UINT64_MAX;
}

uint64_t YuhuVirtualAddressScanner::scan_for_marker_with_offset(
  const uint8_t* code_buffer,
  uint64_t search_start_offset,
  uint64_t max_scan_distance,
  uint64_t expected_virtual_offset
) {
  // Scan for marker with specific virtual_offset
  const uint32_t MOV_W19_IMM_MASK = 0xFFE0001F;
  const uint32_t MOV_W19_IMM_PATTERN = 0x52800013;  // movz w19, #imm16
  
  const uint32_t MOVK_W19_MASK = 0xFFE0001F;
  const uint32_t MOVK_W19_LSL16_PATTERN = 0xF2A00013;  // movk w19, #imm16, lsl #16
  
  // Scan backwards from search_start_offset
  uint64_t scan_end = (search_start_offset > max_scan_distance) 
                      ? search_start_offset - max_scan_distance : 0;
  
  for (uint64_t offset = search_start_offset; offset + 8 >= scan_end && offset >= 8; offset -= 4) {
    // Read two consecutive instructions
    uint32_t inst1 = *(uint32_t*)(code_buffer + offset);
    uint32_t inst2 = *(uint32_t*)(code_buffer + offset + 4);
    
    // Check first instruction: movz w19, #0xDEAD
    if ((inst1 & MOV_W19_IMM_MASK) == MOV_W19_IMM_PATTERN) {
      uint32_t imm16_high = (inst1 >> 5) & 0xFFFF;
      
      // Check if it's the marker magic (0xDEAD)
      if (imm16_high == 0xDEAD) {
        // Check second instruction: movk w19, #virtual_offset, lsl #16
        if ((inst2 & MOVK_W19_MASK) == MOVK_W19_LSL16_PATTERN) {
          uint32_t virtual_offset = (inst2 >> 5) & 0xFFFF;
          uint32_t shift = (inst2 >> 21) & 0x3;
          
          // Check if shift is lsl #16 and virtual_offset matches
          if (shift == 1 && virtual_offset == expected_virtual_offset) {
            // Found the marker with matching virtual_offset!
            return offset;
          }
        }
      }
    }
  }
  
  // Marker with expected virtual_offset not found
  return UINT64_MAX;
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

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
#include "yuhu/yuhu_globals.hpp"

extern "C" void gc_safepoint_poll(JavaThread* thread);
extern "C" void handle_deoptimization();

bool YuhuVirtualAddressScanner::scan_forwards_for_call_targets(
        const uint8_t* code_buffer,
        uint64_t statepoint_call_offset,
        uint64_t max_scan_distance,
        VirtualAddressMatch& out_match
) {
    uint64_t scan_start = statepoint_call_offset;
    uint64_t scan_end = statepoint_call_offset + max_scan_distance;

    // Initialize match structure
    out_match.last_java_pc_va = 0;
    out_match.last_java_pc_placeholder_offset = 0;
    out_match.call_target_va = 0;
    out_match.call_target_placeholder_offset = 0;
    out_match.call_target_blr_offset = 0;
    out_match.virtual_offset = 0;
    out_match.call_target_type = CallTargetType::none;

    bool found_ljpc = false;
    bool found_blr = false;

    // Scan forwards from statepoint call
    for (uint64_t offset = scan_start; offset + 4 <= scan_end; offset += 4) {
        // Read instruction at current offset
        uint32_t* instr = (uint32_t*)(code_buffer + offset);
        uint32_t inst = instr[0];

        if (found_blr && is_blr_pattern(instr)) {
            break;
        }

         if (is_call_site_with_call_target_marker_pattern(instr)) {
             // Found last_Java_pc placeholder
             {
                 uint32_t low16 = (inst >> 5) & 0xFFFF;
                 uint32_t next_inst = instr[1];
                 uint32_t mid16_31 = (next_inst >> 5) & 0xFFFF;
                 out_match.last_java_pc_va = ((uint64_t)mid16_31 << 16) | low16;
                 out_match.last_java_pc_placeholder_offset = offset;

                 // Verify same virtual_offset
                 if (out_match.virtual_offset == 0) {
                     out_match.virtual_offset = low16;
                 } else if (out_match.virtual_offset != low16) {
                     // Mismatched virtual_offsets - this is a critical error
                     assert(out_match.virtual_offset == low16, "Mismatched virtual_offsets - placeholders don't belong to same call site");
                     return false;  // Early exit in all builds
                 }
                 int call_site_type = extract_mov_imm16(instr);
                 out_match.call_target_type = static_cast<CallTargetType>(call_site_type);
                 found_ljpc = true;
             }
             // Found call target placeholder
             {
                 assert(is_placeholder_call_target_pattern(instr + 5), "should be call target pattern");
                 instr += 5; // skip 5 instructions
                 inst = instr[0];
                 uint32_t low16 = (inst >> 5) & 0xFFFF;
                 uint32_t next_inst = instr[1];
                 uint32_t mid16_31 = (next_inst >> 5) & 0xFFFF;
                 uint32_t next_next_inst = instr[2];
                 uint32_t mid32_47 = (next_next_inst >> 5) & 0xFFFF;

                 out_match.call_target_va = ((uint64_t)mid32_47 << 32) | ((uint64_t)mid16_31 << 16) | low16;
                 out_match.call_target_placeholder_offset = offset + 5 * 4;

                 // Verify same virtual_offset
                 if (out_match.virtual_offset == 0) {
                     out_match.virtual_offset = mid16_31;
                 } else if (out_match.virtual_offset != mid16_31) {
                     // Mismatched virtual_offsets - this is a critical error
                     assert(out_match.virtual_offset == mid16_31, "Mismatched virtual_offsets - placeholders don't belong to same call site");
                     return false;  // Early exit in all builds
                 }
             }
         } else if (is_call_site_without_call_target_marker_pattern(instr)) {
             // Found last_Java_pc placeholder
             {
                 uint32_t low16 = (inst >> 5) & 0xFFFF;
                 uint32_t next_inst = instr[1];
                 uint32_t mid16_31 = (next_inst >> 5) & 0xFFFF;
                 out_match.last_java_pc_va = ((uint64_t)mid16_31 << 16) | low16;
                 out_match.last_java_pc_placeholder_offset = offset;

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
         } else if (is_adrp_pattern(instr)) {
            // Locate target page
            int64_t page_offset = extract_page_offset(instr);
            uint64_t pc_page = ((uint64_t)instr) & ~0xFFFULL;
            uint64_t target_page = pc_page + page_offset;

            uint32_t imm12 = (instr[1] >> 10) & 0xFFF;
            uint64_t offset_within_page = imm12 << 3;

            uint64_t target_address = target_page + offset_within_page;

            uint64_t function_address = *(uint64_t*)target_address;
            // this is good as adrp target is only handle_deoptimization for now.
            // will change condition for other adrp function call in the future.
            assert(function_address == (uint64_t)&handle_deoptimization, "should be deoptimization call");
            if (function_address == (uint64_t)&handle_deoptimization) {
                out_match.call_target_type = CallTargetType::deopt;
                out_match.call_target_placeholder_offset = offset;
            }
        } else if (is_blr_pattern(instr)) {
            // Always use first blr instruction as blr offset
            out_match.call_target_blr_offset = offset;
            found_blr = true;
        } else if (found_ljpc && !found_blr && (inst & B_MASK) == B_PATTERN) {
            scan_from_b_target(instr, inst, code_buffer, &out_match, &found_blr);
            // If blr is not found, just stop
            if (!found_blr) {
                break;
            }
        }

        // Stop if we found both placeholders and blr
        if (found_ljpc && found_blr) {
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

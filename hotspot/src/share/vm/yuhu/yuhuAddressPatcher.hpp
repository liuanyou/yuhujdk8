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

// Scanner for finding dual virtual address placeholders in machine code
class YuhuAddressScanner : public AllStatic {
 public:
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

    static bool is_adrp_with_ldr_pattern(uint32_t* instr) {
        // Check instruction encoding (little-endian):
        // adrp   x8, 3
        // ldr    x8, [x8, #0x718]
        if ((instr[0] & 0x9F000000) == 0x90000000 &&
            (instr[1] & 0xFFC00000) == 0xF9400000) {
            return true;
        }
        return false;
    }

    static bool is_adrp_with_add_pattern(uint32_t* instr) {
        // Check instruction encoding (little-endian):
        // adrp   x11, 3
        // add    x11, x11, #0x0
        if ((instr[0] & 0x9F000000) == 0x90000000 &&
            (instr[1] & 0xFFC00000) == 0x91000000) {
            return true;
        }
        return false;
    }

    /**
     * check if it is adrp instruction sequences.
     *
     * @param instr
     * @return
     */
    static bool is_adrp_got_pattern(uint32_t* instr) {
        return is_adrp_with_ldr_pattern(instr);
    };

    /**
     * check if it is direct jump table pattern which is using adrp + add instructions.
     *
     * @param instr
     * @return
     */
    static bool is_adrp_jump_table_pattern(uint32_t* instr) {
        return is_adrp_with_add_pattern(instr);
    };

    /**
     * determine if it is unknown adrp instructions.
     *
     * @param instr
     * @return
     */
    static bool is_unknown_adrp_pattern(uint32_t* instr) {
        if ((instr[0] & 0x9F000000) == 0x90000000) {
            if ((instr[1] & 0xFFC00000) != 0xF9400000 && (instr[1] & 0xFFC00000) != 0x91000000) {
                return true;
            }
        }
        return false;
    }

    static bool is_blr_pattern(uint32_t* instr) {
        uint32_t inst = instr[0];
        return (inst & BLR_MASK) == BLR_PATTERN;
    }

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
        // the movz may carry any shift: llvm starts materializing at the lowest
        // non-zero 16-bit chunk, so an address with zero low chunks begins with
        // movz xN, #imm, lsl #16 (or #32), which is still the sequence head.
        // MOVZ_MASK leaves the hw bits [22:21] out of the comparison already.
        bool is_mov_64 = ((inst & MOV_IMM_MASK) == MOV_IMM_PATTERN_64);
        bool is_movz_64 = ((inst & MOVZ_MASK) == MOVZ_PATTERN_64);
        return is_mov_64 || is_movz_64;
    };
};

class YuhuAddressPatcher : public AllStatic {
public:
    /**
     * Rewrite a value-producing patch point's call instruction as a register move.
     *
     * LLVM materializes a patch point's target operand into a register right
     * before the call (a movz/movk sequence followed by "blr xN").  For an
     * inlined metadata constant the target operand *is* the Metadata* address,
     * so instead of transferring control we turn "blr xN" into "mov x0, xN":
     * the address already sitting in xN is delivered through x0, which is where
     * the AArch64 calling convention puts a call's result and therefore where
     * the IR consumer of the patch point reads it.
     *
     * @param blr_instr  the "blr xN" instruction of the patch point
     * @return false if the instruction is not a blr, true once it is rewritten
     */
    static bool patch_blr_to_mov_x0(uint32_t* blr_instr) {
        uint32_t blr_inst = blr_instr[0];
        if ((blr_inst & BLR_MASK) != BLR_PATTERN) {
            return false;
        }

        // expected instruction sequence of a value-producing patch point's call:
        //   movz   x16, #addr[15:0]
        //   movk   x16, #addr[31:16], lsl #16
        //   movk   x16, #addr[47:32], lsl #32
        //   blr    x16
        // BLR (register) 64-bit: 0xD63F0000 | (Rn << 5)
        uint32_t rn = (blr_inst >> 5) & 0x1F;
        assert(rn != 31, "blr from register 31 cannot be a patch point target");

        // The register being branched to already holds the value the patch point is
        // supposed to produce, so the call only has to become a move into x0.
        // MOV (register) is ORR (shifted register) with xzr as first operand and no
        // shift: orr x0, xzr, xRn -> 0xAA0003E0 | (Rm << 16) | Rd, and the pattern
        // already encodes Rd = 0 because the result must land in x0.
        blr_instr[0] = 0xAA0003E0 | (rn << 16);
        return true;
    }

    static bool patch_oop_index(uint64_t* placeholder_instr, int oop_index) {
        // Update placeholder with real oop_index (high 16 bits = 0)
        uint64_t real_placeholder = oop_index & 0xFFFFFFFFFFFFULL;
        placeholder_instr[0] = real_placeholder;
        return true;
    };

    static bool patch_branch_unwind_handler(uint32_t* blr_instr, uint64_t unwind_handler_address) {
        // Patch b instruction
        uint64_t b_addr = (uint64_t)blr_instr;
        long offset = unwind_handler_address - b_addr;
        uint32_t b_unwind_handler = 0x14000000 | ((offset >> 2) & 0x03FFFFFF);

        blr_instr[0] = b_unwind_handler;
        return true;
    };

    static bool patch_nop(uint32_t* instr, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            instr[i] = 0xD503201F;  // nop instruction
        }
        return true;
    }

    static bool patch_new_adrp(uint32_t* instr, uint64_t function_address) {
        // Calculate new page offset for function_address
        uint32_t rd = instr[0] & 0x1F;  // Extract Rd first

        uint64_t adrp_addr = (uint64_t)instr;  // Address of ADRP instruction in CodeCache
        uint64_t func_page = function_address & ~0xFFFULL;  // Function's page address
        uint64_t new_pc_page = adrp_addr & ~0xFFFULL;  // ADRP's PC page

        int64_t page_diff = (int64_t)(func_page - new_pc_page) >> 12;  // Page difference

        // Encode new ADRP
        uint32_t new_adrp = (instr[0] & 0x9F000000) | rd;  // Keep opcode and Rd
        new_adrp |= (page_diff & 0x3) << 29;  // immlo (bits [30:29])
        new_adrp |= ((page_diff >> 2) & 0x7FFFF) << 5;  // immhi (bits [23:5])

        instr[0] = new_adrp;

        // Patch add instruction
        // Calculate offset within page
        uint32_t new_page_offset = function_address & 0xFFF;  // Lower 12 bits

        // Encode ADD instruction: add x8, x8, #page_offset
        // ADD (immediate): 0x91000000 | (imm12 << 10) | (shift << 22) | Rn | Rd
        uint32_t new_add = 0x91000000;  // ADD opcode
        new_add |= (new_page_offset & 0xFFF) << 10;  // imm12 (bits [21:10])
        new_add |= rd;  // Rd = same register as ADRP
        new_add |= rd << 5;  // Rn = same register

        instr[1] = new_add;
        return true;
    };
};

#endif // SHARE_VM_YUHU_YUHUVIRTUALADDRESSPATCHER_HPP

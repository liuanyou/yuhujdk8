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

bool YuhuVirtualAddressScanner::patch_blr_to_mov_x0(uint32_t* blr_instr) {
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

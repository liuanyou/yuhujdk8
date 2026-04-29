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

// Information about matched placeholders for a single statepoint
struct VirtualAddressMatch {
  uint64_t virtual_offset;              // The shared virtual offset (e.g., 0x1000)
  uint64_t statepoint_id;               // LLVM statepoint ID
  uint64_t instruction_offset;          // Actual offset of the statepoint call (bl instruction)
  
  uint64_t last_java_pc_va;             // Last Java PC placeholder (e.g., 0xDEAD1000)
  uint64_t last_java_pc_placeholder_offset;  // Offset of movz instruction for last_Java_pc
  uint64_t last_java_pc_adr_offset;     // Offset of adr instruction (for adr+marker approach)
  
  uint64_t call_target_va;              // Call target placeholder (e.g., 0xBEEF1000)
  uint64_t call_target_placeholder_offset;   // Offset of movz instruction for call target
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
  
  static bool is_call_target_placeholder(uint64_t va) {
    return (va & 0xFFFFFFFF0000) == CALL_TARGET_MAGIC;
  }
  
  // Extract virtual_offset from a virtual address
  static uint64_t extract_virtual_offset_from_virtual_last_java_pc(uint64_t va) {
    return va & 0x0000FFFF;
  }

  static uint64_t extract_virtual_offset_from_virtual_call_target(uint64_t va) {
    return va & 0x00000000FFFF;
  }
};

#endif // SHARE_VM_YUHU_YUHUVIRTUALADDRESSPATCHER_HPP

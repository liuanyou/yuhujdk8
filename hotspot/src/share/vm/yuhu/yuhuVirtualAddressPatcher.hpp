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

// Virtual address magic numbers for placeholder identification
static const uint32_t LAST_JAVA_PC_MAGIC = 0xDEAD0000;
static const uint32_t CALL_TARGET_MAGIC = 0xBEEF0000;

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
  
  // Extract virtual address from movz/movk instruction pair
  static uint64_t extract_virtual_address_from_movz_movk(
    const uint8_t* code_buffer,
    uint64_t movz_offset
  );
  
  // Patch movz/movk instructions with a new 64-bit value
  static void patch_movz_movk_instructions(
    uint8_t* code_buffer,
    uint64_t movz_offset,
    uint64_t new_value
  );
  
  // Scan for marker pattern (mov w19, #0xDEAD; movk w19, #virtual_offset, lsl #16)
  // Returns offset of marker start, or UINT64_MAX if not found
  static uint64_t scan_for_marker(
    const uint8_t* code_buffer,
    uint64_t search_start_offset,
    uint64_t max_scan_distance
  );
  
  // Scan for marker pattern with specific virtual_offset
  // Returns offset of marker start, or UINT64_MAX if not found
  static uint64_t scan_for_marker_with_offset(
    const uint8_t* code_buffer,
    uint64_t search_start_offset,
    uint64_t max_scan_distance,
    uint64_t expected_virtual_offset
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
    return (va & 0xFFFF0000) == CALL_TARGET_MAGIC;
  }
  
  // Extract virtual_offset from a virtual address
  static uint64_t extract_virtual_offset(uint64_t va) {
    return va & 0x0000FFFF;
  }
};

#endif // SHARE_VM_YUHU_YUHUVIRTUALADDRESSPATCHER_HPP

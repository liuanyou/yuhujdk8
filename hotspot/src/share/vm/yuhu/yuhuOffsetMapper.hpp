/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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
 */

#ifndef SHARE_VM_YUHU_YUHUOFFSETMAPPER_HPP
#define SHARE_VM_YUHU_YUHUOFFSETMAPPER_HPP

#include "memory/allocation.hpp"
#include "utilities/growableArray.hpp"
#include <map>

// Structure to hold offset mapping information
struct OffsetMapping {
  int virtual_offset;  // Virtual offset from IR stage
  int actual_offset;   // Actual offset from machine code stage
};

// YuhuOffsetMapper handles the mapping between virtual offsets (from IR stage)
// and actual offsets (from machine code stage) for OopMap relocation
class YuhuOffsetMapper : public StackObj {
 private:
  // Map to store virtual offset to actual offset mapping
  std::map<int, int> _offset_map;
  
  // Array to store offset mappings for ordered iteration
  GrowableArray<OffsetMapping>* _mappings;

 public:
  YuhuOffsetMapper();
  ~YuhuOffsetMapper();

  // Add a new mapping from virtual offset to actual offset
  void add_mapping(int virtual_offset, int actual_offset);

  // Get the actual offset for a given virtual offset
  int get_actual_offset(int virtual_offset) const;

  // Get the virtual offset for a given actual offset
  int get_virtual_offset(int actual_offset) const;

  // Check if a mapping exists for the given virtual offset
  bool has_mapping(int virtual_offset) const;

  // Get the number of mappings
  int num_mappings() const;

  // Get mapping at a specific index
  OffsetMapping* mapping_at(int index) const;

  // Print all mappings for debugging
  void print_mappings() const;

  // Relocate a virtual offset to actual offset, or return original if no mapping exists
  int relocate_offset(int virtual_offset) const;

  // Get the entire offset map (for debugging/testing)
  const std::map<int, int>& offset_map() const { return _offset_map; }
};

#endif // SHARE_VM_YUHU_YUHUOFFSETMAPPER_HPP
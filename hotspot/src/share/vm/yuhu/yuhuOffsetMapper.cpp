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

#include "precompiled.hpp"
#include "yuhu/yuhuOffsetMapper.hpp"
#include "utilities/debug.hpp"

YuhuOffsetMapper::YuhuOffsetMapper() {
  _mappings = new GrowableArray<OffsetMapping>();
}

YuhuOffsetMapper::~YuhuOffsetMapper() {
  if (_mappings != NULL) {
    _mappings = NULL;
  }
  _offset_map.clear();
}

void YuhuOffsetMapper::add_mapping(int virtual_offset, int actual_offset) {
  // Add to the map for fast lookup
  _offset_map[virtual_offset] = actual_offset;
  
  // Add to the array for ordered iteration
  OffsetMapping mapping;
  mapping.virtual_offset = virtual_offset;
  mapping.actual_offset = actual_offset;
  _mappings->append(mapping);
}

int YuhuOffsetMapper::get_actual_offset(int virtual_offset) const {
  std::map<int, int>::const_iterator it = _offset_map.find(virtual_offset);
  if (it != _offset_map.end()) {
    return it->second;
  }
  // If no mapping exists, return -1 to indicate not found
  return -1;
}

int YuhuOffsetMapper::get_virtual_offset(int actual_offset) const {
  // Search through mappings array to find the virtual offset for given actual offset
  for (int i = 0; i < _mappings->length(); i++) {
    if (_mappings->at(i).actual_offset == actual_offset) {
      return _mappings->at(i).virtual_offset;
    }
  }
  // If no mapping exists, return -1 to indicate not found
  return -1;
}

bool YuhuOffsetMapper::has_mapping(int virtual_offset) const {
  return _offset_map.find(virtual_offset) != _offset_map.end();
}

int YuhuOffsetMapper::num_mappings() const {
  return _mappings->length();
}

OffsetMapping* YuhuOffsetMapper::mapping_at(int index) const {
  if (index >= 0 && index < _mappings->length()) {
    return (OffsetMapping*)&(_mappings->at(index));
  }
  return NULL;
}

void YuhuOffsetMapper::print_mappings() const {
  tty->print_cr("YuhuOffsetMapper: %d mappings", _mappings->length());
  for (int i = 0; i < _mappings->length(); i++) {
    const OffsetMapping& mapping = _mappings->at(i);
    tty->print_cr("  Mapping %d: virtual=%d -> actual=%d", 
                  i, mapping.virtual_offset, mapping.actual_offset);
  }
}

int YuhuOffsetMapper::relocate_offset(int virtual_offset) const {
  std::map<int, int>::const_iterator it = _offset_map.find(virtual_offset);
  if (it != _offset_map.end()) {
    return it->second;
  }
  // If no mapping exists, return the original virtual offset
  return virtual_offset;
}
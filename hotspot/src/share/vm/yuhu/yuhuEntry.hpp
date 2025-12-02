/*
 * Copyright (c) 1999, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2008, 2009 Red Hat, Inc.
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

#ifndef SHARE_VM_YUHU_YUHUENTRY_HPP
#define SHARE_VM_YUHU_YUHUENTRY_HPP

#include "yuhu/llvmHeaders.hpp"
#include "utilities/globalDefinitions.hpp"

class YuhuContext;

// YuhuEntry is similar to ZeroEntry but for AArch64
// It stores the entry point address and related information
class YuhuEntry {
 private:
  address         _entry_point;
  address         _code_limit;
  YuhuContext*   _context;
  llvm::Function* _function;

 public:
  address entry_point() const {
    return _entry_point;
  }
  void set_entry_point(address entry_point) {
    _entry_point = entry_point;
  }

  address code_start() const {
    return entry_point();
  }
  address code_limit() const {
    return _code_limit;
  }
  YuhuContext* context() const {
    return _context;
  }
  llvm::Function* function() const {
    return _function;
  }

 public:
  void set_code_limit(address code_limit) {
    _code_limit = code_limit;
  }
  void set_context(YuhuContext* context) {
    _context = context;
  }
  void set_function(llvm::Function* function) {
    _function = function;
  }

 public:
  // Static method to get offset of entry_point field (similar to ZeroEntry::entry_point_offset)
  static ByteSize entry_point_offset() {
    return byte_offset_of(YuhuEntry, _entry_point);
  }
};

#endif // SHARE_VM_YUHU_YUHUENTRY_HPP

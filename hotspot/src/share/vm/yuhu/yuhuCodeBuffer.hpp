/*
 * Copyright (c) 1999, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2009 Red Hat, Inc.
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

#ifndef SHARE_VM_YUHU_YUHUCODEBUFFER_HPP
#define SHARE_VM_YUHU_YUHUCODEBUFFER_HPP

#include "asm/codeBuffer.hpp"
#include "memory/allocation.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "asm/yuhu/yuhu_macroAssembler.hpp"

class YuhuCodeBuffer : public StackObj {
 public:
  YuhuCodeBuffer(YuhuMacroAssembler& masm)
    : _masm(masm), _base_pc(NULL) {}

 private:
  YuhuMacroAssembler& _masm;
  llvm::Value*        _base_pc;

 private:
  YuhuMacroAssembler& masm() const {
    return _masm;
  }

 public:
  llvm::Value* base_pc() const {
    return _base_pc;
  }
  void set_base_pc(llvm::Value* base_pc) {
    assert(_base_pc == NULL, "only do this once");
    _base_pc = base_pc;
  }

  // Allocate some space in the buffer and return its address.
  // This buffer will have been relocated by the time the method
  // is installed, so you can't inline the result in code.
 public:
  void* malloc(size_t size) const {
    masm().align(BytesPerWord);
    void *result = masm().pc();
    // advance() is implemented by setting the end of the code section
    masm().code_section()->set_end(masm().code_section()->end() + size);
    return result;
  }

  // Create a unique offset in the buffer.
 public:
  int create_unique_offset() const {
    int offset = masm().offset();
    // advance() is implemented by setting the end of the code section
    masm().code_section()->set_end(masm().code_section()->end() + 1);
    return offset;
  }

  // Inline an oop into the buffer and return its offset.
 public:
  int inline_oop(jobject object) const {
    masm().align(BytesPerWord);
    int offset = masm().offset();
    masm().store_oop(object);
    return offset;
  }

  int inline_Metadata(Metadata* metadata) const {
    masm().align(BytesPerWord);
    int offset = masm().offset();
    masm().store_Metadata(metadata);
    return offset;
  }

  // Inline a block of non-oop data into the buffer and return its offset.
 public:
  int inline_data(void *src, size_t size) const {
    masm().align(BytesPerWord);
    int offset = masm().offset();
    void *dst = masm().pc();
    // advance() is implemented by setting the end of the code section
    masm().code_section()->set_end(masm().code_section()->end() + size);
    memcpy(dst, src, size);
    return offset;
  }
};

#endif // SHARE_VM_YUHU_YUHUCODEBUFFER_HPP

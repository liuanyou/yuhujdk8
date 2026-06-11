/*
 * Copyright (c) 1999, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2008, 2009, 2010 Red Hat, Inc.
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

#ifndef SHARE_VM_YUHU_YUHUSTACK_HPP
#define SHARE_VM_YUHU_YUHUSTACK_HPP

#include "yuhu/llvmHeaders.hpp"
#include "yuhu/yuhuInvariants.hpp"
#include "yuhu/yuhuType.hpp"

class YuhuFunction;
class YuhuNativeWrapper;
class YuhuStackWithNormalFrame;
class YuhuStackWithNativeFrame;
enum class CallSiteType : uint8_t;

class YuhuStack : public YuhuCompileInvariants {
 public:
  static YuhuStack* CreateBuildAndPushFrame(
    YuhuFunction* function, llvm::Value* method);
  static YuhuStack* CreateBuildAndPushFrame(
    YuhuNativeWrapper* wrapper, llvm::Value* method);

 protected:
  YuhuStack(const YuhuCompileInvariants* parent)
    : YuhuCompileInvariants(parent) {}

 protected:
  void initialize(llvm::Value* method, llvm::BasicBlock* exit_block);

 protected:
  // Stack overflow check - checks if the new stack pointer (sp) has enough space
  // FIXED: Now only takes sp parameter, checks the actual stack pointer after frame allocation
  // exit_block: unified exit block to jump to on overflow (NULL for native wrappers)
  void CreateStackOverflowCheck(llvm::Value* sp, llvm::BasicBlock* exit_block);

  // Properties of the method being compiled
 protected:
  virtual int arg_size() const = 0;
  virtual int max_locals() const = 0;
  virtual int max_stack() const = 0;
  virtual int max_monitors() const = 0;

  // BasicBlock creation
 protected:
  virtual llvm::BasicBlock* CreateBlock(const char* name = "") const = 0;

  // Interpreter entry point for bailouts
 protected:
  virtual address interpreter_entry_point() const = 0;

  // Get the unified exit block for all return paths
  // Returns NULL for native wrappers which handle their own returns
 protected:
  virtual llvm::BasicBlock* unified_exit_block() const = 0;

  // Interface with the AArch64 stack
  // AArch64 uses standard ABI stack, not ZeroStack
 private:
  // Frame pointer is stored in the frame header
  mutable llvm::Value* _frame_pointer_addr;  // Address where frame pointer is stored in frame header
  mutable llvm::Value* _return_slot_addr;        // PC slot address (reused as return slot)
  
  llvm::Value* frame_pointer_addr() const {
    // Frame pointer is stored in the frame header
    return _frame_pointer_addr;
  }

 public:
  llvm::LoadInst* CreateLoadFramePointer(const char *name = "");
  llvm::StoreInst* CreateStoreFramePointer(llvm::Value* value);

  // Interface with the frame anchor
 private:
  llvm::Value* last_Java_sp_addr() const;
  llvm::Value* last_Java_fp_addr() const;
  llvm::Value* last_Java_pc_addr() const;

 public:
  void CreateSetLastJavaFrame();

  void CreateSetLastJavaFrameWithPlaceholderNoPC(uint64_t virtual_address);
  
  // NEW: CreateSetLastJavaFrameWithPlaceholderPC - stores a 64-bit virtual address placeholder
  // that will be patched by JITLink plugin with the actual return address
  // This generates movz/movk/str pattern that can be scanned and patched
  void CreateSetLastJavaFrameWithPlaceholderPC(uint64_t virtual_address);
  
  void CreateResetLastJavaFrame();

  void CreateResetLastJavaFrameWithNoPC();

  void CreateCallSitePlaceholder(uint64_t virtual_address);

  llvm::Value* CreateCallSitePlaceholderWithCallTarget(uint64_t virtual_address, uint64_t call_target_va, CallSiteType call_site_type);

 private:
  void CreateAssertLastJavaSPIsNull() const PRODUCT_RETURN;

  // Our method's frame
 private:
  llvm::Value* _frame;
  int          _extended_frame_size;
  int          _stack_slots_offset;

 public:
  int extended_frame_size() const {
    return _extended_frame_size;
  }
  int oopmap_frame_size() const {
    return extended_frame_size() - arg_size();
  }

  // Offsets of things in the frame
 private:
  int _monitors_slots_offset;
  int _oop_tmp_slot_offset;
  int _method_slot_offset;
  int _locals_slots_offset;

 public:
  int stack_slots_offset() const {
    return _stack_slots_offset;
  }
  int oop_tmp_slot_offset() const {
    return _oop_tmp_slot_offset;
  }
  int method_slot_offset() const {
    return _method_slot_offset;
  }

  int locals_slots_offset() const {
    return _locals_slots_offset;
  }

  // The unextended_sp is stored in the frame header after method slot
  // and before PC slot (so it's at offset 8 in the header)
  int unextended_sp_slot_offset() const {
    // unextended_sp is stored after oop_tmp and method slots, before pc slot
    // So its offset is method_slot_offset() + 1
    return _method_slot_offset + 1;
  }
  int monitor_offset(int index) const {
    assert(index >= 0 && index < max_monitors(), "invalid monitor index");
    return _monitors_slots_offset +
      (max_monitors() - 1 - index) * frame::interpreter_frame_monitor_size();
  }
  int monitor_object_offset(int index) const {
    return monitor_offset(index) +
      (BasicObjectLock::obj_offset_in_bytes() >> LogBytesPerWord);
  }
  int monitor_header_offset(int index) const {
    return monitor_offset(index) +
      ((BasicObjectLock::lock_offset_in_bytes() +
        BasicLock::displaced_header_offset_in_bytes()) >> LogBytesPerWord);
  }

  // Addresses of things in the frame
 public:
  llvm::Value* slot_addr(int               offset,
                         llvm::Type* type = NULL,
                         const char*       name = "") const;

  llvm::Value* monitor_addr(int index) const {
    return slot_addr(
      monitor_offset(index),
      YuhuType::monitor_type(),
      "monitor");
  }
  llvm::Value* monitor_object_addr(int index) const {
    return slot_addr(
      monitor_object_offset(index),
      YuhuType::oop_addrspace1_type(), // FIXED - monitor object is allocated in heap
      "object_addr");
  }
  llvm::Value* monitor_header_addr(int index) const {
    return slot_addr(
      monitor_header_offset(index),
      YuhuType::intptr_type(),
      "displaced_header_addr");
  }

  // PC slot address (reused as return slot for non-void methods)
  llvm::Value* return_slot_addr() const {
    assert(_return_slot_addr != NULL, "return_slot_addr not initialized");
    return _return_slot_addr;
  }

  // oopmap helpers
 public:
  static int oopmap_slot_munge(int offset) {
    return offset << (LogBytesPerWord - LogBytesPerInt);
  }
  static VMReg slot2reg(int offset) {
    return VMRegImpl::stack2reg(oopmap_slot_munge(offset));
  }
};

class YuhuStackWithNormalFrame : public YuhuStack {
  friend class YuhuStack;

 protected:
  YuhuStackWithNormalFrame(YuhuFunction* function, llvm::Value* method);

 private:
  YuhuFunction* _function;

 private:
  YuhuFunction* function() const {
    return _function;
  }

  // Properties of the method being compiled
 private:
  int arg_size() const;
  int max_locals() const;
  int max_stack() const;
  int max_monitors() const;

  // BasicBlock creation
 private:
  llvm::BasicBlock* CreateBlock(const char* name = "") const;

  // Interpreter entry point for bailouts
 private:
  address interpreter_entry_point() const;

  // Get the unified exit block for all return paths
 private:
  llvm::BasicBlock* unified_exit_block() const;
};

class YuhuStackWithNativeFrame : public YuhuStack {
  friend class YuhuStack;

 protected:
  YuhuStackWithNativeFrame(YuhuNativeWrapper* wrapper, llvm::Value* method);

 private:
  YuhuNativeWrapper* _wrapper;

 private:
  YuhuNativeWrapper* wrapper() const {
    return _wrapper;
  }

  // Properties of the method being compiled
 private:
  int arg_size() const;
  int max_locals() const;
  int max_stack() const;
  int max_monitors() const;

  // BasicBlock creation
 private:
  llvm::BasicBlock* CreateBlock(const char* name = "") const;

  // Interpreter entry point for bailouts
 private:
  address interpreter_entry_point() const;

  // Get the unified exit block for all return paths
 private:
  llvm::BasicBlock* unified_exit_block() const;
};

#endif // SHARE_VM_YUHU_YUHUSTACK_HPP

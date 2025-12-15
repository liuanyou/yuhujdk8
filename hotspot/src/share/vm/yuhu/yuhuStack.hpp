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
  void initialize(llvm::Value* method);

 protected:
  // Stack overflow check - checks if the new stack pointer (sp) has enough space
  // FIXED: Now only takes sp parameter, checks the actual stack pointer after frame allocation
  void CreateStackOverflowCheck(llvm::Value* sp);

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

  // Interface with the AArch64 stack
  // AArch64 uses standard ABI stack, not ZeroStack
 private:
  // Stack pointer is stored as a local variable during frame initialization
  // Frame pointer is stored in the frame header
  mutable llvm::Value* _stack_pointer;  // Current stack pointer value
  mutable llvm::Value* _frame_pointer_addr;  // Address where frame pointer is stored in frame header
  mutable llvm::AllocaInst* _sp_storage;  // Storage for stack pointer

  // Initialize stack pointer and frame pointer storage
  void initialize_stack_pointers(llvm::Value* stack_pointer) {
    _stack_pointer = stack_pointer;
    // Create storage for stack pointer
    _sp_storage = builder()->CreateAlloca(YuhuType::intptr_type(), 0, "sp_storage");
    builder()->CreateStore(stack_pointer, _sp_storage);
    // Frame pointer address will be set in initialize() when frame is created
  }

  llvm::Value* stack_pointer_addr() const {
    // For AArch64, return the address of the stack pointer storage
    if (_sp_storage != NULL) {
      return _sp_storage;
    }
    // Fallback: create a new alloca (should not happen in normal flow)
    return builder()->CreateAlloca(YuhuType::intptr_type(), 0, "sp_addr");
  }
  
  llvm::Value* frame_pointer_addr() const {
    // Frame pointer is stored in the frame header
    return _frame_pointer_addr;
  }

 public:
  llvm::LoadInst* CreateLoadStackPointer(const char *name = "") {
    // For AArch64, load from the stack pointer storage
    // LLVM 20+ requires explicit type parameter for CreateLoad
    return builder()->CreateLoad(
      YuhuType::intptr_type(),
      stack_pointer_addr(),
      name);
  }
  llvm::StoreInst* CreateStoreStackPointer(llvm::Value* value) {
    // For AArch64, store the stack pointer in storage
    _stack_pointer = value;
    if (_sp_storage == NULL) {
      _sp_storage = builder()->CreateAlloca(YuhuType::intptr_type(), 0, "sp_storage");
    }
    return builder()->CreateStore(value, _sp_storage);
  }
  llvm::LoadInst* CreateLoadFramePointer(const char *name = "") {
    // For AArch64, load frame pointer from frame header
    // LLVM 20+ requires explicit type parameter for CreateLoad
    if (_frame_pointer_addr != NULL) {
      return builder()->CreateLoad(
        YuhuType::intptr_type(),
        _frame_pointer_addr,
        name);
    }
    // Fallback: load from last_Java_fp
    return builder()->CreateLoad(
      YuhuType::intptr_type(),
      builder()->CreateAddressOfStructEntry(
        thread(),
        JavaThread::last_Java_fp_offset(),
        llvm::PointerType::getUnqual(YuhuType::intptr_type())),
      name);
  }
  llvm::StoreInst* CreateStoreFramePointer(llvm::Value* value) {
    // For AArch64, store frame pointer in frame header and last_Java_fp
    if (_frame_pointer_addr != NULL) {
      builder()->CreateStore(value, _frame_pointer_addr);
    }
    // Also update last_Java_fp for frame anchor
    return builder()->CreateStore(
      value,
      builder()->CreateAddressOfStructEntry(
        thread(),
        JavaThread::last_Java_fp_offset(),
        llvm::PointerType::getUnqual(YuhuType::intptr_type())));
  }
  llvm::Value* CreatePopFrame(int result_slots);

  // Interface with the frame anchor
 private:
  llvm::Value* last_Java_sp_addr() const {
    return builder()->CreateAddressOfStructEntry(
      thread(),
      JavaThread::last_Java_sp_offset(),
      llvm::PointerType::getUnqual(YuhuType::intptr_type()),
      "last_Java_sp_addr");
  }
  llvm::Value* last_Java_fp_addr() const {
    return builder()->CreateAddressOfStructEntry(
      thread(),
      JavaThread::last_Java_fp_offset(),
      llvm::PointerType::getUnqual(YuhuType::intptr_type()),
      "last_Java_fp_addr");
  }

 public:
  void CreateSetLastJavaFrame() {
    // Note that whenever _last_Java_sp != NULL other anchor fields
    // must be valid.  The profiler apparently depends on this.
    // For normal compilation, we reset last_Java_sp at method entry,
    // so it should be 0 when the first VM call is made.
    NOT_PRODUCT(CreateAssertLastJavaSPIsNull());
    builder()->CreateStore(CreateLoadFramePointer(), last_Java_fp_addr());
    // XXX There's last_Java_pc as well, but I don't think anything uses it
    // Also XXX: should we fence here?  Zero doesn't...
    builder()->CreateStore(CreateLoadStackPointer(), last_Java_sp_addr());
    // Also also XXX: we could probably cache the sp (and the fp we know??)
  }
  void CreateResetLastJavaFrame() {
    builder()->CreateStore(LLVMValue::intptr_constant(0), last_Java_sp_addr());
  }

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
  int _pc_slot_offset;
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
  int pc_slot_offset() const {
    return _pc_slot_offset;
  }
  int locals_slots_offset() const {
    return _locals_slots_offset;
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
      YuhuType::oop_type(),
      "object_addr");
  }
  llvm::Value* monitor_header_addr(int index) const {
    return slot_addr(
      monitor_header_offset(index),
      YuhuType::intptr_type(),
      "displaced_header_addr");
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
};

#endif // SHARE_VM_YUHU_YUHUSTACK_HPP

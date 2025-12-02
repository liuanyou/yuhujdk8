/*
 * Copyright (c) 1999, 2012, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "ci/ciMethod.hpp"
#include "code/debugInfoRec.hpp"
#include "yuhu/llvmValue.hpp"
#include "yuhu/yuhuBuilder.hpp"
#include "yuhu/yuhuCacheDecache.hpp"
#include "yuhu/yuhuFunction.hpp"
#include "yuhu/yuhuState.hpp"

using namespace llvm;

void YuhuDecacher::start_frame() {
  // Start recording the debug information
  _pc_offset = code_buffer()->create_unique_offset();
  _oopmap = new OopMap(
    oopmap_slot_munge(stack()->oopmap_frame_size()),
    oopmap_slot_munge(arg_size()));
  debug_info()->add_safepoint(pc_offset(), oopmap());
}

void YuhuDecacher::start_stack(int stack_depth) {
  // Create the array we'll record our stack slots in
  _exparray = new GrowableArray<ScopeValue*>(stack_depth);

  // Set the stack pointer
  stack()->CreateStoreStackPointer(
    builder()->CreatePtrToInt(
      stack()->slot_addr(
        stack()->stack_slots_offset() + max_stack() - stack_depth),
      YuhuType::intptr_type()));
}

void YuhuDecacher::process_stack_slot(int          index,
                                       YuhuValue** addr,
                                       int          offset) {
  YuhuValue *value = *addr;

  // Write the value to the frame if necessary
  if (stack_slot_needs_write(index, value)) {
    write_value_to_frame(
      YuhuType::to_stackType(value->basic_type()),
      value->generic_value(),
      adjusted_offset(value, offset));
  }

  // Record the value in the oopmap if necessary
  if (stack_slot_needs_oopmap(index, value)) {
    oopmap()->set_oop(slot2reg(offset));
  }

  // Record the value in the debuginfo if necessary
  if (stack_slot_needs_debuginfo(index, value)) {
    exparray()->append(slot2lv(offset, stack_location_type(index, addr)));
  }
}

void YuhuDecacher::start_monitors(int num_monitors) {
  // Create the array we'll record our monitors in
  _monarray = new GrowableArray<MonitorValue*>(num_monitors);
}

void YuhuDecacher::process_monitor(int index, int box_offset, int obj_offset) {
  oopmap()->set_oop(slot2reg(obj_offset));

  monarray()->append(new MonitorValue(
    slot2lv (obj_offset, Location::oop),
    slot2loc(box_offset, Location::normal)));
}

void YuhuDecacher::process_oop_tmp_slot(Value** value, int offset) {
  // Decache the temporary oop slot
  if (*value) {
    write_value_to_frame(
      YuhuType::oop_type(),
      *value,
      offset);

    oopmap()->set_oop(slot2reg(offset));
  }
}

void YuhuDecacher::process_method_slot(Value** value, int offset) {
  // Decache the method pointer
  write_value_to_frame(
    YuhuType::Method_type(),
    *value,
    offset);

}

void YuhuDecacher::process_pc_slot(int offset) {
  // Record the PC
  builder()->CreateStore(
    builder()->code_buffer_address(pc_offset()),
    stack()->slot_addr(offset));
}

void YuhuDecacher::start_locals() {
  // Create the array we'll record our local variables in
  _locarray = new GrowableArray<ScopeValue*>(max_locals());}

void YuhuDecacher::process_local_slot(int          index,
                                       YuhuValue** addr,
                                       int          offset) {
  YuhuValue *value = *addr;

  // Write the value to the frame if necessary
  if (local_slot_needs_write(index, value)) {
    write_value_to_frame(
      YuhuType::to_stackType(value->basic_type()),
      value->generic_value(),
      adjusted_offset(value, offset));
  }

  // Record the value in the oopmap if necessary
  if (local_slot_needs_oopmap(index, value)) {
    oopmap()->set_oop(slot2reg(offset));
  }

  // Record the value in the debuginfo if necessary
  if (local_slot_needs_debuginfo(index, value)) {
    locarray()->append(slot2lv(offset, local_location_type(index, addr)));
  }
}

void YuhuDecacher::end_frame() {
  // Record the scope
  debug_info()->describe_scope(
    pc_offset(),
    target(),
    bci(),
    true,
    false,
    false,
    debug_info()->create_scope_values(locarray()),
    debug_info()->create_scope_values(exparray()),
    debug_info()->create_monitor_values(monarray()));

  // Finish recording the debug information
  debug_info()->end_safepoint(pc_offset());
}

void YuhuCacher::process_stack_slot(int          index,
                                     YuhuValue** addr,
                                     int          offset) {
  YuhuValue *value = *addr;

  // Read the value from the frame if necessary
  if (stack_slot_needs_read(index, value)) {
    *addr = YuhuValue::create_generic(
      value->type(),
      read_value_from_frame(
        YuhuType::to_stackType(value->basic_type()),
        adjusted_offset(value, offset)),
      value->zero_checked());
  }
}

void YuhuOSREntryCacher::process_monitor(int index,
                                          int box_offset,
                                          int obj_offset) {
  // Copy the monitor from the OSR buffer to the frame
  int src_offset = max_locals() + index * 2;
  // LLVM 20+ requires explicit type parameter for CreateLoad
  builder()->CreateStore(
    builder()->CreateLoad(
      YuhuType::intptr_type(),
      CreateAddressOfOSRBufEntry(src_offset, YuhuType::intptr_type())),
    stack()->slot_addr(box_offset, YuhuType::intptr_type()));
  builder()->CreateStore(
    builder()->CreateLoad(
      YuhuType::oop_type(),
      CreateAddressOfOSRBufEntry(src_offset + 1, YuhuType::oop_type())),
    stack()->slot_addr(obj_offset, YuhuType::oop_type()));
}

void YuhuCacher::process_oop_tmp_slot(Value** value, int offset) {
  // Cache the temporary oop
  if (*value)
    *value = read_value_from_frame(YuhuType::oop_type(), offset);
}

void YuhuCacher::process_method_slot(Value** value, int offset) {
  // Cache the method pointer
  *value = read_value_from_frame(YuhuType::Method_type(), offset);
}

void YuhuFunctionEntryCacher::process_method_slot(Value** value, int offset) {
  // "Cache" the method pointer
  *value = method();
}

void YuhuCacher::process_local_slot(int          index,
                                     YuhuValue** addr,
                                     int          offset) {
  YuhuValue *value = *addr;

  // Read the value from the frame if necessary
  if (local_slot_needs_read(index, value)) {
    *addr = YuhuValue::create_generic(
      value->type(),
      read_value_from_frame(
        YuhuType::to_stackType(value->basic_type()),
        adjusted_offset(value, offset)),
      value->zero_checked());
  }
}

Value* YuhuOSREntryCacher::CreateAddressOfOSRBufEntry(int         offset,
                                                       llvm::Type* type) {
  // LLVM 20+ uses opaque pointer types, so we can't get element type from PointerType
  // Instead, we use CreateGEP with explicit index calculation
  // The OSR buffer is a byte array, so we use jbyte_type() as the element type
  Value *result = builder()->CreateGEP(YuhuType::jbyte_type(), osr_buf(), 
                                       LLVMValue::intptr_constant(offset));
  if (type != YuhuType::intptr_type())
    result = builder()->CreateBitCast(result, PointerType::getUnqual(type));
  return result;
}

void YuhuOSREntryCacher::process_local_slot(int          index,
                                             YuhuValue** addr,
                                             int          offset) {
  YuhuValue *value = *addr;

  // Read the value from the OSR buffer if necessary
  if (local_slot_needs_read(index, value)) {
    *addr = YuhuValue::create_generic(
      value->type(),
      // LLVM 20+ requires explicit type parameter for CreateLoad
      builder()->CreateLoad(
        YuhuType::to_stackType(value->basic_type()),
        CreateAddressOfOSRBufEntry(
          adjusted_offset(value, max_locals() - 1 - index),
          YuhuType::to_stackType(value->basic_type()))),
      value->zero_checked());
  }
}

void YuhuDecacher::write_value_to_frame(llvm::Type* type,
                                         Value*      value,
                                         int         offset) {
  builder()->CreateStore(value, stack()->slot_addr(offset, type));
}

Value* YuhuCacher::read_value_from_frame(llvm::Type* type, int offset) {
  return builder()->CreateLoad(type, stack()->slot_addr(offset, type));
}

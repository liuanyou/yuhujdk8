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
  
  // Insert offset marker to create mapping between virtual offset and actual offset
  // This is essential for OopMap relocation after machine code generation
  // Use the builder to create a distinctive LLVM IR marker that can be identified
  // during machine code generation
  builder()->CreateOffsetMarker(_pc_offset);
  
  _oopmap = new OopMap(
    oopmap_slot_munge(stack()->oopmap_frame_size()),
    oopmap_slot_munge(arg_size()));
    
  // Store the oopmap for later processing
  // We do NOT call debug_info()->add_safepoint() here because virtual offsets
  // may not be in proper order. We'll process them after machine code generation.
  function()->add_deferred_oopmap(pc_offset(), oopmap());
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

// Get function argument by local index
// For static methods: local[i] = function_arg[i+1] (skip NULL at function_arg[0])
// For non-static methods: local[i] = function_arg[i] (this is in function_arg[0])
llvm::Argument* YuhuNormalEntryCacher::get_function_arg(int local_index) {
  llvm::Function* func = function()->function();
  llvm::Function::arg_iterator ai = func->arg_begin();
  
  if (is_static()) {
    // Static methods: skip NULL at function_arg[0]
    ai++;  // Skip NULL argument
  }
  
  // Advance to the requested local index
  for (int i = 0; i < local_index; i++) {
    ai++;
    if (ai == func->arg_end()) {
      ShouldNotReachHere();
      return NULL;
    }
  }
  
  return &*ai;  // Dereference iterator to get Argument*
}

// Read stack argument from x20 (esp) for arguments >= 8
// Stack arguments are at [esp + (arg_index - 8) * 8] in caller's frame
// arg_index is the argument index in AArch64 calling convention (0-based, including receiver for non-static)
llvm::Value* YuhuNormalEntryCacher::read_stack_arg(int arg_index) {
  // Read x20 (esp) register
  llvm::Value* esp = builder()->CreateReadRegister("x20");
  
  // Calculate stack argument address: esp + (arg_index - 8) * 8
  // In AArch64 calling convention, stack arguments start at [sp] in caller's frame
  // arg_index 8 means the 9th argument (0-based), which is at [sp + 0]
  // arg_index 9 means the 10th argument, which is at [sp + 8]
  int stack_offset = (arg_index - 8) * wordSize;
  llvm::Value* stack_arg_addr = builder()->CreateGEP(
    YuhuType::intptr_type(),
    builder()->CreateIntToPtr(esp, llvm::PointerType::getUnqual(YuhuType::intptr_type())),
    LLVMValue::intptr_constant(stack_offset / wordSize),
    "stack_arg_addr");
  
  // Load the stack argument as intptr_t (will be cast to correct type by caller)
  return builder()->CreateLoad(
    YuhuType::intptr_type(),
    stack_arg_addr,
    "stack_arg");
}

void YuhuNormalEntryCacher::process_local_slot(int          index,
                                               YuhuValue** addr,
                                               int          offset) {
  YuhuValue *value = *addr;

  // Only populate for real arguments
  if (local_slot_needs_read(index, value) && index < arg_size()) {
    llvm::Type* stack_ty = YuhuType::to_stackType(value->basic_type());
    llvm::Value* loaded = NULL;
    
    // According to 021 design:
    // - Parameters 0-7 are in registers (x0-x7), passed as function arguments
    // - Parameters >= 8 are on the stack, read from x20 (esp)
    
    if (index < 8) {
      // Parameters 0-7: read from function arguments
      llvm::Argument* arg = get_function_arg(index);
      assert(arg != NULL, "function argument should exist");
      loaded = arg;
      
      // Ensure type matches exactly (may need conversion for integers)
      if (loaded->getType() != stack_ty) {
        // For integer types, use CreateIntCast; for pointers, use CreateBitCast
        if (stack_ty->isIntegerTy() && loaded->getType()->isIntegerTy()) {
          loaded = builder()->CreateIntCast(
            loaded,
            stack_ty,
            value->basic_type() != T_CHAR,  // signed unless char
            "arg_typed");
        } else if (stack_ty->isPointerTy() && loaded->getType()->isPointerTy()) {
          loaded = builder()->CreateBitCast(loaded, stack_ty, "arg_typed");
        } else {
          // Fallback: try bitcast
          loaded = builder()->CreateBitCast(loaded, stack_ty, "arg_typed");
        }
      }
    } else {
      // Parameters >= 8: read from stack via x20 (esp)
      // In AArch64 calling convention, arguments are passed as:
      // - Non-static: x0-x7 = args[0-7], stack = args[8+]
      //   So local[8] = arg[8] (in stack, calling convention arg_index = 8)
      // - Static: x0 = NULL (from i2c adapter), x1-x7 = args[0-6], stack = args[7+]
      //   So local[7] = arg[7] (in stack, calling convention arg_index = 7)
      //   local[8] = arg[8] (in stack, calling convention arg_index = 8)
      // 
      // The calling convention arg_index is the same as local_index for both cases
      // because local_index already accounts for the receiver (non-static) or NULL (static)
      int arg_index = index;
      
      loaded = read_stack_arg(arg_index);
      
      // Cast to the expected type if needed
      if (loaded->getType() != stack_ty) {
        // For integer types, use CreateIntCast; for pointers, use CreateBitCast
        if (stack_ty->isIntegerTy() && loaded->getType()->isIntegerTy()) {
          loaded = builder()->CreateIntCast(
            loaded,
            stack_ty,
            value->basic_type() != T_CHAR,  // signed unless char
            "stack_arg_typed");
        } else if (stack_ty->isPointerTy() && loaded->getType()->isPointerTy()) {
          loaded = builder()->CreateBitCast(loaded, stack_ty, "stack_arg_typed");
        } else {
          // Fallback: try bitcast
          loaded = builder()->CreateBitCast(loaded, stack_ty, "stack_arg_typed");
        }
      }
    }

    // Write into frame locals so downstream reads see it in the expected slot
    builder()->CreateStore(
      loaded,
      stack()->slot_addr(adjusted_offset(value, offset), stack_ty));

    // Update the cached value
    *addr = YuhuValue::create_generic(
      value->type(),
      loaded,
      value->zero_checked());
  }
}

void YuhuDecacher::end_frame() {
  // Add the frame information to the deferred collection for later processing
  // This ensures all OopMaps are handled consistently in the relocation process
  function()->add_deferred_frame(pc_offset(), target(), bci(), locarray(), exparray(), monarray());
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
  // method() is read from register (x12) as intptr_type(), but we need Method_type() (pointer)
  llvm::Value* method_val = method();
  if (method_val->getType() != YuhuType::Method_type()) {
    // Convert from intptr_type() to Method_type() using inttoptr
    method_val = builder()->CreateIntToPtr(method_val, YuhuType::Method_type(), "method_ptr");
  }
  *value = method_val;
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

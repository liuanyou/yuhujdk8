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
  // Use forced virtual_offset if provided, otherwise create new one
  if (_forced_virtual_offset >= 0) {
    _pc_offset = _forced_virtual_offset;
  } else {
    _pc_offset = code_buffer()->create_unique_offset();
  }
}

void YuhuDecacher::start_stack(int stack_depth) {
  // Stack decaching - debug info collected via DeoptBundle from StackMap metadata
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
}

void YuhuDecacher::start_monitors(int num_monitors) {
  // Monitor decaching - debug info collected via DeoptBundle from StackMap metadata
}

void YuhuDecacher::process_monitor(int index, int box_offset, int obj_offset) {
  // Monitor decaching handled by acquire_lock/release_lock
  // Debug info collected via DeoptBundle from StackMap metadata
}

void YuhuDecacher::process_oop_tmp_slot(Value** value, int offset) {
  // Decache the temporary oop slot
  if (*value) {
    write_value_to_frame(
      YuhuType::oop_addrspace1_type(), // FIXED - oop tmp is heap object
      *value,
      offset);
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
  // Local decaching - debug info collected via DeoptBundle from StackMap metadata
}

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
}

// Get function argument by local index
// For static methods: local[i] = function_arg[i+1] (skip dummy at function_arg[0])
// For non-static methods: local[i] = function_arg[i+1] (skip dummy at function_arg[0], this is at function_arg[1])
llvm::Argument* YuhuNormalEntryCacher::get_function_arg(int local_index) {
  llvm::Function* func = function()->function();
  llvm::Function::arg_iterator ai = func->arg_begin();

  // Both static and non-static methods skip dummy at function_arg[0]
  // Non-static: (dummy, this, arg0, ...)  → local[0] = this (arg 1)
  // Static:     (dummy, arg0, arg1, ...)  → local[0] = arg0 (arg 1)
  ai++;  // Skip dummy argument at arg 0

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
// Stack arguments are at [esp + stk_args_index * 8] in caller's frame
// arg_index is the argument index in AArch64 calling convention (0-based, including receiver for non-static)
llvm::Value* YuhuNormalEntryCacher::read_stack_arg(int stk_args_index) {
  // Read x29 (frame pointer) register
  llvm::Value* fp = builder()->CreateReadFramePointer();
  
  // Calculate stack argument address: x29 + 16 + stk_args_index * 8
  // Parameters >= 8 are placed by interpreter at:
  //   - 9th parameter (index=8): x29 + 16
  //   - 10th parameter (index=9): x29 + 24
  //   - nth parameter (index>=8): x29 + 16 + stk_args_index * 8
  int stack_offset = 16 + stk_args_index * wordSize;
  llvm::Value* stack_arg_addr = builder()->CreateGEP(
    YuhuType::intptr_type(),
    builder()->CreateIntToPtr(fp, llvm::PointerType::getUnqual(YuhuType::intptr_type())),
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

  // Skip padding slots (second half of long/double)
  if (value == NULL) {
    return;
  }

  // Only populate for real arguments
  if (local_slot_needs_read(index, value) && index < arg_size()) {
    llvm::Type* stack_ty = YuhuType::to_stackType(value->basic_type());
    llvm::Value* loaded = NULL;
    
    // Calculate the LLVM argument index by counting actual parameters (not slots)
    // For example, if we have: long_i, index, buf (static method)
    // Bytecode slots: 0-1(long_i), 2(index), 3(buf)
    // LLVM args: arg0=long_i, arg1=index, arg2=buf
    // When index=0, arg_index=0; when index=2, arg_index=1
    int arg_index = 0;
    ciSignature* sig = function()->target()->signature();
    bool is_static = function()->target()->is_static();
    
    // Count parameters before this slot to get the argument index
    int current_slot = is_static ? 0 : 1;  // Slot 0 is 'this' for non-static
    if (!is_static) {
      if (index == 0) {
        // This is the receiver (this)
        arg_index = 0;
      } else {
        // Count through parameters to find which argument this slot belongs to
        arg_index = 1;  // Start after receiver
        for (int i = 0; i < sig->count(); i++) {
          int slot_size = sig->type_at(i)->size();  // 2 for long/double, 1 for others
          if (current_slot < index && current_slot + slot_size > index) {
            // This slot belongs to parameter i
            break;
          }
          if (current_slot >= index) {
            break;
          }
          current_slot += slot_size;
          arg_index++;
        }
      }
    } else {
      // Static method: count from slot 0
      for (int i = 0; i < sig->count(); i++) {
        int slot_size = sig->type_at(i)->size();
        if (current_slot < index && current_slot + slot_size > index) {
          // This slot belongs to parameter i
          break;
        }
        if (current_slot >= index) {
          break;
        }
        current_slot += slot_size;
        arg_index++;
      }
    }

    // calculate number of int registers, number of float register and number of parameters in stack
    // 8 int registers are x1-x7,x0, and x0 is the 8th argument
    // 8 float registers are d0-d7
    uint int_args = is_static ? 0 : 1;
    uint fp_args = 0;
    uint stk_args = 0;
    bool is_current_arg_float = false;
    int arg_cnt = is_static ? arg_index : (arg_index - 1);

    for (int i = 0; i <= arg_cnt; ++i) {
        if (sig->type_at(i)->basic_type() == T_FLOAT || sig->type_at(i)->basic_type() == T_DOUBLE) {
            if (fp_args < 8) {
                fp_args++;
            } else {
                stk_args++;
            }
            if (i == arg_cnt) {
                is_current_arg_float = true;
            }
        } else {
            if (int_args < 8) {
                int_args++;
            } else {
                stk_args++;
            }
        }
    }

    if (!is_current_arg_float) {
        if (stk_args == 0) {
            // it means argument is not on stack, still in register
            if (int_args == 8) {
                // if it is just the 8th argument
                // Special handling for p7 (8th parameter): read from x22 register where it was saved
                loaded = builder()->CreateReadX22Register();
            } else {
                // Parameters 0-6: read from function arguments (x1-x7)
                llvm::Argument* arg = get_function_arg(arg_index);
                assert(arg != NULL, "function argument should exist");
                loaded = arg;
            }
        } else {
            loaded = read_stack_arg(stk_args - 1);
        }

        // Ensure type matches exactly (may need conversion for integers or pointers)
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
            } else if (stack_ty->isPointerTy() && loaded->getType()->isIntegerTy()) {
                // Convert i64 to ptr
                loaded = builder()->CreateIntToPtr(loaded, stack_ty, "arg_typed");
            } else if (stack_ty->isIntegerTy() && loaded->getType()->isPointerTy()) {
                // Convert ptr to i64
                loaded = builder()->CreatePtrToInt(loaded, stack_ty, "arg_typed");
            } else {
                // Fallback: try bitcast
                loaded = builder()->CreateBitCast(loaded, stack_ty, "arg_typed");
            }
        }
    } else {
        if (stk_args == 0) {
            // it means argument is not on stack, still in register
            llvm::Argument* arg = get_function_arg(arg_index);
            assert(arg != NULL, "function argument should exist");
            loaded = arg;
        } else {
            loaded = read_stack_arg(stk_args - 1);
        }

        if (loaded->getType() != stack_ty) {
            if (stack_ty->isFloatTy()) {
                // i64 → i32 → float (narrow then reinterpret)
                llvm::Value* trunc = builder()->CreateTrunc(loaded, builder()->getInt32Ty());
                loaded = builder()->CreateBitCast(trunc, stack_ty, "arg_typed");
            } else if (stack_ty->isDoubleTy()) {
                // i64 → double (same size, reinterpret)
                loaded = builder()->CreateBitCast(loaded, stack_ty, "arg_typed");
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

}

void YuhuCacher::process_stack_slot(int          index,
                                     YuhuValue** addr,
                                     int          offset) {
  YuhuValue *value = *addr;

  // Read the value from the frame if necessary
  if (stack_slot_needs_read(index, value)) {
    // If the value already has a valid LLVM value (e.g., a call return value
    // set by cache_after_Java_call), keep it as-is — do NOT reload from frame.
    // Reloading from frame would overwrite the actual call result with stale data.
    if (value->generic_value() != NULL) {
      // Already has a live LLVM value — no frame reload needed
      return;
    }
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
      YuhuType::oop_addrspace1_type(), // FIXED - monitor object is allocated in heap
      CreateAddressOfOSRBufEntry(src_offset + 1, YuhuType::oop_addrspace1_type())), // FIXED - monitor object is allocated in heap
    stack()->slot_addr(obj_offset, YuhuType::oop_addrspace1_type())); // FIXED - type should match
}

void YuhuCacher::process_oop_tmp_slot(Value** value, int offset) {
  // Cache the temporary oop
  if (*value)
    *value = read_value_from_frame(YuhuType::oop_addrspace1_type(), offset); // FIXED - oop tmp is allocated in heap
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
    // If the value already has a valid LLVM value, keep it as-is — do NOT reload from frame.
    // This prevents overwriting live values (e.g., call results or recently stored locals)
    // with stale data from the frame.
    if (value->generic_value() != NULL) {
      return;
    }
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

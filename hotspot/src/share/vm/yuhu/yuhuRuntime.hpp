/*
 * Copyright (c) 1999, 2012, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_YUHU_YUHURUNTIME_HPP
#define SHARE_VM_YUHU_YUHURUNTIME_HPP

#include "memory/allocation.hpp"
#include "runtime/thread.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/llvmValue.hpp"

// Runtime helper functions (defined in yuhuRuntime.cpp)
extern "C" jlong yuhu_resolve_static_field(Klass* klass, int field_offset, bool is_object_field, bool is_volatile);

class YuhuRuntime : public AllStatic {
  // VM calls
 public:
  static int find_exception_handler(JavaThread* thread,
                                    int*        indexes,
                                    int         num_indexes);

  static void monitorenter(JavaThread* thread, BasicObjectLock* lock);
  static void monitorexit(JavaThread* thread, BasicObjectLock* lock);

  static void new_instance(JavaThread* thread, int index);
  static void newarray(JavaThread* thread, BasicType type, int size);
  static void anewarray(JavaThread* thread, int index, int size);
  static void multianewarray(JavaThread* thread,
                             int         index,
                             int         ndims,
                             int*        dims);

  static void register_finalizer(JavaThread* thread, oop object);

  static void throw_ArithmeticException(JavaThread* thread,
                                        const char* file,
                                        int         line);
  static void throw_ArrayIndexOutOfBoundsException(JavaThread* thread,
                                                   const char* file,
                                                   int         line,
                                                   int         index);
  static void throw_ClassCastException(JavaThread* thread,
                                       const char* file,
                                       int         line);
  static void throw_NullPointerException(JavaThread* thread,
                                         const char* file,
                                         int         line);

  // Helpers for VM calls
 private:
  // For AArch64, use standard frame API instead of ZeroStack
  static frame last_frame(JavaThread *thread) {
    return thread->last_frame();
  }
  static Method* method(JavaThread *thread) {
    return last_frame(thread).interpreter_frame_method();
  }
  static address bcp(JavaThread *thread, int bci) {
    return method(thread)->code_base() + bci;
  }
  static int two_byte_index(JavaThread *thread, int bci) {
    return Bytes::get_Java_u2(bcp(thread, bci) + 1);
  }
  static intptr_t tos_at(JavaThread *thread, int offset) {
    // For AArch64, access expression stack through standard frame API
    frame f = last_frame(thread);
    intptr_t* tos = f.interpreter_frame_tos_address();
    return *(tos + offset);
  }

  // Non-VM calls
 public:
  static void dump(const char *name, intptr_t value);
  static bool is_subtype_of(Klass* check_klass, Klass* object_klass);
  static int uncommon_trap(JavaThread* thread, int trap_request);
  
  // Debug helper for stack overflow check
  static void debug_stack_overflow_check(JavaThread* thread,
                                         intptr_t current_sp,
                                         intptr_t new_sp,
                                         intptr_t stack_base,
                                         intptr_t stack_size,
                                         intptr_t stack_bottom,
                                         intptr_t min_stack);
};

#endif // SHARE_VM_YUHU_YUHURUNTIME_HPP

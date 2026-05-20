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

class ciMethod;

extern "C" void gc_safepoint_poll();

class YuhuRuntime : public AllStatic {
  // VM call stubs (RuntimeStub wrappers)
 private:
  static address _new_instance_stub;
  static address _newarray_stub;
  static address _anewarray_stub;
  static address _multianewarray_stub;
  static address _monitorenter_stub;
  static address _monitorexit_stub;
  static address _register_finalizer_stub;
  static address _find_exception_handler_stub;
  
  // Stub generation functions
  static address generate_vm_stub(const char* name, address C_function);
  
  // VM calls
 public:
  // Initialize all VM call stubs
  static void initialize_vm_stubs();
  
  // Getter functions for stub addresses
  static address new_instance_stub() { return _new_instance_stub; }
  static address newarray_stub() { return _newarray_stub; }
  static address anewarray_stub() { return _anewarray_stub; }
  static address multianewarray_stub() { return _multianewarray_stub; }
  static address monitorenter_stub() { return _monitorenter_stub; }
  static address monitorexit_stub() { return _monitorexit_stub; }
  static address register_finalizer_stub() { return _register_finalizer_stub; }
  static address find_exception_handler_stub() { return _find_exception_handler_stub; }
  
  // Generate static call stub for direct method calls
  static address generate_static_call_stub(ciMethod* target_method, 
                                           ciMethod* current_method);
  
  // Generate virtual call stub for virtual method calls
  // This stub performs vtable lookup at runtime and jumps to _from_compiled_entry
  static address generate_virtual_call_stub(ciMethod* target_method, 
                                            ciMethod* current_method, 
                                            int vtable_index);
  
  // Generate interface call stub for interface method calls
  // This stub performs itable lookup at runtime and jumps to _from_compiled_entry
  static address generate_interface_call_stub(ciMethod* target_method, 
                                              ciMethod* current_method);

  // NOTE (Option A refactor): VM call entry points no longer derive the
  // current Method* / cp index by walking the caller frame, because the
  // RuntimeStub wrapper makes the youngest frame a non-nmethod CodeBlob.
  // The JIT now passes the resolved Method* / Klass* / oop directly.
  static int find_exception_handler(JavaThread* thread,
                                    Method*     method,
                                    oop         exception,
                                    int*        indexes,
                                    int         num_indexes);

  static void monitorenter(JavaThread* thread, BasicObjectLock* lock);
  static void monitorexit(JavaThread* thread, BasicObjectLock* lock);

  static void new_instance(JavaThread* thread, Klass* klass);
  static void newarray(JavaThread* thread, BasicType type, int size);
  static void anewarray(JavaThread* thread, Klass* element_klass, int size);
  static void multianewarray(JavaThread* thread,
                             Klass*      array_klass,
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

  // NOTE (Option A refactor): the previous private helpers
  //   last_frame() / method() / bcp() / two_byte_index() / tos_at()
  // walked the caller frame to recover the Method* / cp index / TOS.
  // After introducing the VM-call RuntimeStub wrapper, the youngest
  // frame at C-entry is the stub (not the nmethod), so frame-introspection
  // would assert. All required metadata is now passed explicitly by the
  // JIT via embedded Klass*/Method*/oop constants, mirroring C1/C2.

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

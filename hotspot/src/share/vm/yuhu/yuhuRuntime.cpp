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

#include "precompiled.hpp"
#include "runtime/biasedLocking.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/thread.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/yuhuRuntime.hpp"
#ifdef TARGET_ARCH_zero
# include "stack_zero.inline.hpp"
#endif
// Note: AArch64 uses standard stack management, no ZeroStack needed

using namespace llvm;

JRT_ENTRY(int, YuhuRuntime::find_exception_handler(JavaThread* thread,
                                                    int*        indexes,
                                                    int         num_indexes))
  constantPoolHandle pool(thread, method(thread)->constants());
  KlassHandle exc_klass(thread, ((oop) tos_at(thread, 0))->klass());

  for (int i = 0; i < num_indexes; i++) {
    Klass* tmp = pool->klass_at(indexes[i], CHECK_0);
    KlassHandle chk_klass(thread, tmp);

    if (exc_klass() == chk_klass())
      return i;

    if (exc_klass()->is_subtype_of(chk_klass()))
      return i;
  }

  return -1;
JRT_END

JRT_ENTRY(void, YuhuRuntime::monitorenter(JavaThread*      thread,
                                           BasicObjectLock* lock))
  if (PrintBiasedLockingStatistics)
    Atomic::inc(BiasedLocking::slow_path_entry_count_addr());

  Handle object(thread, lock->obj());
  assert(Universe::heap()->is_in_reserved_or_null(object()), "should be");
  if (UseBiasedLocking) {
    // Retry fast entry if bias is revoked to avoid unnecessary inflation
    ObjectSynchronizer::fast_enter(object, lock->lock(), true, CHECK);
  } else {
    ObjectSynchronizer::slow_enter(object, lock->lock(), CHECK);
  }
  assert(Universe::heap()->is_in_reserved_or_null(lock->obj()), "should be");
JRT_END

JRT_ENTRY(void, YuhuRuntime::monitorexit(JavaThread*      thread,
                                          BasicObjectLock* lock))
  Handle object(thread, lock->obj());
  assert(Universe::heap()->is_in_reserved_or_null(object()), "should be");
  if (lock == NULL || object()->is_unlocked()) {
    THROW(vmSymbols::java_lang_IllegalMonitorStateException());
  }
  ObjectSynchronizer::slow_exit(object(), lock->lock(), thread);
JRT_END

JRT_ENTRY(void, YuhuRuntime::new_instance(JavaThread* thread, int index))
  Klass* k_oop = method(thread)->constants()->klass_at(index, CHECK);
  instanceKlassHandle klass(THREAD, k_oop);

  // Make sure we are not instantiating an abstract klass
  klass->check_valid_for_instantiation(true, CHECK);

  // Make sure klass is initialized
  klass->initialize(CHECK);

  // At this point the class may not be fully initialized
  // because of recursive initialization. If it is fully
  // initialized & has_finalized is not set, we rewrite
  // it into its fast version (Note: no locking is needed
  // here since this is an atomic byte write and can be
  // done more than once).
  //
  // Note: In case of classes with has_finalized we don't
  //       rewrite since that saves us an extra check in
  //       the fast version which then would call the
  //       slow version anyway (and do a call back into
  //       Java).
  //       If we have a breakpoint, then we don't rewrite
  //       because the _breakpoint bytecode would be lost.
  oop obj = klass->allocate_instance(CHECK);
  thread->set_vm_result(obj);
JRT_END

JRT_ENTRY(void, YuhuRuntime::newarray(JavaThread* thread,
                                       BasicType   type,
                                       int         size))
  oop obj = oopFactory::new_typeArray(type, size, CHECK);
  thread->set_vm_result(obj);
JRT_END

JRT_ENTRY(void, YuhuRuntime::anewarray(JavaThread* thread,
                                        int         index,
                                        int         size))
  Klass* klass = method(thread)->constants()->klass_at(index, CHECK);
  objArrayOop obj = oopFactory::new_objArray(klass, size, CHECK);
  thread->set_vm_result(obj);
JRT_END

JRT_ENTRY(void, YuhuRuntime::multianewarray(JavaThread* thread,
                                             int         index,
                                             int         ndims,
                                             int*        dims))
  Klass* klass = method(thread)->constants()->klass_at(index, CHECK);
  oop obj = ArrayKlass::cast(klass)->multi_allocate(ndims, dims, CHECK);
  thread->set_vm_result(obj);
JRT_END

JRT_ENTRY(void, YuhuRuntime::register_finalizer(JavaThread* thread,
                                                 oop         object))
  assert(object->is_oop(), "should be");
  assert(object->klass()->has_finalizer(), "should have");
  InstanceKlass::register_finalizer(instanceOop(object), CHECK);
JRT_END

JRT_ENTRY(void, YuhuRuntime::throw_ArithmeticException(JavaThread* thread,
                                                        const char* file,
                                                        int         line))
  Exceptions::_throw_msg(
    thread, file, line,
    vmSymbols::java_lang_ArithmeticException(),
    "");
JRT_END

JRT_ENTRY(void, YuhuRuntime::throw_ArrayIndexOutOfBoundsException(
                                                     JavaThread* thread,
                                                     const char* file,
                                                     int         line,
                                                     int         index))
  char msg[jintAsStringSize];
  snprintf(msg, sizeof(msg), "%d", index);
  Exceptions::_throw_msg(
    thread, file, line,
    vmSymbols::java_lang_ArrayIndexOutOfBoundsException(),
    msg);
JRT_END

JRT_ENTRY(void, YuhuRuntime::throw_ClassCastException(JavaThread* thread,
                                                       const char* file,
                                                       int         line))
  Exceptions::_throw_msg(
    thread, file, line,
    vmSymbols::java_lang_ClassCastException(),
    "");
JRT_END

JRT_ENTRY(void, YuhuRuntime::throw_NullPointerException(JavaThread* thread,
                                                         const char* file,
                                                         int         line))
  Exceptions::_throw_msg(
    thread, file, line,
    vmSymbols::java_lang_NullPointerException(),
    "");
JRT_END

// Non-VM calls
// Nothing in these must ever GC!

void YuhuRuntime::dump(const char *name, intptr_t value) {
  oop valueOop = (oop) value;
  tty->print("%s = ", name);
  if (valueOop->is_oop(true))
    valueOop->print_on(tty);
  else if (value >= ' ' && value <= '~')
    tty->print("'%c' (%d)", value, value);
  else
    tty->print("%p", value);
  tty->print_cr("");
}

bool YuhuRuntime::is_subtype_of(Klass* check_klass, Klass* object_klass) {
  return object_klass->is_subtype_of(check_klass);
}

int YuhuRuntime::uncommon_trap(JavaThread* thread, int trap_request) {
  Thread *THREAD = thread;

  // TODO: AArch64 implementation
  // For AArch64, deoptimization uses standard frame API, not ZeroStack
  return 0;
}

void YuhuRuntime::debug_stack_overflow_check(JavaThread* thread,
                                             intptr_t current_sp,
                                             intptr_t new_sp,
                                             intptr_t stack_base,
                                             intptr_t stack_size,
                                             intptr_t stack_bottom,
                                             intptr_t min_stack) {
  tty->print_cr("=== Yuhu Stack Overflow Check Debug ===");
  tty->print_cr("Thread: %p", thread);
  tty->print_cr("Current SP (from frameaddress): 0x%lx", (unsigned long)current_sp);
  tty->print_cr("New SP (after frame allocation): 0x%lx", (unsigned long)new_sp);
  tty->print_cr("Stack base: 0x%lx", (unsigned long)stack_base);
  tty->print_cr("Stack size: 0x%lx (%lu bytes)", (unsigned long)stack_size, (unsigned long)stack_size);
  tty->print_cr("Stack bottom (base - size): 0x%lx", (unsigned long)stack_bottom);
  tty->print_cr("Min stack (bottom + shadow): 0x%lx", (unsigned long)min_stack);
  tty->print_cr("StackShadowPages: %d, page_size: %d", StackShadowPages, os::vm_page_size());
  tty->print_cr("Shadow size: %lu bytes", (unsigned long)(StackShadowPages * os::vm_page_size()));
  
  intptr_t available_stack = current_sp - stack_bottom;
  intptr_t required_stack = stack_base - new_sp;
  tty->print_cr("Available stack space: 0x%lx (%lu bytes)", (unsigned long)available_stack, (unsigned long)available_stack);
  tty->print_cr("Required stack space: 0x%lx (%lu bytes)", (unsigned long)required_stack, (unsigned long)required_stack);
  tty->print_cr("New SP < Min Stack? %s (0x%lx < 0x%lx)", 
                (new_sp < min_stack) ? "YES - OVERFLOW!" : "NO - OK",
                (unsigned long)new_sp, (unsigned long)min_stack);
  tty->print_cr("======================================");
}

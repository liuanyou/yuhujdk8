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
#include "ci/ciMethod.hpp"
#include "code/codeBlob.hpp"
#include "runtime/biasedLocking.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/thread.hpp"
#include "runtime/frame.hpp"
#include "runtime/registerMap.hpp"
#include "yuhu/yuhuRuntime.hpp"
#include "yuhu/yuhu_globals.hpp"
#ifdef TARGET_ARCH_aarch64
#include "asm/yuhu/yuhu_macroAssembler.hpp"
#endif
#ifdef TARGET_ARCH_zero
# include "stack_zero.inline.hpp"
#endif
// Note: AArch64 uses standard stack management, no ZeroStack needed

// Define _JNI_IMPLEMENTATION_ to get JNIEXPORT visibility (not JNIIMPORT)
#define _JNI_IMPLEMENTATION_
#include "prims/jni.h"

using namespace llvm;

JRT_ENTRY(int, YuhuRuntime::find_exception_handler(JavaThread* thread,
                                                    Method*     caller_method,
                                                    oop         exception,
                                                    int*        indexes,
                                                    int         num_indexes))
  // Option A: caller passes Method* and the exception oop explicitly.
  // The constant pool comes from the caller Method* (the JIT-compiled
  // Yuhu method that threw / re-dispatched the exception).
  constantPoolHandle pool(thread, caller_method->constants());
  KlassHandle exc_klass(thread, exception->klass());

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

JRT_ENTRY(void, YuhuRuntime::new_instance(JavaThread* thread, Klass* k_oop))
  // Option A: JIT passes resolved Klass* directly (embedded as a
  // metadata-relocated constant in the nmethod). No frame walk needed.
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
                                        Klass*      klass,
                                        int         size))
  // Option A: JIT passes the (already resolved) element Klass* directly.
  objArrayOop obj = oopFactory::new_objArray(klass, size, CHECK);
  thread->set_vm_result(obj);
JRT_END

JRT_ENTRY(void, YuhuRuntime::multianewarray(JavaThread* thread,
                                             Klass*      klass,
                                             int         ndims,
                                             int*        dims))
  // Option A: JIT passes the (already resolved) array Klass* directly.
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

extern "C" void gc_safepoint_poll() {
    // just a placeholder function
}

extern "C" void handle_deoptimization() {
    // just a placeholder function
}

extern "C" void go_unwind() {
    // just a placeholder function
}

// ============================================================================
// Java Method Call Stubs (Activity 066)
// ============================================================================
// These stubs are used when Yuhu-compiled code calls Java methods.
// They save/restore x19 (which Yuhu uses but callee may clobber) and set up
// proper frame metadata for GC stack walking.

#ifdef TARGET_ARCH_aarch64

// Generate static call stub for direct method calls
address YuhuRuntime::generate_static_call_stub(ciMethod* target_method, 
                                                ciMethod* current_method) {
  ResourceMark rm;
  
  const int stub_size = 64;
  CodeBuffer cb("yuhu_static_call_stub", stub_size, stub_size);
  YuhuMacroAssembler masm(&cb);

  address begin = masm.current_pc();
  
  // Get the Method* address (target method)
  Method* method_ptr = target_method->get_Method();
  
  // Frame layout:
  // [higher addresses]
  // +------------------+
  // | saved FP (x29)   | <- FP points here
  // +------------------+
  // | saved LR (x30)   |
  // +------------------+
  // | saved x19        |
  // +------------------+
  // | argument area    | <- SP points here (16-byte aligned)
  // [lower addresses]
  
  // Prologue: save FP, LR, x19
  masm.write_inst("sub sp, sp, #32");
  masm.write_inst("stp x29, x30, [sp, #16]");
  masm.write_inst("stp xzr, x19, [sp, #0]");
  masm.write_inst("add x29, sp, #16");
  
  // Get _from_compiled_entry from Method*
  // Use x9 as temporary register
  // Load Method* into x9
  int metadata_index = cb.oop_recorder()->allocate_metadata_index(method_ptr);
  RelocationHolder rspec = metadata_Relocation::spec(metadata_index);
  address pc = masm.current_pc();
  cb.relocate(pc, rspec);
  masm.write_insts_lea(YuhuMacroAssembler::x9, YuhuAddress((address)method_ptr, relocInfo::metadata_type));
  // Move Method* to x12 for c2i adapter
  masm.write_inst_mov_reg(YuhuMacroAssembler::x12, YuhuMacroAssembler::x9);
  // Load _from_compiled_entry
  masm.write_inst_ldr(YuhuMacroAssembler::x9, 
                      YuhuAddress(YuhuMacroAssembler::x9, Method::from_compiled_offset()));
  
  // Call target
  masm.write_inst_blr(YuhuMacroAssembler::x9);
  
  // Epilogue: restore x19, FP, LR
  masm.write_inst("ldp x29, x30, [sp, #16]");
  masm.write_inst("ldp xzr, x19, [sp]");
  masm.write_inst("add sp, sp, #32");
  
  // Return
  masm.write_inst("ret");

  address exception_handler_begin = masm.current_pc();

  if (!_static_call_stub_exception_handler_offset) {
      _static_call_stub_exception_handler_offset = (int)(exception_handler_begin - begin);
  } else {
      assert(_static_call_stub_exception_handler_offset == (int)(exception_handler_begin - begin), "should be always the same offset");
  }

    // pop frame
    masm.write_inst("ldp x29, x30, [sp, #16]");
    masm.write_inst("ldp xzr, x19, [sp]");
    masm.write_inst("add sp, sp, #32");
    // get caller's exception handler
    masm.write_inst("stp x0, x30, [sp, #-16]!");
    masm.write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address,
                                                         SharedRuntime::exception_handler_for_return_address),
                                        YuhuMacroAssembler::x28, YuhuMacroAssembler::x30);
    masm.write_inst_mov_reg(YuhuMacroAssembler::x1, YuhuMacroAssembler::x0);
    masm.write_inst_ldp(YuhuMacroAssembler::x0, YuhuMacroAssembler::x30, YuhuPost(YuhuMacroAssembler::sp, 16));
    masm.write_inst_mov_reg(YuhuMacroAssembler::x3, YuhuMacroAssembler::x30);
    masm.write_inst_br(YuhuMacroAssembler::x1);

  masm.flush();
  
  // Create RuntimeStub
  // Frame size: 3 words (FP, LR, x19) + 1 word padding for alignment = 4 words
  int frame_size_in_words = 4;

  RuntimeStub* stub = RuntimeStub::new_runtime_stub(
      "yuhu_static_call_stub",
      &cb,
      CodeOffsets::frame_never_safe,
      frame_size_in_words,
      NULL,  // no oops saved
      false  // caller_must_gc_arguments
  );
  
  address stub_addr = stub->entry_point();
  
  if (YuhuTraceInstalls) {
    tty->print_cr("Yuhu: Generated static call RuntimeStub at " PTR_FORMAT " for target method %s",
                  p2i(stub_addr), target_method->name()->as_utf8());
  }
  
  return stub_addr;
}

// Generate virtual call stub for virtual method calls
address YuhuRuntime::generate_virtual_call_stub(ciMethod* target_method, 
                                                 ciMethod* current_method, 
                                                 int vtable_index) {
  ResourceMark rm;
  
  const int stub_size = 64;
  CodeBuffer buffer("yuhu_virtual_call_stub", stub_size, stub_size);
  YuhuMacroAssembler masm(&buffer);

  address begin = masm.current_pc();
  
  // Frame layout: same as static call stub
  
  // Prologue: save FP, LR, x19
    masm.write_inst("sub sp, sp, #32");
    masm.write_inst("stp x29, x30, [sp, #16]");
    masm.write_inst("stp xzr, x19, [sp, #0]");
    masm.write_inst("add x29, sp, #16");
  
  // Input: x1 = receiver object (per Yuhu calling convention)
  
  // Step 1: Load klass from receiver object
  masm.write_insts_load_klass(YuhuMacroAssembler::x9, YuhuMacroAssembler::x1);
  
  // Step 2: Load Method* from vtable[vtable_index]
  int vtable_offset = InstanceKlass::vtable_start_offset() * wordSize + 
                      vtable_index * vtableEntry::size() * wordSize;
  masm.write_inst_ldr(YuhuMacroAssembler::x9, 
                      YuhuAddress(YuhuMacroAssembler::x9, vtable_offset));
  
  // Step 3: Move Method* to x12 for c2i adapter
  masm.write_inst_mov_reg(YuhuMacroAssembler::x12, YuhuMacroAssembler::x9);
  
  // Step 4: Load _from_compiled_entry from Method*
  masm.write_inst_ldr(YuhuMacroAssembler::x9, 
                      YuhuAddress(YuhuMacroAssembler::x9, Method::from_compiled_offset()));
  
  // Step 5: Jump to compiled entry
  masm.write_inst_blr(YuhuMacroAssembler::x9);
  
  // Epilogue: restore x19, FP, LR
    masm.write_inst("ldp x29, x30, [sp, #16]");
    masm.write_inst("ldp xzr, x19, [sp]");
    masm.write_inst("add sp, sp, #32");

    // Return
    masm.write_inst("ret");

    address exception_handler_begin = masm.current_pc();

    if (!_virtual_call_stub_exception_handler_offset) {
        _virtual_call_stub_exception_handler_offset = (int)(exception_handler_begin - begin);
    } else {
        assert(_virtual_call_stub_exception_handler_offset == (int)(exception_handler_begin - begin), "should be always the same offset");
    }

    // pop frame
    masm.write_inst("ldp x29, x30, [sp, #16]");
    masm.write_inst("ldp xzr, x19, [sp]");
    masm.write_inst("add sp, sp, #32");
    // get caller's exception handler
    masm.write_inst("stp x0, x30, [sp, #-16]!");
    masm.write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address,
                                                         SharedRuntime::exception_handler_for_return_address),
                                        YuhuMacroAssembler::x28, YuhuMacroAssembler::x30);
    masm.write_inst_mov_reg(YuhuMacroAssembler::x1, YuhuMacroAssembler::x0);
    masm.write_inst_ldp(YuhuMacroAssembler::x0, YuhuMacroAssembler::x30, YuhuPost(YuhuMacroAssembler::sp, 16));
    masm.write_inst_mov_reg(YuhuMacroAssembler::x3, YuhuMacroAssembler::x30);
    masm.write_inst_br(YuhuMacroAssembler::x1);

    masm.flush();
  
  // Create RuntimeStub
  int frame_size_in_words = 4;
  
  RuntimeStub* stub = RuntimeStub::new_runtime_stub(
      "yuhu_virtual_call_stub",
      &buffer,
      CodeOffsets::frame_never_safe,
      frame_size_in_words,
      NULL,  // no oops saved
      false
  );
  
  address stub_addr = stub->entry_point();
  
  if (YuhuTraceInstalls) {
    tty->print_cr("Yuhu: Generated virtual call RuntimeStub at " PTR_FORMAT " for target method %s (vtable_index=%d)",
                  p2i(stub_addr), target_method->name()->as_utf8(), vtable_index);
  }
  
  return stub_addr;
}

// Generate interface call stub for interface method calls
address YuhuRuntime::generate_interface_call_stub(ciMethod* target_method, 
                                                   ciMethod* current_method) {
  ResourceMark rm;
  
  const int stub_size = 128;
  CodeBuffer cb("yuhu_interface_call_stub", stub_size, stub_size);
  YuhuMacroAssembler masm(&cb);

  address begin = masm.current_pc();
  
  ciInstanceKlass* ci_interface_klass = target_method->holder();
  InstanceKlass* interface_klass = ci_interface_klass->get_instanceKlass();
  int itable_index = target_method->itable_index();
  
  // Frame layout: same as static call stub
  
  // Prologue: save FP, LR, x19
    masm.write_inst("sub sp, sp, #32");
    masm.write_inst("stp x29, x30, [sp, #16]");
    masm.write_inst("stp xzr, x19, [sp, #0]");
    masm.write_inst("add x29, sp, #16");
  
  // Input: x1 = receiver object
  
  // Step 1: locate the start of itable
    int metadata_index = cb.oop_recorder()->allocate_metadata_index(interface_klass);
    RelocationHolder rspec = metadata_Relocation::spec(metadata_index);
    address pc = masm.current_pc();
    cb.relocate(pc, rspec);
  masm.write_insts_lea(YuhuMacroAssembler::x12, YuhuAddress((address)interface_klass, relocInfo::metadata_type));
  masm.write_insts_load_klass(YuhuMacroAssembler::x11, YuhuMacroAssembler::x1);
  masm.write_inst_ldr(YuhuMacroAssembler::w8,
                      YuhuAddress(YuhuMacroAssembler::x11, InstanceKlass::vtable_length_offset() * wordSize));
  masm.write_insts_lea(YuhuMacroAssembler::x9, 
                       YuhuAddress(YuhuMacroAssembler::x11, InstanceKlass::vtable_start_offset() * wordSize));
  masm.write_insts_lea(YuhuMacroAssembler::x8, 
                       YuhuAddress(YuhuMacroAssembler::x9, YuhuMacroAssembler::x8, YuhuAddress::lsl(3)));
  
  // Step 2: search logic
  YuhuLabel search, found_method, L_no_such_interface;
  
  for (int peel = 1; peel >= 0; peel--) {
    masm.write_inst_ldr(YuhuMacroAssembler::x9, 
                        YuhuAddress(YuhuMacroAssembler::x8, itableOffsetEntry::interface_offset_in_bytes()));
    masm.write_inst_regs("cmp %s, %s", YuhuMacroAssembler::x12, YuhuMacroAssembler::x9);
    
    if (peel) {
      masm.write_inst_b(YuhuMacroAssembler::eq, found_method);
    } else {
      masm.write_inst_b(YuhuMacroAssembler::ne, search);
    }
    
    if (!peel) break;
    
    masm.pin_label(search);
    masm.write_inst_cbz(YuhuMacroAssembler::x9, L_no_such_interface);
    masm.write_inst("add %s, %s, #%d", 
                    YuhuMacroAssembler::x8, YuhuMacroAssembler::x8, 
                    itableOffsetEntry::size() * wordSize);
  }
  
  // Step 3: load method instructions
  masm.pin_label(found_method);
  masm.write_inst_ldr(YuhuMacroAssembler::w8,
                      YuhuAddress(YuhuMacroAssembler::x8, itableOffsetEntry::offset_offset_in_bytes()));
  masm.write_insts_lea(YuhuMacroAssembler::x9, 
                       YuhuAddress(YuhuMacroAssembler::x11, YuhuMacroAssembler::w8, YuhuAddress::uxtw(0)));
  masm.write_inst("add %s, %s, #%d", 
                  YuhuMacroAssembler::x9, YuhuMacroAssembler::x9, 
                  itable_index * sizeof(itableMethodEntry));
  masm.write_inst_ldr(YuhuMacroAssembler::x9,
                      YuhuAddress(YuhuMacroAssembler::x9, itableMethodEntry::method_offset_in_bytes()));
    masm.write_inst_mov_reg(YuhuMacroAssembler::x12, YuhuMacroAssembler::x9);
  
  // Step 4: Load _from_compiled_entry from Method*
  masm.write_inst_ldr(YuhuMacroAssembler::x9,
                      YuhuAddress(YuhuMacroAssembler::x9, Method::from_compiled_offset()));
  
  // Step 5: Jump to compiled entry
  masm.write_inst_blr(YuhuMacroAssembler::x9);

    // Epilogue: restore x19, FP, LR (only reached if interface not found - should not return)
    masm.write_inst("ldp x29, x30, [sp, #16]");
    masm.write_inst("ldp xzr, x19, [sp]");
    masm.write_inst("add sp, sp, #32");

    // Return
    masm.write_inst("ret");
  
  masm.pin_label(L_no_such_interface);
  masm.write_insts_far_jump(YuhuRuntimeAddress(StubRoutines::throw_IncompatibleClassChangeError_entry()));
  masm.write_insts_stop("should not reach here");

    address exception_handler_begin = masm.current_pc();

    if (!_interface_call_stub_exception_handler_offset) {
        _interface_call_stub_exception_handler_offset = (int)(exception_handler_begin - begin);
    } else {
        assert(_interface_call_stub_exception_handler_offset == (int)(exception_handler_begin - begin), "should be always the same offset");
    }

    // pop frame
    masm.write_inst("ldp x29, x30, [sp, #16]");
    masm.write_inst("ldp xzr, x19, [sp]");
    masm.write_inst("add sp, sp, #32");
    // get caller's exception handler
    masm.write_inst("stp x0, x30, [sp, #-16]!");
    masm.write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address,
                                                         SharedRuntime::exception_handler_for_return_address),
                                        YuhuMacroAssembler::x28, YuhuMacroAssembler::x30);
    masm.write_inst_mov_reg(YuhuMacroAssembler::x1, YuhuMacroAssembler::x0);
    masm.write_inst_ldp(YuhuMacroAssembler::x0, YuhuMacroAssembler::x30, YuhuPost(YuhuMacroAssembler::sp, 16));
    masm.write_inst_mov_reg(YuhuMacroAssembler::x3, YuhuMacroAssembler::x30);
    masm.write_inst_br(YuhuMacroAssembler::x1);

    masm.flush();
  
  // Create RuntimeStub
  int frame_size_in_words = 4;
  
  RuntimeStub* stub = RuntimeStub::new_runtime_stub(
      "yuhu_interface_call_stub",
      &cb,
      CodeOffsets::frame_never_safe,
      frame_size_in_words,
      NULL,  // no oops saved
      false
  );
  
  address stub_addr = stub->entry_point();
  
  if (YuhuTraceInstalls) {
    tty->print_cr("Yuhu: Generated interface call RuntimeStub at " PTR_FORMAT " for target method %s",
                  p2i(stub_addr), target_method->name()->as_utf8());
  }
  
  return stub_addr;
}

#endif // TARGET_ARCH_aarch64

// ============================================================================
// VM Call RuntimeStubs (Activity 068)
// ============================================================================
// These stubs wrap VM function calls to provide proper frame boundaries for GC.
// They prevent the preserve_callee_argument_oops() assertion failure by ensuring
// the youngest frame during GC is a RuntimeStub (which has a no-op implementation)
// rather than an nmethod frame at a non-invoke BCI.

#ifdef TARGET_ARCH_aarch64

// Initialize static stub addresses
address YuhuRuntime::_new_instance_stub = NULL;
address YuhuRuntime::_newarray_stub = NULL;
address YuhuRuntime::_anewarray_stub = NULL;
address YuhuRuntime::_multianewarray_stub = NULL;
address YuhuRuntime::_monitorenter_stub = NULL;
address YuhuRuntime::_monitorexit_stub = NULL;
address YuhuRuntime::_register_finalizer_stub = NULL;
address YuhuRuntime::_find_exception_handler_stub = NULL;
address YuhuRuntime::_is_subtype_of_stub = NULL;
address YuhuRuntime::_current_time_millis_stub = NULL;
address YuhuRuntime::_handle_deoptimization_stub = NULL;

volatile int YuhuRuntime::_static_call_stub_exception_handler_offset = 0;
volatile int YuhuRuntime::_virtual_call_stub_exception_handler_offset = 0;
volatile int YuhuRuntime::_interface_call_stub_exception_handler_offset = 0;

// Initialize all VM call stubs
void YuhuRuntime::initialize_vm_stubs() {
  _new_instance_stub = generate_vm_stub("yuhu_new_instance_stub", (address) YuhuRuntime::new_instance);
  _newarray_stub = generate_vm_stub("yuhu_newarray_stub", (address) YuhuRuntime::newarray);
  _anewarray_stub = generate_vm_stub("yuhu_anewarray_stub", (address) YuhuRuntime::anewarray);
  _multianewarray_stub = generate_vm_stub("yuhu_multianewarray_stub", (address) YuhuRuntime::multianewarray);
  _monitorenter_stub = generate_vm_stub("yuhu_monitorenter_stub", (address) YuhuRuntime::monitorenter);
  _monitorexit_stub = generate_vm_stub("yuhu_monitorexit_stub", (address) YuhuRuntime::monitorexit);
  _register_finalizer_stub = generate_vm_stub("yuhu_register_finalizer_stub", (address) YuhuRuntime::register_finalizer);
  _find_exception_handler_stub = generate_vm_stub("yuhu_find_exception_handler_stub", (address) YuhuRuntime::find_exception_handler);
  _is_subtype_of_stub = generate_vm_stub("yuhu_is_subtype_of_stub", (address) YuhuRuntime::is_subtype_of);
  _current_time_millis_stub = generate_vm_stub("yuhu_current_time_millis_stub", (address) os::javaTimeMillis);

  _handle_deoptimization_stub = generate_handle_deoptimization_stub();
  
  if (YuhuTraceInstalls) {
    tty->print_cr("Yuhu: VM call stubs initialized");
    tty->print_cr("  new_instance_stub:           " PTR_FORMAT, p2i(_new_instance_stub));
    tty->print_cr("  newarray_stub:               " PTR_FORMAT, p2i(_newarray_stub));
    tty->print_cr("  anewarray_stub:              " PTR_FORMAT, p2i(_anewarray_stub));
    tty->print_cr("  multianewarray_stub:         " PTR_FORMAT, p2i(_multianewarray_stub));
    tty->print_cr("  monitorenter_stub:           " PTR_FORMAT, p2i(_monitorenter_stub));
    tty->print_cr("  monitorexit_stub:            " PTR_FORMAT, p2i(_monitorexit_stub));
    tty->print_cr("  register_finalizer_stub:     " PTR_FORMAT, p2i(_register_finalizer_stub));
    tty->print_cr("  find_exception_handler_stub: " PTR_FORMAT, p2i(_find_exception_handler_stub));
    tty->print_cr("  is_subtype_of_stub:          " PTR_FORMAT, p2i(_is_subtype_of_stub));
    tty->print_cr("  current_time_millis_stub:    " PTR_FORMAT, p2i(_current_time_millis_stub));
    tty->print_cr("  handle_deoptimization_stub:  " PTR_FORMAT, p2i(_handle_deoptimization_stub));
  }
}

address YuhuRuntime::generate_vm_stub(const char* name, address C_function) {
    ResourceMark rm;
    const int stub_size = 128;
    CodeBuffer cb(name, stub_size, stub_size);
    YuhuMacroAssembler masm(&cb);

    masm.write_inst("sub sp, sp, #16");
    masm.write_inst("stp x29, x30, [sp]");
    masm.write_inst("mov x29, sp");

    masm.write_insts_set_last_java_frame(YuhuMacroAssembler::sp, YuhuMacroAssembler::noreg, YuhuMacroAssembler::noreg, YuhuMacroAssembler::x9);

    YuhuLabel label;
    masm.write_inst_adr(YuhuMacroAssembler::x9, label);
    masm.write_inst("stp xzr, x9, [sp, #-16]!");

    masm.write_insts_lea(YuhuMacroAssembler::x8, YuhuExternalAddress(C_function));
    masm.write_inst_blr(YuhuMacroAssembler::x8);
    masm.pin_label(label);

    masm.write_inst("add sp, sp, #16");

    masm.write_insts_reset_last_java_frame(true);

    masm.write_inst("ldp x29, x30, [sp]");
    masm.write_inst("add sp, sp, #16");
    masm.write_inst("ret");
    masm.flush();

    // Create RuntimeStub
    // Frame size: 2 words (FP + LR)
    int frame_size_in_words = 2;

    RuntimeStub* stub = RuntimeStub::new_runtime_stub(
            name,
            &cb,
            CodeOffsets::frame_never_safe,
            frame_size_in_words,
            NULL,  // no oopmap needed - stub has no live oops
            false  // caller_must_gc_arguments
    );

    return stub->entry_point();
}

address YuhuRuntime::generate_handle_deoptimization_stub() {
    ResourceMark rm;
    const int stub_size = 64;
    const char* name = "yuhu_handle_deoptimization_stub";
    CodeBuffer cb(name, stub_size, stub_size);
    YuhuMacroAssembler masm(&cb);

    int trap_request = Deoptimization::make_trap_request(
            Deoptimization::Reason_unloaded,
            Deoptimization::Action_reinterpret);
    masm.write_insts_mov_imm32(YuhuMacroAssembler::x1, trap_request);
    masm.write_insts_far_jump(YuhuRuntimeAddress(SharedRuntime::uncommon_trap_blob()->entry_point()));
    masm.flush();

    int frame_size_in_words = 0;

    RuntimeStub* stub = RuntimeStub::new_runtime_stub(
            name,
            &cb,
            CodeOffsets::frame_never_safe,
            frame_size_in_words,
            NULL,  // no oopmap needed - stub has no live oops
            false  // caller_must_gc_arguments
    );

    return stub->entry_point();
}

#endif // TARGET_ARCH_aarch64

// Check if an address belongs to a Yuhu RuntimeStub
// All Yuhu RuntimeStubs have names starting with "yuhu_"
bool YuhuRuntime::is_yuhu_call_stub(address addr) {
  CodeBlob* blob = CodeCache::find_blob(addr);
  if (blob == NULL || !blob->is_runtime_stub()) {
    return false;
  }
  
  // Check if the stub name starts with "yuhu_"
  const char* name = blob->name();
  if (name == NULL) {
    return false;
  }
  
  return strcmp(name, "yuhu_static_call_stub") == 0 ||
            strcmp(name, "yuhu_virtual_call_stub") == 0 ||
            strcmp(name, "yuhu_interface_call_stub") == 0;
}

address YuhuRuntime::exception_begin(address addr) {
    CodeBlob* blob = CodeCache::find_blob(addr);
    assert(blob->is_runtime_stub(), "must be runtime stub");
    const char* name = blob->name();

    if (strcmp(name, "yuhu_static_call_stub") == 0) {
        return blob->code_begin() + _static_call_stub_exception_handler_offset;
    } else if (strcmp(name, "yuhu_virtual_call_stub") == 0) {
        return blob->code_begin() + _virtual_call_stub_exception_handler_offset;
    } else if (strcmp(name, "yuhu_interface_call_stub") == 0) {
        return blob->code_begin() + _interface_call_stub_exception_handler_offset;
    }
    ShouldNotReachHere();
    return NULL;
}
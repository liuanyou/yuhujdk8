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

#include <gc_implementation/parallelScavenge/vmPSOperations.hpp>
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
#include "interpreter/linkResolver.hpp"
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
#include "yuhuStack.hpp"

using namespace llvm;

// File-based logging for debugging
static FILE* yuhu_stack_map_log = NULL;
static void yuhu_stack_map_log_init() {
    if (yuhu_stack_map_log == NULL) {
        yuhu_stack_map_log = fopen(YuhuStackMapFile, "a");
    }
}
#define YUHU_STACK_MAP_LOG(fmt, ...) \
    do { \
        yuhu_stack_map_log_init(); \
        if (yuhu_stack_map_log) { \
            fprintf(yuhu_stack_map_log, fmt "\n", ##__VA_ARGS__); \
            fflush(yuhu_stack_map_log); \
        } \
    } while(0)

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

JRT_ENTRY(void, YuhuRuntime::register_finalizer(JavaThread* thread,
                                                 oop         object))
  assert(object->is_oop(), "should be");
  assert(object->klass()->has_finalizer(), "should have");
  InstanceKlass::register_finalizer(instanceOop(object), CHECK);
JRT_END

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

// ------
// current_time_millis - os::javaTimeMillis
// ------

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

// Resolve invokedynamic call site.
// Calls InterpreterRuntime::resolve_invokedynamic to run the bootstrap method
// and populate the CP cache entry, then returns the resolved target's
// verified_code_entry. Stores the resolved Method* in thread->vm_result_2()
// for the stub to load into x12 (rmethod) for the c2i adapter.
JRT_ENTRY(address, YuhuRuntime::resolve_dynamic_call(JavaThread* thread,
                                                      Method* target_method,
                                                      Klass* current_klass))
  // The target_method is the resolved method from the constant pool.
  // We need to resolve the invokedynamic call site which will run the
  // bootstrap method if not already resolved, populating the CP cache.
  //
  // Since we can't easily get the caller's bcp from here (the caller frame
  // is the stub, not the nmethod), we use the target_method directly.
  // The CP cache should already be resolved by the time we get here
  // (the interpreter or runtime resolves it on first encounter).
  //
  // For invokedynamic, the target_method's _from_compiled_entry is the
  // call target. If it's a method handle intrinsic or compiled lambda form,
  // we need to call through the method handle invocation mechanism.

  methodHandle sel_method(THREAD, target_method);
  assert(sel_method.not_null(), "resolved method should not be null");

  // Store Method* for the stub to retrieve and set in x12 for c2i adapter
  thread->set_vm_result_2(sel_method());
  return sel_method->verified_code_entry();
JRT_END

// Resolve interface call dynamically.
// Used for interface methods with itable_index() < 0 (e.g. Object methods
// re-declared in interfaces like Set.equals()). These methods cannot use
// itable dispatch and require runtime resolution via LinkResolver.
JRT_ENTRY(address, YuhuRuntime::resolve_interface_call(JavaThread* thread,
                                                        oop recv_oop,
                                                        Klass* interface_klass,
                                                        Method* target_method,
                                                        Klass* current_klass))
  Handle recv(THREAD, recv_oop);
  KlassHandle recv_klass(THREAD, recv_oop->klass());
  KlassHandle resolved_klass(THREAD, interface_klass);
  KlassHandle h_current_klass(THREAD, current_klass);

  CallInfo call_info;
    Symbol* method_name = target_method->name();
    Symbol* method_signature = target_method->signature();
  LinkResolver::resolve_interface_call(call_info, recv, recv_klass, resolved_klass,
                                        method_name, method_signature, h_current_klass,
                                        true, true, CHECK_NULL);

  methodHandle sel_method = call_info.selected_method();
  assert(sel_method.not_null(), "resolved method should not be null");
  // Store Method* for the stub to retrieve and set in x12 for c2i adapter
  thread->set_vm_result_2(sel_method());
  return sel_method->verified_code_entry();
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

extern "C" void gc_safepoint_poll(JavaThread* thread) {
    if (SafepointSynchronize::do_call_back()) {
        SafepointSynchronize::block(thread);
    }

//    {
//        Thread::WXWriteFromExecSetter __wx_write;
//        ThreadInVMfromJava __tiv(thread);
//
//        unsigned int gc_count = Universe::heap()->total_collections();
//        unsigned int full_gc_count = Universe::heap()->total_full_collections();
//        VM_ParallelGCSystemGC op(gc_count, full_gc_count, GCCause::_java_lang_system_gc);
//        VMThread::execute(&op);
//    }
}

extern "C" void handle_deoptimization() {
    // just a placeholder function
}

extern "C" void go_unwind() {
    // just a placeholder function
}

int count_stk_args(GrowableArray<BasicType>* stk_basic_types) {
    int stk_args = 0;
    bool is_first_int_checked = false;
    if (stk_basic_types != NULL) {
        for (int i = 0; i < stk_basic_types->length(); ++i) {
            switch (stk_basic_types->at(i)) {
                case T_BOOLEAN:
                case T_BYTE:
                case T_CHAR:
                case T_SHORT:
                case T_INT:
                    if (!is_first_int_checked) {
                        is_first_int_checked = true;
                    } else {
                        stk_args++;
                    }
                    break;
                case T_FLOAT:
                    stk_args++;
                    break;
                case T_DOUBLE:
                    stk_args++;
                    break;
                case T_LONG:
                case T_OBJECT:
                case T_ARRAY:
                    if (!is_first_int_checked) {
                        is_first_int_checked = true;
                    } else {
                        stk_args++;
                    }
                    break;
                default:
                    ShouldNotReachHere();
            }
        }
    }

    return stk_args;
}

void generate_stk_args(YuhuMacroAssembler* masm, int frame_size_in_bytes, GrowableArray<BasicType>* stk_basic_types) {
    if (stk_basic_types != NULL) {
        int offset_in_bytes = 0;
        int stk_args = 0;
        bool is_first_int_checked = false;
        for (int i = 0; i < stk_basic_types->length(); ++i) {
            switch (stk_basic_types->at(i)) {
                case T_BOOLEAN:
                case T_BYTE:
                case T_CHAR:
                case T_SHORT:
                case T_INT:
                    if (!is_first_int_checked) {
                        is_first_int_checked = true;
                        masm->write_inst_ldr(YuhuMacroAssembler::w0, YuhuAddress(YuhuMacroAssembler::sp, frame_size_in_bytes + offset_in_bytes));
                        offset_in_bytes += 4;
                    } else {
                        stk_args++;
                        masm->write_inst_ldr(YuhuMacroAssembler::w9, YuhuAddress(YuhuMacroAssembler::sp, frame_size_in_bytes + offset_in_bytes));
                        masm->write_inst_str(YuhuMacroAssembler::w9, YuhuAddress(YuhuMacroAssembler::sp, (stk_args - 1) * wordSize));
                        offset_in_bytes += 4;
                    }
                    break;
                case T_FLOAT:
                    stk_args++;
                    masm->write_inst_ldr(YuhuMacroAssembler::s16, YuhuAddress(YuhuMacroAssembler::sp, frame_size_in_bytes + offset_in_bytes));
                    masm->write_inst_str(YuhuMacroAssembler::s16, YuhuAddress(YuhuMacroAssembler::sp, (stk_args - 1) * wordSize));
                    offset_in_bytes += 4;
                    break;
                case T_DOUBLE:
                    stk_args++;
                    // 8-bytes alignment
                    masm->write_inst_ldr(YuhuMacroAssembler::d16, YuhuAddress(YuhuMacroAssembler::sp, frame_size_in_bytes + align_size_up(offset_in_bytes, 8)));
                    masm->write_inst_str(YuhuMacroAssembler::d16, YuhuAddress(YuhuMacroAssembler::sp, (stk_args - 1) * wordSize));
                    offset_in_bytes = align_size_up(offset_in_bytes, 8) + 8; // 8-bytes alignment
                    break;
                case T_LONG:
                case T_OBJECT:
                case T_ARRAY:
                    if (!is_first_int_checked) {
                        is_first_int_checked = true;
                        masm->write_inst_ldr(YuhuMacroAssembler::x0, YuhuAddress(YuhuMacroAssembler::sp, frame_size_in_bytes + align_size_up(offset_in_bytes, 8)));
                        offset_in_bytes = align_size_up(offset_in_bytes, 8) + 8;
                    } else {
                        stk_args++;
                        masm->write_inst_ldr(YuhuMacroAssembler::x9, YuhuAddress(YuhuMacroAssembler::sp, frame_size_in_bytes + align_size_up(offset_in_bytes, 8)));
                        masm->write_inst_str(YuhuMacroAssembler::x9, YuhuAddress(YuhuMacroAssembler::sp, (stk_args - 1) * wordSize));
                        offset_in_bytes = align_size_up(offset_in_bytes, 8) + 8;
                    }
                    break;
                default:
                    ShouldNotReachHere();
            }
        }
    }
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
                                                ciMethod* current_method,
                                                GrowableArray<BasicType>* stk_basic_types) {
  // Check stub cache first
  YuhuRuntimeStub* cached = _stub_cache->find(target_method, current_method, YUHUSTUB_STATIC_CALL);
  if (cached != NULL) {
    if (YuhuTraceInstalls) {
        if (YuhuStackMapFile != NULL) {
            YUHU_STACK_MAP_LOG("Yuhu: Using cached static call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s from current method %s.%s signature %s",
                               p2i(cached->entry_point()),
                               target_method->holder()->name()->as_utf8(),
                               target_method->name()->as_utf8(),
                               target_method->signature()->as_symbol()->as_utf8(),
                               current_method->holder()->name()->as_utf8(),
                               current_method->name()->as_utf8(),
                               current_method->signature()->as_symbol()->as_utf8());
        } else {
            tty->print_cr("Yuhu: Generated static call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s from current method %s.%s signature %s",
                          p2i(cached->entry_point()),
                          target_method->holder()->name()->as_utf8(),
                          target_method->name()->as_utf8(),
                          target_method->signature()->as_symbol()->as_utf8(),
                          current_method->holder()->name()->as_utf8(),
                          current_method->name()->as_utf8(),
                          current_method->signature()->as_symbol()->as_utf8());
        }
    }
    return cached->entry_point();
  }

  ResourceMark rm;
  
  const int stub_size = 64;
  CodeBuffer cb("yuhu_static_call_stub", stub_size, stub_size);
  YuhuMacroAssembler masm(&cb);

  address begin = masm.current_pc();

  int stk_args = count_stk_args(stk_basic_types);
  
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
  int frame_size_in_bytes = align_size_up((4 + stk_args) * wordSize, 16);
  masm.write_inst("sub sp, sp, #%d", frame_size_in_bytes);
  masm.write_inst("stp x29, x30, [sp, #%d]", frame_size_in_bytes - 16);
  masm.write_inst("stp xzr, x19, [sp, #%d]", frame_size_in_bytes - 32);
  masm.write_inst("add x29, sp, #%d", frame_size_in_bytes - 16);

    generate_stk_args(&masm, frame_size_in_bytes, stk_basic_types);
  
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

  YuhuLabel normal_exit;
  masm.write_inst_b(normal_exit);

    int exception_handler_begin_offset = (int) (masm.current_pc() - begin);

    // x0 contains exception oop, save x0 to pending exception field
    masm.write_inst_str(YuhuMacroAssembler::x0, YuhuAddress(YuhuMacroAssembler::x28, in_bytes(JavaThread::pending_exception_offset())));

    masm.pin_label(normal_exit);

    // save return value and return address
    NOT_PRODUCT(YuhuLabel is_valid_return_address);
    NOT_PRODUCT(masm.write_inst("stp x0, lr, [sp, #-16]!"));
    NOT_PRODUCT(masm.write_inst("ldr x8, [sp, #%d]", frame_size_in_bytes + 8));
    NOT_PRODUCT(masm.write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address, YuhuRuntime::is_yuhu_nmethod), YuhuMacroAssembler::x8));
    NOT_PRODUCT(masm.write_inst_cbnz(YuhuMacroAssembler::x0, is_valid_return_address));
    NOT_PRODUCT(masm.write_insts_stop("invalid return address"));
    NOT_PRODUCT(masm.pin_label(is_valid_return_address));
    NOT_PRODUCT(masm.write_inst("ldp x0, lr, [sp], #16"));
  
  // Epilogue: restore x19, FP, LR
  masm.write_inst("ldp x29, x30, [sp, #%d]", frame_size_in_bytes - 16);
  masm.write_inst("ldp xzr, x19, [sp, #%d]", frame_size_in_bytes - 32);
  masm.write_inst("add sp, sp, #%d", frame_size_in_bytes);

  // Return
  masm.write_inst("ret");

  masm.flush();
  
  // Create RuntimeStub
  // Frame size: 3 words (FP, LR, x19) + 1 word padding for alignment = 4 words
  // plus arguments on stack
  int frame_size_in_words = frame_size_in_bytes / wordSize;

  YuhuRuntimeStub* stub = YuhuRuntimeStub::new_yuhu_runtime_stub(
      "yuhu_static_call_stub",
      &cb,
      CodeOffsets::frame_never_safe,
      frame_size_in_words,
      NULL,  // no oops saved
      false,  // caller_must_gc_arguments
      exception_handler_begin_offset
  );
  
  address stub_addr = stub->entry_point();
  
  if (YuhuTraceInstalls) {
      if (YuhuStackMapFile != NULL) {
          YUHU_STACK_MAP_LOG("Yuhu: Generated static call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s from current method %s.%s signature %s",
                             p2i(stub_addr),
                             target_method->holder()->name()->as_utf8(),
                             target_method->name()->as_utf8(),
                             target_method->signature()->as_symbol()->as_utf8(),
                             current_method->holder()->name()->as_utf8(),
                             current_method->name()->as_utf8(),
                             current_method->signature()->as_symbol()->as_utf8());
      } else {
          tty->print_cr("Yuhu: Generated static call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s from current method %s.%s signature %s",
                        p2i(stub_addr),
                        target_method->holder()->name()->as_utf8(),
                        target_method->name()->as_utf8(),
                        target_method->signature()->as_symbol()->as_utf8(),
                        current_method->holder()->name()->as_utf8(),
                        current_method->name()->as_utf8(),
                        current_method->signature()->as_symbol()->as_utf8());
      }
  }

  // Add to stub cache
  _stub_cache->add(target_method, current_method, YUHUSTUB_STATIC_CALL, stub);
  
  return stub_addr;
}

// Generate virtual call stub for virtual method calls
address YuhuRuntime::generate_virtual_call_stub(ciMethod* target_method, 
                                                 ciMethod* current_method, 
                                                 int vtable_index,
                                                 GrowableArray<BasicType>* stk_basic_types) {
  // Check stub cache first
  YuhuRuntimeStub* cached = _stub_cache->find(target_method, current_method, YUHUSTUB_VIRTUAL_CALL);
  if (cached != NULL) {
    if (YuhuTraceInstalls) {
        if (YuhuStackMapFile != NULL) {
            YUHU_STACK_MAP_LOG("Yuhu: Using cached virtual call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s (vtable_index=%d) from current method %s.%s signature %s",
                               p2i(cached->entry_point()),
                               target_method->holder()->name()->as_utf8(),
                               target_method->name()->as_utf8(),
                               target_method->signature()->as_symbol()->as_utf8(),
                               vtable_index,
                               current_method->holder()->name()->as_utf8(),
                               current_method->name()->as_utf8(),
                               current_method->signature()->as_symbol()->as_utf8());
        } else {
            tty->print_cr("Yuhu: Using cached virtual call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s (vtable_index=%d) from current method %s.%s signature %s",
                          p2i(cached->entry_point()),
                          target_method->holder()->name()->as_utf8(),
                          target_method->name()->as_utf8(),
                          target_method->signature()->as_symbol()->as_utf8(),
                          vtable_index,
                          current_method->holder()->name()->as_utf8(),
                          current_method->name()->as_utf8(),
                          current_method->signature()->as_symbol()->as_utf8());
        }
    }
    return cached->entry_point();
  }

  ResourceMark rm;
  
  const int stub_size = 64;
  CodeBuffer cb("yuhu_virtual_call_stub", stub_size, stub_size);
  YuhuMacroAssembler masm(&cb);

  address begin = masm.current_pc();

    int stk_args = count_stk_args(stk_basic_types);
  
  // Frame layout: same as static call stub
  
  // Prologue: save FP, LR, x19
    int frame_size_in_bytes = align_size_up((4 + stk_args) * wordSize, 16);
    masm.write_inst("sub sp, sp, #%d", frame_size_in_bytes);
    masm.write_inst("stp x29, x30, [sp, #%d]", frame_size_in_bytes - 16);
    masm.write_inst("stp xzr, x19, [sp, #%d]", frame_size_in_bytes - 32);
    masm.write_inst("add x29, sp, #%d", frame_size_in_bytes - 16);

    generate_stk_args(&masm, frame_size_in_bytes, stk_basic_types);
  
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

    YuhuLabel normal_exit;
    masm.write_inst_b(normal_exit);

    int exception_handler_begin_offset = (int)(masm.current_pc() - begin);

    // x0 contains exception oop, save x0 to pending exception field
    masm.write_inst_str(YuhuMacroAssembler::x0, YuhuAddress(YuhuMacroAssembler::x28, in_bytes(JavaThread::pending_exception_offset())));

    masm.pin_label(normal_exit);

    // save return value and return address
    NOT_PRODUCT(YuhuLabel is_valid_return_address);
    NOT_PRODUCT(masm.write_inst("stp x0, lr, [sp, #-16]!"));
    NOT_PRODUCT(masm.write_inst("ldr x8, [sp, #%d]", frame_size_in_bytes + 8));
    NOT_PRODUCT(masm.write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address, YuhuRuntime::is_yuhu_nmethod), YuhuMacroAssembler::x8));
    NOT_PRODUCT(masm.write_inst_cbnz(YuhuMacroAssembler::x0, is_valid_return_address));
    NOT_PRODUCT(masm.write_insts_stop("invalid return address"));
    NOT_PRODUCT(masm.pin_label(is_valid_return_address));
    NOT_PRODUCT(masm.write_inst("ldp x0, lr, [sp], #16"));
  
  // Epilogue: restore x19, FP, LR
    masm.write_inst("ldp x29, x30, [sp, #%d]", frame_size_in_bytes - 16);
    masm.write_inst("ldp xzr, x19, [sp, #%d]", frame_size_in_bytes - 32);
    masm.write_inst("add sp, sp, #%d", frame_size_in_bytes);

    // Return
    masm.write_inst("ret");

    masm.flush();
  
  // Create RuntimeStub
  int frame_size_in_words = frame_size_in_bytes / wordSize;
  
  YuhuRuntimeStub* stub = YuhuRuntimeStub::new_yuhu_runtime_stub(
      "yuhu_virtual_call_stub",
      &cb,
      CodeOffsets::frame_never_safe,
      frame_size_in_words,
      NULL,  // no oops saved
      false,
      exception_handler_begin_offset
  );
  
  address stub_addr = stub->entry_point();

    if (YuhuTraceInstalls) {
        if (YuhuStackMapFile != NULL) {
            YUHU_STACK_MAP_LOG("Yuhu: Generated virtual call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s (vtable_index=%d) from current method %s.%s signature %s",
                               p2i(stub_addr),
                               target_method->holder()->name()->as_utf8(),
                               target_method->name()->as_utf8(),
                               target_method->signature()->as_symbol()->as_utf8(),
                               vtable_index,
                               current_method->holder()->name()->as_utf8(),
                               current_method->name()->as_utf8(),
                               current_method->signature()->as_symbol()->as_utf8());
        } else {
            tty->print_cr("Yuhu: Generated virtual call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s (vtable_index=%d) from current method %s.%s signature %s",
                          p2i(stub_addr),
                          target_method->holder()->name()->as_utf8(),
                          target_method->name()->as_utf8(),
                          target_method->signature()->as_symbol()->as_utf8(),
                          vtable_index,
                          current_method->holder()->name()->as_utf8(),
                          current_method->name()->as_utf8(),
                          current_method->signature()->as_symbol()->as_utf8());
        }
    }

  // Add to stub cache
  _stub_cache->add(target_method, current_method, YUHUSTUB_VIRTUAL_CALL, stub);
  
  return stub_addr;
}

// Generate interface call stub for interface method calls
address YuhuRuntime::generate_interface_call_stub(ciMethod* target_method, 
                                                   ciMethod* current_method,
                                                   GrowableArray<BasicType>* stk_basic_types) {
  // Check stub cache first
  YuhuRuntimeStub* cached = _stub_cache->find(target_method, current_method, YUHUSTUB_INTERFACE_CALL);
  if (cached != NULL) {
    if (YuhuTraceInstalls) {
        if (YuhuStackMapFile != NULL) {
            YUHU_STACK_MAP_LOG("Yuhu: Using cached interface call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s from current method %s.%s signature %s",
                               p2i(cached->entry_point()),
                               target_method->holder()->name()->as_utf8(),
                               target_method->name()->as_utf8(),
                               target_method->signature()->as_symbol()->as_utf8(),
                               current_method->holder()->name()->as_utf8(),
                               current_method->name()->as_utf8(),
                               current_method->signature()->as_symbol()->as_utf8());
        } else {
            tty->print_cr("Yuhu: Using cached interface call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s from current method %s.%s signature %s",
                          p2i(cached->entry_point()),
                          target_method->holder()->name()->as_utf8(),
                          target_method->name()->as_utf8(),
                          target_method->signature()->as_symbol()->as_utf8(),
                          current_method->holder()->name()->as_utf8(),
                          current_method->name()->as_utf8(),
                          current_method->signature()->as_symbol()->as_utf8());
        }
    }
    return cached->entry_point();
  }

  ResourceMark rm;
  
  const int stub_size = 128;
  CodeBuffer cb("yuhu_interface_call_stub", stub_size, stub_size);
  YuhuMacroAssembler masm(&cb);

  address begin = masm.current_pc();

    int stk_args = count_stk_args(stk_basic_types);
  
  ciInstanceKlass* ci_interface_klass = target_method->holder();
  InstanceKlass* interface_klass = ci_interface_klass->get_instanceKlass();
  int itable_index = target_method->itable_index();
  
  // Frame layout: same as static call stub
  
  // Prologue: save FP, LR, x19
    int frame_size_in_bytes = align_size_up((4 + stk_args) * wordSize, 16);
    masm.write_inst("sub sp, sp, #%d", frame_size_in_bytes);
    masm.write_inst("stp x29, x30, [sp, #%d]", frame_size_in_bytes - 16);
    masm.write_inst("stp xzr, x19, [sp, #%d]", frame_size_in_bytes - 32);
    masm.write_inst("add x29, sp, #%d", frame_size_in_bytes - 16);

    generate_stk_args(&masm, frame_size_in_bytes, stk_basic_types);
  
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

    YuhuLabel normal_exit;
    masm.write_inst_b(normal_exit);

    int exception_handler_begin_offset = (int)(masm.current_pc() - begin);

    // x0 contains exception oop, save x0 to pending exception field
    masm.write_inst_str(YuhuMacroAssembler::x0, YuhuAddress(YuhuMacroAssembler::x28, in_bytes(JavaThread::pending_exception_offset())));

    masm.pin_label(normal_exit);

    // save return value and return address
    NOT_PRODUCT(YuhuLabel is_valid_return_address);
    NOT_PRODUCT(masm.write_inst("stp x0, lr, [sp, #-16]!"));
    NOT_PRODUCT(masm.write_inst("ldr x8, [sp, #%d]", frame_size_in_bytes + 8));
    NOT_PRODUCT(masm.write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address, YuhuRuntime::is_yuhu_nmethod), YuhuMacroAssembler::x8));
    NOT_PRODUCT(masm.write_inst_cbnz(YuhuMacroAssembler::x0, is_valid_return_address));
    NOT_PRODUCT(masm.write_insts_stop("invalid return address"));
    NOT_PRODUCT(masm.pin_label(is_valid_return_address));
    NOT_PRODUCT(masm.write_inst("ldp x0, lr, [sp], #16"));

    // Epilogue: restore x19, FP, LR (only reached if interface not found - should not return)
    masm.write_inst("ldp x29, x30, [sp, #%d]", frame_size_in_bytes - 16);
    masm.write_inst("ldp xzr, x19, [sp, #%d]", frame_size_in_bytes - 32);
    masm.write_inst("add sp, sp, #%d", frame_size_in_bytes);

    // Return
    masm.write_inst("ret");
  
  masm.pin_label(L_no_such_interface);
  masm.write_insts_far_jump(YuhuRuntimeAddress(StubRoutines::throw_IncompatibleClassChangeError_entry()));
  masm.write_insts_stop("should not reach here");

    masm.flush();
  
  // Create RuntimeStub
  int frame_size_in_words = frame_size_in_bytes / wordSize;
  
  YuhuRuntimeStub* stub = YuhuRuntimeStub::new_yuhu_runtime_stub(
      "yuhu_interface_call_stub",
      &cb,
      CodeOffsets::frame_never_safe,
      frame_size_in_words,
      NULL,  // no oops saved
      false,
      exception_handler_begin_offset
  );
  
  address stub_addr = stub->entry_point();

    if (YuhuTraceInstalls) {
        if (YuhuStackMapFile != NULL) {
            YUHU_STACK_MAP_LOG("Yuhu: Generated interface call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s from current method %s.%s signature %s",
                               p2i(stub_addr),
                               target_method->holder()->name()->as_utf8(),
                               target_method->name()->as_utf8(),
                               target_method->signature()->as_symbol()->as_utf8(),
                               current_method->holder()->name()->as_utf8(),
                               current_method->name()->as_utf8(),
                               current_method->signature()->as_symbol()->as_utf8());
        } else {
            tty->print_cr("Yuhu: Generated interface call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s from current method %s.%s signature %s",
                          p2i(stub_addr),
                          target_method->holder()->name()->as_utf8(),
                          target_method->name()->as_utf8(),
                          target_method->signature()->as_symbol()->as_utf8(),
                          current_method->holder()->name()->as_utf8(),
                          current_method->name()->as_utf8(),
                          current_method->signature()->as_symbol()->as_utf8());
        }
    }

  // Add to stub cache
  _stub_cache->add(target_method, current_method, YUHUSTUB_INTERFACE_CALL, stub);
  
  return stub_addr;
}

void store_reg_args(YuhuMacroAssembler* masm, int stk_args_size_in_bytes, int reg_args_size_in_bytes,
                    GrowableArray<BasicType>* reg_basic_types, GrowableArray<BasicType>* stk_basic_types, OopMap* oopmap) {
    if (reg_basic_types != NULL) {
        for (int i = 0; i < reg_basic_types->length(); i = i+2) {
            // if num of registers is odd, then add x0 as pair which can also cover 8th argument x0 scenario
            if (reg_basic_types->length() % 2 == 1 && i == reg_basic_types->length() - 1) {
                masm->write_inst_stp(YuhuMacroAssembler::as_register(i+1, 0, 0b10).as_general_register(),
                                     YuhuMacroAssembler::x0,
                                     YuhuAddress(YuhuMacroAssembler::sp, stk_args_size_in_bytes + i*wordSize));
            } else {
                masm->write_inst_stp(YuhuMacroAssembler::as_register(i+1, 0, 0b10).as_general_register(),
                                     YuhuMacroAssembler::as_register(i+2, 0, 0b10).as_general_register(),
                                     YuhuAddress(YuhuMacroAssembler::sp, stk_args_size_in_bytes + i*wordSize));
            }
        }
        // reg_basic_types does not include 8th argument x0
        for (int i = 0; i < reg_basic_types->length(); ++i) {
            switch (reg_basic_types->at(i)) {
                case T_BOOLEAN:
                case T_BYTE:
                case T_CHAR:
                case T_SHORT:
                case T_INT:
                case T_LONG:
                case T_FLOAT:
                case T_DOUBLE:
                    break;
                case T_OBJECT:
                case T_ARRAY:
                    oopmap->set_oop(YuhuStack::slot2reg((stk_args_size_in_bytes + i*wordSize) >> LogBytesPerWord));
                    break;
                default:
                    ShouldNotReachHere();
            }
        }
    }
    if (stk_basic_types != NULL) {
        int stk_args = 0;
        bool is_first_int_checked = false;
        for (int i = 0; i < stk_basic_types->length(); ++i) {
            switch (stk_basic_types->at(i)) {
                case T_BOOLEAN:
                case T_BYTE:
                case T_CHAR:
                case T_SHORT:
                case T_INT:
                case T_LONG:
                    if (!is_first_int_checked) {
                        is_first_int_checked = true;
                    } else {
                        stk_args++;
                    }
                    break;
                case T_FLOAT:
                case T_DOUBLE:
                    stk_args++;
                    break;
                case T_OBJECT:
                case T_ARRAY:
                    if (!is_first_int_checked) {
                        is_first_int_checked = true;
                        // record the slot at reg args area
                        oopmap->set_oop(YuhuStack::slot2reg((stk_args_size_in_bytes + reg_args_size_in_bytes - wordSize) >> LogBytesPerWord));
                    } else {
                        stk_args++;
                        oopmap->set_oop(YuhuStack::slot2reg(((stk_args - 1) * wordSize) >> LogBytesPerWord));
                    }
                    break;
                default:
                    ShouldNotReachHere();
            }
        }
    }
}

void load_reg_args(YuhuMacroAssembler* masm, int stk_args_size_in_bytes, GrowableArray<BasicType>* reg_basic_types) {
    if (reg_basic_types != NULL) {
        for (int i = 0; i < reg_basic_types->length(); i = i+2) {
            // if num of registers is odd, then add x0 as pair which can also cover 8th argument x0 scenario
            if (reg_basic_types->length() % 2 == 1 && i == reg_basic_types->length() - 1) {
                masm->write_inst_ldp(YuhuMacroAssembler::as_register(i+1, 0, 0b10).as_general_register(),
                                     YuhuMacroAssembler::x0,
                                     YuhuAddress(YuhuMacroAssembler::sp, stk_args_size_in_bytes + i*wordSize));
            } else {
                masm->write_inst_ldp(YuhuMacroAssembler::as_register(i+1, 0, 0b10).as_general_register(),
                                     YuhuMacroAssembler::as_register(i+2, 0, 0b10).as_general_register(),
                                     YuhuAddress(YuhuMacroAssembler::sp, stk_args_size_in_bytes + i*wordSize));
            }
        }
    }
}

// Generate dynamic resolution call stub for interface methods with itable_index() < 0.
// These are typically Object methods (equals, hashCode, toString) re-declared in interfaces.
// The stub calls resolve_interface_call to dynamically resolve the target method at runtime
// via LinkResolver, then jumps to the resolved method's verified_code_entry.
// Coz we have to call YuhuRuntime::resolve_interface_call to get call target, need to save registers
// in stack, so the layout contains stk area, saved register area and prologue area.
/*
        0x16f7ea530: 0x000000076ad73c40 0x00000006c000e280 - stk 0 / stk 1
        0x16f7ea540: 0x00000000deadbeef 0x0000000000000000 - stk 2 / padding
        0x16f7ea550: 0x00000001000000c8 0x0000000100000064 - x1 / x2
        0x16f7ea560: 0x000000076ad73c30 0x000000076ad73c20 - x3 / x4
        0x16f7ea570: 0x0000000100000001 0x000000076ad73c30 - x5 / x6
        0x16f7ea580: 0x0000000000000000 0x000000010c345060 - x7 / x0
        0x16f7ea590: 0x0000000000000003 0x00000000dead0048 - xzr / x19
        0x16f7ea5a0: 0x000000016f7ea5c0 0x00000001308185ec - prologue x29 / x30
 */
address YuhuRuntime::generate_indeterminate_interface_call_stub(ciMethod* target_method,
                                                            ciMethod* current_method,
                                                            GrowableArray<BasicType>* reg_basic_types,
                                                            GrowableArray<BasicType>* stk_basic_types) {
  // Check stub cache first
  YuhuRuntimeStub* cached = _stub_cache->find(target_method, current_method, YUHUSTUB_INDETERMINATE_INTERFACE_CALL);
  if (cached != NULL) {
    if (YuhuTraceInstalls) {
        if (YuhuStackMapFile != NULL) {
            YUHU_STACK_MAP_LOG("Yuhu: Using cached indeterminate interface call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s from current method %s.%s signature %s",
                               p2i(cached->entry_point()),
                               target_method->holder()->name()->as_utf8(),
                               target_method->name()->as_utf8(),
                               target_method->signature()->as_symbol()->as_utf8(),
                               current_method->holder()->name()->as_utf8(),
                               current_method->name()->as_utf8(),
                               current_method->signature()->as_symbol()->as_utf8());
        } else {
            tty->print_cr("Yuhu: Using cached indeterminate interface call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s from current method %s.%s signature %s",
                          p2i(cached->entry_point()),
                          target_method->holder()->name()->as_utf8(),
                          target_method->name()->as_utf8(),
                          target_method->signature()->as_symbol()->as_utf8(),
                          current_method->holder()->name()->as_utf8(),
                          current_method->name()->as_utf8(),
                          current_method->signature()->as_symbol()->as_utf8());
        }
    }
    return cached->entry_point();
  }

  ResourceMark rm;

  const int stub_size = 256;
  CodeBuffer cb("yuhu_indeterminate_interface_call_stub", stub_size, stub_size);
  YuhuMacroAssembler masm(&cb);

  address begin = masm.current_pc();

  int stk_args = count_stk_args(stk_basic_types);

  assert(reg_basic_types->length() <= 7, "should be less than or equal to 7");

  // Prologue: save FP, LR, x19
  int stk_args_size_in_bytes = align_size_up(stk_args * wordSize, 16);
  int reg_args_size_in_bytes = align_size_up(reg_basic_types->length() * wordSize, 16);
  int frame_size_in_bytes = stk_args_size_in_bytes + reg_args_size_in_bytes + 4 * wordSize;
  masm.write_inst("sub sp, sp, #%d", frame_size_in_bytes);
  masm.write_inst("stp x29, x30, [sp, #%d]", frame_size_in_bytes - 16);
  masm.write_inst("stp xzr, x19, [sp, #%d]", frame_size_in_bytes - 32);
  masm.write_inst("add x29, sp, #%d", frame_size_in_bytes - 16);

  generate_stk_args(&masm, frame_size_in_bytes, stk_basic_types);

    auto *oopmap_set = new OopMapSet();
    int arg_count = 0;
    auto *oopmap = new OopMap(YuhuStack::oopmap_slot_munge(frame_size_in_bytes / wordSize),
                              YuhuStack::oopmap_slot_munge(arg_count));

  // store reg args
  store_reg_args(&masm, stk_args_size_in_bytes, reg_args_size_in_bytes, reg_basic_types, stk_basic_types, oopmap);

    masm.write_insts_set_last_java_frame(YuhuMacroAssembler::sp, YuhuMacroAssembler::noreg, YuhuMacroAssembler::noreg, YuhuMacroAssembler::x9);

    YuhuLabel label;
    masm.write_inst_adr(YuhuMacroAssembler::x9, label);
    masm.write_inst("stp xzr, x9, [sp, #-16]!");

    // Load arguments for resolve_interface_call:
    //   x0 = JavaThread* (thread)
    //   x1 = oop recv_oop (receiver)
    //   x2 = Klass* interface_klass
    //   x3 = Method* target_method
    //   x4 = Klass* current_klass

    // x0 = thread (x28 holds the thread register per Yuhu convention)
    masm.write_inst_mov_reg(YuhuMacroAssembler::x0, YuhuMacroAssembler::x28);

    // x1 = receiver object (per Yuhu calling convention)

    // x2 = interface_klass (embedded as metadata constant for GC tracking)
    ciInstanceKlass* ci_interface_klass = target_method->holder();
    InstanceKlass* interface_klass = ci_interface_klass->get_instanceKlass();
    int interface_metadata_index = cb.oop_recorder()->allocate_metadata_index(interface_klass);
    RelocationHolder interface_metadata_rspec = metadata_Relocation::spec(interface_metadata_index);
    cb.relocate(masm.current_pc(), interface_metadata_rspec);
    masm.write_insts_lea(YuhuMacroAssembler::x2, YuhuAddress((address)interface_klass, relocInfo::metadata_type));

    // x3 = target_method Method*
    Method* method_ptr = target_method->get_Method();
    int method_metadata_index = cb.oop_recorder()->allocate_metadata_index(method_ptr);
    RelocationHolder method_metadata_rspec = metadata_Relocation::spec(method_metadata_index);
    cb.relocate(masm.current_pc(), method_metadata_rspec);
    masm.write_insts_lea(YuhuMacroAssembler::x3, YuhuAddress((address)method_ptr, relocInfo::metadata_type));

    // x4 = current_klass (the class containing the call, for access checking)
    ciInstanceKlass* ci_current_klass = current_method->holder();
    InstanceKlass* current_klass = ci_current_klass->get_instanceKlass();
    int current_metadata_index = cb.oop_recorder()->allocate_metadata_index(current_klass);
    RelocationHolder current_metadata_rspec = metadata_Relocation::spec(current_metadata_index);
    cb.relocate(masm.current_pc(), current_metadata_rspec);
    masm.write_insts_lea(YuhuMacroAssembler::x4, YuhuAddress((address)current_klass, relocInfo::metadata_type));

    masm.write_insts_lea(YuhuMacroAssembler::x8, YuhuExternalAddress(CAST_FROM_FN_PTR(address, YuhuRuntime::resolve_interface_call)));
    masm.write_inst_blr(YuhuMacroAssembler::x8);

    oopmap_set->add_gc_map(masm.current_pc() - begin, oopmap);

    masm.pin_label(label);
    masm.write_inst_mov_reg(YuhuMacroAssembler::x8, YuhuMacroAssembler::x0);

    masm.write_inst("add sp, sp, #16");

    masm.write_insts_reset_last_java_frame(true);

    // load reg args
    load_reg_args(&masm, stk_args_size_in_bytes, reg_basic_types);

  // Load Method* from thread->vm_result_2() into x12 for c2i adapter
  masm.write_inst_ldr(YuhuMacroAssembler::x12,
                      YuhuAddress(YuhuMacroAssembler::x28, in_bytes(JavaThread::vm_result_2_offset())));
  // Clear vm_result_2
  masm.write_inst_str(YuhuMacroAssembler::xzr,
                      YuhuAddress(YuhuMacroAssembler::x28, in_bytes(JavaThread::vm_result_2_offset())));

  // Jump to resolved compiled entry (x8 = verified_code_entry)
  masm.write_inst_blr(YuhuMacroAssembler::x8);

  oopmap_set->add_gc_map(masm.current_pc() - begin, new OopMap(YuhuStack::oopmap_slot_munge(frame_size_in_bytes / wordSize),
                                                               YuhuStack::oopmap_slot_munge(arg_count)));

  YuhuLabel normal_exit;
  masm.write_inst_b(normal_exit);

  int exception_handler_begin_offset = (int)(masm.current_pc() - begin);

  // x0 contains exception oop, save x0 to pending exception field
  masm.write_inst_str(YuhuMacroAssembler::x0, YuhuAddress(YuhuMacroAssembler::x28, in_bytes(JavaThread::pending_exception_offset())));

  masm.pin_label(normal_exit);

    // save return value and return address
    NOT_PRODUCT(YuhuLabel is_valid_return_address);
    NOT_PRODUCT(masm.write_inst("stp x0, lr, [sp, #-16]!"));
    NOT_PRODUCT(masm.write_inst("ldr x8, [sp, #%d]", frame_size_in_bytes + 8));
    NOT_PRODUCT(masm.write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address, YuhuRuntime::is_yuhu_nmethod), YuhuMacroAssembler::x8));
    NOT_PRODUCT(masm.write_inst_cbnz(YuhuMacroAssembler::x0, is_valid_return_address));
    NOT_PRODUCT(masm.write_insts_stop("invalid return address"));
    NOT_PRODUCT(masm.pin_label(is_valid_return_address));
    NOT_PRODUCT(masm.write_inst("ldp x0, lr, [sp], #16"));

  // Epilogue: restore x19, FP, LR
  masm.write_inst("ldp x29, x30, [sp, #%d]", frame_size_in_bytes - 16);
  masm.write_inst("ldp xzr, x19, [sp, #%d]", frame_size_in_bytes - 32);
  masm.write_inst("add sp, sp, #%d", frame_size_in_bytes);

  // Return
  masm.write_inst("ret");

  masm.flush();

  // Create RuntimeStub
  int frame_size_in_words = frame_size_in_bytes / wordSize;

  YuhuRuntimeStub* stub = YuhuRuntimeStub::new_yuhu_runtime_stub(
      "yuhu_indeterminate_interface_call_stub",
      &cb,
      CodeOffsets::frame_never_safe,
      frame_size_in_words,
      oopmap_set,  // collect oop in stack
      false,
      exception_handler_begin_offset
  );

  address stub_addr = stub->entry_point();

  if (YuhuTraceInstalls) {
      if (YuhuStackMapFile != NULL) {
          YUHU_STACK_MAP_LOG("Yuhu: Generated indeterminate interface call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s from current method %s.%s signature %s",
                             p2i(stub_addr),
                             target_method->holder()->name()->as_utf8(),
                             target_method->name()->as_utf8(),
                             target_method->signature()->as_symbol()->as_utf8(),
                             current_method->holder()->name()->as_utf8(),
                             current_method->name()->as_utf8(),
                             current_method->signature()->as_symbol()->as_utf8());
      } else {
          tty->print_cr("Yuhu: Generated indeterminate interface call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s from current method %s.%s signature %s",
                        p2i(stub_addr),
                        target_method->holder()->name()->as_utf8(),
                        target_method->name()->as_utf8(),
                        target_method->signature()->as_symbol()->as_utf8(),
                        current_method->holder()->name()->as_utf8(),
                        current_method->name()->as_utf8(),
                        current_method->signature()->as_symbol()->as_utf8());
      }
  }

  // Add to stub cache
  _stub_cache->add(target_method, current_method, YUHUSTUB_INDETERMINATE_INTERFACE_CALL, stub);

  return stub_addr;
}

// Generate dynamic call stub for invokedynamic bytecodes.
// The stub calls resolve_dynamic_call to resolve the CallSite at runtime,
// then jumps to the resolved target's verified_code_entry.
// invokedynamic is like invokestatic (no receiver) but the target method
// is resolved at runtime via the bootstrap method.
// Frame layout (same pattern as indeterminate_interface_call_stub):
//   [stk args] [saved reg args] [prologue: x29, x30, xzr, x19]
address YuhuRuntime::generate_dynamic_call_stub(ciMethod* target_method,
                                                ciMethod* current_method,
                                                GrowableArray<BasicType>* reg_basic_types,
                                                GrowableArray<BasicType>* stk_basic_types) {
  // Check stub cache first
  YuhuRuntimeStub* cached = _stub_cache->find(target_method, current_method, YUHUSTUB_DYNAMIC_CALL);
  if (cached != NULL) {
    if (YuhuTraceInstalls) {
        if (YuhuStackMapFile != NULL) {
            YUHU_STACK_MAP_LOG("Yuhu: Using cached dynamic call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s from current method %s.%s signature %s",
                               p2i(cached->entry_point()),
                               target_method->holder()->name()->as_utf8(),
                               target_method->name()->as_utf8(),
                               target_method->signature()->as_symbol()->as_utf8(),
                               current_method->holder()->name()->as_utf8(),
                               current_method->name()->as_utf8(),
                               current_method->signature()->as_symbol()->as_utf8());
        } else {
            tty->print_cr("Yuhu: Using cached dynamic call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s from current method %s.%s signature %s",
                          p2i(cached->entry_point()),
                          target_method->holder()->name()->as_utf8(),
                          target_method->name()->as_utf8(),
                          target_method->signature()->as_symbol()->as_utf8(),
                          current_method->holder()->name()->as_utf8(),
                          current_method->name()->as_utf8(),
                          current_method->signature()->as_symbol()->as_utf8());
        }
    }
    return cached->entry_point();
  }

  ResourceMark rm;

  const int stub_size = 256;
  CodeBuffer cb("yuhu_dynamic_call_stub", stub_size, stub_size);
  YuhuMacroAssembler masm(&cb);

  address begin = masm.current_pc();

  int stk_args = count_stk_args(stk_basic_types);

  assert(reg_basic_types->length() <= 7, "should be less than or equal to 7");

  // Prologue: save FP, LR, x19
  int stk_args_size_in_bytes = align_size_up(stk_args * wordSize, 16);
  int reg_args_size_in_bytes = align_size_up(reg_basic_types->length() * wordSize, 16);
  int frame_size_in_bytes = stk_args_size_in_bytes + reg_args_size_in_bytes + 4 * wordSize;
  masm.write_inst("sub sp, sp, #%d", frame_size_in_bytes);
  masm.write_inst("stp x29, x30, [sp, #%d]", frame_size_in_bytes - 16);
  masm.write_inst("stp xzr, x19, [sp, #%d]", frame_size_in_bytes - 32);
  masm.write_inst("add x29, sp, #%d", frame_size_in_bytes - 16);

  generate_stk_args(&masm, frame_size_in_bytes, stk_basic_types);

    auto *oopmap_set = new OopMapSet();
    int arg_count = 0;
    auto *oopmap = new OopMap(YuhuStack::oopmap_slot_munge(frame_size_in_bytes / wordSize),
                              YuhuStack::oopmap_slot_munge(arg_count));

  // store reg args
  store_reg_args(&masm, stk_args_size_in_bytes, reg_args_size_in_bytes, reg_basic_types, stk_basic_types, oopmap);

    masm.write_insts_set_last_java_frame(YuhuMacroAssembler::sp, YuhuMacroAssembler::noreg, YuhuMacroAssembler::noreg, YuhuMacroAssembler::x9);

    YuhuLabel label;
    masm.write_inst_adr(YuhuMacroAssembler::x9, label);
    masm.write_inst("stp xzr, x9, [sp, #-16]!");

    // Load arguments for resolve_dynamic_call:
    //   x0 = JavaThread* (thread)
    //   x1 = Method* target_method
    //   x2 = Klass* current_klass

    // x0 = thread (x28 holds the thread register per Yuhu convention)
    masm.write_inst_mov_reg(YuhuMacroAssembler::x0, YuhuMacroAssembler::x28);

    // x1 = target_method Method*
    Method* method_ptr = target_method->get_Method();
    int method_metadata_index = cb.oop_recorder()->allocate_metadata_index(method_ptr);
    RelocationHolder method_metadata_rspec = metadata_Relocation::spec(method_metadata_index);
    cb.relocate(masm.current_pc(), method_metadata_rspec);
    masm.write_insts_lea(YuhuMacroAssembler::x1, YuhuAddress((address)method_ptr, relocInfo::metadata_type));

    // x2 = current_klass (the class containing the call, for access checking)
    ciInstanceKlass* ci_current_klass = current_method->holder();
    InstanceKlass* current_klass = ci_current_klass->get_instanceKlass();
    int current_metadata_index = cb.oop_recorder()->allocate_metadata_index(current_klass);
    RelocationHolder current_metadata_rspec = metadata_Relocation::spec(current_metadata_index);
    cb.relocate(masm.current_pc(), current_metadata_rspec);
    masm.write_insts_lea(YuhuMacroAssembler::x2, YuhuAddress((address)current_klass, relocInfo::metadata_type));

    masm.write_insts_lea(YuhuMacroAssembler::x8, YuhuExternalAddress(CAST_FROM_FN_PTR(address, YuhuRuntime::resolve_dynamic_call)));
    masm.write_inst_blr(YuhuMacroAssembler::x8);

    oopmap_set->add_gc_map(masm.current_pc() - begin, oopmap);

    masm.pin_label(label);
    masm.write_inst_mov_reg(YuhuMacroAssembler::x8, YuhuMacroAssembler::x0);

    masm.write_inst("add sp, sp, #16");

    masm.write_insts_reset_last_java_frame(true);

    // load reg args
    load_reg_args(&masm, stk_args_size_in_bytes, reg_basic_types);

  // Load Method* from thread->vm_result_2() into x12 for c2i adapter
  masm.write_inst_ldr(YuhuMacroAssembler::x12,
                      YuhuAddress(YuhuMacroAssembler::x28, in_bytes(JavaThread::vm_result_2_offset())));
  // Clear vm_result_2
  masm.write_inst_str(YuhuMacroAssembler::xzr,
                      YuhuAddress(YuhuMacroAssembler::x28, in_bytes(JavaThread::vm_result_2_offset())));

  // Jump to resolved compiled entry (x8 = verified_code_entry)
  masm.write_inst_blr(YuhuMacroAssembler::x8);

    oopmap_set->add_gc_map(masm.current_pc() - begin, new OopMap(YuhuStack::oopmap_slot_munge(frame_size_in_bytes / wordSize),
                                                                 YuhuStack::oopmap_slot_munge(arg_count)));

  YuhuLabel normal_exit;
  masm.write_inst_b(normal_exit);

  int exception_handler_begin_offset = (int)(masm.current_pc() - begin);

  // x0 contains exception oop, save x0 to pending exception field
  masm.write_inst_str(YuhuMacroAssembler::x0, YuhuAddress(YuhuMacroAssembler::x28, in_bytes(JavaThread::pending_exception_offset())));

  masm.pin_label(normal_exit);

    // save return value and return address
    NOT_PRODUCT(YuhuLabel is_valid_return_address);
    NOT_PRODUCT(masm.write_inst("stp x0, lr, [sp, #-16]!"));
    NOT_PRODUCT(masm.write_inst("ldr x8, [sp, #%d]", frame_size_in_bytes + 8));
    NOT_PRODUCT(masm.write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address, YuhuRuntime::is_yuhu_nmethod), YuhuMacroAssembler::x8));
    NOT_PRODUCT(masm.write_inst_cbnz(YuhuMacroAssembler::x0, is_valid_return_address));
    NOT_PRODUCT(masm.write_insts_stop("invalid return address"));
    NOT_PRODUCT(masm.pin_label(is_valid_return_address));
    NOT_PRODUCT(masm.write_inst("ldp x0, lr, [sp], #16"));

  // Epilogue: restore x19, FP, LR
  masm.write_inst("ldp x29, x30, [sp, #%d]", frame_size_in_bytes - 16);
  masm.write_inst("ldp xzr, x19, [sp, #%d]", frame_size_in_bytes - 32);
  masm.write_inst("add sp, sp, #%d", frame_size_in_bytes);

  // Return
  masm.write_inst("ret");

  masm.flush();

  // Create RuntimeStub
  int frame_size_in_words = frame_size_in_bytes / wordSize;

  YuhuRuntimeStub* stub = YuhuRuntimeStub::new_yuhu_runtime_stub(
      "yuhu_dynamic_call_stub",
      &cb,
      CodeOffsets::frame_never_safe,
      frame_size_in_words,
      oopmap_set,  // collect oops in stack
      false,
      exception_handler_begin_offset
  );

  address stub_addr = stub->entry_point();

  if (YuhuTraceInstalls) {
      if (YuhuStackMapFile != NULL) {
          YUHU_STACK_MAP_LOG("Yuhu: Generated dynamic call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s from current method %s.%s signature %s",
                             p2i(stub_addr),
                             target_method->holder()->name()->as_utf8(),
                             target_method->name()->as_utf8(),
                             target_method->signature()->as_symbol()->as_utf8(),
                             current_method->holder()->name()->as_utf8(),
                             current_method->name()->as_utf8(),
                             current_method->signature()->as_symbol()->as_utf8());
      } else {
          tty->print_cr("Yuhu: Generated dynamic call RuntimeStub at " PTR_FORMAT " for target method %s.%s signature %s from current method %s.%s signature %s",
                        p2i(stub_addr),
                        target_method->holder()->name()->as_utf8(),
                        target_method->name()->as_utf8(),
                        target_method->signature()->as_symbol()->as_utf8(),
                        current_method->holder()->name()->as_utf8(),
                        current_method->name()->as_utf8(),
                        current_method->signature()->as_symbol()->as_utf8());
      }
  }

  // Add to stub cache
  _stub_cache->add(target_method, current_method, YUHUSTUB_DYNAMIC_CALL, stub);

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
address YuhuRuntime::_throw_ArithmeticException_stub = NULL;
address YuhuRuntime::_throw_ArrayIndexOutOfBoundsException_stub = NULL;
address YuhuRuntime::_throw_ClassCastException_stub = NULL;
address YuhuRuntime::_throw_NullPointerException_stub = NULL;
address YuhuRuntime::_throw_StackOverflowError_stub = NULL;

address YuhuRuntime::_safepoint_poll_stub = NULL;

address YuhuRuntime::_handle_deoptimization_stub = NULL;

// Stub cache for sharing YuhuRuntimeStubs
YuhuRuntimeStubHashtable* YuhuRuntime::_stub_cache = NULL;

// ============================================================================
// YuhuRuntimeStubCacheKey implementation
// ============================================================================

void YuhuRuntimeStubCacheKey::init_from(ciMethod* m) {
  klass_name   = m->holder()->name()->get_symbol();
  method_name  = m->name()->get_symbol();
  signature    = m->signature()->get_symbol();
  access_flags = m->flags().as_int();
}

bool YuhuRuntimeStubCacheKey::equals(const YuhuRuntimeStubCacheKey& other) const {
  return klass_name   == other.klass_name   &&
         method_name  == other.method_name  &&
         signature    == other.signature    &&
         access_flags == other.access_flags;
}

unsigned int YuhuRuntimeStubCacheKey::hash() const {
  unsigned int h = (unsigned int)((uintptr_t)klass_name ^ ((uintptr_t)klass_name >> 32));
  h = h * 31 + (unsigned int)((uintptr_t)method_name ^ ((uintptr_t)method_name >> 32));
  h = h * 31 + (unsigned int)((uintptr_t)signature ^ ((uintptr_t)signature >> 32));
  h = h * 31 + (unsigned int)access_flags;
  return h;
}

// ============================================================================
// YuhuRuntimeStubHashtableEntry implementation
// ============================================================================

void YuhuRuntimeStubHashtableEntry::init(ciMethod* target, ciMethod* current, YuhuStubCallType call_type) {
  _target_key.init_from(target);
  _current_key.init_from(current);
  _call_type = call_type;
}

bool YuhuRuntimeStubHashtableEntry::matches(ciMethod* target, ciMethod* current, YuhuStubCallType call_type) {
  YuhuRuntimeStubCacheKey tk, ck;
  tk.init_from(target);
  ck.init_from(current);
  return _target_key.equals(tk) && _current_key.equals(ck) && _call_type == call_type;
}

// ============================================================================
// YuhuRuntimeStubHashtable implementation
// ============================================================================

unsigned int YuhuRuntimeStubHashtable::compute_hash(ciMethod* target, ciMethod* current, YuhuStubCallType call_type) {
  YuhuRuntimeStubCacheKey tk, ck;
  tk.init_from(target);
  ck.init_from(current);
  // Combine both keys and call type into a single hash using Knuth's multiplicative hash
  unsigned int h = tk.hash() ^ (ck.hash() * 2654435761u);
  return h ^ ((unsigned int)call_type * 31);
}

YuhuRuntimeStubHashtableEntry* YuhuRuntimeStubHashtable::new_entry(unsigned int hash,
                                                                    ciMethod* target, ciMethod* current,
                                                                    YuhuStubCallType call_type,
                                                                    YuhuRuntimeStub* stub) {
  YuhuRuntimeStubHashtableEntry* entry =
    (YuhuRuntimeStubHashtableEntry*)BasicHashtable<mtCode>::new_entry(hash);
  entry->init(target, current, call_type);
  entry->set_literal(stub);
  return entry;
}

YuhuRuntimeStub* YuhuRuntimeStubHashtable::find(ciMethod* target, ciMethod* current, YuhuStubCallType call_type) {
  unsigned int hash = compute_hash(target, current, call_type);
  int index = hash_to_index(hash);
  for (YuhuRuntimeStubHashtableEntry* e = (YuhuRuntimeStubHashtableEntry*)bucket(index);
       e != NULL;
       e = (YuhuRuntimeStubHashtableEntry*)e->next()) {
    if (e->hash() == hash && e->matches(target, current, call_type)) {
      return e->literal();
    }
  }
  return NULL;
}

void YuhuRuntimeStubHashtable::add(ciMethod* target, ciMethod* current, YuhuStubCallType call_type, YuhuRuntimeStub* stub) {
  unsigned int hash = compute_hash(target, current, call_type);
  int index = hash_to_index(hash);
  YuhuRuntimeStubHashtableEntry* entry = new_entry(hash, target, current, call_type, stub);
  add_entry(index, entry);
}

// Initialize all VM call stubs
void YuhuRuntime::initialize_vm_stubs() {
  // Initialize stub cache
  _stub_cache = new YuhuRuntimeStubHashtable(1009);  // prime number for better distribution
  _new_instance_stub = generate_vm_stub("yuhu_new_instance_stub", (address) YuhuRuntime::new_instance);
  _newarray_stub = generate_vm_stub("yuhu_newarray_stub", (address) YuhuRuntime::newarray);
  _anewarray_stub = generate_vm_stub("yuhu_anewarray_stub", (address) YuhuRuntime::anewarray);
  _multianewarray_stub = generate_vm_stub("yuhu_multianewarray_stub", (address) YuhuRuntime::multianewarray);
  _monitorenter_stub = generate_vm_stub("yuhu_monitorenter_stub", (address) YuhuRuntime::monitorenter);
  _monitorexit_stub = generate_vm_stub("yuhu_monitorexit_stub", (address) YuhuRuntime::monitorexit);
  _register_finalizer_stub = generate_vm_stub("yuhu_register_finalizer_stub", (address) YuhuRuntime::register_finalizer);
  _find_exception_handler_stub = generate_vm_stub("yuhu_find_exception_handler_stub", (address) YuhuRuntime::find_exception_handler);
  _throw_ArithmeticException_stub = generate_vm_stub("yuhu_throw_ArithmeticException_stub", (address) YuhuRuntime::throw_ArithmeticException);
  _throw_ArrayIndexOutOfBoundsException_stub = generate_vm_stub("yuhu_throw_ArrayIndexOutOfBoundsException_stub", (address) YuhuRuntime::throw_ArrayIndexOutOfBoundsException);
  _throw_ClassCastException_stub = generate_vm_stub("yuhu_throw_ClassCastException_stub", (address) YuhuRuntime::throw_ClassCastException);
  _throw_NullPointerException_stub = generate_vm_stub("yuhu_throw_NullPointerException_stub", (address) YuhuRuntime::throw_NullPointerException);
  _throw_StackOverflowError_stub = generate_vm_stub("yuhu_throw_StackOverflowError_stub", (address) SharedRuntime::throw_StackOverflowError);

  _safepoint_poll_stub = generate_vm_stub("yuhu_safepoint_poll_stub", (address) gc_safepoint_poll);

  _handle_deoptimization_stub = generate_handle_deoptimization_stub();
  
  if (YuhuTraceInstalls) {
    tty->print_cr("Yuhu: VM call stubs initialized");
    tty->print_cr("  new_instance_stub:           " PTR_FORMAT,                  p2i(_new_instance_stub));
    tty->print_cr("  newarray_stub:               " PTR_FORMAT,                  p2i(_newarray_stub));
    tty->print_cr("  anewarray_stub:              " PTR_FORMAT,                  p2i(_anewarray_stub));
    tty->print_cr("  multianewarray_stub:         " PTR_FORMAT,                  p2i(_multianewarray_stub));
    tty->print_cr("  monitorenter_stub:           " PTR_FORMAT,                  p2i(_monitorenter_stub));
    tty->print_cr("  monitorexit_stub:            " PTR_FORMAT,                  p2i(_monitorexit_stub));
    tty->print_cr("  register_finalizer_stub:     " PTR_FORMAT,                  p2i(_register_finalizer_stub));
    tty->print_cr("  find_exception_handler_stub: " PTR_FORMAT,                  p2i(_find_exception_handler_stub));
    tty->print_cr("  throw_ArithmeticException_stub:    " PTR_FORMAT,            p2i(_throw_ArithmeticException_stub));
    tty->print_cr("  throw_ArrayIndexOutOfBoundsException_stub:    " PTR_FORMAT, p2i(_throw_ArrayIndexOutOfBoundsException_stub));
    tty->print_cr("  throw_ClassCastException_stub:    " PTR_FORMAT,             p2i(_throw_ClassCastException_stub));
    tty->print_cr("  throw_NullPointerException_stub:    " PTR_FORMAT,           p2i(_throw_NullPointerException_stub));
    tty->print_cr("  _throw_StackOverflowError_stub:    " PTR_FORMAT,            p2i(_throw_StackOverflowError_stub));
    tty->print_cr("  safepoint_poll_stub:  " PTR_FORMAT,                         p2i(_safepoint_poll_stub));
    tty->print_cr("  handle_deoptimization_stub:  " PTR_FORMAT,                  p2i(_handle_deoptimization_stub));
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
JRT_LEAF(bool, YuhuRuntime::is_yuhu_call_stub(address addr))
  CodeBlob* blob = CodeCache::find_blob(addr);
  return blob != NULL && blob->is_yuhu_runtime_stub();
JRT_END

JRT_LEAF(bool, YuhuRuntime::is_yuhu_nmethod(address addr))
    CodeBlob* blob = CodeCache::find_blob(addr);
    return blob != NULL && blob->is_nmethod() && blob->is_compiled_by_yuhu();
JRT_END

address YuhuRuntime::exception_handler_begin(address addr) {
    CodeBlob* blob = CodeCache::find_blob(addr);
    assert(blob != NULL && blob->is_yuhu_runtime_stub(), "must be yuhu runtime stub");

    YuhuRuntimeStub* stub = (YuhuRuntimeStub*) blob;

    return stub->code_begin() + stub->exception_handler_begin_offset();
}
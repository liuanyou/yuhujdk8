//
// Created by Anyou Liu on 2025/5/6.
//
#include "interpreter/yuhu/yuhu_interpreterGenerator.hpp"
#include "interpreter/yuhu/yuhu_templateTable.hpp"
#include "asm/yuhu/yuhu_macroAssembler.hpp"
#include "interpreter/interpreterRuntime.hpp"

# define __ _masm->

address YuhuInterpreterGenerator::generate_error_exit(const char* msg) {
    address entry = __ current_pc();
    __ write_insts_stop(msg);
    return entry;
}

address YuhuInterpreterGenerator::generate_return_entry_for(TosState state, int step, size_t index_size) {
    address entry = __ pc();
    // Restore stack bottom in case i2c adjusted stack
    __ write_inst("ldr x20, [x29, #%d]", frame::interpreter_frame_last_sp_offset * wordSize);
    // and NULL it as marker that esp is now tos until next java call
    __ write_inst("str xzr, [x29, #%d]", frame::interpreter_frame_last_sp_offset * wordSize);
    __ write_insts_restore_bcp();
    __ write_insts_restore_locals();
    __ write_insts_restore_constant_pool_cache();
    __ write_insts_get_method(__ x12);

    // Pop N words from the stack
    __ write_insts_get_cache_and_index_at_bcp(__ x1, __ x2, 1, index_size);
    __ write_inst("ldr x1, [x1, #%d]", in_bytes(ConstantPoolCache::base_offset() + ConstantPoolCacheEntry::flags_offset()));
    __ write_inst("and x1, x1, #%d", ConstantPoolCacheEntry::parameter_size_mask);

    __ write_inst("add x20, x20, x1, lsl #3");

    // Restore machine SP
    __ write_inst("ldr x8, [x12, #%d]", in_bytes(Method::const_offset()));
    __ write_inst("ldrh w8, [x8, #%d]", in_bytes(ConstMethod::max_stack_offset()));
    __ write_inst("add x8, x8, #%d", frame::interpreter_frame_monitor_size() + 2);
    __ write_inst("ldr x9, [x29, #%d]", frame::interpreter_frame_initial_sp_offset * wordSize);
    __ write_inst("sub x8, x9, w8, uxtw #3");
    __ write_inst("and sp, x8, #-16");

    __ write_insts_get_dispatch();
    __ write_insts_dispatch_next(state, step);

    return entry;
}

address YuhuInterpreterGenerator::generate_deopt_entry_for(TosState state, int step) {
    address entry = __ pc();
    __ write_insts_restore_bcp();
    __ write_insts_restore_locals();
    __ write_insts_restore_constant_pool_cache();
    __ write_insts_get_method(__ x12);
    __ write_insts_get_dispatch();

    // Calculate stack limit
    __ write_inst("ldr x8, [x12, #%d]", in_bytes(Method::const_offset()));
    __ write_inst("ldrh %s, [x8, #%d]", __ w8, in_bytes(ConstMethod::max_stack_offset()));
    __ write_inst("add x8, x8, #%d", frame::interpreter_frame_monitor_size()
                                     + (EnableInvokeDynamic ? 2 : 0));
    __ write_inst("ldr x9, [x29, #%d]", frame::interpreter_frame_initial_sp_offset * wordSize);
    __ write_inst("sub x8, x9, x8, uxtx #3");
    __ write_inst("and sp, x8, #-16");

    // Restore expression stack pointer
    __ write_inst("ldr x20, [x29, #%d]", frame::interpreter_frame_last_sp_offset * wordSize);
    // NULL last_sp until next java call
    __ write_inst("str xzr, [x29, #%d]", frame::interpreter_frame_last_sp_offset * wordSize);

    // handle exceptions
    {
        YuhuLabel L;
        __ write_inst("ldr x8, [x28, #%d]", in_bytes(Thread::pending_exception_offset()));
        __ write_inst_cbz(__ x8, L);
        __ write_insts_final_call_VM(__ noreg,
                   CAST_FROM_FN_PTR(address,
                                    InterpreterRuntime::throw_pending_exception));
        __ write_insts_stop("should not reach here");
        __ pin_label(L);
    }

    __ write_insts_dispatch_next(state, step);
    return entry;
}

void YuhuInterpreterGenerator::generate_and_dispatch(YuhuTemplate* t, TosState tos_out) {
    // TODO
//    if (PrintBytecodeHistogram)                                    histogram_bytecode(t);
//#ifndef PRODUCT
//    // debugging code
//    if (CountBytecodes || TraceBytecodes || StopInterpreterAt > 0) count_bytecode();
//    if (PrintBytecodePairHistogram)                                histogram_bytecode_pair(t);
//    if (TraceBytecodes)                                            trace_bytecode(t);
//    if (StopInterpreterAt > 0)                                     stop_interpreter_at();
//    __ verify_FPU(1, t->tos_in());
//#endif // !PRODUCT
    int step;
    if (!t->does_dispatch()) {
        step = t->is_wide() ? Bytecodes::wide_length_for(t->bytecode()) : Bytecodes::length_for(t->bytecode());
        if (tos_out == ilgl) tos_out = t->tos_out();
        // compute bytecode size
        assert(step > 0, "just checkin'");
        // setup stuff for dispatching next bytecode
        // TODO
//        if (ProfileInterpreter && VerifyDataPointer
//            && MethodData::bytecode_has_profile(t->bytecode())) {
//            __ verify_method_data_pointer();
//        }
        // do nothing
    }
    // generate template
    t->generate(_masm);
    // advance
    if (t->does_dispatch()) {
#ifdef ASSERT
        // make sure execution doesn't go beyond this point if code is broken
        __ write_insts_stop("should not reach here");
#endif // ASSERT
    } else {
        // dispatch to next bytecode
        __ write_insts_dispatch_next(tos_out, step);
    }
}

void YuhuInterpreterGenerator::set_vtos_entry_points(YuhuTemplate* t,
                                                     address& bep,
                                                     address& cep,
                                                     address& sep,
                                                     address& aep,
                                                     address& iep,
                                                     address& lep,
                                                     address& fep,
                                                     address& dep,
                                                     address& vep) {
    assert(t->is_valid() && t->tos_in() == vtos, "illegal template");
    YuhuLabel generate_and_dispatch_label;

    aep = __ current_pc();
    // pre mode
    __ write_inst("str x0, [x20, #-8]!");
    // b #32
    __ write_inst_b(generate_and_dispatch_label);

    fep = __ current_pc();
    __ write_inst("str s0, [x20, #-8]!");
    // b #24
    __ write_inst_b(generate_and_dispatch_label);

    dep = __ current_pc();
    __ write_inst("str d0, [x20, #-16]!");
    // b #16
    __ write_inst_b(generate_and_dispatch_label);

    lep = __ current_pc();
    __ write_inst("str x0, [x20, #-16]!");
    // b #8
    __ write_inst_b(generate_and_dispatch_label);

    bep = cep = sep =
    iep = __ current_pc();
    __ write_inst("str w0, [x20, #-8]!");

    vep = __ current_pc();
    __ pin_label(generate_and_dispatch_label);
    generate_and_dispatch(t);
}

void YuhuInterpreterGenerator::set_short_entry_points(YuhuTemplate* t, address& bep, address& cep, address& sep, address& aep, address& iep, address& lep, address& fep, address& dep, address& vep) {
    assert(t->is_valid(), "template must exist");
    switch (t->tos_in()) {
        case btos:
        case ctos:
        case stos:
            ShouldNotReachHere();  // btos/ctos/stos should use itos.
            break;
        case atos:
            vep = __ current_pc();
            __ write_insts_pop(atos);
//            __ write_inst("ldr x0, [x20], #8");
            aep = __ current_pc();
            generate_and_dispatch(t);
            break;
        case itos:
            vep = __ current_pc();
            __ write_insts_pop(itos);
//            __ write_inst("ldr w0, [x20], #8");
            iep = __ current_pc();
            generate_and_dispatch(t);
            break;
        case ltos:
            vep = __ current_pc();
            __ write_insts_pop(ltos);
//            __ write_inst("ldr x0, [x20], #16");
            lep = __ current_pc();
            generate_and_dispatch(t);
            break;
        case ftos:
            vep = __ current_pc();
            __ write_insts_pop(ftos);
//            __ write_inst("ldr s0, [x20], #8");
            fep = __ current_pc();
            generate_and_dispatch(t);
            break;
        case dtos:
            vep = __ current_pc();
            __ write_insts_pop(dtos);
//            __ write_inst("ldr d0, [x20], #16");
            dep = __ current_pc();
            generate_and_dispatch(t);
            break;
        case vtos:
            set_vtos_entry_points(t, bep, cep, sep, aep, iep, lep, fep, dep, vep);
            break;
        default  :
            ShouldNotReachHere();
            break;
    }
}

void YuhuInterpreterGenerator::set_wide_entry_point(YuhuTemplate* t, address& wep) {
    assert(t->is_valid(), "template must exist");
    assert(t->tos_in() == vtos, "only vtos tos_in supported for wide instructions");
    wep = __ current_pc();
    generate_and_dispatch(t);
}

address YuhuInterpreterGenerator::generate_method_entry(
        YuhuInterpreter::MethodKind kind) {
    // determine code generation flags
    bool synchronized = false;
    address entry_point = NULL;

    switch (kind) {
        case YuhuInterpreter::zerolocals             :                                                                             break;
        case YuhuInterpreter::zerolocals_synchronized: synchronized = true;                                                        break;
        case YuhuInterpreter::native                 : entry_point = ((YuhuInterpreterGenerator*) this)->generate_native_entry(false); break;
        case YuhuInterpreter::native_synchronized    : entry_point = ((YuhuInterpreterGenerator*) this)->generate_native_entry(true);  break;
        case YuhuInterpreter::empty                  : entry_point = ((YuhuInterpreterGenerator*) this)->generate_empty_entry();       break;
        case YuhuInterpreter::accessor               : entry_point = ((YuhuInterpreterGenerator*) this)->generate_accessor_entry();    break;
        case YuhuInterpreter::abstract               : entry_point = ((YuhuInterpreterGenerator*) this)->generate_abstract_entry();    break;
//
        case YuhuInterpreter::java_lang_math_sin     : // fall thru
        case YuhuInterpreter::java_lang_math_cos     : // fall thru
        case YuhuInterpreter::java_lang_math_tan     : // fall thru
        case YuhuInterpreter::java_lang_math_abs     : // fall thru
        case YuhuInterpreter::java_lang_math_log     : // fall thru
        case YuhuInterpreter::java_lang_math_log10   : // fall thru
        case YuhuInterpreter::java_lang_math_sqrt    : // fall thru
        case YuhuInterpreter::java_lang_math_pow     : // fall thru
        case YuhuInterpreter::java_lang_math_exp     : entry_point = ((YuhuInterpreterGenerator*) this)->generate_math_entry(kind);    break;
        case YuhuInterpreter::java_lang_ref_reference_get
            : entry_point = ((YuhuInterpreterGenerator*)this)->generate_Reference_get_entry(); break;
        case YuhuInterpreter::java_util_zip_CRC32_update
            : entry_point = ((YuhuInterpreterGenerator*)this)->generate_CRC32_update_entry();  break;
        case YuhuInterpreter::java_util_zip_CRC32_updateBytes
            : // fall thru
        case YuhuInterpreter::java_util_zip_CRC32_updateByteBuffer
            : entry_point = ((YuhuInterpreterGenerator*)this)->generate_CRC32_updateBytes_entry(kind); break;
        default                                  : __ write_insts_stop("should not reach here");                                                       break;
    }

    if (entry_point) {
        return entry_point;
    }

    return ((YuhuInterpreterGenerator*) this)->
    generate_normal_entry(synchronized);
}

address YuhuInterpreterGenerator::generate_normal_entry(bool synchronized) {
    // TODO
//    // determine code generation flags
//    bool inc_counter  = UseCompiler || CountCompiledCalls;
//
//    // rscratch1: sender sp
    address entry_point = __ current_pc();
//
//    const Address constMethod(rmethod, Method::const_offset());
//    const Address access_flags(rmethod, Method::access_flags_offset());
//    const Address size_of_parameters(r3,
//                                     ConstMethod::size_of_parameters_offset());
//    const Address size_of_locals(r3, ConstMethod::size_of_locals_offset());

    // get parameter size (always needed)
    // need to load the const method first
    __ write_inst("ldr x3, [x12, #%d]", in_bytes(Method::const_offset()));
    __ write_inst("ldrh w2, [x3, #%d]", in_bytes(ConstMethod::size_of_parameters_offset()));

    // r2: size of parameters
    // get size of locals in words
    __ write_inst("ldrh w3, [x3, #%d]", in_bytes(ConstMethod::size_of_locals_offset()));
    __ write_inst("sub x3, x3, x2"); // r3 = no. of additional locals

    // see if we've got enough room on the stack for locals plus overhead.
    generate_stack_overflow_check();

    // compute beginning of parameters (rlocals)
    __ write_inst("add x24, x20, x2, uxtx #3");
    __ write_inst("sub x24, x24, #0x8");

    // Make room for locals
    __ write_inst("sub x8, x20, x3, uxtx #3");
    __ write_inst("and sp, x8, #-16"); // sp-adjustment

    // r3 - # of additional locals
    // allocate space for locals
    // explicitly initialize locals
    {
        YuhuLabel exit, loop;
        __ write_inst("ands xzr, x3, x3");
        __ write_inst_b(__ le, exit); // do nothing if r3 <= 0
        __ pin_label(loop);
        __ write_inst("str xzr, [x8], #%d", wordSize);
        __ write_inst("sub x3, x3, 1"); // until everything initialized
        __ write_inst_cbnz(__ x3, loop);
        __ pin_label(exit);
    }

    // And the base dispatch table
    __ write_insts_get_dispatch();

    // initialize fixed part of activation frame
    generate_fixed_frame(false);

    // make sure method is not native & not abstract
#ifdef ASSERT
    __ write_inst("ldr w0, [x12, #%d]", in_bytes(Method::access_flags_offset()));
  {
    YuhuLabel L;
    __ write_inst("tst x0, #%d", JVM_ACC_NATIVE);
    __ write_inst_b(__ eq, L);
    __ write_insts_stop("tried to execute native method as non-native");
    __ pin_label(L);
  }
 {
    YuhuLabel L;
    __ write_inst("tst x0, #%d", JVM_ACC_ABSTRACT);
    __ write_inst_b(__ eq, L);
    __ write_insts_stop("tried to execute abstract method in interpreter");
    __ pin_label(L);
  }
#endif

    // Since at this point in the method invocation the exception
    // handler would try to exit the monitor of synchronized methods
    // which hasn't been entered yet, we set the thread local variable
    // _do_not_unlock_if_synchronized to true. The remove_activation
    // will check this flag.

//    const Address do_not_unlock_if_synchronized(rthread,
//                                                in_bytes(JavaThread::do_not_unlock_if_synchronized_offset()));
    __ write_inst("mov x9, #1");
    __ write_inst("strb w9, [x28, #%d]", in_bytes(JavaThread::do_not_unlock_if_synchronized_offset()));

    // TODO
//    // increment invocation count & check for overflow
//    Label invocation_counter_overflow;
//    Label profile_method;
//    Label profile_method_continue;
//    if (inc_counter) {
//        generate_counter_incr(&invocation_counter_overflow,
//                              &profile_method,
//                              &profile_method_continue);
//        if (ProfileInterpreter) {
//            __ bind(profile_method_continue);
//        }
//    }
//
//    Label continue_after_compile;
//    __ bind(continue_after_compile);

    bang_stack_shadow_pages(false);

    // reset the _do_not_unlock_if_synchronized flag
    __ write_inst("strb wzr, [x28, #%d]", in_bytes(JavaThread::do_not_unlock_if_synchronized_offset()));

    // check for synchronized methods
    // Must happen AFTER invocation_counter check and stack overflow check,
    // so method is not locked if overflows.
    if (synchronized) {
        // Allocate monitor and lock method
        lock_method();
    } else {
        // no synchronization necessary
#ifdef ASSERT
        {
      YuhuLabel L;
      __ write_inst("ldr w0, [x12, #%d]", in_bytes(Method::access_flags_offset()));
      __ write_inst("tst x0, #%d", JVM_ACC_SYNCHRONIZED);
      __ write_inst_b(__ eq, L);
      __ write_insts_stop("method needs synchronization");
      __ pin_label(L);
    }
#endif
    }

    // start execution
#ifdef ASSERT
    {
    YuhuLabel L;
//     const Address monitor_block_top (rfp,
//                 frame::interpreter_frame_monitor_block_top_offset * wordSize);
    __ write_inst("ldr x8, [x29, #%d]", frame::interpreter_frame_monitor_block_top_offset * wordSize);
    __ write_inst("cmp x20, x8");
    __ write_inst_b(__ eq, L);
    __ write_insts_stop("broken stack frame setup in interpreter");
    __ pin_label(L);
  }
#endif

    // TODO
//    // jvmti support
//    __ notify_method_entry();


    __ write_insts_dispatch_next(vtos);

    // TODO
//    // invocation counter overflow
//    if (inc_counter) {
//        if (ProfileInterpreter) {
//            // We have decided to profile this method in the interpreter
//            __ bind(profile_method);
//            __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::profile_method));
//            __ set_method_data_pointer_for_bcp();
//            // don't think we need this
//            __ get_method(r1);
//            __ b(profile_method_continue);
//        }
//        // Handle overflow of counter and compile method
//        __ bind(invocation_counter_overflow);
//        generate_counter_overflow(&continue_after_compile);
//    }

    return entry_point;
}

address YuhuInterpreterGenerator::generate_native_entry(bool synchronized) {
    // TODO
//    // determine code generation flags
//    bool inc_counter  = UseCompiler || CountCompiledCalls;

    // r1: Method*
    // rscratch1: sender sp

    address entry_point = __ current_pc();

//    const Address constMethod       (rmethod, Method::const_offset());
//    const Address access_flags      (rmethod, Method::access_flags_offset());
//    const Address size_of_parameters(r2, ConstMethod::
//    size_of_parameters_offset());

    // get parameter size (always needed)
    __ write_inst("ldr x2, [x12, #%d]", in_bytes(Method::const_offset()));
    __ write_inst("ldrh w2, [x2, #%d]", in_bytes(ConstMethod::size_of_parameters_offset()));

    // native calls don't need the stack size check since they have no
    // expression stack and the arguments are already on the stack and
    // we only add a handful of words to the stack

    // rmethod: Method*
    // r2: size of parameters
    // rscratch1: sender sp

    // for natives the size of locals is zero

    // compute beginning of parameters (rlocals)
    __ write_inst("add x24, x20, x2, uxtx #3");
    __ write_inst("add x24, x24, #-8");

    // Pull SP back to minimum size: this avoids holes in the stack
    __ write_inst("and sp, x20, #-16");

    // initialize fixed part of activation frame
    generate_fixed_frame(true);

    // make sure method is native & not abstract
#ifdef ASSERT
    __ write_inst("ldr w0, [x12, #%d]", in_bytes(Method::access_flags_offset()));
    {
        YuhuLabel L;
        __ write_inst("tst x0, #%d", JVM_ACC_NATIVE);
        __ write_inst_b(__ ne, L);
        __ write_insts_stop("tried to execute non-native method as native");
        __ pin_label(L);
    }
    {
        YuhuLabel L;
        __ write_inst("tst x0, #%d", JVM_ACC_ABSTRACT);
        __ write_inst_b(__ eq, L);
        __ write_insts_stop("tried to execute abstract method in interpreter");
        __ pin_label(L);
    }
#endif

    // Since at this point in the method invocation the exception
    // handler would try to exit the monitor of synchronized methods
    // which hasn't been entered yet, we set the thread local variable
    // _do_not_unlock_if_synchronized to true. The remove_activation
    // will check this flag.

//    const Address do_not_unlock_if_synchronized(rthread,
//                                                in_bytes(JavaThread::do_not_unlock_if_synchronized_offset()));
    __ write_insts_mov_imm64(__ x9, true);
    __ write_inst("strb w9, [x28, #%d]", in_bytes(JavaThread::do_not_unlock_if_synchronized_offset()));

    // TODO
//    // increment invocation count & check for overflow
//    Label invocation_counter_overflow;
//    if (inc_counter) {
//        generate_counter_incr(&invocation_counter_overflow, NULL, NULL);
//    }

    YuhuLabel continue_after_compile;
    __ pin_label(continue_after_compile);

    bang_stack_shadow_pages(true);

    // reset the _do_not_unlock_if_synchronized flag
    __ write_inst("strb wzr, [x28, #%d]", in_bytes(JavaThread::do_not_unlock_if_synchronized_offset()));

    // check for synchronized methods
    // Must happen AFTER invocation_counter check and stack overflow check,
    // so method is not locked if overflows.
    if (synchronized) {
        lock_method();
    } else {
        // no synchronization necessary
#ifdef ASSERT
        {
            YuhuLabel L;
            __ write_inst("ldr w0, [x12, #%d]", in_bytes(Method::access_flags_offset()));
            __ write_inst("tst x0, #%d", JVM_ACC_SYNCHRONIZED);
            __ write_inst_b(__ eq, L);
            __ write_insts_stop("method needs synchronization");
            __ pin_label(L);
        }
#endif
    }

    // start execution
#ifdef ASSERT
    {
        YuhuLabel L;
//        const Address monitor_block_top(rfp,
//                                        frame::interpreter_frame_monitor_block_top_offset * wordSize);
        __ write_inst("ldr x8, [x29, #%d]", frame::interpreter_frame_monitor_block_top_offset * wordSize);
        __ write_inst("cmp x20, x8");
        __ write_inst_b(__ eq, L);
        __ write_insts_stop("broken stack frame setup in interpreter");
        __ pin_label(L);
    }
#endif

    // TODO
//    // jvmti support
//    __ notify_method_entry();

    // work registers
    const YuhuMacroAssembler::YuhuRegister t = __ x17;
    const YuhuMacroAssembler::YuhuRegister result_handler = __ x19;

    // allocate space for parameters
    __ write_inst("ldr %s, [x12, #%d]", t, in_bytes(Method::const_offset()));
    __ write_inst("ldrh %s, [%s, #%d]", __ w_reg(t), t, in_bytes(ConstMethod::size_of_parameters_offset()));

    __ write_inst("sub x8, x20, %s, uxtx #%d", t, Interpreter::logStackElementSize);
    __ write_inst("and sp, x8, #-16");
    __ write_inst_mov_reg(__ x20, __ x8);

    // get signature handler
    {
        YuhuLabel L;
        __ write_inst("ldr %s, [x12, #%d]", t, in_bytes(Method::signature_handler_offset()));
        __ write_inst_cbnz(t, L);
        __ write_insts_final_call_VM(__ noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::prepare_native_call),
                   __ x12);
        __ write_inst("ldr %s, [x12, #%d]", t, in_bytes(Method::signature_handler_offset()));
        __ pin_label(L);
    }

    // call signature handler
//    assert(InterpreterRuntime::SignatureHandlerGenerator::from() == rlocals,
//           "adjust this code");
//    assert(InterpreterRuntime::SignatureHandlerGenerator::to() == sp,
//           "adjust this code");
//    assert(InterpreterRuntime::SignatureHandlerGenerator::temp() == rscratch1,
//           "adjust this code");

    // The generated handlers do not touch rmethod (the method).
    // However, large signatures cannot be cached and are generated
    // each time here.  The slow-path generator can do a GC on return,
    // so we must reload it after the call.
    __ write_inst_blr(t);
    __ write_insts_get_method(__ x12);        // slow path can do a GC, reload rmethod


    // result handler is in r0
    // set result handler
    __ write_inst_mov_reg(result_handler, __ x0);
    // pass mirror handle if static call
    {
        YuhuLabel L;
        const int mirror_offset = in_bytes(Klass::java_mirror_offset());
        __ write_inst("ldr %s, [x12, #%d]", __ w_reg(t), in_bytes(Method::access_flags_offset()));
        __ write_inst("tst %s, #%d", t, JVM_ACC_STATIC);
        __ write_inst_b(__ eq, L);
        // get mirror
        __ write_inst("ldr %s, [x12, #%d]", t, in_bytes(Method::const_offset()));
        __ write_inst("ldr %s, [%s, #%d]", t, t, in_bytes(ConstMethod::constants_offset()));
        __ write_inst("ldr %s, [%s, #%d]", t, t, ConstantPool::pool_holder_offset_in_bytes());
        __ write_inst("ldr %s, [%s, #%d]", t, t, mirror_offset);
        // copy mirror into activation frame
        __ write_inst("str %s, [x29, #%d]", t, frame::interpreter_frame_oop_temp_offset * wordSize);
        // pass handle to mirror
        __ write_inst("add x1, x29, #%d", frame::interpreter_frame_oop_temp_offset * wordSize);
        __ pin_label(L);
    }

    // get native function entry point in r10
    {
        YuhuLabel L;
        __ write_inst("ldr x10, [x12, #%d]", in_bytes(Method::native_function_offset()));
        address unsatisfied = (SharedRuntime::native_method_throw_unsatisfied_link_error_entry());
        __ write_insts_mov_imm64(__ x9, (uint64_t) unsatisfied);
        __ write_inst("ldr x9, [x9]");
        __ write_inst("cmp x10, x9");
        __ write_inst_b(__ ne, L);
        __ write_insts_final_call_VM(__ noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::prepare_native_call),
                   __ x12);
        __ write_insts_get_method(__ x12);
        __ write_inst("ldr x10, [x12, #%d]", in_bytes(Method::native_function_offset()));
        __ pin_label(L);
    }

    // pass JNIEnv
    __ write_inst("add x0, x28, #%d", in_bytes(JavaThread::jni_environment_offset()));

    // Set the last Java PC in the frame anchor to be the return address from
    // the call to the native method: this will allow the debugger to
    // generate an accurate stack trace.
    YuhuLabel native_return;
    __ write_insts_set_last_java_frame(__ x20, __ x29, native_return, __ x8);

    // change thread state
#ifdef ASSERT
    {
        YuhuLabel L;
        __ write_inst("ldr %s, [x28, #%d]", __ w_reg(t), in_bytes(JavaThread::thread_state_offset()));
        __ write_inst("cmp %s, #%d", t, _thread_in_Java);
        __ write_inst_b(__ eq, L);
        __ write_insts_stop("Wrong thread state in native stub");
        __ pin_label(L);
    }
#endif

    // Change state to native
    __ write_insts_mov_imm64(__ x8, _thread_in_native);
    __ write_insts_lea(__ x9, YuhuAddress(__ x28, JavaThread::thread_state_offset()));
    __ write_inst_regs("stlr %s, [%s]", __ w8, __ x9);

    // Call the native method.
    __ write_inst_blr(__ x10);
    __ pin_label(native_return);
    __ write_inst("isb");
    __ write_insts_get_method(__ x12);
    // result potentially in r0 or v0

    // make room for the pushes we're about to do
    __ write_inst("sub x8, x20, #%d", 4 * wordSize);
    __ write_inst("and sp, x8, #-16");

    // NOTE: The order of these pushes is known to frame::interpreter_frame_result
    // in order to extract the result of a method call. If the order of these
    // pushes change or anything else is added to the stack then the code in
    // interpreter_frame_result must also change.
    __ write_insts_push(dtos);
    __ write_insts_push(ltos);

    // change thread state
    __ write_insts_mov_imm64(__ x8, _thread_in_native_trans);
    __ write_insts_lea(__ x9, YuhuAddress(__ x28, JavaThread::thread_state_offset()));
    __ write_inst_regs("stlr %s, [%s]", __ w8, __ x9);

    if (os::is_MP()) {
        if (UseMembar) {
            // Force this write out before the read below
            __ write_inst("dsb sy");
        } else {
            // Write serialization page so VM thread can do a pseudo remote membar.
            // We use the current thread pointer to calculate a thread specific
            // offset to write to within the page. This minimizes bus traffic
            // due to cache line collision.
            __ write_inst("dsb sy"); // __ serialize_memory(rthread, rscratch2);
        }
    }

    // check for safepoint operation in progress and/or pending suspend requests
    {
        YuhuLabel Continue;
        {
            uint64_t offset;
            __ write_insts_adrp(__ x9, SafepointSynchronize::address_of_state(), offset);
            __ write_inst("ldr %s, [x9, #%d]", __ w9, offset);
        }
        assert(SafepointSynchronize::_not_synchronized == 0,
               "SafepointSynchronize::_not_synchronized");
        YuhuLabel L;
        __ write_inst_cbnz(__ x9, L);
        __ write_inst("ldr %s, [x28, #%d]", __ w9, in_bytes(JavaThread::suspend_flags_offset()));
        __ write_inst_cbz(__ x9, Continue);
        __ pin_label(L);

        // Don't use call_VM as it will see a possible pending exception
        // and forward it and never return here preventing us from
        // clearing _last_native_pc down below.  Also can't use
        // call_VM_leaf either as it will check to see if r13 & r14 are
        // preserved and correspond to the bcp/locals pointers. So we do a
        // runtime call by hand.
        //
        __ write_inst("mov x0, x28");
        __ write_insts_mov_imm64(__ x9, (uint64_t) CAST_FROM_FN_PTR(address, JavaThread::check_special_condition_for_native_trans));
        __ write_inst_blr(__ x9);
        __ write_inst("isb");
        __ write_insts_get_method(__ x12);
        // TODO
//        __ reinit_heapbase();
        __ pin_label(Continue);
    }

    // change thread state
    __ write_insts_mov_imm64(__ x8, _thread_in_Java);
    __ write_insts_lea(__ x29, YuhuAddress(__ x28, JavaThread::thread_state_offset()));
    __ write_inst_regs("stlr %s, [%s]", __ w8, __ x9);

    // reset_last_Java_frame
    __ write_insts_reset_last_java_frame(true);

    // reset handle block
    __ write_inst("ldr %s, [x28, #%d]", t, in_bytes(JavaThread::active_handles_offset()));
    __ write_inst("str xzr, [%s, #%d]", t, JNIHandleBlock::top_offset_in_bytes());

    // If result is an oop unbox and store it in frame where gc will see it
    // and result handler will pick it up

    {
        YuhuLabel no_oop, not_weak, store_result;
        __ write_inst_adr(t, AbstractInterpreter::result_handler(T_OBJECT));
        __ write_inst_regs("cmp %s, %s", t, result_handler);
        __ write_inst_b(__ ne, no_oop);
        // Unbox oop result, e.g. JNIHandles::resolve result.
        __ write_insts_pop(ltos);
        __ write_inst_cbz(__ x0, store_result); // Use NULL as-is.
//    STATIC_ASSERT(JNIHandles::weak_tag_mask == 1u);
        __ write_inst_tbz(__ x0, 0, not_weak); // Test for jweak tag.
        // Resolve jweak.
//    __ ldr(r0, Address(r0, -JNIHandles::weak_tag_value));
#if INCLUDE_ALL_GCS
        if (UseG1GC) {
            __ write_insts_enter(); // Barrier may call runtime.
            __ write_insts_g1_write_barrier_pre(__ noreg /* obj */,
                                    __ x0 /* pre_val */,
                                    __ x28 /* thread */,
                                    t /* tmp */,
                                    true /* tosca_live */,
                                    true /* expand_call */);
            __ write_insts_leave();
        }
#endif // INCLUDE_ALL_GCS
        __ write_inst_b(store_result);
        __ pin_label(not_weak);
        // Resolve (untagged) jobject.
        __ write_inst("ldr x0, [x0]");
        __ pin_label(store_result);
        __ write_inst("str x0, [%s, #%d]", __ x29, frame::interpreter_frame_oop_temp_offset*wordSize);
        // keep stack depth as expected by pushing oop which will eventually be discarded
        __ write_insts_push(ltos);
        __ pin_label(no_oop);
    }

    {
        YuhuLabel no_reguard;
        __ write_insts_lea(__ x8, YuhuAddress(__ x28, in_bytes(JavaThread::stack_guard_state_offset())));
        __ write_inst_regs("ldrb %s, [%s]", __ w8, __ x8);
        __ write_inst("cmp x8, #%d", JavaThread::stack_guard_yellow_disabled);
        __ write_inst_b(__ ne, no_reguard);

        __ write_insts_pusha(); // XXX only save smashed registers
        __ write_inst_mov_reg(__ x0, __ x28);
        __ write_insts_mov_imm64(__ x9, (uint64_t) CAST_FROM_FN_PTR(address, SharedRuntime::reguard_yellow_pages));
        __ write_inst_blr(__ x9);
        __ write_insts_popa(); // XXX only restore smashed registers
        __ pin_label(no_reguard);
    }

    // The method register is junk from after the thread_in_native transition
    // until here.  Also can't call_VM until the bcp has been
    // restored.  Need bcp for throwing exception below so get it now.
    __ write_insts_get_method(__ x12);

    // restore bcp to have legal interpreter frame, i.e., bci == 0 <=>
    // rbcp == code_base()
    __ write_inst("ldr x22, [x12, #%d]", in_bytes(Method::const_offset())); // get ConstMethod*
    __ write_inst("add x22, x22, #%d", in_bytes(ConstMethod::codes_offset())); // get codebase
    // handle exceptions (exception handling will handle unlocking!)
    {
        YuhuLabel L;
        __ write_inst("ldr x8, [x28, #%d]", in_bytes(Thread::pending_exception_offset()));
        __ write_inst_cbz(__ x8, L);
        // Note: At some point we may want to unify this with the code
        // used in call_VM_base(); i.e., we should use the
        // StubRoutines::forward_exception code. For now this doesn't work
        // here because the rsp is not correctly set at this point.
        __ write_insts_final_call_VM(__ noreg,
                                   CAST_FROM_FN_PTR(address,
                                                    InterpreterRuntime::throw_pending_exception));
        __ write_insts_stop("should not reach here");
        __ pin_label(L);
    }

    // do unlocking if necessary
    {
        YuhuLabel L;
        __ write_inst("ldr %s, [x12, #%d]", __ w_reg(t), in_bytes(Method::access_flags_offset()));
        __ write_inst("tst %s, #%d", t, JVM_ACC_SYNCHRONIZED);
        __ write_inst_b(__ eq, L);
        // the code below should be shared with interpreter macro
        // assembler implementation
        {
            YuhuLabel unlock;
            // BasicObjectLock will be first in list, since this is a
            // synchronized method. However, need to check that the object
            // has not been unlocked by an explicit monitorexit bytecode.

            // monitor expect in c_rarg1 for slow unlock path
            __ write_insts_lea(__ x1, YuhuAddress(__ x29,
                                                  (intptr_t)(frame::interpreter_frame_initial_sp_offset * wordSize
                                                  - sizeof(BasicObjectLock)))); // address of first monitor

            __ write_inst("ldr %s, [x1, #%d]", t, BasicObjectLock::obj_offset_in_bytes());
            __ write_inst_cbnz(t, unlock);

            // Entry already unlocked, need to throw exception
            __ write_insts_final_call_VM(__ noreg,
                                       CAST_FROM_FN_PTR(address,
                                                        InterpreterRuntime::throw_illegal_monitor_state_exception));
            __ write_insts_stop("should not reach here");

            __ pin_label(unlock);
            __ write_insts_unlock_object(__ x1);
        }
        __ pin_label(L);
    }

    // jvmti support
    // Note: This must happen _after_ handling/throwing any exceptions since
    //       the exception handler code notifies the runtime of method exits
    //       too. If this happens before, method entry/exit notifications are
    //       not properly paired (was bug - gri 11/22/99).
    // TODO
//    __ notify_method_exit(vtos, InterpreterMacroAssembler::NotifyJVMTI);

    // restore potential result in r0:d0, call result handler to
    // restore potential result in ST0 & handle result

    __ write_insts_pop(ltos);
    __ write_insts_pop(dtos);

    __ write_inst_blr(result_handler);

    // remove activation
    __ write_inst("ldr x20, [x29, #%d]", frame::interpreter_frame_sender_sp_offset *
                                         wordSize); // get sender sp
    // remove frame anchor
    __ write_insts_leave();

    // resture sender sp
    __ write_inst("mov sp, x20");

    __ write_inst("ret");

    // TODO
//    if (inc_counter) {
//        // Handle overflow of counter and compile method
//        __ bind(invocation_counter_overflow);
//        generate_counter_overflow(&continue_after_compile);
//    }

    return entry_point;
}

address YuhuInterpreterGenerator::generate_empty_entry(void) {
    // rmethod: Method*
    // r13: sender sp must set sp to this value on return

    if (!UseFastEmptyMethods) {
        return NULL;
    }

    address entry_point = __ current_pc();

    // If we need a safepoint check, generate full interpreter entry.
    YuhuLabel slow_path;
    {
        uint64_t offset;
        assert(SafepointSynchronize::_not_synchronized == 0,
               "SafepointSynchronize::_not_synchronized");
        __ write_insts_adrp(__ x9, SafepointSynchronize::address_of_state(), offset);
        __ write_inst("ldr w9, [x9, #%d]", offset);
        __ write_inst_cbnz(__ x9, slow_path);
    }

    // do nothing for empty methods (do not even increment invocation counter)
    // Code: _return
    // _return
    // return w/o popping parameters
    __ write_inst_mov_reg(__ sp, __ x13); // Restore caller's SP
    __ write_inst_br(__ lr);

    __ pin_label(slow_path);
    (void) generate_normal_entry(false);
    return entry_point;
}

address YuhuInterpreterGenerator::generate_accessor_entry(void) {
    return NULL;
}

address YuhuInterpreterGenerator::generate_abstract_entry(void) {
    // rmethod: Method*
    // r13: sender SP

    address entry_point = __ current_pc();

    // abstract method entry

    //  pop return address, reset last_sp to NULL
    __ write_insts_empty_expression_stack();
    __ write_insts_restore_bcp();      // bcp must be correct for exception handler   (was destroyed)
    __ write_insts_restore_locals();   // make sure locals pointer is correct as well (was destroyed)

    // throw exception
    __ write_insts_final_call_VM(__ noreg, CAST_FROM_FN_PTR(address,
                                       InterpreterRuntime::throw_AbstractMethodError));
    // the call_VM checks for exception, so we should never return here.
    __ write_insts_stop("should not reach here");

    return entry_point;
}

address YuhuInterpreterGenerator::generate_math_entry(YuhuInterpreter::MethodKind kind) {
    // rmethod: Method*
    // r13: sender sp
    // esp: args

    if (!InlineIntrinsics) return NULL; // Generate a vanilla entry

    // These don't need a safepoint check because they aren't virtually
    // callable. We won't enter these intrinsics from compiled code.
    // If in the future we added an intrinsic which was virtually callable
    // we'd have to worry about how to safepoint so that this code is used.

    // mathematical functions inlined by compiler
    // (interpreter must provide identical implementation
    // in order to avoid monotonicity bugs when switching
    // from interpreter to compiler in the middle of some
    // computation)
    //
    // stack:
    //        [ arg ] <-- esp
    //        [ arg ]
    // retaddr in lr

    address entry_point = NULL;
    YuhuMacroAssembler::YuhuRegister continuation = __ lr;
    switch (kind) {
        case YuhuInterpreter::java_lang_math_abs:
            entry_point = __ current_pc();
            __ write_inst("ldr d0, [x20]");
            __ write_inst("fabs d0, d0");
            __ write_inst("mov sp, x13"); // Restore caller's SP
            break;
        case YuhuInterpreter::java_lang_math_sqrt:
            entry_point = __ current_pc();
            __ write_inst("ldr d0, [x20]");
            __ write_inst("fsqrt d0, d0");
            __ write_inst("mov sp, x13");
            break;
        case YuhuInterpreter::java_lang_math_sin :
        case YuhuInterpreter::java_lang_math_cos :
        case YuhuInterpreter::java_lang_math_tan :
        case YuhuInterpreter::java_lang_math_log :
        case YuhuInterpreter::java_lang_math_log10 :
        case YuhuInterpreter::java_lang_math_exp :
            entry_point = __ current_pc();
            __ write_inst("ldr d0, [x20]");
            __ write_inst("mov sp, x13");
            __ write_inst("mov x19, lr");
            continuation = __ x19;  // The first callee-saved register
            generate_transcendental_entry(kind, 1);
            break;
        case YuhuInterpreter::java_lang_math_pow :
            entry_point = __ current_pc();
            __ write_inst("mov x19, lr");
            continuation = __ x19;
            __ write_inst("ldr d0, [x20, #%d]", 2 * Interpreter::stackElementSize);
            __ write_inst("ldr d1, [x20]");
            __ write_inst("mov sp, x13");
            generate_transcendental_entry(kind, 2);
            break;
        default:
            ;
    }
    if (entry_point) {
        __ write_inst_br(continuation);
    }

    return entry_point;
}

void YuhuInterpreterGenerator::generate_transcendental_entry(YuhuInterpreter::MethodKind kind, int fpargs) {
    address fn;
    switch (kind) {
        case YuhuInterpreter::java_lang_math_sin :
            fn = CAST_FROM_FN_PTR(address, SharedRuntime::dsin);
            break;
        case YuhuInterpreter::java_lang_math_cos :
            fn = CAST_FROM_FN_PTR(address, SharedRuntime::dcos);
            break;
        case YuhuInterpreter::java_lang_math_tan :
            fn = CAST_FROM_FN_PTR(address, SharedRuntime::dtan);
            break;
        case YuhuInterpreter::java_lang_math_log :
            fn = CAST_FROM_FN_PTR(address, SharedRuntime::dlog);
            break;
        case YuhuInterpreter::java_lang_math_log10 :
            fn = CAST_FROM_FN_PTR(address, SharedRuntime::dlog10);
            break;
        case YuhuInterpreter::java_lang_math_exp :
            fn = CAST_FROM_FN_PTR(address, SharedRuntime::dexp);
            break;
        case YuhuInterpreter::java_lang_math_pow :
            fn = CAST_FROM_FN_PTR(address, SharedRuntime::dpow);
            break;
        default:
            ShouldNotReachHere();
    }
    __ write_insts_mov_imm64(__ x8, (uint64_t) fn);
    __ write_inst_blr(__ x8);
}

address YuhuInterpreterGenerator::generate_Reference_get_entry(void) {
#if INCLUDE_ALL_GCS
    // Code: _aload_0, _getfield, _areturn
    // parameter size = 1
    //
    // The code that gets generated by this routine is split into 2 parts:
    //    1. The "intrinsified" code for G1 (or any SATB based GC),
    //    2. The slow path - which is an expansion of the regular method entry.
    //
    // Notes:-
    // * In the G1 code we do not check whether we need to block for
    //   a safepoint. If G1 is enabled then we must execute the specialized
    //   code for Reference.get (except when the Reference object is null)
    //   so that we can log the value in the referent field with an SATB
    //   update buffer.
    //   If the code for the getfield template is modified so that the
    //   G1 pre-barrier code is executed when the current method is
    //   Reference.get() then going through the normal method entry
    //   will be fine.
    // * The G1 code can, however, check the receiver object (the instance
    //   of java.lang.Reference) and jump to the slow path if null. If the
    //   Reference object is null then we obviously cannot fetch the referent
    //   and so we don't need to call the G1 pre-barrier. Thus we can use the
    //   regular method entry code to generate the NPE.
    //
    // This code is based on generate_accessor_entry.
    //
    // rmethod: Method*
    // r13: senderSP must preserve for slow path, set SP to it on fast path

    address entry = __ current_pc();

    const int referent_offset = java_lang_ref_Reference::referent_offset;
    guarantee(referent_offset > 0, "referent offset not initialized");

    if (UseG1GC) {
        YuhuLabel slow_path;
        const YuhuMacroAssembler::YuhuRegister local_0 = __ x0;
        // Check if local 0 != NULL
        // If the receiver is null then it is OK to jump to the slow path.
        __ write_inst("ldr %s, [x20, #%d]", local_0, 0);
        __ write_inst_cbz(local_0, slow_path);

        // Load the value of the referent field.
//        const Address field_address(local_0, referent_offset);
        __ write_insts_load_heap_oop(local_0, YuhuAddress(local_0, referent_offset));

        __ write_inst("mov x19, x13"); // Move senderSP to a callee-saved register
        // Generate the G1 pre-barrier code to log the value of
        // the referent field in an SATB buffer.
        __ write_insts_enter(); // g1_write may call runtime
        __ write_insts_g1_write_barrier_pre(__ noreg /* obj */,
                                local_0 /* pre_val */,
                                __ x28 /* thread */,
                                __ x9 /* tmp */,
                                true /* tosca_live */,
                                true /* expand_call */);
        __ write_insts_leave();
        // areturn
        __ write_inst("and sp, x19, #-16"); // done with stack
        __ write_inst("ret");

        // generate a vanilla interpreter entry as the slow path
        __ pin_label(slow_path);
        (void) generate_normal_entry(false);

        return entry;
    }
#endif // INCLUDE_ALL_GCS

    // If G1 is not enabled then attempt to go through the accessor entry point
    // Reference.get is an accessor
    return generate_accessor_entry();
}

address YuhuInterpreterGenerator::generate_CRC32_update_entry() {
    if (UseCRC32Intrinsics) {
        address entry = __ current_pc();

        // rmethod: Method*
        // r13: senderSP must preserved for slow path
        // esp: args

        YuhuLabel slow_path;
        // If we need a safepoint check, generate full interpreter entry.
//        ExternalAddress state(SafepointSynchronize::address_of_state());
        uint64_t offset;
        __ write_insts_adrp(__ x8, SafepointSynchronize::address_of_state(), offset);
        __ write_inst("ldr x8, [x8, #%d]", offset);
        assert(SafepointSynchronize::_not_synchronized == 0, "rewrite this code");
        __ write_inst_cbnz(__ x8, slow_path);

        // We don't generate local frame and don't align stack because
        // we call stub code and there is no safepoint on this path.

        // Load parameters
        const YuhuMacroAssembler::YuhuRegister crc = __ x0;  // crc
        const YuhuMacroAssembler::YuhuRegister val = __ x1;  // source java byte value
        const YuhuMacroAssembler::YuhuRegister tbl = __ x2;  // scratch

        // Arguments are reversed on java expression stack
        __ write_inst("ldr %s, [x20, #%d]", __ w_reg(val), 0); // byte value
        __ write_inst("ldr %s, [x20, #%d]", __ w_reg(crc), wordSize); // Initial CRC

        __ write_insts_adrp(tbl, StubRoutines::crc_table_addr(), offset);
        __ write_inst("add %s, %s, #%d", tbl, tbl, offset);

        __ write_inst_regs("orn %s, %s, %s", __ w_reg(crc), __ wzr, __ w_reg(crc)); // ~crc

        __ write_insts_update_byte_crc32(crc, val, tbl); // ~crc

        // result in c_rarg0

        __ write_inst("and sp, x13, #-16");
        __ write_inst("ret");

        // generate a vanilla native entry as the slow path
        __ pin_label(slow_path);

        (void) generate_native_entry(false);

        return entry;
    }
    return generate_native_entry(false);
}

address YuhuInterpreterGenerator::generate_CRC32_updateBytes_entry(YuhuInterpreter::MethodKind kind) {
    if (UseCRC32Intrinsics) {
        address entry = __ current_pc();

        // rmethod,: Method*
        // r13: senderSP must preserved for slow path

        YuhuLabel slow_path;
        // If we need a safepoint check, generate full interpreter entry.
//        ExternalAddress state(SafepointSynchronize::address_of_state());
        uint64_t offset;
        __ write_insts_adrp(__ x8, SafepointSynchronize::address_of_state(), offset);
        __ write_inst("ldr w8, [x8, #%d]", offset);
        assert(SafepointSynchronize::_not_synchronized == 0, "rewrite this code");
        __ write_inst_cbnz(__ x8, slow_path);

        // We don't generate local frame and don't align stack because
        // we call stub code and there is no safepoint on this path.

        // Load parameters
        const YuhuMacroAssembler::YuhuRegister crc = __ x0;  // crc
        const YuhuMacroAssembler::YuhuRegister buf = __ x1;  // source java byte array address
        const YuhuMacroAssembler::YuhuRegister len = __ x2;  // length
        const YuhuMacroAssembler::YuhuRegister off = len;      // offset (never overlaps with 'len')

        // Arguments are reversed on java expression stack
        // Calculate address of start element
        if (kind == YuhuInterpreter::java_util_zip_CRC32_updateByteBuffer) {
            __ write_inst("ldr %s, [x20, #%d]", buf, 2 * wordSize); // long buf
            __ write_inst("ldr %s, [x20, #%d]", __ w_reg(off), wordSize); // offset
            __ write_inst_regs("add %s, %s, %s", buf, buf, off); // + offset
            __ write_inst("ldr %s, [x20, #%d]", __ w_reg(crc), 4 * wordSize); // Initial CRC
        } else {
            __ write_inst("ldr %s, [x20, #%d]", buf, 2 * wordSize); // byte[] array
            __ write_inst("add %s, %s, #%d", buf, buf, arrayOopDesc::base_offset_in_bytes(T_BYTE)); // + header size
            __ write_inst("ldr %s, [x20, #%d]", __ w_reg(off), wordSize); // offset
            __ write_inst_regs("add %s, %s, %s", buf, buf, off); // + offset
            __ write_inst("ldr %s, [x20, #%d]", __ w_reg(crc), 3 * wordSize); // Initial CRC
        }
        // Can now load 'len' since we're finished with 'off'
        __ write_inst("ldr %s, [x20, #%d]", __ w_reg(len), 0x0); // Length

        __ write_inst("and sp, x13, #-16"); // Restore the caller's SP

        // We are frameless so we can just jump to the stub.
        __ write_inst_b(CAST_FROM_FN_PTR(address, StubRoutines::updateBytesCRC32()));

        // generate a vanilla native entry as the slow path
        __ pin_label(slow_path);

        (void) generate_native_entry(false);

        return entry;
    }
    return generate_native_entry(false);
}

void YuhuInterpreterGenerator::generate_fixed_frame(bool native_call) {
    // initialize fixed part of activation frame
    if (native_call) {
        __ write_inst("sub x20, sp, #%d", 12 * wordSize);
        __ write_inst_mov_reg(__ x22, __ xzr);
        __ write_inst("stp x20, xzr, [sp, #%d]!", -12 * wordSize);
        // add 2 zero-initialized slots for native calls
        __ write_inst("stp xzr, xzr, [sp, #%d]", 10 * wordSize);
    } else {
        __ write_inst("sub x20, sp, #%d", 10 * wordSize);
        __ write_inst("ldr x8, [x12, #%d]", in_bytes(Method::const_offset())); // get ConstMethod
        __ write_inst("add x22, x8, #%d", in_bytes(ConstMethod::codes_offset())); // get codebase
        __ write_inst("stp x20, x22, [sp, #%d]!", -10 * wordSize);
    }

    // TODO
//    if (ProfileInterpreter) {
//        Label method_data_continue;
//        __ ldr(rscratch1, Address(rmethod, Method::method_data_offset()));
//        __ cbz(rscratch1, method_data_continue);
//        __ lea(rscratch1, Address(rscratch1, in_bytes(MethodData::data_offset())));
//        __ bind(method_data_continue);
//        __ stp(rscratch1, rmethod, Address(sp, 4 * wordSize));  // save Method* and mdp (method data pointer)
//    } else {
        __ write_inst("stp xzr, x12, [sp, #%d]", 4 * wordSize); // save Method* (no mdp)
//    }

    __ write_inst("ldr x26, [x12, #%d]", in_bytes(Method::const_offset()));
    __ write_inst("ldr x26, [x26, #%d]", in_bytes(ConstMethod::constants_offset()));
    __ write_inst("ldr x26, [x26, #%d]", ConstantPool::cache_offset_in_bytes());
    __ write_inst("stp x24, x26, [sp, #%d]", 2 * wordSize);

    __ write_inst("stp x29, x30, [sp, #%d]", 8 * wordSize);
    __ write_insts_lea(__ x29, YuhuAddress(__ sp, 8 * wordSize));

    // set sender sp
    // leave last_sp as null
    __ write_inst("stp xzr, x13, [sp, #%d]", 6 * wordSize);

    // Move SP out of the way
    if (! native_call) {
        __ write_inst("ldr x8, [x12, #%d]", in_bytes(Method::const_offset()));
        __ write_inst("ldrh w8, [x8, #%d]", in_bytes(ConstMethod::max_stack_offset()));
        __ write_inst("add x8, x8, #%d", frame::interpreter_frame_monitor_size()
                                         + (EnableInvokeDynamic ? 2 : 0));
        __ write_inst("sub x8, sp, w8, uxtw #3"); // w8, uxtw, #3 is used in old code
        __ write_inst("and sp, x8, #-16"); // sp-adjustment
    }
}

void YuhuInterpreterGenerator::generate_stack_overflow_check(void) {

    // monitor entry size: see picture of stack set
    // (generate_method_entry) and frame_amd64.hpp
    const int entry_size = frame::interpreter_frame_monitor_size() * wordSize;

    // total overhead size: entry_size + (saved rbp through expr stack
    // bottom).  be sure to change this if you add/subtract anything
    // to/from the overhead area
    const int overhead_size =
            -(frame::interpreter_frame_initial_sp_offset * wordSize) + entry_size;

    const int page_size = os::vm_page_size();

    YuhuLabel after_frame_check;

    // see if the frame is greater than one page in size. If so,
    // then we need to verify there is enough stack space remaining
    // for the additional locals.
    //
    // Note that we use SUBS rather than CMP here because the immediate
    // field of this instruction may overflow.  SUBS can cope with this
    // because it is a macro that will expand to some number of MOV
    // instructions and a register operation.
    __ write_inst("subs x8, x3, #%d", (page_size - overhead_size) / Interpreter::stackElementSize);
    __ write_inst_b(__ ls, after_frame_check);

    // compute rsp as if this were going to be the last frame on
    // the stack before the red zone

//    const Address stack_base(rthread, Thread::stack_base_offset());
//    const Address stack_size(rthread, Thread::stack_size_offset());

    // locals + overhead, in bytes
    __ write_insts_mov_imm64(__ x0, overhead_size);
    __ write_inst("add x0, x0, x3, lsl #%d", Interpreter::logStackElementSize); // 2 slots per parameter.

    __ write_inst("ldr x8, [x28, #%d]", in_bytes(Thread::stack_base_offset()));
    __ write_inst("ldr x9, [x28, #%d]", in_bytes(Thread::stack_size_offset()));

#ifdef ASSERT
    YuhuLabel stack_base_okay, stack_size_okay;
    // verify that thread stack base is non-zero
    // cbnz x8, #8
    __ write_inst_cbnz(__ x8, stack_base_okay);
    __ write_insts_stop("stack base is zero");
    // verify that thread stack size is non-zero
    __ pin_label(stack_base_okay);
    // cbnz x9, #8
    __ write_inst_cbnz(__ x9, stack_size_okay);
    __ write_insts_stop("stack size is zero");
    __ pin_label(stack_size_okay);
#endif

    // Add stack base to locals and subtract stack size
    __ write_inst("sub x8, x8, x9"); // Stack limit
    __ write_inst("add x0, x0, x8");

    // Use the maximum number of pages we might bang.
    const int max_pages = StackShadowPages > (StackRedPages+StackYellowPages) ? StackShadowPages :
                          (StackRedPages+StackYellowPages);

    // add in the red and yellow zone sizes
    __ write_inst("add x0, x0, #%d", max_pages * page_size * 2);

    // check against the current stack bottom
    __ write_inst("cmp sp, x0");
    __ write_inst_b(__ hi, after_frame_check);

    // Remove the incoming args, peeling the machine SP back to where it
    // was in the caller.  This is not strictly necessary, but unless we
    // do so the stack frame may have a garbage FP; this ensures a
    // correct call stack that we can always unwind.  The ANDR should be
    // unnecessary because the sender SP in r13 is always aligned, but
    // it doesn't hurt.
    __ write_inst("and sp, x13, #%d", -16);

    // Note: the restored frame is not necessarily interpreted.
    // Use the shared runtime version of the StackOverflowError.
    assert(YuhuStubRoutines::throw_StackOverflowError_entry() != NULL, "stub not yet generated");
    __ write_insts_far_jump(YuhuStubRoutines::throw_StackOverflowError_entry());

    // all done with frame size check
    __ pin_label(after_frame_check);
}

void YuhuInterpreterGenerator::bang_stack_shadow_pages(bool native_call) {
    // Bang each page in the shadow zone. We can't assume it's been done for
    // an interpreter frame with greater than a page of locals, so each page
    // needs to be checked.  Only true for non-native.
    if (UseStackBanging) {
        const int start_page = native_call ? StackShadowPages : 1;
        const int page_size = os::vm_page_size();
        for (int pages = start_page; pages <= StackShadowPages ; pages++) {
            __ write_inst("sub x9, sp, #%d", pages*page_size);
            __ write_inst("str xzr, [x9]");
        }
    }
}

void YuhuInterpreterGenerator::lock_method(void) {
    // synchronize method
//    const Address access_flags(rmethod, Method::access_flags_offset());
//    const Address monitor_block_top(
//            rfp,
//            frame::interpreter_frame_monitor_block_top_offset * wordSize);
    const int entry_size = frame::interpreter_frame_monitor_size() * wordSize;

#ifdef ASSERT
    {
        YuhuLabel L;
        __ write_inst("ldr w0, [x12, #%d]", in_bytes(Method::access_flags_offset()));
        __ write_inst("tst x0, #%d", JVM_ACC_SYNCHRONIZED);
        __ write_inst_b(__ ne, L);
        __ write_insts_stop("method doesn't need synchronization");
        __ pin_label(L);
    }
#endif // ASSERT

    // get synchronization object
    {
        const int mirror_offset = in_bytes(Klass::java_mirror_offset());
        YuhuLabel done;
        __ write_inst("ldr w0, [x12, #%d]", in_bytes(Method::access_flags_offset()));
        __ write_inst("tst x0, #%d", JVM_ACC_STATIC);
        // get receiver (assume this is frequent case)
        __ write_inst("ldr x0, [x24, #%d]", Interpreter::local_offset_in_bytes(0));
        __ write_inst_b(__ eq, done);
        __ write_inst("ldr x0, [x12, #%d]", in_bytes(Method::const_offset()));
        __ write_inst("ldr x0, [x0, #%d]", in_bytes(ConstMethod::constants_offset()));
        __ write_inst("ldr x0, [x0, #%d]", ConstantPool::pool_holder_offset_in_bytes());
        __ write_inst("ldr x0, [x0, #%d]", mirror_offset);

#ifdef ASSERT
        {
            YuhuLabel L;
            __ write_inst_cbnz(__ x0, L);
            __ write_insts_stop("synchronization object is NULL");
            __ pin_label(L);
        }
#endif // ASSERT

        __ pin_label(done);
    }

    // add space for monitor & lock
    __ write_inst("sub sp, sp, #%d", entry_size); // add space for a monitor entry
    __ write_inst("sub x20, x20, #%d", entry_size);
    __ write_inst_mov_reg(__ x8, __ x20);
    __ write_inst("str x8, [x29, #%d]", frame::interpreter_frame_monitor_block_top_offset * wordSize); // set new monitor block top
    // store object
    __ write_inst("str x0, [x20, #%d]", BasicObjectLock::obj_offset_in_bytes());
    __ write_inst_mov_reg(__ x1, __ x20); // object address
    __ write_insts_lock_object(__ x1);
}

address YuhuInterpreterGenerator::generate_result_handler_for(
        BasicType type) {
    address entry = __ current_pc();
    switch (type) {
        case T_BOOLEAN: __ write_insts_c2bool(__ x0);          break;
        case T_CHAR   : __ write_inst_regs("uxth %s, %s", __ w0, __ w0);        break;
        case T_BYTE   : __ write_inst_regs("sxtb %s, %s", __ w0, __ w0);        break;
        case T_SHORT  : __ write_inst_regs("sxth %s, %s", __ w0, __ w0);        break;
        case T_INT    : __ write_inst_regs("uxtw %s, %s", __ x0, __ w0);        break;  // FIXME: We almost certainly don't need this
        case T_LONG   : /* nothing to do */        break;
        case T_VOID   : /* nothing to do */        break;
        case T_FLOAT  : /* nothing to do */        break;
        case T_DOUBLE : /* nothing to do */        break;
        case T_OBJECT :
            // retrieve result from frame
            __ write_inst("ldr x0, [x29, #%d]", frame::interpreter_frame_oop_temp_offset*wordSize);
            // and verify it
            __ write_insts_verify_oop(__ x0, "broken oop");
            break;
        default       : ShouldNotReachHere();
    }
    __ write_inst("ret");  // return from result handler
    return entry;
}

address YuhuInterpreterGenerator::generate_continuation_for(TosState state) {
    address entry = __ current_pc();
    // NULL last_sp until next java call
    __ write_inst("str xzr [x29, #%d]", frame::interpreter_frame_last_sp_offset * wordSize);
    __ write_insts_dispatch_next(state);
    return entry;
}

address YuhuInterpreterGenerator::generate_safept_entry_for(
        TosState state,
        address runtime_entry) {
    address entry = __ current_pc();
    __ write_insts_push(state);
    __ write_insts_final_call_VM(__ noreg, runtime_entry);
    __ write_inst("dmb ish");
    __ write_insts_dispatch_via(vtos, YuhuInterpreter::_normal_table.table_for(vtos));
    return entry;
}

void YuhuInterpreterGenerator::generate_throw_exception() {
    // Entry point in previous activation (i.e., if the caller was
    // interpreted)
    YuhuInterpreter::_rethrow_exception_entry = __ current_pc();
    // Restore sp to interpreter_frame_last_sp even though we are going
    // to empty the expression stack for the exception processing.
    __ write_inst("str xzr [x29, #%d]", frame::interpreter_frame_last_sp_offset * wordSize);
    // r0: exception
    // r3: return address/pc that threw exception
    __ write_insts_restore_bcp();    // rbcp points to call/send
    __ write_insts_restore_locals();
    __ write_insts_restore_constant_pool_cache();
    // TODO
//    __ reinit_heapbase();  // restore rheapbase as heapbase.
    __ write_insts_get_dispatch();

    // Entry point for exceptions thrown within interpreter code
    YuhuInterpreter::_throw_exception_entry = __ current_pc();
    // If we came here via a NullPointerException on the receiver of a
    // method, rmethod may be corrupt.
    __ write_insts_get_method(__ x12);
    // expression stack is undefined here
    // r0: exception
    // rbcp: exception bcp
    __ write_insts_verify_oop(__ x0, "broken oop");
    __ write_inst_mov_reg(__ x1, __ x0);

    // expression stack must be empty before entering the VM in case of
    // an exception
    __ write_insts_empty_expression_stack();
    // find exception handler address and preserve exception oop
    __ write_insts_final_call_VM(__ x3,
               CAST_FROM_FN_PTR(address,
                                InterpreterRuntime::exception_handler_for_exception),
               __ x1);

    // Calculate stack limit
    __ write_inst("ldr x8, [x12, #%d]", in_bytes(Method::const_offset()));
    __ write_inst("ldrh %s, [x8, #%d]", __ w8, in_bytes(ConstMethod::max_stack_offset()));
    __ write_inst("add x8, x8, #%d", frame::interpreter_frame_monitor_size()
                                     + (EnableInvokeDynamic ? 2 : 0) + 2);
    __ write_inst("ldr x9, [x29, #%d]", frame::interpreter_frame_initial_sp_offset * wordSize);
    __ write_inst("sub x8, x9, x8, uxtx #3");
    __ write_inst("and sp, x8, #-16");

    // r0: exception handler entry point
    // r3: preserved exception oop
    // rbcp: bcp for exception handler
    __ write_inst_push_ptr(__ x3); // push exception which is now the only value on the stack
    __ write_inst_br(__ x0); // jump to exception handler (may be _remove_activation_entry!)

    // If the exception is not handled in the current frame the frame is
    // removed and the exception is rethrown (i.e. exception
    // continuation is _rethrow_exception).
    //
    // Note: At this point the bci is still the bxi for the instruction
    // which caused the exception and the expression stack is
    // empty. Thus, for any VM calls at this point, GC will find a legal
    // oop map (with empty expression stack).

    //
    // JVMTI PopFrame support
    //

    YuhuInterpreter::_remove_activation_preserving_args_entry = __ pc();
    __ write_insts_empty_expression_stack();
    // Set the popframe_processing bit in pending_popframe_condition
    // indicating that we are currently handling popframe, so that
    // call_VMs that may happen later do not trigger new popframe
    // handling cycles.
    __ write_inst("ldr w3, [x28, #%d]", in_bytes(JavaThread::popframe_condition_offset()));
    __ write_inst("orr x3, x3, #%d", JavaThread::popframe_processing_bit);
    __ write_inst("str w3, [x28, #%d]", in_bytes(JavaThread::popframe_condition_offset()));

    {
        // Check to see whether we are returning to a deoptimized frame.
        // (The PopFrame call ensures that the caller of the popped frame is
        // either interpreted or compiled and deoptimizes it if compiled.)
        // In this case, we can't call dispatch_next() after the frame is
        // popped, but instead must save the incoming arguments and restore
        // them after deoptimization has occurred.
        //
        // Note that we don't compare the return PC against the
        // deoptimization blob's unpack entry because of the presence of
        // adapter frames in C2.
        YuhuLabel caller_not_deoptimized;
        __ write_inst("ldr x1, [x29, #%d]", frame::return_addr_offset * wordSize);
        __ write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address,
                                               InterpreterRuntime::interpreter_contains), __ x1);
        __ write_inst_cbnz(__ x0, caller_not_deoptimized);

        // Compute size of arguments for saving when returning to
        // deoptimized caller
        __ write_insts_get_method(__ x0);
        __ write_inst("ldr x0, [x0, #%d]", in_bytes(Method::const_offset()));
        __ write_inst("ldrh w0, [x0, #%d]", in_bytes(ConstMethod::
                                                     size_of_parameters_offset()));
        __ write_inst("lsl x0, x0, #%d", Interpreter::logStackElementSize);
        __ write_insts_restore_locals(); // XXX do we need this?
        __ write_inst("sub x24, x24, x0");
        __ write_inst("add x24, x24, #%d", wordSize);
        // Save these arguments
        __ write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address,
                                               Deoptimization::
                                                       popframe_preserve_args),
                              __ x28, __ x0, __ x24);

        __ write_insts_remove_activation(vtos,
                /* throw_monitor_exception */ false,
                /* install_monitor_exception */ false,
                /* notify_jvmdi */ false);

        // Inform deoptimization that it is responsible for restoring
        // these arguments
        __ write_insts_mov_imm64(__ x8, JavaThread::popframe_force_deopt_reexecution_bit);
        __ write_inst("str w8, [x28, #%d]", in_bytes(JavaThread::popframe_condition_offset()));

        // Continue in deoptimization handler
        __ write_inst("ret");

        __ pin_label(caller_not_deoptimized);
    }

    __ write_insts_remove_activation(vtos,
            /* throw_monitor_exception */ false,
            /* install_monitor_exception */ false,
            /* notify_jvmdi */ false);

    // Restore the last_sp and null it out
    __ write_inst("ldr x20, [x29, #%d]", frame::interpreter_frame_last_sp_offset * wordSize);
    __ write_inst("str xzr, [x29, #%d]", frame::interpreter_frame_last_sp_offset * wordSize);

    __ write_insts_restore_bcp();
    __ write_insts_restore_locals();
    __ write_insts_restore_constant_pool_cache();
    __ write_insts_get_method(__ x12);
    __ write_insts_get_dispatch();

    // The method data pointer was incremented already during
    // call profiling. We have to restore the mdp for the current bcp.
    // TODO
//    if (ProfileInterpreter) {
//        __ set_method_data_pointer_for_bcp();
//    }

    // Clear the popframe condition flag
    __ write_inst("str wzr, [x28, #%d]", in_bytes(JavaThread::popframe_condition_offset()));
    assert(JavaThread::popframe_inactive == 0, "fix popframe_inactive");

#if INCLUDE_JVMTI
    if (EnableInvokeDynamic) {
        YuhuLabel L_done;

        __ write_inst("ldrb w8, [x22, #0]");
        __ write_inst("cmp x8, #%d", Bytecodes::_invokestatic);
        __ write_inst_b(__ ne, L_done);

        // The member name argument must be restored if _invokestatic is re-executed after a PopFrame call.
        // Detect such a case in the InterpreterRuntime function and return the member name argument, or NULL.

        __ write_inst("ldr x0, [x24, #0]");
        __ write_insts_final_call_VM(__ x0, CAST_FROM_FN_PTR(address, InterpreterRuntime::member_name_arg_or_null), __ x0, __ x12, __ x22);

        __ write_inst_cbz(__ x0, L_done);

        __ write_inst("str x0, [x20, #0]");
        __ pin_label(L_done);
    }
#endif // INCLUDE_JVMTI

    // Restore machine SP
    __ write_inst("ldr x8, [x12, #%d]", in_bytes(Method::const_offset()));
    __ write_inst("ldrh w8, [x8, #%d]", in_bytes(ConstMethod::max_stack_offset()));
    __ write_inst("add x8, x8, #%d", frame::interpreter_frame_monitor_size()
                                     + (EnableInvokeDynamic ? 2 : 0));
    __ write_inst("ldr x9, [x29, #%d]", frame::interpreter_frame_initial_sp_offset * wordSize);
    __ write_inst("sub x8, x9, w8, uxtw #3");
    __ write_inst("and sp, x8, #-16");

    __ write_insts_dispatch_next(vtos);
    // end of PopFrame support

    YuhuInterpreter::_remove_activation_entry = __ current_pc();

    // preserve exception over this code sequence
    __ write_inst_pop_ptr(__ x0);
    __ write_inst("str x0, [x28, #%d]", in_bytes(JavaThread::vm_result_offset()));
    // remove the activation (without doing throws on illegalMonitorExceptions)
    __ write_insts_remove_activation(vtos, false, true, false);
    // restore exception
    __ write_insts_get_vm_result(__ x0, __ x28);

    // In between activations - previous activation type unknown yet
    // compute continuation point - the continuation point expects the
    // following registers set up:
    //
    // r0: exception
    // lr: return address/pc that threw exception
    // esp: expression stack of caller
    // rfp: fp of caller
    __ write_inst("stp x0, lr, [sp, #%d]!", -2 * wordSize); // save exception & return address
    __ write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address,
                                           SharedRuntime::exception_handler_for_return_address),
                          __ x28, __ lr);
    __ write_inst_mov_reg(__ x1, __ x0); // save exception handler
    __ write_inst("ldp x0, lr, [sp], #%d", 2 * wordSize); // restore exception & return address
    // We might be returning to a deopt handler that expects r3 to
    // contain the exception pc
    __ write_inst_mov_reg(__ x3, __ lr);
    // Note that an "issuing PC" is actually the next PC after the call
    __ write_inst_br(__ x1); // jump to exception
    // handler of caller
}

address YuhuInterpreterGenerator::generate_ArrayIndexOutOfBounds_handler(
        const char* name) {
    address entry = __ current_pc();
    // expression stack must be empty before entering the VM if an
    // exception happened
    __ write_insts_empty_expression_stack();
    // setup parameters
    // ??? convention: expect aberrant index in register r1
    __ write_inst("mov w2, w1");
    __ write_insts_mov_imm64(__ x1, (uint64_t)(address)name);
    __ write_insts_final_call_VM(__ noreg,
               CAST_FROM_FN_PTR(address,
                                InterpreterRuntime::
                                        throw_ArrayIndexOutOfBoundsException),
               __ x1, __ x2);
    return entry;
}

address YuhuInterpreterGenerator::generate_exception_handler_common(
        const char* name, const char* message, bool pass_oop) {
    assert(!pass_oop || message == NULL, "either oop or message but not both");
    address entry = __ current_pc();
    if (pass_oop) {
        // object is at TOS
        __ write_inst_pop(__ x2);
    }
    // expression stack must be empty before entering the VM if an
    // exception happened
    __ write_insts_empty_expression_stack();
    // setup parameters
    __ write_insts_lea(__ x1, YuhuAddress((address)name));
    if (pass_oop) {
        __ write_insts_final_call_VM(__ x0, CAST_FROM_FN_PTR(address,
                                        InterpreterRuntime::
                                                create_klass_exception),
                   __ x1, __ x2);
    } else {
        // kind of lame ExternalAddress can't take NULL because
        // external_word_Relocation will assert.
        if (message != NULL) {
            __ write_insts_lea(__ x2, YuhuAddress((address)message));
        } else {
            __ write_insts_mov_imm64(__ x2, (uint64_t)NULL_WORD);
        }
        __ write_insts_final_call_VM(__ x0,
                   CAST_FROM_FN_PTR(address, InterpreterRuntime::create_exception),
                   __ x1, __ x2);
    }
    // throw exception
    __ write_inst_b(address(Interpreter::throw_exception_entry()));
    return entry;
}

address YuhuInterpreterGenerator::generate_ClassCastException_handler() {
    address entry = __ current_pc();

    // object is at TOS
    __ write_inst_pop(__ x1);

    // expression stack must be empty before entering the VM if an
    // exception happened
    __ write_insts_empty_expression_stack();

    __ write_insts_final_call_VM(__ noreg,
               CAST_FROM_FN_PTR(address,
                                InterpreterRuntime::
                                        throw_ClassCastException),
               __ x1);
    return entry;
}

address YuhuInterpreterGenerator::generate_StackOverflowError_handler() {
    address entry = __ current_pc();

#ifdef ASSERT
    {
        YuhuLabel L;
        __ write_inst("ldr x8, [x29, #%d]", frame::interpreter_frame_monitor_block_top_offset *
                                            wordSize);
        __ write_inst("mov x9, sp");
        __ write_inst("cmp x8, x9"); // maximal rsp for current rfp (stack
        // grows negative)
        __ write_inst_b(__ hs, L); // check if frame is complete
        __ write_insts_stop ("interpreter frame not set up");
        __ pin_label(L);
    }
#endif // ASSERT
    // Restore bcp under the assumption that the current frame is still
    // interpreted
    __ write_insts_restore_bcp();

    // expression stack must be empty before entering the VM if an
    // exception happened
    __ write_insts_empty_expression_stack();
    // throw exception
    __ write_insts_final_call_VM(__ noreg,
               CAST_FROM_FN_PTR(address,
                                InterpreterRuntime::throw_StackOverflowError));
    return entry;
}
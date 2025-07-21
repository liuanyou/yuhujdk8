//
// Created by Anyou Liu on 2025/4/1.
//
#include "precompiled.hpp"
#include "asm/yuhu/yuhu_macroAssembler.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "interpreter/interpreter.hpp"
#include "nativeInst_aarch64.hpp"
#include "oops/instanceOop.hpp"
#include "oops/method.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/oop.inline.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/yuhu/yuhu_stubCodeGenerator.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/thread.inline.hpp"
#include "utilities/top.hpp"

#undef __
#define __ _masm->

class YuhuStubGenerator : public YuhuStubCodeGenerator {
private:
    // Call stub stack layout word offsets from fp
    enum call_stub_layout {
        sp_after_call_off = -26,

        d15_off            = -26,
        d13_off            = -24,
        d11_off            = -22,
        d9_off             = -20,

        r28_off            = -18,
        r26_off            = -16,
        r24_off            = -14,
        r22_off            = -12,
        r20_off            = -10,
        call_wrapper_off   =  -8,
        result_off         =  -7,
        result_type_off    =  -6,
        method_off         =  -5,
        entry_point_off    =  -4,
        parameter_size_off =  -2,
        thread_off         =  -1,
        fp_f               =   0,
        retaddr_off        =   1,
    };

    address generate_call_stub(address& return_address) {
        assert((int)frame::entry_frame_after_call_words == -(int)sp_after_call_off + 1 &&
               (int)frame::entry_frame_call_wrapper_offset == (int)call_wrapper_off,
               "adjust this code");

//        StubCodeMark mark(this, "StubRoutines", "call_stub");
        address start = __ current_pc();

//        const Address sp_after_call(rfp, sp_after_call_off * wordSize);

//        const Address call_wrapper  (rfp, call_wrapper_off   * wordSize);
//        const Address result        (rfp, result_off         * wordSize);
//        const Address result_type   (rfp, result_type_off    * wordSize);
//        const Address method        (rfp, method_off         * wordSize);
//        const Address entry_point   (rfp, entry_point_off    * wordSize);
//        const Address parameter_size(rfp, parameter_size_off * wordSize);

//        const Address thread        (rfp, thread_off         * wordSize);

//        const Address d15_save      (rfp, d15_off * wordSize);
//        const Address d13_save      (rfp, d13_off * wordSize);
//        const Address d11_save      (rfp, d11_off * wordSize);
//        const Address d9_save       (rfp, d9_off * wordSize);

//        const Address r28_save      (rfp, r28_off * wordSize);
//        const Address r26_save      (rfp, r26_off * wordSize);
//        const Address r24_save      (rfp, r24_off * wordSize);
//        const Address r22_save      (rfp, r22_off * wordSize);
//        const Address r20_save      (rfp, r20_off * wordSize);

        // stub code

        address aarch64_entry = __ current_pc();

        // set up frame and move sp to end of save area
        __ write_insts_enter();
        __ write_inst("sub sp, x29, #%d", -sp_after_call_off * wordSize);

        // save register parameters and Java scratch/global registers
        // n.b. we save thread even though it gets installed in
        // rthread because we want to sanity check rthread later
        __ write_inst("str x7, [x29, #%d]", thread_off         * wordSize);
        __ write_inst("str w6, [x29, #%d]", parameter_size_off * wordSize);
        __ write_inst("stp x4, x5, [x29, #%d]", entry_point_off    * wordSize);
        __ write_inst("stp x2, x3, [x29, #%d]", result_type_off    * wordSize);
        __ write_inst("stp x0, x1, [x29, #%d]", call_wrapper_off   * wordSize);

        __ write_inst("stp x20, x19, [x29, #%d]", r20_off * wordSize);
        __ write_inst("stp x22, x21, [x29, #%d]", r22_off * wordSize);
        __ write_inst("stp x24, x23, [x29, #%d]", r24_off * wordSize);
        __ write_inst("stp x26, x25, [x29, #%d]", r26_off * wordSize);
        __ write_inst("stp x28, x27, [x29, #%d]", r28_off * wordSize);

        __ write_inst("stp d9, d8, [x29, #%d]", d9_off * wordSize);
        __ write_inst("stp d11, d10, [x29, #%d]", d11_off * wordSize);
        __ write_inst("stp d13, d12, [x29, #%d]", d13_off * wordSize);
        __ write_inst("stp d15, d14, [x29, #%d]", d15_off * wordSize);

        // install Java thread in global register now we have saved
        // whatever value it held
        __ write_inst("mov x28, x7");
        // And method
        __ write_inst("mov x12, x3");

        // set up the heapbase register
        // TODO
//        __ reinit_heapbase();

#ifdef ASSERT
        // make sure we have no pending exceptions
        {
            YuhuLabel L;
            __ write_inst("ldr x8, [x28, #%d]", in_bytes(Thread::pending_exception_offset()));
            __ write_inst("cmp x8, #%d", (unsigned)NULL_WORD);
            __ write_inst_b(__ eq, L);
            __ write_insts_stop("YuhuStubRoutines::call_stub: entered with pending exception");
            __ pin_label(L);
        }
#endif
        // pass parameters if any
        __ write_inst("mov x20, sp");
        __ write_inst("sub x8, sp, w6, uxtw #%d", LogBytesPerWord); // Move SP out of the way
        __ write_inst("and sp, x8, #%d", -2 * wordSize);

//        BLOCK_COMMENT("pass parameters if any");
        YuhuLabel parameters_done;
        // parameter count is still in c_rarg6
        // and parameter pointer identifying param 1 is in c_rarg5
        __ write_inst_cbz(__ w6, parameters_done);

        address loop = __ current_pc();
        __ write_inst("ldr x8, [x5], #%d", wordSize);
        __ write_inst("subs w6, w6, #1");
        __ write_inst_push(__ x8);
        __ write_inst_b(__ gt, loop);

        __ pin_label(parameters_done);

        // call Java entry -- passing methdoOop, and current sp
        //      rmethod: Method*
        //      r13: sender sp
//        BLOCK_COMMENT("call Java function");
        __ write_inst("mov x13, sp");
        __ write_inst_blr(__ x4);

        // we do this here because the notify will already have been done
        // if we get to the next instruction via an exception
        //
        // n.b. adding this instruction here affects the calculation of
        // whether or not a routine returns to the call stub (used when
        // doing stack walks) since the normal test is to check the return
        // pc against the address saved below. so we may need to allow for
        // this extra instruction in the check.

        // save current address for use by exception handling code

        return_address = __ current_pc();

        // store result depending on type (everything that is not
        // T_OBJECT, T_LONG, T_FLOAT or T_DOUBLE is treated as T_INT)
        // n.b. this assumes Java returns an integral result in r0
        // and a floating result in j_farg0
        __ write_inst("ldr x3, [x29, #%d]", result_off         * wordSize);
        YuhuLabel is_long, is_float, is_double, exit;
        __ write_inst("ldr x2, [x29, #%d]", result_type_off    * wordSize);
        __ write_inst("cmp x2, #%d", T_OBJECT);
        __ write_inst_b(__ eq, is_long);
        __ write_inst("cmp x2, #%d", T_LONG);
        __ write_inst_b(__ eq, is_long);
        __ write_inst("cmp x2, #%d", T_FLOAT);
        __ write_inst_b(__ eq, is_float);
        __ write_inst("cmp x2, #%d", T_DOUBLE);
        __ write_inst_b(__ eq, is_double);

        // handle T_INT case
        __ write_inst("str w0, [x3]");

        __ pin_label(exit);

        // pop parameters
        __ write_inst("sub x20, x29, #%d", -sp_after_call_off * wordSize);

#ifdef ASSERT
        // verify that threads correspond
        {
            YuhuLabel L, S;
            __ write_inst("ldr x8, [x29, #%d]", thread_off         * wordSize);
            __ write_inst("cmp x28, x8");
            __ write_inst_b(__ ne, S);
            __ write_insts_get_thread(__ x8);
            __ write_inst("cmp x28, x8");
            __ write_inst_b(__ eq, L);
            __ pin_label(S);
            __ write_insts_stop("YuhuStubRoutines::call_stub: threads must correspond");
            __ pin_label(L);
        }
#endif

        // restore callee-save registers
        __ write_inst("ldp d15, d14, [x29, #%d]", d15_off * wordSize);
        __ write_inst("ldp d13, d12, [x29, #%d]", d13_off * wordSize);
        __ write_inst("ldp d11, d10, [x29, #%d]", d11_off * wordSize);
        __ write_inst("ldp d9, d8, [x29, #%d]", d9_off * wordSize);

        __ write_inst("ldp x28, x27, [x29, #%d]", r28_off * wordSize);
        __ write_inst("ldp x26, x25, [x29, #%d]", r26_off * wordSize);
        __ write_inst("ldp x24, x23, [x29, #%d]", r24_off * wordSize);
        __ write_inst("ldp x22, x21, [x29, #%d]", r22_off * wordSize);
        __ write_inst("ldp x20, x19, [x29, #%d]", r20_off * wordSize);

        __ write_inst("ldp x0, x1, [x29, #%d]", call_wrapper_off   * wordSize);
        __ write_inst("ldr w2, [x29, #%d]", result_type_off    * wordSize);
        __ write_inst("ldr x3, [x29, #%d]", method_off         * wordSize);
        __ write_inst("ldp x4, x5, [x29, #%d]", entry_point_off    * wordSize);
        __ write_inst("ldp x6, x7, [x29, #%d]", parameter_size_off * wordSize);

        // leave frame and return to caller
        __ write_insts_leave();
        __ write_inst("ret");

        // handle return types different from T_INT

        __ pin_label(is_long);
        __ write_inst("str x0, [x3, #0]");
        __ write_inst_b(__ al, exit);

        __ pin_label(is_float);
        __ write_inst("str s0, [x3, #0]");
        __ write_inst_b(__ al, exit);

        __ pin_label(is_double);
        __ write_inst("str d0, [x3, #0]");
        __ write_inst_b(__ al, exit);

        // 使用 Disassembler 反汇编
        tty->print_cr("Disassembly:");
        __ code()->decode_all();

        return start;
    }

    address generate_catch_exception() {
//        StubCodeMark mark(this, "StubRoutines", "catch_exception");
        address start = __ current_pc();

        // same as in generate_call_stub():
//        const Address sp_after_call(rfp, sp_after_call_off * wordSize);
//        const Address thread        (rfp, thread_off         * wordSize);

#ifdef ASSERT
        // verify that threads correspond
        {
            YuhuLabel L, S;
            __ write_inst("ldr x8, [x29, #%d]", thread_off         * wordSize);
            __ write_inst("cmp x28, x8");
            __ write_inst_b(__ ne, S);
            __ write_insts_get_thread(__ x8);
            __ write_inst("cmp x28, x8");
            __ write_inst_b(__ eq, L);
            __ pin_label(S);
            __ write_insts_stop("YuhuStubRoutines::catch_exception: threads must correspond");
            __ pin_label(L);
        }
#endif

        // set pending exception
        __ write_insts_verify_oop(__ x0, "broken oop");

        __ write_inst("str x0, [x28, #%d]", in_bytes(Thread::pending_exception_offset()));
        __ write_insts_mov_imm64(__ x8, (uint64_t)((address) __FILE__));
        __ write_inst("str x8, [x28, #%d]", in_bytes(Thread::exception_file_offset()));
        __ write_insts_mov_imm32(__ w8, (uint32_t)((int) __LINE__));
        __ write_inst("str w8, [x28, #%d]", in_bytes(Thread::exception_line_offset()));

        // complete return to VM
        assert(YuhuStubRoutines::_call_stub_return_address != NULL,
               "_call_stub_return_address must have been generated before");
        __ write_inst_b(YuhuStubRoutines::_call_stub_return_address);

        // 使用 Disassembler 反汇编
        tty->print_cr("Disassembly:");
        __ code()->decode_all();

        return start;
    }

    address generate_verify_oop() {

//        StubCodeMark mark(this, "StubRoutines", "verify_oop");
        address start = __ current_pc();

        YuhuLabel exit, error;

        // save c_rarg2 and c_rarg3
        __ write_inst("stp x3, x2, [sp, #-0x10]!");

        // __ incrementl(ExternalAddress((address) StubRoutines::verify_oop_count_addr()));
        __ write_insts_mov_imm64(__ x2, (uint64_t)(address) YuhuStubRoutines::verify_oop_count_addr()); // __ lea(c_rarg2, ExternalAddress((address) StubRoutines::verify_oop_count_addr()));
        __ write_inst("ldr x3, [x2]");
        __ write_inst("add x3, x3, 1");
        __ write_inst("str x3, [x2]");

        // object is in r0
        // make sure object is 'reasonable'
        __ write_inst_cbz(__ x0, exit); // if obj is NULL it is OK

        // Check if the oop is in the right area of memory
        __ write_insts_mov_ptr(__ x3, (intptr_t) Universe::verify_oop_mask());
        __ write_inst("and x2, x0, x3");
        __ write_insts_mov_ptr(__ x3, (intptr_t) Universe::verify_oop_bits());

        // Compare c_rarg2 and c_rarg3.  We don't use a compare
        // instruction here because the flags register is live.
        __ write_inst("eor x2, x2, x3");
        __ write_inst_cbnz(__ x2, error);

        // make sure klass is 'reasonable', which is not zero.
        __ write_insts_load_klass(__ x0, __ x0);  // get klass
        __ write_inst_cbz(__ x0, error);      // if klass is NULL it is broken

        // return if everything seems ok
        __ pin_label(exit);

        __ write_inst( "ldp x3, x2, [sp], #0x10");
        __ write_inst("ret");

        // handle errors
        __ pin_label(error);
        __ write_inst( "ldp x3, x2, [sp], #0x10");

        __ write_insts_push(YuhuRegSet::range(__ x0, __ x29), __ sp);

        // debug(char* msg, int64_t pc, int64_t regs[])
        __ write_inst_mov_reg(__ x0, __ x8); // pass address of error message
        __ write_inst_mov_reg(__ x1, __ lr); // pass return address
        __ write_inst_mov_reg(__ x2, __ sp); // pass address of regs on stack
#ifndef PRODUCT
        assert(frame::arg_reg_save_area_bytes == 0, "not expecting frame reg save area");
#endif
//        BLOCK_COMMENT("call MacroAssembler::debug");
        __ write_insts_mov_imm64(__ x8, (uint64_t) CAST_FROM_FN_PTR(address, MacroAssembler::debug64));
        __ write_inst("blr x8");

        return start;
    }

    address generate_forward_exception() {
//        StubCodeMark mark(this, "StubRoutines", "forward exception");
        address start = __ current_pc();

        // Upon entry, LR points to the return address returning into
        // Java (interpreted or compiled) code; i.e., the return address
        // becomes the throwing pc.
        //
        // Arguments pushed before the runtime call are still on the stack
        // but the exception handler will reset the stack pointer ->
        // ignore them.  A potential result in registers can be ignored as
        // well.

#ifdef ASSERT
        // make sure this code is only executed if there is a pending exception
        {
            YuhuLabel L;
            __ write_inst("ldr x8, [x28, #%d]", in_bytes(Thread::pending_exception_offset()));
            __ write_inst_cbnz(__ x8, L);
            __ write_insts_stop("YuhuStubRoutines::forward exception: no pending exception (1)");
            __ pin_label(L);
        }
#endif

        // compute exception handler into r19

        // call the VM to find the handler address associated with the
        // caller address. pass thread in r0 and caller pc (ret address)
        // in r1. n.b. the caller pc is in lr, unlike x86 where it is on
        // the stack.
        __ write_inst("mov x1, x30");
        // lr will be trashed by the VM call so we move it to R19
        // (callee-saved) because we also need to pass it to the handler
        // returned by this call.
        __ write_inst("mov x19, x30");
//        BLOCK_COMMENT("call exception_handler_for_return_address");

        __ write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address,
                                         SharedRuntime::exception_handler_for_return_address),
                        __ x28, __ x1);
        // we should not really care that lr is no longer the callee
        // address. we saved the value the handler needs in r19 so we can
        // just copy it to r3. however, the C2 handler will push its own
        // frame and then calls into the VM and the VM code asserts that
        // the PC for the frame above the handler belongs to a compiled
        // Java method. So, we restore lr here to satisfy that assert.
        __ write_inst_mov_reg(__ x30, __ x19);
        // setup r0 & r3 & clear pending exception
        __ write_inst_mov_reg(__ x3, __ x19);
        __ write_inst_mov_reg(__ x19, __ x0);
        __ write_inst("ldr x0, [x28, #%d]", in_bytes(Thread::pending_exception_offset()));
        __ write_inst("str xzr, [x28, #%d]", in_bytes(Thread::pending_exception_offset()));

#ifdef ASSERT
        // make sure exception is set
        {
            YuhuLabel L;
            __ write_inst_cbnz(__ x0, L);
            __ write_insts_stop("YuhuStubRoutines::forward exception: no pending exception (2)");
            __ pin_label(L);
        }
#endif

        // continue at exception handler
        // r0: exception
        // r3: throwing pc
        // r19: exception handler
        __ write_insts_verify_oop(__ x0, "broken oop");
        __ write_inst_br(__ x19);

        return start;
    }

#undef __
#define __ masm->

    address generate_throw_exception(const char* name,
                                     address runtime_entry,
                                     YuhuMacroAssembler::YuhuRegister arg1 = YuhuMacroAssembler::noreg,
                                     YuhuMacroAssembler::YuhuRegister arg2 = YuhuMacroAssembler::noreg) {
        // Information about frame layout at time of blocking runtime call.
        // Note that we only have to preserve callee-saved registers since
        // the compilers are responsible for supplying a continuation point
        // if they expect all registers to be preserved.
        // n.b. aarch64 asserts that frame::arg_reg_save_area_bytes == 0
        enum layout {
            rfp_off = 0,
            rfp_off2,
            return_off,
            return_off2,
            framesize // inclusive of return address
        };

        int insts_size = 512;
        int locs_size  = 64;

        CodeBuffer code(name, insts_size, locs_size);
        OopMapSet* oop_maps  = new OopMapSet();
        YuhuMacroAssembler* masm = new YuhuMacroAssembler(&code);

        address start = __ current_pc();

        // This is an inlined and slightly modified version of call_VM
        // which has the ability to fetch the return PC out of
        // thread-local storage and also sets up last_Java_sp slightly
        // differently than the real call_VM

        __ write_insts_enter(); // Save FP and LR before call

        assert(is_even(framesize/2), "sp not 16-byte aligned");

        // lr and fp are already in place
        __ write_inst("sub sp, x29, #%d", ((unsigned)framesize-4) << LogBytesPerInt);  // prolog

        int frame_complete = __ current_pc() - start;

        // Set up last_Java_sp and last_Java_fp
        address the_pc = __ current_pc();
        __ write_insts_set_last_java_frame(__ sp,
                                           __ x29, (address) NULL, __ x8);

        // Call runtime
        if (arg1 != __ noreg) {
//            assert(arg2 != c_rarg1, "clobbered");
            assert(arg2 != __ x1, "clobbered");
            __ write_inst_mov_reg(__ x1, arg1);
        }
        if (arg2 != __ noreg) {
            __ write_inst_mov_reg(__ x2, arg2);
        }
        __ write_inst_mov_reg(__ x0, __ x28);
//        BLOCK_COMMENT("call runtime_entry");
        __ write_insts_mov_imm64(__ x8, (uint64_t) runtime_entry);
        __ write_inst("blr x8");

        // Generate oop map
        OopMap* map = new OopMap(framesize, 0);

        oop_maps->add_gc_map(the_pc - start, map);

        __ write_insts_reset_last_java_frame(true);
        __ write_inst("isb");

        __ write_insts_leave();

        // check for pending exceptions
#ifdef ASSERT
        YuhuLabel L;
        __ write_inst("ldr x8, [x28, #%d]", in_bytes(Thread::pending_exception_offset()));
        __ write_inst_cbnz(__ x8, L);
        __ write_insts_stop("should not reach here");
        __ pin_label(L);
#endif // ASSERT
        __ write_insts_far_jump(YuhuStubRoutines::forward_exception_entry());


        // codeBlob framesize is in words (not VMRegImpl::slot_size)
        RuntimeStub* stub =
                RuntimeStub::new_runtime_stub(name,
                                              &code,
                                              frame_complete,
                                              (framesize >> (LogBytesPerWord - LogBytesPerInt)),
                                              oop_maps, false);
        return stub->entry_point();
    }

    void generate_initial() {
        YuhuStubRoutines::_forward_exception_entry = generate_forward_exception();

        YuhuStubRoutines::_call_stub_entry = generate_call_stub(YuhuStubRoutines::_call_stub_return_address);
        YuhuStubRoutines::_catch_exception_entry = generate_catch_exception();

        // Build this early so it's available for the interpreter.
        YuhuStubRoutines::_throw_StackOverflowError_entry =
                generate_throw_exception("yuhu StackOverflowError throw_exception",
                                         CAST_FROM_FN_PTR(address,
                                                          SharedRuntime::
                                                                  throw_StackOverflowError));
    }

    void generate_all() {
        YuhuStubRoutines::_verify_oop_subroutine_entry     = generate_verify_oop();
    }

public:
    YuhuStubGenerator(CodeBuffer* code, bool all) : YuhuStubCodeGenerator(code) {
        if (all) {
            generate_all();
        } else {
            generate_initial();
        }
    }
    ~YuhuStubGenerator() {

    }
};

void YuhuStubGenerator_generate(CodeBuffer* code, bool all) {
    YuhuStubGenerator g(code, all);
}
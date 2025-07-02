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
    address generate_call_stub(address& return_address) {
        address start = __ current_pc();

        // x0 -> call wrapper (&)
        // x1 -> result (intptr_t*)
        // x2 -> result type
        // x3 -> method
        // x4 -> entry point
        // x5 -> parameters (intptr_t*)
        // x6 -> parameter size (int)

        /* open new stack frame */
        __ write_insts_enter();
        __ write_inst("sub sp, x29, #0xd0");
        /* save method parameters */
        __ write_inst("stur x7, [x29, #-0x08]");
        __ write_inst("stur x6, [x29, #-0x10]");
        __ write_inst("stp x4, x5, [x29, #-0x20]");
        __ write_inst("stp x2, x3, [x29, #-0x30]");
        __ write_inst("stp x0, x1, [x29, #-0x40]");
        /* save callee-saved registers */
        __ write_inst("stp x20, x19, [x29, #-0x50]");
        __ write_inst("stp x22, x21, [x29, #-0x60]");
        __ write_inst("stp x24, x23, [x29, #-0x70]");
        __ write_inst("stp x26, x25, [x29, #-0x80]");
        __ write_inst("stp x28, x27, [x29, #-0x90]");
        /* save floating registers */
        __ write_inst("stp d9, d8, [x29, #-0xa0]");
        __ write_inst("stp d11, d10, [x29, #-0xb0]");
        __ write_inst( "stp d13, d12, [x29, #-0xc0]");
        __ write_inst( "stp d15, d14, [x29, #-0xd0]");
        // x7 holds thread, save it into global register x28
        __ write_inst( "mov x28, x7");
        // x3 holds method, save it into global register x12
        __ write_inst( "mov x12, x3");
        /* just disable compressed oops for now (-XX:-UseCompressedOops), handle it later */
        /* setup java stack frame for parameters */
        // save sp into x20
        __ write_inst( "mov x20, sp");
        // use this way a.
        // SUB X8, SP, W6, UXTW #3 // X8 = SP - N*8
        // AND SP, X8, #0xFFFFFFFFFFFFFFF0 // 16 bytes alignment
        // or another way b.
        // ADD W8, W6, #1 // W8 = N + 1
        __ write_inst( "add w8, w6, #1");
        // AND W8, W8, #0xFFFFFFFE // W8 &= ~1 // to even number
        __ write_inst( "and w8, w8, #0xfffffffe");
        // SUB SP, SP, W8, UXTW #3 // SP -= W8 * 8
        __ write_inst( "sub sp, sp, w8, uxtw #3");
        // way a. saves bytes than b., but may be less efficient than b.
        /* push parameters into stack */
        // jump to label parameters_done
        YuhuLabel parameters_done;
        // cbz w6, #20
        __ write_inst_cbz(YuhuMacroAssembler::w6, parameters_done);
        // -- label loop_start
        address loop_start = __ current_pc();
        // load parameter and move pointer forward + 8 bytes
        __ write_inst( "ldr x8, [x5], #8");
        // this updates NZCV flags
        __ write_inst( "subs w6, w6, #0x1");
        // str x8, [x20, #-8]! has the same effective as stur x8, [x20, #-8]! when using !
        __ write_inst( "str x8, [x20, #-8]!");
        // b.gt loop_start
        // jump to label loop_start
        // b.gt #-12
        __ write_inst_b(YuhuMacroAssembler::gt, loop_start);
        // -- label parameters_done
        __ pin_label(parameters_done);
        /* call java method */
        // save sp in x13 (sender sp)
        __ write_inst( "mov x13, sp");
        __ write_inst( "blr x4");
        return_address = __ current_pc();
        /* store result depending on type */
        // load result address into x3
        __ write_inst( "ldur x3, [x29, #-56]");
        // load result type into x2
        __ write_inst( "ldur x2, [x29, #-48]");
        YuhuLabel exit;
        // this updates NZCV flags
        // check result type is T_OBJECT
        __ write_inst( "cmp x2, #%d", T_OBJECT);
        // jump to check T_LONG
        YuhuLabel check_long;
        // b.ne #12
        __ write_inst_b(YuhuMacroAssembler::ne, check_long);
        // handle T_OBJECT
        __ write_inst( "str x0, [x3]");
        // jump to label exit
        // b.al #56
        __ write_inst_b(YuhuMacroAssembler::al, exit);
        // check result type is T_LONG
        __ pin_label(check_long);
        __ write_inst( "cmp x2, #%d", T_LONG);
        // jump to check T_FLOAT
        YuhuLabel check_float;
        // b.ne #12
        __ write_inst_b(YuhuMacroAssembler::ne, check_float);
        // handle T_LONG
        __ write_inst( "str x0, [x3]");
        // jump to label exit
        // b.al #40
        __ write_inst_b(YuhuMacroAssembler::al, exit);
        // check result type is T_FLOAT
        __ pin_label(check_float);
        __ write_inst( "cmp x2, #%d", T_FLOAT);
        // jump to check T_DOUBLE
        YuhuLabel check_double;
        // b.ne #12
        __ write_inst_b(YuhuMacroAssembler::ne, check_double);
        // handle T_FLOAT
        __ write_inst( "str s0, [x3]");
        // jump to label exit
        // b.al #24
        __ write_inst_b(YuhuMacroAssembler::al, exit);
        // check result type is T_DOUBLE
        __ pin_label(check_double);
        __ write_inst( "cmp x2, #%d", T_DOUBLE);
        // jump to handle result is T_INT
        YuhuLabel check_int;
        // b.ne #12
        __ write_inst_b(YuhuMacroAssembler::ne, check_int);
        // handle T_DOUBLE
        __ write_inst( "str d0, [x3]");
        // jump to label exit
        // b.al #8
        __ write_inst_b(YuhuMacroAssembler::al, exit);
        // the result is T_INT for the rest of scenarios, and use 32-bytes register
        // x3 holds result address
        __ pin_label(check_int);
        __ write_inst( "str w0, [x3]");
        // -- label exit
        __ pin_label(exit);
        /* pop parameters */
        // reassign x20 to pop java parameters
        __ write_inst( "sub x20, x29, #0xd0");
        /* restore callee-saved registers */
        __ write_inst( "ldp d15, d14, [x29, #-208]");
        __ write_inst( "ldp d13, d12, [x29, #-192]");
        __ write_inst( "ldp d11, d10, [x29, #-176]");
        __ write_inst( "ldp d9, d8, [x29, #-160]");
        __ write_inst( "ldp x28, x27, [x29, #-144]");
        __ write_inst( "ldp x26, x25, [x29, #-128]");
        __ write_inst( "ldp x24, x23, [x29, #-112]");
        __ write_inst( "ldp x22, x21, [x29, #-96]");
        __ write_inst( "ldp x20, x19, [x29, #-80]");
        __ write_inst( "ldp x0, x1, [x29, #-64]");
        __ write_inst( "ldur w2, [x29, #-48]");
        __ write_inst( "ldur x3, [x29, #-40]");
        __ write_inst( "ldp x4, x5, [x29, #-32]");
        __ write_inst( "ldp x6, x7, [x29, #-16]");
        /* leave frame and return to caller */
        __ write_insts_leave();
        __ write_inst( "ret");

        // 使用 Disassembler 反汇编
        tty->print_cr("Disassembly:");
        __ code()->decode_all();

        return start;
    }

    address generate_catch_exception() {
        address start = __ current_pc();

        // x28 holds current thread
        /* save exception */
        __ write_inst( "str x0, [x28, #0x8]");
        /* save file */
        address imm64 = (address) __FILE__;
        __ write_insts_mov_imm64(YuhuMacroAssembler::x8, (uint64_t)imm64);
        __ write_inst( "str x8, [x28, #0x10]");
        /* save line */
        int imm32 = (int) __LINE__;
        __ write_insts_mov_imm32(YuhuMacroAssembler::w8, (uint32_t)imm32);
        __ write_inst( "str w8, [x28, #0x18]");
        /* return to VM */
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
        __ write_insts_mov_imm64(YuhuMacroAssembler::x2, (uint64_t)(address) YuhuStubRoutines::verify_oop_count_addr()); // __ lea(c_rarg2, ExternalAddress((address) StubRoutines::verify_oop_count_addr()));
        __ write_inst("ldr x3, [x2]");
        __ write_inst("add x3, x3, 1");
        __ write_inst("str x3, [x2]");

        // object is in r0
        // make sure object is 'reasonable'
        __ write_inst_cbz(YuhuMacroAssembler::x0, exit); // if obj is NULL it is OK

        // Check if the oop is in the right area of memory
        __ write_insts_mov_ptr(YuhuMacroAssembler::x3, (intptr_t) Universe::verify_oop_mask());
        __ write_inst("and x2, x0, x3");
        __ write_insts_mov_ptr(YuhuMacroAssembler::x3, (intptr_t) Universe::verify_oop_bits());

        // Compare c_rarg2 and c_rarg3.  We don't use a compare
        // instruction here because the flags register is live.
        __ write_inst("eor x2, x2, x3");
        __ write_inst_cbnz(YuhuMacroAssembler::x2, error);

        // make sure klass is 'reasonable', which is not zero.
        __ write_insts_load_klass(YuhuMacroAssembler::x0, YuhuMacroAssembler::x0);  // get klass
        __ write_inst_cbz(YuhuMacroAssembler::x0, error);      // if klass is NULL it is broken

        // return if everything seems ok
        __ pin_label(exit);

        __ write_inst( "ldp x3, x2, [sp], #0x10");
        __ write_inst("ret");

        // handle errors
        __ pin_label(error);
        __ write_inst( "ldp x3, x2, [sp], #0x10");

        __ write_insts_pushrange(YuhuMacroAssembler::x0, YuhuMacroAssembler::x29, YuhuMacroAssembler::sp);

        // debug(char* msg, int64_t pc, int64_t regs[])
        __ write_inst_mov_reg(YuhuMacroAssembler::x0, YuhuMacroAssembler::x8); // pass address of error message
        __ write_inst_mov_reg(YuhuMacroAssembler::x1, YuhuMacroAssembler::lr); // pass return address
        __ write_inst_mov_reg(YuhuMacroAssembler::x2, YuhuMacroAssembler::sp); // pass address of regs on stack
#ifndef PRODUCT
        assert(frame::arg_reg_save_area_bytes == 0, "not expecting frame reg save area");
#endif
//        BLOCK_COMMENT("call MacroAssembler::debug");
        __ write_insts_mov_imm64(YuhuMacroAssembler::x8, (uint64_t) CAST_FROM_FN_PTR(address, MacroAssembler::debug64));
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
            __ write_inst_cbnz(YuhuMacroAssembler::x8, L);
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

        __ write_insts_call_VM_leaf(CAST_FROM_FN_PTR(address,
                                         SharedRuntime::exception_handler_for_return_address),
                        YuhuMacroAssembler::x28, YuhuMacroAssembler::x1);
        // we should not really care that lr is no longer the callee
        // address. we saved the value the handler needs in r19 so we can
        // just copy it to r3. however, the C2 handler will push its own
        // frame and then calls into the VM and the VM code asserts that
        // the PC for the frame above the handler belongs to a compiled
        // Java method. So, we restore lr here to satisfy that assert.
        __ write_inst_mov_reg(YuhuMacroAssembler::x30, YuhuMacroAssembler::x19);
        // setup r0 & r3 & clear pending exception
        __ write_inst_mov_reg(YuhuMacroAssembler::x3, YuhuMacroAssembler::x19);
        __ write_inst_mov_reg(YuhuMacroAssembler::x19, YuhuMacroAssembler::x0);
        __ write_inst("ldr x0, [x28, #%d]", in_bytes(Thread::pending_exception_offset()));
        __ write_inst("str xzr, [x28, #%d]", in_bytes(Thread::pending_exception_offset()));

#ifdef ASSERT
        // make sure exception is set
        {
            YuhuLabel L;
            __ write_inst_cbnz(YuhuMacroAssembler::x0, L);
            __ write_insts_stop("YuhuStubRoutines::forward exception: no pending exception (2)");
            __ pin_label(L);
        }
#endif

        // continue at exception handler
        // r0: exception
        // r3: throwing pc
        // r19: exception handler
        __ write_insts_verify_oop(YuhuMacroAssembler::x0);
        __ write_inst_br(YuhuMacroAssembler::x19);

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
        __ write_insts_set_last_java_frame(YuhuMacroAssembler::sp,
                                           YuhuMacroAssembler::x29, (address) NULL, YuhuMacroAssembler::x8);

        // Call runtime
        if (arg1 != YuhuMacroAssembler::noreg) {
//            assert(arg2 != c_rarg1, "clobbered");
            assert(arg2 != YuhuMacroAssembler::x1, "clobbered");
            __ write_inst_mov_reg(YuhuMacroAssembler::x1, arg1);
        }
        if (arg2 != YuhuMacroAssembler::noreg) {
            __ write_inst_mov_reg(YuhuMacroAssembler::x2, arg2);
        }
        __ write_inst_mov_reg(YuhuMacroAssembler::x0, YuhuMacroAssembler::x28);
//        BLOCK_COMMENT("call runtime_entry");
        __ write_insts_mov_imm64(YuhuMacroAssembler::x8, (uint64_t) runtime_entry);
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
        __ write_inst_cbnz(YuhuMacroAssembler::x8, L);
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
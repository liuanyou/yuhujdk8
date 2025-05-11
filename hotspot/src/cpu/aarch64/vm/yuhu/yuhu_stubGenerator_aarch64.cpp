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
        __ write_inst("stp x29, x30, [sp, #-0x10]!");
        __ write_inst("mov x29, sp");
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
        __ write_inst( "mov sp, x29");
        __ write_inst( "ldp x29, x30, [sp], #16");
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

    void generate_initial() {
        YuhuStubRoutines::_call_stub_entry = generate_call_stub(YuhuStubRoutines::_call_stub_return_address);
        YuhuStubRoutines::_catch_exception_entry = generate_catch_exception();
    }

public:
    YuhuStubGenerator(CodeBuffer* code, bool all) : YuhuStubCodeGenerator(code) {
        if (all) {
//            generate_all();
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
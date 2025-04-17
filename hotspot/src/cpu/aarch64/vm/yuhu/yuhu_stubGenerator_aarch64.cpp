//
// Created by Anyou Liu on 2025/4/1.
//
#include "precompiled.hpp"
#include "asm/macroAssembler.hpp"
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
#include "runtime/stubCodeGenerator.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/thread.inline.hpp"
#include "utilities/top.hpp"

class YuhuStubGenerator : public StubCodeGenerator {
private:
    // Helper method to write 4 bytes to the code buffer
    // return the address after writing 4 bytes
    static address write_inst(CodeSection* insts, uint32_t value) {
        address current_end = insts->end();
        CodeSection::csize_t remaining = insts->remaining();

        if (remaining >= 4) {
            *(uint32_t*)current_end = value;
            current_end += 4;
            insts->set_end(current_end);
        } else {
            bool expanded = insts->maybe_expand_to_ensure_remaining(4);
            assert(expanded, "no enough room, write failed");

            current_end = insts->end();
            *(uint32_t*)current_end = value;
            current_end += 4;
            insts->set_end(current_end);
        }
        return current_end;
    }

    address generate_call_stub(address& return_address) {
        CodeSection* insts = _masm->code_section();
        address start = insts->start();
        CodeSection* consts = _masm->code()->consts();
        address current_end = insts->end();
        address limit = insts->limit();

        // x0 -> call wrapper (&)
        // x1 -> result (intptr_t*)
        // x2 -> result type
        // x3 -> method
        // x4 -> entry point
        // x5 -> parameters (intptr_t*)
        // x6 -> parameter size (int)

        /* open new stack frame */
        // stp x29, x30, [sp, #-0x10]!
        write_inst(insts, 0xa9bf7bfd);
        // mov x29, sp
        write_inst(insts, 0x910003fd);
        // sub sp, x29, #0xd0
        write_inst(insts, 0xd10343bf);
        /* save method parameters */
        // stur x7, [x29, #-0x08]
        write_inst(insts, 0xf81f83a7);
        // stur x6, [x29, #-0x10]
        write_inst(insts, 0xf81f03a6);
        // stp x4, x5, [x29, #-0x20]
        write_inst(insts, 0xa93e17a4);
        // stp x2, x3, [x29, #-0x30]
        write_inst(insts, 0xa93d0fa2);
        // stp x0, x1, [x29, #-0x40]
        write_inst(insts, 0xa93c07a0);
        /* save callee-saved registers */
        // stp x20, x19, [x29, #-0x50]
        write_inst(insts, 0xa93b4fb4);
        // stp x22, x21, [x29, #-0x60]
        write_inst(insts, 0xa93a57b6);
        // stp x24, x23, [x29, #-0x70]
        write_inst(insts, 0xa9395fb8);
        // stp x26, x25, [x29, #-0x80]
        write_inst(insts, 0xa93867ba);
        // stp x28, x27, [x29, #-0x90]
        write_inst(insts, 0xa9376fbc);
        /* save floating registers */
        // stp d9, d8, [x29, #-0xa0]
        write_inst(insts, 0x6d3623a9);
        // stp d11, d10, [x29, #-0xb0]
        write_inst(insts, 0x6d352bab);
        // stp d13, d12, [x29, #-0xc0]
        write_inst(insts, 0x6d3433ad);
        // stp d15, d14, [x29, #-0xd0]
        write_inst(insts, 0x6d333baf);
        // x7 holds thread, save it into global register x28
        // mov x28, x7
        write_inst(insts, 0xaa0703fc);
        // x3 holds method, save it into global register x12
        // mov x12, x3
        write_inst(insts, 0xaa0303ec);
        /* just disable compressed oops for now (-XX:-UseCompressedOops), handle it later */
        /* setup java stack frame for parameters */
        // save sp into x20
        // mov x20, sp
        write_inst(insts, 0x910003f4);
        // use this way a.
        // SUB X8, SP, W6, UXTW #3 // X8 = SP - N*8
        // AND SP, X8, #0xFFFFFFFFFFFFFFF0 // 16 bytes alignment
        // or another way b.
        // ADD W8, W6, #1 // W8 = N + 1
        write_inst(insts, 0x110004c8);
        // AND W8, W8, #0xFFFFFFFE // W8 &= ~1 // to even number
        write_inst(insts, 0x121f7908);
        // SUB SP, SP, W8, UXTW #3 // SP -= W8 * 8
        write_inst(insts, 0xcb284fff);
        // way a. saves bytes than b., but may be less efficient than b.
        /* push parameters into stack */
        // cbz w6, #20
        // jump to label parameters_done
        write_inst(insts, 0x340000a6);
        // -- label loop_start
        // load parameter and move pointer forward + 8 bytes
        // ldr x8, [x5], #8
        write_inst(insts, 0xf84084a8);
        // subs w6, w6, #0x1
        // this updates NZCV flags
        write_inst(insts, 0x710004c6);
        // str x8, [x20, #-8]!
        // has the same effective as stur x8, [x20, #-8]! when using !
        write_inst(insts, 0xf81f8e88);
        // b.gt loop_start / b.gt #-12
        // jump to label loop_start
        write_inst(insts, 0x54ffffac);
        // -- label parameters_done
        /* call java method */
        // mov x13, sp
        // save sp in x13 (sender sp)
        write_inst(insts, 0x910003ed);
        // blr x4
        return_address = write_inst(insts, 0xd63f0080);
        /* store result depending on type */
        // ldur x3, [x29, #-56]
        // load result address into x3
        write_inst(insts, 0xf85c83a3);
        // ldur x2, [x29, #-48]
        // load result type into x2
        write_inst(insts, 0xf85d03a2);
        // cmp x2, #0xc
        // this updates NZCV flags
        // check result type is T_OBJECT
        write_inst(insts, 0xf100305f);
        // b.ne #12
        // jump to check T_LONG
        write_inst(insts, 0x54000061);
        // handle T_OBJECT
        // str x0, [x3]
        write_inst(insts, 0xf9000060);
        // b.al #56
        // jump to label exit
        write_inst(insts, 0x540001ce);
        // cmp x2, #0xb
        // check result type is T_LONG
        write_inst(insts, 0xf1002c5f);
        // b.ne #12
        // jump to check T_FLOAT
        write_inst(insts, 0x54000061);
        // handle T_LONG
        // str x0, [x3]
        write_inst(insts, 0xf9000060);
        // b.al #40
        // jump to label exit
        write_inst(insts, 0x5400014e);
        // cmp x2, #0x6
        // check result type is T_FLOAT
        write_inst(insts, 0xf100185f);
        // b.ne #12
        // jump to check T_DOUBLE
        write_inst(insts, 0x54000061);
        // handle T_FLOAT
        // str s0, [x3]
        write_inst(insts, 0xbd000060);
        // b.al #24
        // jump to label exit
        write_inst(insts, 0x540000ce);
        // cmp x2, #0x7
        // check result type is T_DOUBLE
        write_inst(insts, 0xf1001c5f);
        // b.ne #12
        // jump to handle result is T_INT
        write_inst(insts, 0x54000061);
        // handle T_DOUBLE
        // str d0, [x3]
        write_inst(insts, 0xfd000060);
        // b.al #8
        // jump to label exit
        write_inst(insts, 0x5400004e);
        // the result is T_INT for the rest of scenarios, and use 32-bytes register
        // str w0, [x3]
        // x3 holds result address
        write_inst(insts, 0xb9000060);
        // -- label exit
        /* pop parameters */
        // sub x20, x29, #0xd0
        // reassign x20 to pop java parameters
        write_inst(insts, 0xd10343b4);
        /* restore callee-saved registers */
        // ldp d15, d14, [x29, #-208]
        write_inst(insts, 0x6d733baf);
        // ldp d13, d12, [x29, #-192]
        write_inst(insts, 0x6d7433ad);
        // ldp d11, d10, [x29, #-176]
        write_inst(insts, 0x6d752bab);
        // ldp d9, d8, [x29, #-160]
        write_inst(insts, 0x6d7623a9);
        // ldp x28, x27, [x29, #-144]
        write_inst(insts, 0xa9776fbc);
        // ldp x26, x25, [x29, #-128]
        write_inst(insts, 0xa97867ba);
        // ldp x24, x23, [x29, #-112]
        write_inst(insts, 0xa9795fb8);
        // ldp x22, x21, [x29, #-96]
        write_inst(insts, 0xa97a57b6);
        // ldp x20, x19, [x29, #-80]
        write_inst(insts, 0xa97b4fb4);
        // ldp x0, x1, [x29, #-64]
        write_inst(insts, 0xa97c07a0);
        // ldur w2, [x29, #-48]
        write_inst(insts, 0xb85d03a2);
        // ldur x3, [x29, #-40]
        write_inst(insts, 0xf85d83a3);
        // ldp x4, x5, [x29, #-32]
        write_inst(insts, 0xa97e17a4);
        // ldp x6, x7, [x29, #-16]
        write_inst(insts, 0xa97f1fa6);
        /* leave frame and return to caller */
        // mov sp, x29
        write_inst(insts, 0x910003bf);
        // ldp x29, x30, [sp], #16
        write_inst(insts, 0xa8c17bfd);
        // ret
        write_inst(insts, 0xd65f03c0);

        // 使用 Disassembler 反汇编
        tty->print_cr("Disassembly:");
        Disassembler::decode(start, start + 4);

        _masm->code()->decode_all();

        return start;
    }

    void generate_initial() {
        YuhuStubRoutines::_call_stub_entry = generate_call_stub(YuhuStubRoutines::_call_stub_return_address);
    }

public:
    YuhuStubGenerator(CodeBuffer* code, bool all) : StubCodeGenerator(code) {
        if (all) {
//            generate_all();
        } else {
            generate_initial();
        }
    }
};

void YuhuStubGenerator_generate(CodeBuffer* code, bool all) {
    YuhuStubGenerator g(code, all);
}
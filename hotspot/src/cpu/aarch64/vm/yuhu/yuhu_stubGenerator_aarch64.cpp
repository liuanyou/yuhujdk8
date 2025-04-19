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
#include <keystone/keystone.h>

class YuhuStubGenerator : public StubCodeGenerator {
private:
    ks_engine *ks;
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
private:
    uint32_t machine_code(const char* assembly) {
        unsigned char *encode;
        size_t size;
        size_t count;
        int ks_result = ks_asm(ks, assembly, 0, &encode, &size, &count);
        assert(ks_result == KS_ERR_OK, "Failed to assemble!");
        uint32_t machine_code;
        memcpy(&machine_code, encode, sizeof(uint32_t));
        ks_free(encode);
        return machine_code;
    }

    address write_inst(CodeSection* insts, const char* assembly) {
        return write_inst(insts, machine_code(assembly));
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
        write_inst(insts, "stp x29, x30, [sp, #-0x10]!");
        write_inst(insts, "mov x29, sp");
        write_inst(insts, "sub sp, x29, #0xd0");
        /* save method parameters */
        write_inst(insts, "stur x7, [x29, #-0x08]");
        write_inst(insts, "stur x6, [x29, #-0x10]");
        write_inst(insts, "stp x4, x5, [x29, #-0x20]");
        write_inst(insts, "stp x2, x3, [x29, #-0x30]");
        write_inst(insts, "stp x0, x1, [x29, #-0x40]");
        /* save callee-saved registers */
        write_inst(insts, "stp x20, x19, [x29, #-0x50]");
        write_inst(insts, "stp x22, x21, [x29, #-0x60]");
        write_inst(insts, "stp x24, x23, [x29, #-0x70]");
        write_inst(insts, "stp x26, x25, [x29, #-0x80]");
        write_inst(insts, "stp x28, x27, [x29, #-0x90]");
        /* save floating registers */
        write_inst(insts, "stp d9, d8, [x29, #-0xa0]");
        write_inst(insts, "stp d11, d10, [x29, #-0xb0]");
        write_inst(insts, "stp d13, d12, [x29, #-0xc0]");
        write_inst(insts, "stp d15, d14, [x29, #-0xd0]");
        // x7 holds thread, save it into global register x28
        write_inst(insts, "mov x28, x7");
        // x3 holds method, save it into global register x12
        write_inst(insts, "mov x12, x3");
        /* just disable compressed oops for now (-XX:-UseCompressedOops), handle it later */
        /* setup java stack frame for parameters */
        // save sp into x20
        write_inst(insts, "mov x20, sp");
        // use this way a.
        // SUB X8, SP, W6, UXTW #3 // X8 = SP - N*8
        // AND SP, X8, #0xFFFFFFFFFFFFFFF0 // 16 bytes alignment
        // or another way b.
        // ADD W8, W6, #1 // W8 = N + 1
        write_inst(insts, "add w8, w6, #1");
        // AND W8, W8, #0xFFFFFFFE // W8 &= ~1 // to even number
        write_inst(insts, "and w8, w8, #0xfffffffe");
        // SUB SP, SP, W8, UXTW #3 // SP -= W8 * 8
        write_inst(insts, "sub sp, sp, w8, uxtw #3");
        // way a. saves bytes than b., but may be less efficient than b.
        /* push parameters into stack */
        // jump to label parameters_done
        write_inst(insts, "cbz w6, #20");
        // -- label loop_start
        // load parameter and move pointer forward + 8 bytes
        write_inst(insts, "ldr x8, [x5], #8");
        // this updates NZCV flags
        write_inst(insts, "subs w6, w6, #0x1");
        // str x8, [x20, #-8]! has the same effective as stur x8, [x20, #-8]! when using !
        write_inst(insts, "str x8, [x20, #-8]!");
        // b.gt loop_start
        // jump to label loop_start
        write_inst(insts, "b.gt #-12");
        // -- label parameters_done
        /* call java method */
        // save sp in x13 (sender sp)
        write_inst(insts, "mov x13, sp");
        return_address = write_inst(insts, "blr x4");
        /* store result depending on type */
        // load result address into x3
        write_inst(insts, "ldur x3, [x29, #-56]");
        // load result type into x2
        write_inst(insts, "ldur x2, [x29, #-48]");
        // this updates NZCV flags
        // check result type is T_OBJECT
        write_inst(insts, "cmp x2, #0xc");
        // jump to check T_LONG
        write_inst(insts, "b.ne #12");
        // handle T_OBJECT
        write_inst(insts, "str x0, [x3]");
        // jump to label exit
        write_inst(insts, "b.al #56");
        // check result type is T_LONG
        write_inst(insts, "cmp x2, #0xb");
        // jump to check T_FLOAT
        write_inst(insts, "b.ne #12");
        // handle T_LONG
        write_inst(insts, "str x0, [x3]");
        // jump to label exit
        write_inst(insts, "b.al #40");
        // check result type is T_FLOAT
        write_inst(insts, "cmp x2, #0x6");
        // jump to check T_DOUBLE
        write_inst(insts, "b.ne #12");
        // handle T_FLOAT
        write_inst(insts, "str s0, [x3]");
        // jump to label exit
        write_inst(insts, "b.al #24");
        // check result type is T_DOUBLE
        write_inst(insts, "cmp x2, #0x7");
        // jump to handle result is T_INT
        write_inst(insts, "b.ne #12");
        // handle T_DOUBLE
        write_inst(insts, "str d0, [x3]");
        // jump to label exit
        write_inst(insts, "b.al #8");
        // the result is T_INT for the rest of scenarios, and use 32-bytes register
        // x3 holds result address
        write_inst(insts, "str w0, [x3]");
        // -- label exit
        /* pop parameters */
        // reassign x20 to pop java parameters
        write_inst(insts, "sub x20, x29, #0xd0");
        /* restore callee-saved registers */
        write_inst(insts, "ldp d15, d14, [x29, #-208]");
        write_inst(insts, "ldp d13, d12, [x29, #-192]");
        write_inst(insts, "ldp d11, d10, [x29, #-176]");
        write_inst(insts, "ldp d9, d8, [x29, #-160]");
        write_inst(insts, "ldp x28, x27, [x29, #-144]");
        write_inst(insts, "ldp x26, x25, [x29, #-128]");
        write_inst(insts, "ldp x24, x23, [x29, #-112]");
        write_inst(insts, "ldp x22, x21, [x29, #-96]");
        write_inst(insts, "ldp x20, x19, [x29, #-80]");
        write_inst(insts, "ldp x0, x1, [x29, #-64]");
        write_inst(insts, "ldur w2, [x29, #-48]");
        write_inst(insts, "ldur x3, [x29, #-40]");
        write_inst(insts, "ldp x4, x5, [x29, #-32]");
        write_inst(insts, "ldp x6, x7, [x29, #-16]");
        /* leave frame and return to caller */
        write_inst(insts, "mov sp, x29");
        write_inst(insts, "ldp x29, x30, [sp], #16");
        write_inst(insts, "ret");

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
        ks_err err = ks_open(KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN, &ks);
        assert(err == KS_ERR_OK, "Failed to initialize Keystone!");
        if (all) {
//            generate_all();
        } else {
            generate_initial();
        }
    }
    ~YuhuStubGenerator() {
        if (ks != nullptr) {
            ks_close(ks);
            ks = nullptr;
        }
    }
};

void YuhuStubGenerator_generate(CodeBuffer* code, bool all) {
    YuhuStubGenerator g(code, all);
}
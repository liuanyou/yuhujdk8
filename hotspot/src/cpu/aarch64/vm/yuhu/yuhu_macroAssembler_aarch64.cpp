//
// Created by Anyou Liu on 2025/4/29.
//

#include "precompiled.hpp"
#include "asm/yuhu/yuhu_macroAssembler.hpp"
#include "interpreter/interpreter.hpp"
#include "interpreter/interpreterRuntime.hpp"
#include "oops/arrayOop.hpp"
#include "oops/markOop.hpp"
#include "oops/methodData.hpp"
#include "oops/method.hpp"
#include "prims/jvmtiExport.hpp"
#include "prims/jvmtiRedefineClassesTrace.hpp"
#include "prims/jvmtiThreadState.hpp"
#include "runtime/basicLock.hpp"
#include "runtime/biasedLocking.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/thread.inline.hpp"

address YuhuMacroAssembler::write_inst(uint32_t value) {
    CodeSection* insts = code_section();
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

address YuhuMacroAssembler::current_pc() {
    CodeSection* insts = code_section();
    return insts->end();
}

address YuhuMacroAssembler::write_inst(const char* assembly) {
    return write_inst(machine_code(assembly));
}

address YuhuMacroAssembler::write_inst(const char* assembly_format, YuhuRegister reg, unsigned int imm16) {
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg), imm16);
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_b(long offset) {
    char buffer[50];
    snprintf(buffer, sizeof(buffer), "b #%d", offset);
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_insts_stop(const char *msg) {
    address ip = current_pc();
    write_insts_pusha();
    write_insts_mov_ptr(x0, (uintptr_t)(address)msg);
    write_insts_mov_ptr(x1, (uintptr_t)ip);
    write_inst("mov x2, sp");
    write_insts_mov_imm64(x3, (uint64_t)CAST_FROM_FN_PTR(address, MacroAssembler::debug64));
    write_inst("blr x3");
    write_inst("hlt #0");
    return current_pc();
}

address YuhuMacroAssembler::write_insts_pusha() {
    write_inst("stp x0, x1, [sp, #-0x100]!");
    write_inst("stp x2, x3, [sp, #0x10]");
    write_inst("stp x4, x5, [sp, #0x20]");
    write_inst("stp x6, x7, [sp, #0x30]");
    write_inst("stp x8, x9, [sp, #0x40]");
    write_inst("stp x10, x11, [sp, #0x50]");
    write_inst("stp x12, x13, [sp, #0x60]");
    write_inst("stp x14, x15, [sp, #0x70]");
    write_inst("stp x16, x17, [sp, #0x80]");
    write_inst("stp x18, x19, [sp, #0x90]");
    write_inst("stp x20, x21, [sp, #0xa0]");
    write_inst("stp x22, x23, [sp, #0xb0]");
    write_inst("stp x24, x25, [sp, #0xc0]");
    write_inst("stp x26, x27, [sp, #0xd0]");
    write_inst("stp x28, x29, [sp, #0xe0]");
    write_inst("stp x30, xzr, [sp, #0xf0]");
    return current_pc();
}

address YuhuMacroAssembler::write_insts_mov_ptr(YuhuRegister reg, uintptr_t imm64) {
    // get low 16 bits
    write_inst("movz %s, #%d", reg, imm64 & 0xffff);
    // get 16-32 bits
    write_inst("movk %s, #%d, lsl #16", reg, (imm64 >> 16) & 0xffff);
    // get 32-48 bits
    write_inst("movk %s, #%d, lsl #32", reg, (imm64 >> 32) & 0xffff);
    // get high 16 bits
    write_inst("movk %s, #%d, lsl #48", reg, (imm64 >> 48) & 0xffff);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_mov_imm64(YuhuRegister reg, uint64_t imm64) {
    // get low 16 bits
    write_inst("movz %s, #%d", reg, imm64 & 0xffff);
    // get 16-32 bits
    write_inst("movk %s, #%d, lsl #16", reg, (imm64 >> 16) & 0xffff);
    // get 32-48 bits
    write_inst("movk %s, #%d, lsl #32", reg, (imm64 >> 32) & 0xffff);
    // get high 16 bits
    write_inst("movk %s, #%d, lsl #48", reg, (imm64 >> 48) & 0xffff);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_mov_imm32(YuhuRegister reg, uint32_t imm32) {
    // get low 16 bits
    write_inst( "movz %s, #%d", reg, imm32 & 0xffff);
    // get high 16 bits
    write_inst( "movk %s, #%d, lsl #16", reg, (imm32 >> 16) & 0xffff);
    return current_pc();
}
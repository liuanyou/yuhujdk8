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

address YuhuMacroAssembler::target(YuhuLabel& L, address branch_pc) {
    if (L.is_bound()) {
        int loc = L.loc();
        if (code_section()->index() == CodeBuffer::locator_sect(loc)) {
            return code_section()->start() + CodeBuffer::locator_pos(loc);
        } else {
            return code_section()->outer()->locator_address(loc);
        }
    } else {
        assert(code_section()->allocates2(branch_pc), "sanity");
        address base = code_section()->start();
        int patch_loc = CodeBuffer::locator(branch_pc - base, code_section()->index());
        L.add_patch_at(code_section()->outer(), patch_loc);

        // Need to return a pc, doesn't matter what it is since it will be
        // replaced during resolution later.
        // Don't return NULL or badAddress, since branches shouldn't overflow.
        // Don't return base either because that could overflow displacements
        // for shorter branches.  It will get checked when bound.
        return branch_pc;
    }
}

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

address YuhuMacroAssembler::write_inst(const char* assembly_format, YuhuRegister reg, unsigned int imm32) {
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg), imm32);
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst(const char* assembly_format, YuhuFloatRegister reg, unsigned int imm32) {
    char buffer[50];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, f_reg_name(reg), imm32);
#pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst(const char* assembly_format, YuhuRegister reg1, YuhuRegister reg2, unsigned int imm32) {
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg1), reg_name(reg2), imm32);
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst(const char* assembly_format, YuhuRegister reg1, YuhuRegister reg2, YuhuRegister reg3, unsigned int imm32) {
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg1), reg_name(reg2), reg_name(reg3), imm32);
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst(const char* assembly_format, unsigned int imm32) {
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, imm32);
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_regs(const char* assembly_format, YuhuRegister reg1, YuhuRegister reg2) {
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg1), reg_name(reg2));
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_regs(const char* assembly_format, YuhuRegister reg1, YuhuRegister reg2, YuhuRegister reg3) {
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg1), reg_name(reg2), reg_name(reg3));
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_mov_reg(YuhuRegister reg1, YuhuRegister reg2) {
    char buffer[50];
    snprintf(buffer, sizeof(buffer), "mov %s, %s", reg_name(reg1), reg_name(reg2));
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_br(YuhuRegister reg) {
    char buffer[50];
    snprintf(buffer, sizeof(buffer), "br %s", reg_name(reg));
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_blr(YuhuRegister reg) {
    char buffer[50];
    snprintf(buffer, sizeof(buffer), "blr %s", reg_name(reg));
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_b(address target) {
    long offset = target - current_pc();
    char buffer[50];
    snprintf(buffer, sizeof(buffer), "b #%d", offset);
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_b(YuhuLabel& label) {
    if (label.is_bound()) {
        write_inst_b(target(label, current_pc()));
    } else {
        label.add_patch_at(code(), locator());
        write_inst_b(current_pc());
    }
    return current_pc();
}

address YuhuMacroAssembler::write_inst_b(YuhuCond cond, address target) {
    long offset = target - current_pc();
    char buffer[50];
    snprintf(buffer, sizeof(buffer), "b.%s #%d", cond_name(cond), offset);
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_b(YuhuCond cond, YuhuLabel& label) {
    if (label.is_bound()) {
        write_inst_b(cond, target(label, current_pc()));
    } else {
        label.add_patch_at(code(), locator());
        write_inst_b(cond, current_pc());
    }
    return current_pc();
}

address YuhuMacroAssembler::write_inst_cbz(YuhuRegister reg, address target) {
    long offset = target - current_pc();
    return write_inst("cbz %s, #%d", reg, offset);
}

address YuhuMacroAssembler::write_inst_cbz(YuhuRegister reg, YuhuLabel &label) {
    if (label.is_bound()) {
        write_inst_cbz(reg, target(label, current_pc()));
    } else {
        label.add_patch_at(code(), locator());
        write_inst_cbz(reg, current_pc());
    }
    return current_pc();
}

address YuhuMacroAssembler::write_inst_cbnz(YuhuRegister reg, address target) {
    long offset = target - current_pc();
    return write_inst("cbnz %s, #%d", reg, offset);
}

address YuhuMacroAssembler::write_inst_cbnz(YuhuRegister reg, YuhuLabel &label) {
    if (label.is_bound()) {
        write_inst_cbnz(reg, target(label, current_pc()));
    } else {
        label.add_patch_at(code(), locator());
        write_inst_cbnz(reg, current_pc());
    }
    return current_pc();
}

address YuhuMacroAssembler::write_inst_tbz(YuhuRegister reg, int bitpos, address target) {
    long offset = target - current_pc();
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), "tbz %s, #%d, #%d", reg_name(reg), bitpos, offset);
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_tbz(YuhuRegister reg, int bitpos, YuhuLabel &label) {
    if (label.is_bound()) {
        write_inst_tbz(reg, bitpos, target(label, current_pc()));
    } else {
        label.add_patch_at(code(), locator());
        write_inst_tbz(reg, bitpos, current_pc());
    }
    return current_pc();
}

address YuhuMacroAssembler::write_inst_tbnz(YuhuRegister reg, int bitpos, address target) {
    long offset = target - current_pc();
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), "tbnz %s, #%d, #%d", reg_name(reg), bitpos, offset);
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_tbnz(YuhuRegister reg, int bitpos, YuhuLabel &label) {
    if (label.is_bound()) {
        write_inst_tbnz(reg, bitpos, target(label, current_pc()));
    } else {
        label.add_patch_at(code(), locator());
        write_inst_tbnz(reg, bitpos, current_pc());
    }
    return current_pc();
}

address YuhuMacroAssembler::write_inst_adrp(YuhuRegister reg, address target) {
    // 确保目标地址页面对齐
    uint64_t target_page = ((uint64_t)target >> 12) << 12;
    return write_inst("adrp %s, 0x%lx", reg, target_page);
}

address YuhuMacroAssembler::write_inst_adr(YuhuRegister reg, address target) {
    long offset = target - current_pc();
    return write_inst("adr %s, #%d", reg, offset);
}

address YuhuMacroAssembler::write_inst_adr(YuhuRegister reg, YuhuLabel &label) {
    if (label.is_bound()) {
        write_inst_adr(reg, target(label, current_pc()));
    } else {
        label.add_patch_at(code(), locator());
        write_inst_adr(reg, current_pc());
    }
    return current_pc();
}

void YuhuMacroAssembler::pin_label(YuhuLabel& label) {
    if (label.is_bound()) {
        // Assembler can bind a label more than once to the same place.
        guarantee(label.loc() == locator(), "attempt to redefine label");
        return;
    }
    label.bind_loc(locator());
    label.patch_instructions(this);
}

address YuhuMacroAssembler::write_inst_pop_ptr(YuhuRegister r) {
    return write_inst("ldr %s, [x20], #%d", r, wordSize);
}

address YuhuMacroAssembler::write_inst_pop_i(YuhuRegister r) {
    return write_inst("ldr %s, [x20], #%d", w_reg(r), wordSize);
}

address YuhuMacroAssembler::write_inst_pop_l(YuhuRegister r) {
    return write_inst("ldr %s, [x20], #%d", r, 2 * Interpreter::stackElementSize);
}

address YuhuMacroAssembler::write_inst_push_ptr(YuhuRegister r) {
    return write_inst("str %s, [x20, #%d]!", r, -wordSize);
}

address YuhuMacroAssembler::write_inst_push_i(YuhuRegister r) {
    return write_inst("str %s, [x20, #%d]!", r, -wordSize);
}

address YuhuMacroAssembler::write_inst_push_l(YuhuRegister r) {
    return write_inst("str %s, [x20, #%d]!", r, 2 * -wordSize);
}

address YuhuMacroAssembler::write_inst_pop_f(YuhuFloatRegister r) {
    return write_inst("ldr %s, [x20], #%d", r, wordSize);
}

address YuhuMacroAssembler::write_inst_pop_d(YuhuFloatRegister r) {
    return write_inst("ldr d0, [x20], #%d", 2 * Interpreter::stackElementSize);
}

address YuhuMacroAssembler::write_inst_push_f(YuhuFloatRegister r) {
    return write_inst("str s0, [x20, #%d]!", r, -wordSize);
}

address YuhuMacroAssembler::write_inst_push_d(YuhuFloatRegister r) {
    return write_inst("str d0, [x20, #%d]!", r, 2 * -wordSize);
}

address YuhuMacroAssembler::write_insts_enter() {
    write_inst("stp x29, x30, [sp, #-0x10]!");
    write_inst("mov x29, sp");
    return current_pc();
}

address YuhuMacroAssembler::write_insts_leave() {
    write_inst("mov sp, x29");
    write_inst("ldp x29, x30, [sp], #0x10");
    return current_pc();
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

address YuhuMacroAssembler::write_insts_pushrange(YuhuRegister start, YuhuRegister end, YuhuRegister stack) {
    write_inst("str %s, [%s, #-%d]!", start, stack, (end - start + 1) * wordSize);
    if (end > start) {
        for (int i = start + 1; i <= end; i++) {
            write_inst("str %s, [%s, #%d]", (YuhuRegister) (start+1), stack, (i - start) * wordSize);
        }
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_popa() {
    write_inst("ldp x2, x3, [sp, #0x10]");
    write_inst("ldp x4, x5, [sp, #0x20]");
    write_inst("ldp x6, x7, [sp, #0x30]");
    write_inst("ldp x8, x9, [sp, #0x40]");
    write_inst("ldp x10, x11, [sp, #0x50]");
    write_inst("ldp x12, x13, [sp, #0x60]");
    write_inst("ldp x14, x15, [sp, #0x70]");
    write_inst("ldp x16, x17, [sp, #0x80]");
    write_inst("ldp x18, x19, [sp, #0x90]");
    write_inst("ldp x20, x21, [sp, #0xa0]");
    write_inst("ldp x22, x23, [sp, #0xb0]");
    write_inst("ldp x24, x25, [sp, #0xc0]");
    write_inst("ldp x26, x27, [sp, #0xd0]");
    write_inst("ldp x28, x29, [sp, #0xe0]");
    write_inst("ldp x30, xzr, [sp, #0xf0]");
    write_inst("ldp x0, x1, [sp], #0x100");
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

address YuhuMacroAssembler::write_insts_dispatch_base(TosState state, address* table, bool verifyoop) {
//    if (VerifyActivationFrameSize) {
//        Unimplemented();
//    }
//    if (verifyoop) {
//        verify_oop(r0, state);
//    }
    if (table == YuhuInterpreter::dispatch_table(state)) {
        write_inst("add w9, %s, #%d", w8, YuhuInterpreter::distance_from_dispatch_table(state));
        write_inst("ldr x9, [x21, w9, uxtw #3]");
    } else {
        mov(rscratch2, (address)table);
        ldr(rscratch2, Address(rscratch2, rscratch1, Address::uxtw(3)));
    }
    write_inst("br x9");
    return current_pc();
}

address YuhuMacroAssembler::write_insts_get_dispatch() {
    uint64_t offset;
    write_insts_adrp(x21, (address) YuhuInterpreter::dispatch_table(), offset);
    write_inst("add x21, x21, #%d", offset); // lea(rdispatch, Address(rdispatch, offset));
    return current_pc();
}

address YuhuMacroAssembler::write_insts_dispatch_next(TosState state, int step) {
    // load next bytecode
    write_inst("ldrb %s, [x22, #%d]!", w8, step);
    write_insts_dispatch_base(state, YuhuInterpreter::dispatch_table(state));
    return current_pc();
}

address YuhuMacroAssembler::write_insts_far_jump(address entry, CodeBuffer *cbuf, YuhuRegister tmp) {
    assert(ReservedCodeCacheSize < 4*G, "branch out of range");
    assert(CodeCache::find_blob(entry) != NULL,
           "destination of far call not found in code cache");
    if (far_branches()) {
        uint64_t offset;
        // We can use ADRP here because we know that the total size of
        // the code cache cannot exceed 2Gb.
        write_insts_adrp(tmp, entry, offset);
        write_inst("add %s, %s, %d", tmp, tmp, offset);
//        if (cbuf) cbuf->set_insts_mark();
        write_inst_br(tmp);
    } else {
//        if (cbuf) cbuf->set_insts_mark();
        write_inst_b(entry);
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_adrp(YuhuRegister reg, const address &dest, uint64_t &byte_offset) {
//    relocInfo::relocType rtype = dest.rspec().reloc()->type();
    uint64_t low_page = (uint64_t)CodeCache::low_bound() >> 12;
    uint64_t high_page = (uint64_t)(CodeCache::high_bound()-1) >> 12;
    uint64_t dest_page = (uint64_t) dest >> 12;
    int64_t offset_low = dest_page - low_page;
    int64_t offset_high = dest_page - high_page;

    assert(is_valid_AArch64_address(dest), "bad address");
//    assert(dest.getMode() == Address::literal, "ADRP must be applied to a literal address");

//    InstructionMark im(this);
//    code_section()->relocate(inst_mark(), dest.rspec());
    // 8143067: Ensure that the adrp can reach the dest from anywhere within
    // the code cache so that if it is relocated we know it will still reach
    if (offset_high >= -(1<<20) && offset_low < (1<<20)) {
        write_inst_adrp(reg, dest);
    } else {
        uint64_t target = (uint64_t)dest;
        uint64_t adrp_target
                = (target & 0xffffffffUL) | ((uint64_t)current_pc() & 0xffff00000000UL);
        write_inst_adrp(reg, (address)adrp_target);
        write_inst("movk %s, #%d, lsl #32", reg, (target >> 32) & 0xffff);
    }
    byte_offset = (uint64_t)dest & 0xfff;
    return current_pc();
}

address YuhuMacroAssembler::write_insts_set_last_java_frame(YuhuRegister last_java_sp,
                                                            YuhuRegister last_java_fp,
                                                            address last_java_pc,
                                                            YuhuRegister scratch) {
    if (last_java_pc != NULL) {
        write_inst_adr(scratch, last_java_pc);
    } else {
        // FIXME: This is almost never correct.  We should delete all
        // cases of set_last_Java_frame with last_java_pc=NULL and use the
        // correct return address instead.
        write_inst_adr(scratch, current_pc());
    }

    write_inst("str %s, [x28, #%d]", scratch, in_bytes(
            JavaThread::frame_anchor_offset() + JavaFrameAnchor::last_Java_pc_offset()));

    write_insts_set_last_java_frame(last_java_sp, last_java_fp, noreg, scratch);

    return current_pc();
}

address YuhuMacroAssembler::write_insts_set_last_java_frame(YuhuRegister last_java_sp,
                                         YuhuRegister last_java_fp,
                                         YuhuLabel &L,
                                         YuhuRegister scratch) {
    if (L.is_bound()) {
        write_insts_set_last_java_frame(last_java_sp, last_java_fp, target(L, current_pc()), scratch);
    } else {
//        InstructionMark im(this);
        L.add_patch_at(code(), locator());
        write_insts_set_last_java_frame(last_java_sp, last_java_fp, (address)NULL, scratch);
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_reset_last_java_frame(bool clear_fp) {
    // we must set sp to zero to clear frame
    write_inst("str xzr, [x28, #%d]", in_bytes(JavaThread::last_Java_sp_offset()));
    // must clear fp, so that compiled frames are not confused; it is
    // possible that we need it only for debugging
    if (clear_fp) {
        write_inst("str xzr, [x28, #%d]", in_bytes(JavaThread::last_Java_fp_offset()));
    }

    // Always clear the pc because it could have been set by make_walkable()
    write_inst("str xzr, [x28, #%d]", in_bytes(JavaThread::last_Java_pc_offset()));
    return current_pc();
}

address YuhuMacroAssembler::write_insts_set_last_java_frame(YuhuRegister last_java_sp,
                                                            YuhuRegister last_java_fp,
                                                            YuhuRegister last_java_pc,
                                                            YuhuRegister scratch) {
    if (last_java_pc != noreg) {
        write_inst("str %s, [x28, #%d]", last_java_pc, in_bytes(
                JavaThread::frame_anchor_offset() + JavaFrameAnchor::last_Java_pc_offset()));
    }

    // determine last_java_sp register
    if (last_java_sp == sp) {
        write_inst_mov_reg(scratch, sp);
        last_java_sp = scratch;
    } else if (last_java_sp == noreg) {
        last_java_sp = x20;
    }

    write_inst("str %s, [x28, #%d]", last_java_sp, in_bytes(JavaThread::last_Java_sp_offset()));

    // last_java_fp is optional
    if (last_java_fp != noreg) {
        write_inst("str %s, [x28, #%d]", last_java_fp, in_bytes(JavaThread::last_Java_fp_offset()));
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_call_VM_base(YuhuRegister oop_result, YuhuRegister java_thread, YuhuRegister last_java_sp, address entry_point, int number_of_arguments, bool check_exceptions) {
    // interpreter specific
    //
    // Note: Could avoid restoring locals ptr (callee saved) - however doesn't
    //       really make a difference for these runtime calls, since they are
    //       slow anyway. Btw., bcp must be saved/restored since it may change
    //       due to GC.
    // assert(java_thread == noreg , "not expecting a precomputed java thread");
    write_insts_save_bcp();
#ifdef ASSERT
    {
        YuhuLabel L;
        write_inst("ldr x8, [x29, #%d]", frame::interpreter_frame_last_sp_offset * wordSize);
        write_inst_cbz(x8, L);
        write_insts_stop("InterpreterMacroAssembler::call_VM_leaf_base:"
             " last_sp != NULL");
        pin_label(L);
    }
#endif /* ASSERT */
    // super call
    write_insts_final_call_VM_base(oop_result, noreg, last_java_sp,
                                 entry_point, number_of_arguments,
                                 check_exceptions);
// interpreter specific
    write_insts_restore_bcp();
    write_insts_restore_locals();
    return current_pc();
}

address YuhuMacroAssembler::write_insts_call_VM_leaf_base(address entry_point,
                                       int number_of_arguments,
                                       YuhuLabel *retaddr) {
    write_inst("stp x8, x12, [sp, #-0x10]!");

    // We add 1 to number_of_arguments because the thread in arg0 is
    // not counted
    write_insts_mov_imm64(x8, (uint64_t) entry_point);
    write_inst("blr x8");
    if (retaddr) {
        pin_label(*retaddr);
    }

    write_inst("ldp x8, x12, [sp], #0x10");
    write_inst("isb");
    return current_pc();
}

address YuhuMacroAssembler::write_insts_verify_oop(YuhuRegister reg, const char* s) {
    if (!VerifyOops) return current_pc();

    // Pass register number to verify_oop_subroutine
    const char* b = NULL;
    {
        ResourceMark rm;
        stringStream ss;
        ss.print("verify_oop: %s: %s", reg_name(reg), s);
        b = code_string(ss.as_string());
    }
//    BLOCK_COMMENT("verify_oop {");
    write_inst("stp x0, x8, [sp, #-0x10]!");
    write_inst("stp x9, x30, [sp, #-0x10]!");

    write_inst_mov_reg(x0, reg);
    write_insts_mov_ptr(x8, (uintptr_t)(address)b);

    // call indirectly to solve generation ordering problem
//    lea(rscratch2, ExternalAddress(StubRoutines::verify_oop_subroutine_entry_address()));
    // here lea is equivalent to mov
    write_insts_mov_imm64(YuhuMacroAssembler::x9, (uint64_t) YuhuStubRoutines::verify_oop_subroutine_entry_address());

    write_inst("ldr x9, [x9]");
    write_inst("blr x9");

    write_inst("ldp x9, x30, [sp], #0x10");
    write_inst("ldp x0, x8, [sp], #0x10");

//    BLOCK_COMMENT("} verify_oop");
    return current_pc();
}

address YuhuMacroAssembler::write_insts_verify_oop(YuhuRegister reg, TosState state) {
    if (state == atos) {
        write_insts_verify_oop(reg, "broken oop");
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_load_klass(YuhuRegister dst, YuhuRegister src) {
    if (UseCompressedClassPointers) {
        write_insts_stop("unimplemented");
//        ldrw(dst, Address(src, oopDesc::klass_offset_in_bytes()));
//        decode_klass_not_null(dst);
    } else {
        write_inst("ldr %s, [%s, #%d]", dst, src, oopDesc::klass_offset_in_bytes());
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_lock_object(YuhuRegister lock_reg) {
    assert(lock_reg == x1, "The argument is only for looks. It must be c_rarg1");
    if (UseHeavyMonitors) {
        write_insts_final_call_VM(noreg,
                CAST_FROM_FN_PTR(address, InterpreterRuntime::monitorenter),
                lock_reg);
    } else {
        YuhuLabel done;

        const YuhuRegister swap_reg = x0;
        const YuhuRegister tmp = x2;
        const YuhuRegister obj_reg = x3; // Will contain the oop

        const int obj_offset = BasicObjectLock::obj_offset_in_bytes();
        const int lock_offset = BasicObjectLock::lock_offset_in_bytes ();
        const int mark_offset = lock_offset +
                                BasicLock::displaced_header_offset_in_bytes();

        YuhuLabel slow_case;

        // Load object pointer into obj_reg %c_rarg3
        write_inst("ldr x3, [%s, #%d]", lock_reg, obj_offset);

        if (UseBiasedLocking) {
            write_insts_biased_locking_enter(lock_reg, obj_reg, swap_reg, tmp, false, done, &slow_case);
        }

        // Load (object->mark() | 1) into swap_reg
        write_inst("ldr x8, [x3, #%d]", 0);
        write_inst("orr x0, x8, #%d", 1);

        // Save (object->mark() | 1) into BasicLock's displaced header
        write_inst("str x0, [%s, #%d]", lock_reg, mark_offset);

        assert(lock_offset == 0,
               "displached header must be first word in BasicObjectLock");

        YuhuLabel fail;
        if (PrintBiasedLockingStatistics) {
            YuhuLabel fast;
            write_insts_cmpxchgptr(swap_reg, lock_reg, obj_reg, x8, fast, &fail);
            pin_label(fast);
            write_insts_atomic_incw((address)BiasedLocking::fast_path_entry_count_addr(), x9, x8, tmp);
            write_inst_b(done);
            pin_label(fail);
        } else {
            write_insts_cmpxchgptr(swap_reg, lock_reg, obj_reg, x8, done, /*fallthrough*/NULL);
        }

        // Test if the oopMark is an obvious stack pointer, i.e.,
        //  1) (mark & 7) == 0, and
        //  2) rsp <= mark < mark + os::pagesize()
        //
        // These 3 tests can be done by evaluating the following
        // expression: ((mark - rsp) & (7 - os::vm_page_size())),
        // assuming both stack pointer and pagesize have their
        // least significant 3 bits clear.
        // NOTE: the oopMark is in swap_reg %r0 as the result of cmpxchg
        // NOTE2: aarch64 does not like to subtract sp from rn so take a
        // copy
        write_inst("mov x8, sp");
        write_inst("sub x0, x0, x8");
        write_inst("ands x0, x0, #%d", (uint64_t)(7 - os::vm_page_size()));

        // Save the test result, for recursive case, the result is zero
        write_inst("str x0, [%s, #%d]", lock_reg, mark_offset);

        if (PrintBiasedLockingStatistics) {
            write_inst_b(ne, slow_case);
            write_insts_atomic_incw((address)BiasedLocking::fast_path_entry_count_addr(), x9, x8, tmp);
        }
        write_inst_b(eq, done);

        pin_label(slow_case);

        // Call the runtime routine for slow case
        write_insts_final_call_VM(noreg,
                CAST_FROM_FN_PTR(address, InterpreterRuntime::monitorenter),
                lock_reg);

        pin_label(done);
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_unlock_object(YuhuRegister lock_reg)
{
    assert(lock_reg == x1, "The argument is only for looks. It must be rarg1");

    if (UseHeavyMonitors) {
        write_insts_final_call_VM(noreg,
                CAST_FROM_FN_PTR(address, InterpreterRuntime::monitorexit),
                lock_reg);
    } else {
        YuhuLabel done;

        const YuhuRegister swap_reg   = x0;
        const YuhuRegister header_reg = x2;  // Will contain the old oopMark
        const YuhuRegister obj_reg    = x3;  // Will contain the oop

        write_insts_save_bcp(); // Save in case of exception

        // Convert from BasicObjectLock structure to object and BasicLock
        // structure Store the BasicLock address into %r0
        write_inst("ldr %s, [%s, #%d]", swap_reg, lock_reg, BasicObjectLock::lock_offset_in_bytes()); // lea(swap_reg, Address(lock_reg, BasicObjectLock::lock_offset_in_bytes()));

        // Load oop into obj_reg(%c_rarg3)
        write_inst("ldr %s, [%s, #%d]", obj_reg, lock_reg, BasicObjectLock::obj_offset_in_bytes());

        // Free entry
        write_inst("str xzr, [%s, #%d]", lock_reg, BasicObjectLock::obj_offset_in_bytes());

        if (UseBiasedLocking) {
            write_insts_biased_locking_exit(obj_reg, header_reg, done);
        }

        // Load the old header from BasicLock structure
        write_inst("ldr %s, [%s, #%d]", header_reg, swap_reg, BasicLock::displaced_header_offset_in_bytes());

        // Test for recursion
        write_inst_cbz(header_reg, done);

        // Atomic swap back the old header
        write_insts_cmpxchgptr(swap_reg, header_reg, obj_reg, x8, done, /*fallthrough*/NULL);

        // Call the runtime routine for slow case.
        write_inst("str %s, [%s, #%d]", obj_reg, lock_reg, BasicObjectLock::obj_offset_in_bytes()); // restore obj
        write_insts_final_call_VM(noreg,
                CAST_FROM_FN_PTR(address, InterpreterRuntime::monitorexit),
                lock_reg);

        pin_label(done);

        write_insts_restore_bcp();
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_final_call_VM(YuhuRegister oop_result, address entry_point, bool check_exceptions) {
    return write_insts_final_call_VM_helper(oop_result, entry_point, 0, check_exceptions);
}

address YuhuMacroAssembler::write_insts_final_call_VM(YuhuRegister oop_result, address entry_point, YuhuRegister arg_1, bool check_exceptions) {
    if (arg_1 != x1) {
        write_inst_mov_reg(x1, arg_1);
    }
    write_insts_final_call_VM_helper(oop_result, entry_point, 1, check_exceptions);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_final_call_VM_helper(YuhuRegister oop_result, address entry_point, int number_of_arguments, bool check_exceptions) {
    write_insts_call_VM_base(oop_result, noreg, noreg, entry_point, number_of_arguments, check_exceptions);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_final_call_VM_base(YuhuRegister oop_result, YuhuRegister java_thread, YuhuRegister last_java_sp,
                                                           address  entry_point, int number_of_arguments, bool check_exceptions) {
    // determine java_thread register
    if (java_thread == noreg) {
        java_thread = x28;
    }

    // determine last_java_sp register
    if (last_java_sp == noreg) {
        last_java_sp = x20;
    }

    // debugging support
    assert(number_of_arguments >= 0   , "cannot have negative number of arguments");
    assert(java_thread == x28, "unexpected register");
#ifdef ASSERT
    // TraceBytecodes does not use r12 but saves it over the call, so don't verify
    // if ((UseCompressedOops || UseCompressedClassPointers) && !TraceBytecodes) verify_heapbase("call_VM_base: heap base corrupted?");
#endif // ASSERT

    assert(java_thread != oop_result  , "cannot use the same register for java_thread & oop_result");
    assert(java_thread != last_java_sp, "cannot use the same register for java_thread & last_java_sp");

    // push java thread (becomes first argument of C function)

    write_inst_mov_reg(x0, java_thread);

    // set last Java frame before call
    assert(last_java_sp != fp, "can't use rfp");

    YuhuLabel l;
    write_insts_set_last_java_frame(last_java_sp, fp, l, x8);

    // do the call, remove parameters
    write_insts_final_call_VM_leaf_base(entry_point, number_of_arguments, &l);

    // lr could be poisoned with PAC signature during throw_pending_exception
    // if it was tail-call optimized by compiler, since lr is not callee-saved
    // reload it with proper value
    write_inst_adr(lr, l);

    // reset last Java frame
    // Only interpreter should have to clear fp
    write_insts_reset_last_java_frame(true);

    // C++ interp handles this in the interpreter
    // TODO
//    check_and_handle_popframe(java_thread);
//    check_and_handle_earlyret(java_thread);

    if (check_exceptions) {
        // check for pending exceptions (java_thread is set upon return)
        write_inst("ldr x8, [%s, #%d]", java_thread, in_bytes(Thread::pending_exception_offset()));
        YuhuLabel ok;
        write_inst_cbz(x8, ok);
        write_insts_mov_imm64(x8, (uint64_t) YuhuStubRoutines::forward_exception_entry());
        write_inst_br(x8);
        pin_label(ok);
    }

    // get oop result if there is one and reset the value in the thread
    if (oop_result != noreg) {
        write_insts_get_vm_result(oop_result, java_thread);
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_final_call_VM_leaf_base(address entry_point, int number_of_arguments, YuhuLabel *retaddr) {
    write_inst("stp x8, x12, [sp, #%d]!", -2 * wordSize);

    // We add 1 to number_of_arguments because the thread in arg0 is
    // not counted
    write_insts_mov_imm64(x8, (uint64_t) entry_point);
    write_inst("blr x8");
    if (retaddr)
        pin_label(*retaddr);

    write_inst("ldp x8, x12, [sp], #%d", 2 * wordSize);
    write_inst("isb");
    return current_pc();
}

address YuhuMacroAssembler::write_insts_final_call_VM_leaf(address entry_point, YuhuRegister arg_0, YuhuRegister arg_1) {
    if (arg_0 != x0) {
        write_inst_mov_reg(x0, arg_0);
    }
    if (arg_1 != x1) {
        write_inst_mov_reg(x1, arg_1);
    }
    write_insts_call_VM_leaf_base(entry_point, 2);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_get_vm_result(YuhuRegister oop_result, YuhuRegister java_thread) {
    write_inst("ldr %s, [%s, #%d]", oop_result, java_thread, in_bytes(JavaThread::vm_result_offset()));
    write_inst("str %s, [%s, #%d]", xzr, java_thread, in_bytes(JavaThread::vm_result_offset()));
    write_insts_verify_oop(oop_result, "broken oop in call_VM_base");
    return current_pc();
}

int YuhuMacroAssembler::write_insts_biased_locking_enter(YuhuRegister lock_reg,
                                         YuhuRegister obj_reg,
                                         YuhuRegister swap_reg,
                                         YuhuRegister tmp_reg,
                                         bool swap_reg_contains_mark,
                                         YuhuLabel& done,
                                         YuhuLabel* slow_case,
                                         BiasedLockingCounters* counters) {
    assert(UseBiasedLocking, "why call this otherwise?");
//    assert_different_registers(lock_reg, obj_reg, swap_reg);

    // TODO
    if (PrintBiasedLockingStatistics && counters == NULL)
        counters = BiasedLocking::counters();

//    assert_different_registers(lock_reg, obj_reg, swap_reg, tmp_reg, rscratch1, rscratch2, noreg);
    assert(markOopDesc::age_shift == markOopDesc::lock_bits + markOopDesc::biased_lock_bits, "biased locking makes assumptions about bit layout");
//    Address mark_addr      (obj_reg, oopDesc::mark_offset_in_bytes());
//    Address klass_addr     (obj_reg, oopDesc::klass_offset_in_bytes());
//    Address saved_mark_addr(lock_reg, 0);

    // Biased locking
    // See whether the lock is currently biased toward our thread and
    // whether the epoch is still valid
    // Note that the runtime guarantees sufficient alignment of JavaThread
    // pointers to allow age to be placed into low bits
    // First check to see whether biasing is even enabled for this object
    YuhuLabel cas_label;
    int null_check_offset = -1;
    if (!swap_reg_contains_mark) {
        null_check_offset = offset();
        write_inst("ldr %s, [%s, #%d]", swap_reg, obj_reg, oopDesc::mark_offset_in_bytes());
    }
    write_inst("and %s, %s, #%d", tmp_reg, swap_reg, markOopDesc::biased_lock_mask_in_place);
    write_inst("cmp %s, #%d", tmp_reg, markOopDesc::biased_lock_pattern);
    write_inst_b(ne, cas_label);
    // The bias pattern is present in the object's header. Need to check
    // whether the bias owner and the epoch are both still current.
    write_insts_load_prototype_header(tmp_reg, obj_reg);
    write_inst_regs("orr %s, %s, x28", tmp_reg, tmp_reg);
    write_inst_regs("eor %s, %s, %s", tmp_reg, swap_reg, tmp_reg);
    write_inst("and %s, %s, #%d", tmp_reg, tmp_reg, ~((int) markOopDesc::age_mask_in_place));

    if (counters != NULL) {
        YuhuLabel around;
        write_inst_cbnz(tmp_reg, around);
        write_insts_atomic_incw((address)counters->biased_lock_entry_count_addr(), tmp_reg, x8, x9);
        write_inst_b(done);
        pin_label(around);
    } else {
        write_inst_cbz(tmp_reg, done);
    }

    YuhuLabel try_revoke_bias;
    YuhuLabel try_rebias;

    // At this point we know that the header has the bias pattern and
    // that we are not the bias owner in the current epoch. We need to
    // figure out more details about the state of the header in order to
    // know what operations can be legally performed on the object's
    // header.

    // If the low three bits in the xor result aren't clear, that means
    // the prototype header is no longer biased and we have to revoke
    // the bias on this object.
    write_inst("and x8, %s, #%d", tmp_reg, markOopDesc::biased_lock_mask_in_place);
    write_inst_cbnz(x8, try_revoke_bias);

    // Biasing is still enabled for this data type. See whether the
    // epoch of the current bias is still valid, meaning that the epoch
    // bits of the mark word are equal to the epoch bits of the
    // prototype header. (Note that the prototype header's epoch bits
    // only change at a safepoint.) If not, attempt to rebias the object
    // toward the current thread. Note that we must be absolutely sure
    // that the current epoch is invalid in order to do this because
    // otherwise the manipulations it performs on the mark word are
    // illegal.
    write_inst("and x8, %s, #%d", tmp_reg, markOopDesc::epoch_mask_in_place);
    write_inst_cbnz(x8, try_rebias);

    // The epoch of the current bias is still valid but we know nothing
    // about the owner; it might be set or it might be clear. Try to
    // acquire the bias of the object using an atomic operation. If this
    // fails we will go in to the runtime to revoke the object's bias.
    // Note that we first construct the presumed unbiased header so we
    // don't accidentally blow away another thread's valid bias.
    {
        YuhuLabel here;
        write_insts_mov_imm64(x8, (markOopDesc::biased_lock_mask_in_place | markOopDesc::age_mask_in_place | markOopDesc::epoch_mask_in_place));
        write_inst_regs("and %s, %s, x8", swap_reg, swap_reg);
        write_inst_regs("orr %s, %s, x28", tmp_reg, swap_reg);
        write_insts_cmpxchgptr(swap_reg, tmp_reg, obj_reg, x8, here, slow_case);
        // If the biasing toward our thread failed, this means that
        // another thread succeeded in biasing it toward itself and we
        // need to revoke that bias. The revocation will occur in the
        // interpreter runtime in the slow case.
        pin_label(here);
        if (counters != NULL) {
            write_insts_atomic_incw((address)counters->anonymously_biased_lock_entry_count_addr(), tmp_reg, x8, x9);
        }
    }
    write_inst_b(done);

    pin_label(try_rebias);
    // At this point we know the epoch has expired, meaning that the
    // current "bias owner", if any, is actually invalid. Under these
    // circumstances _only_, we are allowed to use the current header's
    // value as the comparison value when doing the cas to acquire the
    // bias in the current epoch. In other words, we allow transfer of
    // the bias from one thread to another directly in this situation.
    //
    // FIXME: due to a lack of registers we currently blow away the age
    // bits in this situation. Should attempt to preserve them.
    {
        YuhuLabel here;
        write_insts_load_prototype_header(tmp_reg, obj_reg);
        write_inst_regs("orr %s, x28, %s", tmp_reg, tmp_reg);
        write_inst_regs("orr %s, x28, %s", tmp_reg, tmp_reg);
        write_insts_cmpxchgptr(swap_reg, tmp_reg, obj_reg, x8, here, slow_case);
        // If the biasing toward our thread failed, then another thread
        // succeeded in biasing it toward itself and we need to revoke that
        // bias. The revocation will occur in the runtime in the slow case.
        pin_label(here);
        if (counters != NULL) {
            write_insts_atomic_incw((address)counters->rebiased_lock_entry_count_addr(), tmp_reg, x8, x9);
        }
    }
    write_inst_b(done);

    pin_label(try_revoke_bias);
    // The prototype mark in the klass doesn't have the bias bit set any
    // more, indicating that objects of this data type are not supposed
    // to be biased any more. We are going to try to reset the mark of
    // this object to the prototype value and fall through to the
    // CAS-based locking scheme. Note that if our CAS fails, it means
    // that another thread raced us for the privilege of revoking the
    // bias of this particular object, so it's okay to continue in the
    // normal locking code.
    //
    // FIXME: due to a lack of registers we currently blow away the age
    // bits in this situation. Should attempt to preserve them.
    {
        YuhuLabel here, nope;
        write_insts_load_prototype_header(tmp_reg, obj_reg);
        write_insts_cmpxchgptr(swap_reg, tmp_reg, obj_reg, x8, here, &nope);
        pin_label(here);

        // Fall through to the normal CAS-based lock, because no matter what
        // the result of the above CAS, some thread must have succeeded in
        // removing the bias bit from the object's header.
        if (counters != NULL) {
            write_insts_atomic_incw((address)counters->revoked_lock_entry_count_addr(), tmp_reg, x8, x9);
        }
        pin_label(done);
    }

    pin_label(cas_label);

    return null_check_offset;
}

void YuhuMacroAssembler::write_insts_biased_locking_exit(YuhuRegister obj_reg, YuhuRegister temp_reg, YuhuLabel& done) {
    assert(UseBiasedLocking, "why call this otherwise?");

    // Check for biased locking unlock case, which is a no-op
    // Note: we do not have to check the thread ID for two reasons.
    // First, the interpreter checks for IllegalMonitorStateException at
    // a higher level. Second, if the bias was revoked while we held the
    // lock, the object could not be rebiased toward another thread, so
    // the bias bit would be clear.
    write_inst("ldr %s, [%s, #%d]", temp_reg, obj_reg, oopDesc::mark_offset_in_bytes());
    write_inst("and %s, %s, #%d", temp_reg, temp_reg, markOopDesc::biased_lock_mask_in_place);
    write_inst("cmp %s, #%d", temp_reg, markOopDesc::biased_lock_pattern);
    write_inst_b(eq, done);
}

address YuhuMacroAssembler::write_insts_load_prototype_header(YuhuRegister dst, YuhuRegister src) {
    write_insts_load_klass(dst, src);
    write_inst("ldr %s, [%s, #%d]", dst, dst, in_bytes(Klass::prototype_header_offset()));
    return current_pc();
}

address YuhuMacroAssembler::write_insts_cmpxchgptr(YuhuRegister oldv, YuhuRegister newv, YuhuRegister addr, YuhuRegister tmp,
                                YuhuLabel &succeed, YuhuLabel *fail) {
    // oldv holds comparison value
    // newv holds value to write in exchange
    // addr identifies memory word to compare against/update
    if (UseLSE) {
        write_inst_mov_reg(tmp, oldv);
        write_inst_regs("casal %s, %s, [%s]", oldv, newv, addr);
        write_inst_regs("cmp %s, %s", tmp, oldv);
        write_inst_b(eq, succeed);
        write_inst("dmb ish");
    } else {
        YuhuLabel retry_load, nope;
        if ((VM_Version::cpu_cpuFeatures() & VM_Version::CPU_STXR_PREFETCH))
            write_inst("prfm PSTL1STRM, [%s]", addr);
        pin_label(retry_load);
        // flush and load exclusive from the memory location
        // and fail if it is not what we expect
        write_inst_regs("ldaxr %s, [%s]", tmp, addr);
        write_inst_regs("cmp %s, %s", tmp, oldv);
        write_inst_b(ne, nope);
        // if we store+flush with no intervening write tmp wil be zero
        write_inst_regs("stlxr %s, %s, [%s]", w_reg(tmp), newv, addr);
        write_inst_cbz(w_reg(tmp), succeed);
        // retry so we only ever return after a load fails to compare
        // ensures we don't return a stale value after a failed write.
        write_inst_b(retry_load);
        // if the memory word differs we return it in oldv and signal a fail
        pin_label(nope);
        write_inst("dmb ish");
        write_inst_mov_reg(oldv, tmp);
    }
    if (fail)
        write_inst_b(*fail);

    return current_pc();
}

address YuhuMacroAssembler::write_insts_atomic_incw(YuhuRegister counter_addr, YuhuRegister tmp, YuhuRegister tmp2) {
    if (UseLSE) {
        write_insts_mov_imm32(tmp, 1);
        write_inst_regs("ldadd %s, %s, [%s]", w_reg(tmp), w_reg(xzr), counter_addr);
        return current_pc();
    }
    YuhuLabel retry_load;
    if ((VM_Version::cpu_cpuFeatures() & VM_Version::CPU_STXR_PREFETCH))
        write_inst("prfm PSTL1STRM, [%s]", counter_addr);
    pin_label(retry_load);
    // flush and load exclusive from the memory location
    write_inst_regs("ldxr %s, [%s]", w_reg(tmp), counter_addr);
    write_inst_regs("add %s, %s, #1", w_reg(tmp), w_reg(tmp));
    // if we store+flush with no intervening write tmp wil be zero
    write_inst_regs("stxr %s, %s, [%s]", w_reg(tmp2), w_reg(tmp), counter_addr);
    write_inst_cbnz(w_reg(tmp2), retry_load);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_pop(TosState state) {
    switch (state) {
        case atos: write_inst_pop_ptr();                 break;
        case btos:
//  case ztos:
        case ctos:
        case stos:
        case itos: write_inst_pop_i();                   break;
        case ltos: write_inst_pop_l();                   break;
        case ftos: write_inst_pop_f();                   break;
        case dtos: write_inst_pop_d();                   break;
        case vtos: /* nothing to do */        break;
        default:   ShouldNotReachHere();
    }
    write_insts_verify_oop(x0, state);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_push(TosState state) {
    write_insts_verify_oop(x0, state);
    switch (state) {
        case atos: write_inst_push_ptr();                break;
        case btos:
//  case ztos:
        case ctos:
        case stos:
        case itos: write_inst_push_i();                  break;
        case ltos: write_inst_push_l();                  break;
        case ftos: write_inst_push_f();                  break;
        case dtos: write_inst_push_d();                  break;
        case vtos: /* nothing to do */        break;
        default  : ShouldNotReachHere();
    }
    return current_pc();
}

int YuhuMacroAssembler::write_insts_push(unsigned int bitset, YuhuRegister stack) {
    int words_pushed = 0;

    // Scan bitset to accumulate register pairs
    unsigned char regs[32];
    int count = 0;
    for (int reg = 0; reg <= 30; reg++) {
        if (1 & bitset)
            regs[count++] = reg;
        bitset >>= 1;
    }
    regs[count++] = xzr - x0;
    count &= ~1;  // Only push an even nuber of regs

    if (count) {
        write_inst("stp %s, %s, [%s, #%d]!", (YuhuRegister) (regs[0] + x0),
                   (YuhuRegister) (regs[1] + x0), stack, -count * wordSize);
        words_pushed += 2;
    }
    for (int i = 2; i < count; i += 2) {
        write_inst("stp %s, %s, [%s, #%d]", (YuhuRegister) (regs[i] + x0),
                   (YuhuRegister) (regs[i+1] + x0), stack, i * wordSize);
        words_pushed += 2;
    }

    assert(words_pushed == count, "oops, pushed != count");

    return count;
}

int YuhuMacroAssembler::write_insts_pop(unsigned int bitset, YuhuRegister stack) {
    int words_pushed = 0;

    // Scan bitset to accumulate register pairs
    unsigned char regs[32];
    int count = 0;
    for (int reg = 0; reg <= 30; reg++) {
        if (1 & bitset)
            regs[count++] = reg;
        bitset >>= 1;
    }
    regs[count++] = xzr - x0;
    count &= ~1;

    for (int i = 2; i < count; i += 2) {
        write_inst("ldp %s, %s, [%s, #%d]", (YuhuRegister) (regs[i] + x0),
                   (YuhuRegister) (regs[i+1] + x0), stack, i * wordSize);
        words_pushed += 2;
    }
    if (count) {
        write_inst("ldp %s, %s, [%s], #%d", (YuhuRegister) (regs[0] + x0),
                   (YuhuRegister) (regs[1] + x0), stack, count * wordSize);
        words_pushed += 2;
    }

    assert(words_pushed == count, "oops, pushed != count");

    return count;
}

address YuhuMacroAssembler::write_insts_g1_write_barrier_pre(YuhuRegister obj,
                                                             YuhuRegister pre_val,
                                                             YuhuRegister thread,
                                                             YuhuRegister tmp,
                                                             bool tosca_live,
                                                             bool expand_call) {
    // If expand_call is true then we expand the call_VM_leaf macro
    // directly to skip generating the check by
    // InterpreterMacroAssembler::call_VM_leaf_base that checks _last_sp.

#ifdef _LP64
    assert(thread == x28, "must be");
#endif // _LP64

    YuhuLabel done;
    YuhuLabel runtime;

//    assert_different_registers(obj, pre_val, tmp, rscratch1);
    assert(pre_val != noreg &&  tmp != noreg, "expecting a register");

//    Address in_progress(thread, in_bytes(JavaThread::satb_mark_queue_offset() +
//                                         PtrQueue::byte_offset_of_active()));
//    Address index(thread, in_bytes(JavaThread::satb_mark_queue_offset() +
//                                   PtrQueue::byte_offset_of_index()));
//    Address buffer(thread, in_bytes(JavaThread::satb_mark_queue_offset() +
//                                    PtrQueue::byte_offset_of_buf()));


    // Is marking active?
    if (in_bytes(PtrQueue::byte_width_of_active()) == 4) {
        write_inst("ldr %s, [x28, #%d]", w_reg(tmp), in_bytes(JavaThread::satb_mark_queue_offset() +
                                                                                                PtrQueue::byte_offset_of_active()));
    } else {
        assert(in_bytes(PtrQueue::byte_width_of_active()) == 1, "Assumption");
        write_inst("ldrb %s, [x28, #%d]", w_reg(tmp), in_bytes(JavaThread::satb_mark_queue_offset() +
                                                                                                    PtrQueue::byte_offset_of_active()));
    }
    write_inst_cbz(w_reg(tmp), done);

    // Do we need to load the previous value?
    if (obj != noreg) {
        write_insts_load_heap_oop(pre_val, obj, 0);
    }

    // Is the previous value null?
    write_inst_cbz(pre_val, done);

    // Can we store original value in the thread's buffer?
    // Is index == 0?
    // (The index field is typed as size_t.)

    write_inst("ldr %s, [%s, #%d]", tmp, thread, in_bytes(JavaThread::satb_mark_queue_offset() +
                                                          PtrQueue::byte_offset_of_index())); // tmp := *index_adr
    write_inst_cbz(tmp, runtime); // tmp == 0?
    // If yes, goto runtime

    write_inst("sub %s, %s, #%d", tmp, tmp, wordSize); // tmp := tmp - wordSize
    write_inst("str %s, [%s, #%d]", tmp, thread, in_bytes(JavaThread::satb_mark_queue_offset() +
                                                          PtrQueue::byte_offset_of_index())); // *index_adr := tmp
    write_inst("ldr x8, [%s, #%d]", thread, in_bytes(JavaThread::satb_mark_queue_offset() +
                                                     PtrQueue::byte_offset_of_buf()));
    write_inst_regs("add %s, %s, x8", tmp, tmp); // tmp := tmp + *buffer_adr

    // Record the previous value
    write_inst_regs("str, %s, [%s, #0]", pre_val, tmp);
    write_inst_b(done);

    pin_label(runtime);
    // save the live input values
    write_insts_push(bit(x0, tosca_live) | bit(obj, obj != noreg) | bit(pre_val, true), sp);

    // Calling the runtime using the regular call_VM_leaf mechanism generates
    // code (generated by InterpreterMacroAssember::call_VM_leaf_base)
    // that checks that the *(rfp+frame::interpreter_frame_last_sp) == NULL.
    //
    // If we care generating the pre-barrier without a frame (e.g. in the
    // intrinsified Reference.get() routine) then ebp might be pointing to
    // the caller frame and so this check will most likely fail at runtime.
    //
    // Expanding the call directly bypasses the generation of the check.
    // So when we do not have have a full interpreter frame on the stack
    // expand_call should be passed true.

    if (expand_call) {
        LP64_ONLY( assert(pre_val != x1, "smashed arg"); )
        if (thread != x1) {
            write_inst_mov_reg(x1, thread);
        }
        if (pre_val != x0) {
            write_inst_mov_reg(x0, pre_val);
        }
        write_insts_final_call_VM_leaf_base(CAST_FROM_FN_PTR(address, SharedRuntime::g1_wb_pre), 2);
    } else {
        write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::g1_wb_pre), pre_val, thread);
    }

    write_insts_pop(bit(x0, tosca_live) | bit(obj, obj != noreg) | bit(pre_val, true), sp);
    pin_label(done);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_load_heap_oop(YuhuRegister dst, YuhuRegister obj, int offset) {
    // TODO
//    if (UseCompressedOops) {
//        ldrw(dst, src);
//        decode_heap_oop(dst);
//    } else {
        write_inst("ldr %s, [%s, #%d]", dst, obj, offset);
//    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_update_byte_crc32(YuhuRegister crc, YuhuRegister val, YuhuRegister table) {
    write_inst_regs("eor %s, %s, %s", crc, val, table);
    write_inst_regs("and %s, %s, #0xff", val, val);
    write_inst_regs("ldr %s, [%s, %s, lsl #2]", w_reg(val), table, val);
    write_inst_regs("eor %s, %s, %s, lsr #8", crc, val, crc);
    return current_pc();
}
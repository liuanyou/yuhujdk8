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

address YuhuMacroAssembler::write_inst(const char* assembly_format, YuhuRegister reg1, YuhuRegister reg2, unsigned int imm32) {
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg1), reg_name(reg2), imm32);
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

address YuhuMacroAssembler::write_inst_adrp(YuhuRegister reg, address target) {
    // 确保目标地址页面对齐
    uint64_t target_page = ((uint64_t)target >> 12) << 12;
    return write_inst("adrp %s, 0x%lx", reg, target_page);
}

address YuhuMacroAssembler::write_inst_adr(YuhuRegister reg, address target) {
    long offset = target - current_pc();
    return write_inst("adr %s, #%d", reg, offset);
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

address YuhuMacroAssembler::write_insts_call_VM_leaf(address entry_point, YuhuRegister arg_0, YuhuRegister arg_1) {
    if (arg_0 != x0) {
        write_inst_mov_reg(x0, arg_0);
    }
    if (arg_1 != x1) {
        write_inst_mov_reg(x1, arg_1);
    }
    write_insts_call_VM_leaf_base(entry_point, 2);
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

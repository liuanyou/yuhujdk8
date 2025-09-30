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
#include "gc_implementation/g1/g1SATBCardTableModRefBS.hpp"
#include "gc_implementation/g1/heapRegion.hpp"

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

/**
 * It requires assembly_format has 2 params:
 * 1) first param is %s
 * 2) second param is %d, if it uses another format such as %ld or %lx,
 * snprintf reads additional 4 bytes for imm32, it gives unpredictable result
 *
 * @param assembly_format
 * @param reg
 * @param imm32
 * @return
 */
address YuhuMacroAssembler::write_inst(const char* assembly_format, YuhuRegister reg, int imm32) {
    // Validate assembly format string (expects 2 parameters: %s and %d/x/u/o)
    validate_assembly_format_2_reg_imm(assembly_format);
    
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg), imm32);
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

/**
 * It requires assembly_format has 2 params:
 * 1) first param is %s
 * 2) second param is %ld or %lx, if it uses another format such as %d,
 * snprintf reads only lower 4 bytes for imm64, it gives unpredictable result
 *
 * @param assembly_format
 * @param reg
 * @param imm64
 * @return
 */
address YuhuMacroAssembler::write_inst_reg_imm64(const char* assembly_format, YuhuRegister reg, long imm64) {
    // Validate assembly format string (expects 2 parameters: %s and %ld/%lx/%lu/%lo)
    validate_assembly_format_long(assembly_format);
    
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg), imm64);
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

/**
 * It requires assembly_format has 2 params:
 * 1) first param is %s
 * 2) second param is %d, if it uses another format such as %ld or %lx,
 * snprintf reads additional 4 bytes for imm32, it gives unpredictable result
 *
 * @param assembly_format
 * @param reg
 * @param imm32
 * @return
 */
address YuhuMacroAssembler::write_inst(const char* assembly_format, YuhuFloatRegister reg, int imm32) {
    // Validate assembly format string (expects 2 parameters: %s and %d/x/u/o)
    validate_assembly_format_2_reg_imm(assembly_format);
    
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, f_reg_name(reg), imm32);
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

/**
 * It requires assembly_format has 3 params:
 * 1) first param is %s
 * 2) second param is %s
 * 3) third param is %d, if it uses another format such as %ld or %lx,
 * snprintf reads additional 4 bytes for imm32, it gives unpredictable result
 *
 * @param assembly_format
 * @param reg1
 * @param reg2
 * @param imm32
 * @return
 */
address YuhuMacroAssembler::write_inst(const char* assembly_format, YuhuRegister reg1, YuhuRegister reg2, int imm32) {
    // Validate assembly format string (expects 3 parameters: %s, %s, and %d/x/u/o)
    validate_assembly_format_3_reg_reg_imm(assembly_format);
    
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg1), reg_name(reg2), imm32);
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

/**
 * It requires assembly_format has 4 params:
 * 1) first param is %s
 * 2) second param is %s
 * 3) third param is %s
 * 4) fourth param is %d, if it uses another format such as %ld or %lx,
 * snprintf reads additional 4 bytes for imm32, it gives unpredictable result
 *
 * @param assembly_format
 * @param reg1
 * @param reg2
 * @param reg3
 * @param imm32
 * @return
 */
address YuhuMacroAssembler::write_inst(const char* assembly_format, YuhuRegister reg1, YuhuRegister reg2, YuhuRegister reg3, int imm32) {
    // Validate assembly format string (expects 4 parameters: %s, %s, %s, and %d/x/u/o)
    validate_assembly_format_4_reg_reg_reg_imm(assembly_format);
    
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg1), reg_name(reg2), reg_name(reg3), imm32);
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

/**
 * It requires assembly_format has 1 params:
 * 1) first param is %d, if it uses another format such as %ld or %lx,
 * snprintf reads additional 4 bytes for imm32, it gives unpredictable result
 *
 * @param assembly_format
 * @param imm32
 * @return
 */
address YuhuMacroAssembler::write_inst(const char* assembly_format, int imm32) {
    // Validate assembly format string (expects 1 parameter: %d/x/u/o)
    validate_assembly_format_1_imm(assembly_format);
    
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, imm32);
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst(const char* assembly_format, YuhuRegister reg, YuhuAddress addr) {
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    switch (addr.getMode()) {
        case YuhuAddress::base_plus_offset:
            snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg), reg_name(addr.base()), addr.offset());
            break;
        case YuhuAddress::base_plus_offset_reg:
            snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg), reg_name(addr.base()),
                     reg_name(addr.index()), op_name(addr.ext().op()), MAX(addr.ext().shift(), 0));
            break;
        case YuhuAddress::pre:
            snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg), reg_name(addr.base()), addr.offset());
            break;
        case YuhuAddress::post:
            snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg), reg_name(addr.base()), addr.offset());
            break;
        default:
            ShouldNotReachHere();
    }
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst(const char* assembly_format, YuhuFloatRegister reg, YuhuAddress addr) {
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    switch (addr.getMode()) {
        case YuhuAddress::base_plus_offset:
            snprintf(buffer, sizeof(buffer), assembly_format, f_reg_name(reg), reg_name(addr.base()), addr.offset());
            break;
        case YuhuAddress::base_plus_offset_reg:
            snprintf(buffer, sizeof(buffer), assembly_format, f_reg_name(reg), reg_name(addr.base()),
                     reg_name(addr.index()), op_name(addr.ext().op()), MAX(addr.ext().shift(), 0));
            break;
        case YuhuAddress::pre:
            snprintf(buffer, sizeof(buffer), assembly_format, f_reg_name(reg), reg_name(addr.base()), addr.offset());
            break;
        case YuhuAddress::post:
            snprintf(buffer, sizeof(buffer), assembly_format, f_reg_name(reg), reg_name(addr.base()), addr.offset());
            break;
        default:
            ShouldNotReachHere();
    }
#pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_regs(const char* assembly_format, YuhuRegister reg1) {
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg1));
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

address YuhuMacroAssembler::write_inst_regs(const char* assembly_format, YuhuRegister reg1, YuhuRegister reg2, YuhuRegister reg3, YuhuRegister reg4) {
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg1), reg_name(reg2), reg_name(reg3), reg_name(reg4));
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_imms(const char* assembly_format, YuhuRegister reg1, YuhuRegister reg2, int imm1, int imm2) {
    char buffer[50];
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(buffer, sizeof(buffer), assembly_format, reg_name(reg1), reg_name(reg2), imm1, imm2);
    #pragma clang diagnostic pop
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_add(YuhuRegister reg, YuhuRegister base, YuhuRegister index, YuhuOperation op, int shift) {
    char buffer[50];
    snprintf(buffer, sizeof(buffer), "add %s, %s, %s, %s #%d",
             reg_name(reg), reg_name(base), reg_name(index), op_name(op), shift);
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_str(YuhuRegister reg, YuhuAddress addr) {
    switch (addr.getMode()) {
        case YuhuAddress::base_plus_offset:
            return write_inst("str %s, [%s, #%d]", reg, addr);
        case YuhuAddress::base_plus_offset_reg:
            return write_inst("str %s, [%s, %s, %s #%d]", reg, addr);
        case YuhuAddress::pre:
            return write_inst("str %s, [%s, #%d]!", reg, addr);
        case YuhuAddress::post:
            return write_inst("str %s, [%s], #%d", reg, addr);
        default:
            ShouldNotReachHere();
    }
    return current_pc();
}

address YuhuMacroAssembler::write_inst_strh(YuhuRegister reg, YuhuAddress addr) {
    switch (addr.getMode()) {
        case YuhuAddress::base_plus_offset:
            return write_inst("strh %s, [%s, #%d]", reg, addr);
        case YuhuAddress::base_plus_offset_reg:
            return write_inst("strh %s, [%s, %s, %s #%d]", reg, addr);
        case YuhuAddress::pre:
            return write_inst("strh %s, [%s, #%d]!", reg, addr);
        case YuhuAddress::post:
            return write_inst("strh %s, [%s], #%d", reg, addr);
        default:
            ShouldNotReachHere();
    }
    return current_pc();
}

address YuhuMacroAssembler::write_inst_str(YuhuFloatRegister reg, YuhuAddress addr) {
    switch (addr.getMode()) {
        case YuhuAddress::base_plus_offset:
            return write_inst("str %s, [%s, #%d]", reg, addr);
        case YuhuAddress::base_plus_offset_reg:
            return write_inst("str %s, [%s, %s, %s #%d]", reg, addr);
        case YuhuAddress::pre:
            return write_inst("str %s, [%s, #%d]!", reg, addr);
        case YuhuAddress::post:
            return write_inst("str %s, [%s], #%d", reg, addr);
        default:
            ShouldNotReachHere();
    }
    return current_pc();
}

address YuhuMacroAssembler::write_inst_strb(YuhuRegister reg, YuhuAddress addr) {
    switch (addr.getMode()) {
        case YuhuAddress::base_plus_offset:
            return write_inst("strb %s, [%s, #%d]", reg, addr);
        case YuhuAddress::base_plus_offset_reg:
            return write_inst("strb %s, [%s, %s, %s #%d]", reg, addr);
        case YuhuAddress::pre:
            return write_inst("strb %s, [%s, #%d]!", reg, addr);
        case YuhuAddress::post:
            return write_inst("strb %s, [%s], #%d", reg, addr);
        default:
            ShouldNotReachHere();
    }
    return current_pc();
}

address YuhuMacroAssembler::write_inst_ldr(YuhuRegister reg, YuhuAddress addr) {
    switch (addr.getMode()) {
        case YuhuAddress::base_plus_offset:
            return write_inst("ldr %s, [%s, #%d]", reg, addr);
        case YuhuAddress::base_plus_offset_reg:
            return write_inst("ldr %s, [%s, %s, %s #%d]", reg, addr);
        case YuhuAddress::pre:
            return write_inst("ldr %s, [%s, #%d]!", reg, addr);
        case YuhuAddress::post:
            return write_inst("ldr %s, [%s], #%d", reg, addr);
        default:
            ShouldNotReachHere();
    }
    return current_pc();
}

address YuhuMacroAssembler::write_inst_ldr(YuhuFloatRegister reg, YuhuAddress addr) {
    switch (addr.getMode()) {
        case YuhuAddress::base_plus_offset:
            return write_inst("ldr %s, [%s, #%d]", reg, addr);
        case YuhuAddress::base_plus_offset_reg:
            return write_inst("ldr %s, [%s, %s, %s #%d]", reg, addr);
        case YuhuAddress::pre:
            return write_inst("ldr %s, [%s, #%d]!", reg, addr);
        case YuhuAddress::post:
            return write_inst("ldr %s, [%s], #%d", reg, addr);
        default:
            ShouldNotReachHere();
    }
    return current_pc();
}

address YuhuMacroAssembler::write_inst_ldrh(YuhuRegister reg, YuhuAddress addr) {
    switch (addr.getMode()) {
        case YuhuAddress::base_plus_offset:
            return write_inst("ldrh %s, [%s, #%d]", reg, addr);
        case YuhuAddress::base_plus_offset_reg:
            return write_inst("ldrh %s, [%s, %s, %s #%d]", reg, addr);
        case YuhuAddress::pre:
            return write_inst("ldrh %s, [%s, #%d]!", reg, addr);
        case YuhuAddress::post:
            return write_inst("ldrh %s, [%s], #%d", reg, addr);
        default:
            ShouldNotReachHere();
    }
    return current_pc();
}

address YuhuMacroAssembler::write_inst_ldrb(YuhuRegister reg, YuhuAddress addr) {
    switch (addr.getMode()) {
        case YuhuAddress::base_plus_offset:
            return write_inst("ldrb %s, [%s, #%d]", reg, addr);
        case YuhuAddress::base_plus_offset_reg:
            return write_inst("ldrb %s, [%s, %s, %s #%d]", reg, addr);
        case YuhuAddress::pre:
            return write_inst("ldrb %s, [%s, #%d]!", reg, addr);
        case YuhuAddress::post:
            return write_inst("ldrb %s, [%s], #%d", reg, addr);
        default:
            ShouldNotReachHere();
    }
    return current_pc();
}

address YuhuMacroAssembler::write_inst_ldrsb(YuhuRegister reg, YuhuAddress addr) {
    switch (addr.getMode()) {
        case YuhuAddress::base_plus_offset:
            return write_inst("ldrsb %s, [%s, #%d]", reg, addr);
        case YuhuAddress::base_plus_offset_reg:
            return write_inst("ldrsb %s, [%s, %s, %s #%d]", reg, addr);
        case YuhuAddress::pre:
            return write_inst("ldrsb %s, [%s, #%d]!", reg, addr);
        case YuhuAddress::post:
            return write_inst("ldrsb %s, [%s], #%d", reg, addr);
        default:
            ShouldNotReachHere();
    }
    return current_pc();
}

address YuhuMacroAssembler::write_inst_ldrsh(YuhuRegister reg, YuhuAddress addr) {
    switch (addr.getMode()) {
        case YuhuAddress::base_plus_offset:
            return write_inst("ldrsh %s, [%s, #%d]", reg, addr);
        case YuhuAddress::base_plus_offset_reg:
            return write_inst("ldrsh %s, [%s, %s, %s #%d]", reg, addr);
        case YuhuAddress::pre:
            return write_inst("ldrsh %s, [%s, #%d]!", reg, addr);
        case YuhuAddress::post:
            return write_inst("ldrsh %s, [%s], #%d", reg, addr);
        default:
            ShouldNotReachHere();
    }
    return current_pc();
}

address YuhuMacroAssembler::write_inst_mov_reg(YuhuRegister reg1, YuhuRegister reg2) {
    char buffer[50];
    snprintf(buffer, sizeof(buffer), "mov %s, %s", reg_name(reg1), reg_name(reg2));
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_fmov_reg(YuhuFloatRegister reg1, YuhuRegister reg2) {
    char buffer[50];
    snprintf(buffer, sizeof(buffer), "fmov %s, %s", f_reg_name(reg1), reg_name(reg2));
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
    // 计算页偏移：直接计算目标页和当前页的差值
    uint64_t current_pc_addr = (uint64_t)current_pc();
    uint64_t target_addr = (uint64_t)target;
    
    // 计算页偏移（以页为单位）
    int64_t page_offset = ((int64_t)(target_addr >> 12)) - ((int64_t)(current_pc_addr >> 12));
    
    // 检查是否在有效范围内
    assert(page_offset >= -(1<<20) && page_offset < (1<<20), "ADRP immediate out of range");
    
    // 使用直接编码方法而不是Keystone Engine
    return write_inst_adrp_direct(reg, (int32_t)page_offset);
}

address YuhuMacroAssembler::write_inst_adrp_direct(YuhuRegister reg, int32_t page_offset) {
    // 直接编码ADRP指令，不依赖Keystone Engine
    // ADRP指令格式：
    // 31-24: 10010000 (0x90)
    // 23-5:  高19位立即数
    // 4-0:   目标寄存器
    // 30-29: 低2位立即数
    
    uint32_t instruction = 0x90000000;  // 基础指令码
    
    // 设置目标寄存器 (位 4-0)
    instruction |= (reg & 0x1F);
    
    // 设置立即数
    // 高19位放在位 23-5
    uint32_t immhi = (page_offset >> 2) & 0x7FFFF;
    instruction |= (immhi << 5);
    
    // 低2位放在位 30-29
    uint32_t immlo = page_offset & 0x3;
    instruction |= (immlo << 29);
    
    // 将编码的指令写入代码缓冲区
    emit_int32(instruction);
    
    return current_pc();
}

address YuhuMacroAssembler::write_inst_adr(YuhuRegister reg, address target) {
    long offset = target - current_pc();
    return write_inst("adr %s, #%d", reg, offset);
}

// Validate format string for: write_inst(assembly_format, imm32) - expects 1 parameter: %d/x/u/o
void YuhuMacroAssembler::validate_assembly_format_1_imm(const char* assembly_format) {
    assert(assembly_format != NULL, "assembly_format cannot be NULL");
    
    const char* format_str = assembly_format;
    int specifier_count = 0;
    bool has_valid_immediate = false;
    
    while (*format_str != '\0') {
        if (*format_str == '%') {
            format_str++;
            if (*format_str == '\0') break;
            
            specifier_count++;
            
            if (*format_str == 's') {
                assert(false, "assembly_format with 1 parameter should use %d/x/u/o, not %s");
            } else if (*format_str == 'd' || *format_str == 'x' || *format_str == 'u' || *format_str == 'o') {
                has_valid_immediate = true;
            } else if (*format_str == 'l' && *(format_str + 1) != '\0') {
                format_str++;
                if (*format_str == 'd' || *format_str == 'x' || *format_str == 'u' || *format_str == 'o') {
                    assert(false, "Invalid format specifier: Use %d instead of %l%c for int parameter");
                }
            } else {
                assert(false, "Unsupported format specifier in assembly format string");
            }
        }
        format_str++;
    }
    
    assert(specifier_count == 1, "assembly_format must have exactly 1 format specifier for immediate value");
    assert(has_valid_immediate, "assembly_format must contain %d, %x, %u, or %o as parameter");
}

// Validate format string for: write_inst(assembly_format, reg, imm32) - expects 2 parameters: %s and %d/x/u/o
void YuhuMacroAssembler::validate_assembly_format_2_reg_imm(const char* assembly_format) {
    assert(assembly_format != NULL, "assembly_format cannot be NULL");
    
    const char* format_str = assembly_format;
    int specifier_count = 0;
    bool has_percent_s = false;
    bool has_valid_immediate = false;
    
    while (*format_str != '\0') {
        if (*format_str == '%') {
            format_str++;
            if (*format_str == '\0') break;
            
            specifier_count++;
            
            if (*format_str == 's') {
                if (specifier_count == 1) has_percent_s = true;
                else assert(false, "assembly_format should only have %s as first parameter");
            } else if (*format_str == 'd' || *format_str == 'x' || *format_str == 'u' || *format_str == 'o') {
                if (specifier_count == 2) has_valid_immediate = true;
                else assert(false, "assembly_format should have %d/x/u/o as second parameter");
            } else if (*format_str == 'l' && *(format_str + 1) != '\0') {
                format_str++;
                if (*format_str == 'd' || *format_str == 'x' || *format_str == 'u' || *format_str == 'o') {
                    assert(false, "Invalid format specifier: Use %d instead of %l%c for int parameter");
                }
            } else {
                assert(false, "Unsupported format specifier in assembly format string");
            }
        }
        format_str++;
    }
    
    assert(specifier_count == 2, "assembly_format must have exactly 2 format specifiers for register + immediate");
    assert(has_percent_s, "assembly_format must contain %s as first parameter (register)");
    assert(has_valid_immediate, "assembly_format must contain %d, %x, %u, or %o as second parameter (immediate)");
}

// Validate format string for: write_inst(assembly_format, reg1, reg2, imm32) - expects 3 parameters: %s, %s, and %d/x/u/o
void YuhuMacroAssembler::validate_assembly_format_3_reg_reg_imm(const char* assembly_format) {
    assert(assembly_format != NULL, "assembly_format cannot be NULL");
    
    const char* format_str = assembly_format;
    int specifier_count = 0;
    bool has_first_s = false;
    bool has_second_s = false;
    bool has_valid_immediate = false;
    
    while (*format_str != '\0') {
        if (*format_str == '%') {
            format_str++;
            if (*format_str == '\0') break;
            
            specifier_count++;
            
            if (*format_str == 's') {
                if (specifier_count == 1) has_first_s = true;
                else if (specifier_count == 2) has_second_s = true;
                else assert(false, "assembly_format should only have %s as first two parameters");
            } else if (*format_str == 'd' || *format_str == 'x' || *format_str == 'u' || *format_str == 'o') {
                if (specifier_count == 3) has_valid_immediate = true;
                else assert(false, "assembly_format should have %d/x/u/o as third parameter");
            } else if (*format_str == 'l' && *(format_str + 1) != '\0') {
                format_str++;
                if (*format_str == 'd' || *format_str == 'x' || *format_str == 'u' || *format_str == 'o') {
                    assert(false, "Invalid format specifier: Use %d instead of %l%c for int parameter");
                }
            } else {
                assert(false, "Unsupported format specifier in assembly format string");
            }
        }
        format_str++;
    }
    
    assert(specifier_count == 3, "assembly_format must have exactly 3 format specifiers for register + register + immediate");
    assert(has_first_s, "assembly_format must contain %s as first parameter (register)");
    assert(has_second_s, "assembly_format must contain %s as second parameter (register)");
    assert(has_valid_immediate, "assembly_format must contain %d, %x, %u, or %o as third parameter (immediate)");
}

// Validate format string for: write_inst(assembly_format, reg1, reg2, reg3, imm32) - expects 4 parameters: %s, %s, %s, and %d/x/u/o
void YuhuMacroAssembler::validate_assembly_format_4_reg_reg_reg_imm(const char* assembly_format) {
    assert(assembly_format != NULL, "assembly_format cannot be NULL");
    
    const char* format_str = assembly_format;
    int specifier_count = 0;
    bool has_first_s = false;
    bool has_second_s = false;
    bool has_third_s = false;
    bool has_valid_immediate = false;
    
    while (*format_str != '\0') {
        if (*format_str == '%') {
            format_str++;
            if (*format_str == '\0') break;
            
            specifier_count++;
            
            if (*format_str == 's') {
                if (specifier_count == 1) has_first_s = true;
                else if (specifier_count == 2) has_second_s = true;
                else if (specifier_count == 3) has_third_s = true;
                else assert(false, "assembly_format should only have %s as first three parameters");
            } else if (*format_str == 'd' || *format_str == 'x' || *format_str == 'u' || *format_str == 'o') {
                if (specifier_count == 4) has_valid_immediate = true;
                else assert(false, "assembly_format should have %d/x/u/o as fourth parameter");
            } else if (*format_str == 'l' && *(format_str + 1) != '\0') {
                format_str++;
                if (*format_str == 'd' || *format_str == 'x' || *format_str == 'u' || *format_str == 'o') {
                    assert(false, "Invalid format specifier: Use %d instead of %l%c for int parameter");
                }
            } else {
                assert(false, "Unsupported format specifier in assembly format string");
            }
        }
        format_str++;
    }
    
    assert(specifier_count == 4, "assembly_format must have exactly 4 format specifiers for register + register + register + immediate");
    assert(has_first_s, "assembly_format must contain %s as first parameter (register)");
    assert(has_second_s, "assembly_format must contain %s as second parameter (register)");
    assert(has_third_s, "assembly_format must contain %s as third parameter (register)");
    assert(has_valid_immediate, "assembly_format must contain %d, %x, %u, or %o as fourth parameter (immediate)");
}

void YuhuMacroAssembler::validate_assembly_format_long(const char* assembly_format) {
    assert(assembly_format != NULL, "assembly_format cannot be NULL");
    
    // Check for format specifiers - expects exactly 2: %s and %ld/%lx
    const char* format_str = assembly_format;
    int specifier_count = 0;
    bool has_percent_s = false;
    bool has_valid_long_specifier = false;
    
    while (*format_str != '\0') {
        if (*format_str == '%') {
            format_str++; // Skip the %
            if (*format_str == '\0') break; // Handle edge case of % at end
            
            specifier_count++;
            
            if (*format_str == 's') {
                if (specifier_count == 1) {
                    has_percent_s = true;
                } else {
                    assert(false, "assembly_format with long immediate should only have %s as first parameter");
                }
            } else if (*format_str == 'l' && *(format_str + 1) != '\0') {
                // Check for %ld, %lx, %lu, %lo
                format_str++; // Skip the 'l'
                if (*format_str == 'd' || *format_str == 'x' || *format_str == 'u' || *format_str == 'o') {
                    if (specifier_count == 2) {
                        has_valid_long_specifier = true;
                    } else {
                        assert(false, "assembly_format with long immediate should have %ld/%lx as second parameter");
                    }
                } else {
                    assert(false, "Invalid long format specifier");
                }
            } else if (*format_str == 'd') {
                // %d is not valid for long parameter
                assert(false, "Invalid format specifier: Use %ld instead of %d for long parameter");
            } else if (*format_str == 'x' || *format_str == 'u' || *format_str == 'o') {
                // %x, %u, %o without 'l' are not valid for long parameter
                assert(false, "Invalid format specifier: Use %lx/%lu/%lo instead of %x/%u/%o for long parameter");
            } else {
                assert(false, "Unsupported format specifier in assembly format string");
            }
        }
        format_str++;
    }
    
    // Validate format string requirements
    assert(specifier_count == 2, "assembly_format must have exactly 2 format specifiers for register + long immediate");
    assert(has_percent_s, "assembly_format must contain %s as first parameter (register)");
    assert(has_valid_long_specifier, "assembly_format must contain %ld, %lx, %lu, or %lo as second parameter (long immediate)");
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
    return write_inst("ldr %s, [x20], #%d", r, 2 * Interpreter::stackElementSize);
}

address YuhuMacroAssembler::write_inst_push_f(YuhuFloatRegister r) {
    return write_inst("str %s, [x20, #%d]!", r, -wordSize);
}

address YuhuMacroAssembler::write_inst_push_d(YuhuFloatRegister r) {
    return write_inst("str %s, [x20, #%d]!", r, 2 * -wordSize);
}

address YuhuMacroAssembler::write_inst_push(YuhuRegister src) {
    return write_inst("str %s, [x20, #%d]!", src, -1 * wordSize);
}

address YuhuMacroAssembler::write_inst_pop(YuhuRegister dst) {
    return write_inst("ldr %s, [x20], #%d", dst, 1 * wordSize);
}

address YuhuMacroAssembler::write_inst_cset(YuhuRegister reg, YuhuCond cond) {
    char buffer[50];
    snprintf(buffer, sizeof(buffer), "cset %s, %s", reg_name(reg), cond_name(cond));
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_csel(YuhuRegister reg1, YuhuRegister reg2, YuhuRegister reg3, YuhuCond cond) {
    char buffer[50];
    snprintf(buffer, sizeof(buffer), "csel %s, %s, %s, %s", reg_name(reg1), reg_name(reg2), reg_name(reg3), cond_name(cond));
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_csinc(YuhuRegister reg1, YuhuRegister reg2, YuhuRegister reg3, YuhuCond cond) {
    char buffer[50];
    snprintf(buffer, sizeof(buffer), "csinc %s, %s, %s, %s", reg_name(reg1), reg_name(reg2), reg_name(reg3), cond_name(cond));
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_csinv(YuhuRegister reg1, YuhuRegister reg2, YuhuRegister reg3, YuhuCond cond) {
    char buffer[50];
    snprintf(buffer, sizeof(buffer), "csinv %s, %s, %s, %s", reg_name(reg1), reg_name(reg2), reg_name(reg3), cond_name(cond));
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_insts_load_unsigned_short(YuhuRegister dst, YuhuAddress src) {
    return write_inst_ldrh(dst, src);
}

address YuhuMacroAssembler::write_insts_load_unsigned_byte(YuhuRegister dst, YuhuAddress src) {
    return write_inst_ldrb(dst, src);
}

address YuhuMacroAssembler::write_insts_get_unsigned_2_byte_index_at_bcp(YuhuRegister reg, int bcp_offset) {
    assert(bcp_offset >= 0, "bcp is still pointing to start of bytecode");
    write_inst_ldrh(reg, YuhuAddress(x22, bcp_offset));
    write_inst_regs("rev16 %s, %s", reg, reg);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_load_signed_byte(YuhuRegister dst, YuhuAddress src) {
    return write_inst_ldrsb(dst, src);
}

address YuhuMacroAssembler::write_insts_load_signed_short(YuhuRegister dst, YuhuAddress src) {
    return write_inst_ldrsh(dst, src);
}

address YuhuMacroAssembler::write_insts_load_signed_short32(YuhuRegister dst, YuhuAddress src) {
    return write_inst_ldrsh(dst, src);
}

address YuhuMacroAssembler::write_insts_load_signed_byte32(YuhuRegister dst, YuhuAddress src) {
    return write_inst_ldrsb(dst, src);
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
    if (verifyoop) {
        write_insts_verify_oop(x0, state);
    }
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

address YuhuMacroAssembler::write_insts_dispatch_only(TosState state) {
    return write_insts_dispatch_base(state, YuhuInterpreter::dispatch_table(state));
}

address YuhuMacroAssembler::write_insts_dispatch_only_normal(TosState state) {
    return write_insts_dispatch_base(state, YuhuInterpreter::normal_table(state));
}

address YuhuMacroAssembler::write_insts_get_dispatch() {
    uint64_t offset;
    write_insts_adrp(x21, (address) YuhuInterpreter::dispatch_table(), offset);
    write_insts_lea(x21, YuhuAddress(x21, offset));
    return current_pc();
}

address YuhuMacroAssembler::write_insts_dispatch_next(TosState state, int step) {
    // load next bytecode
    write_inst("ldrb %s, [x22, #%d]!", w8, step);
    write_insts_dispatch_base(state, YuhuInterpreter::dispatch_table(state));
    return current_pc();
}

address YuhuMacroAssembler::write_insts_dispatch_via(TosState state, address* table) {
    // load current bytecode
    write_inst("ldrb w8, [x22, #0]");
    write_insts_dispatch_base(state, table);
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

address YuhuMacroAssembler::write_insts_adrp(YuhuRegister reg, const YuhuExternalAddress &dest, uint64_t &byte_offset) {
//    relocInfo::relocType rtype = dest.rspec().reloc()->type();
    uint64_t low_page = (uint64_t)CodeCache::low_bound() >> 12;
    uint64_t high_page = (uint64_t)(CodeCache::high_bound()-1) >> 12;
    uint64_t dest_page = (uint64_t) dest.target() >> 12;
    int64_t offset_low = dest_page - low_page;
    int64_t offset_high = dest_page - high_page;

    assert(is_valid_AArch64_address(dest.target()), "bad address");
//    assert(dest.getMode() == Address::literal, "ADRP must be applied to a literal address");

    InstructionMark im(this);
    code_section()->relocate(inst_mark(), dest.rspec());
    // 8143067: Ensure that the adrp can reach the dest from anywhere within
    // the code cache so that if it is relocated we know it will still reach
    if (offset_high >= -(1<<20) && offset_low < (1<<20)) {
        write_inst_adrp(reg, dest.target());
    } else {
        uint64_t target = (uint64_t)dest.target();
        uint64_t adrp_target
                = (target & 0xffffffffUL) | ((uint64_t)current_pc() & 0xffff00000000UL);
        write_inst_adrp(reg, (address)adrp_target);
        write_inst("movk %s, #%d, lsl #32", reg, (target >> 32) & 0xffff);
    }
    byte_offset = (uint64_t)dest.target() & 0xfff;
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
    write_insts_lea(x9, YuhuAddress(StubRoutines::verify_oop_subroutine_entry_address()));

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
        // TODO
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
            write_insts_atomic_incw(YuhuAddress((address)BiasedLocking::fast_path_entry_count_addr()), x9, x8, tmp);
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
            write_insts_atomic_incw(YuhuAddress((address)BiasedLocking::fast_path_entry_count_addr()), x9, x8, tmp);
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
        write_insts_lea(swap_reg, YuhuAddress(lock_reg, BasicObjectLock::lock_offset_in_bytes()));

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

address YuhuMacroAssembler::write_insts_final_call_VM(YuhuRegister oop_result, address entry_point, YuhuRegister arg_1, YuhuRegister arg_2, YuhuRegister arg_3, bool check_exceptions) {
    assert(arg_1 != x3, "smashed arg");
    assert(arg_2 != x3, "smashed arg");
    if (x3 != arg_3) {
        write_inst_mov_reg(x3, arg_3);
    }

    assert(arg_1 != x2, "smashed arg");
    if (x2 != arg_2) {
        write_inst_mov_reg(x2, arg_2);
    }

    if (x1 != arg_1) {
        write_inst_mov_reg(x1, arg_1);
    }
    write_insts_final_call_VM_helper(oop_result, entry_point, 3, check_exceptions);
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
        write_insts_mov_imm64(x8, (uint64_t) StubRoutines::forward_exception_entry());
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

address YuhuMacroAssembler::write_insts_final_call_VM_leaf(address entry_point, int number_of_arguments) {
    write_insts_final_call_VM_leaf_base(entry_point, number_of_arguments);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_final_call_VM_leaf(address entry_point, YuhuRegister arg_0) {
    if (arg_0 != x0) {
        write_inst_mov_reg(x0, arg_0);
    }
    write_insts_call_VM_leaf_base(entry_point, 1);
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

address YuhuMacroAssembler::write_insts_final_call_VM_leaf(address entry_point, YuhuRegister arg_0, YuhuRegister arg_1, YuhuRegister arg_2) {
    if (arg_0 != x0) {
        write_inst_mov_reg(x0, arg_0);
    }
    if (arg_1 != x1) {
        write_inst_mov_reg(x1, arg_1);
    }
    if (arg_2 != x2) {
        write_inst_mov_reg(x2, arg_2);
    }
    write_insts_call_VM_leaf_base(entry_point, 3);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_get_vm_result(YuhuRegister oop_result, YuhuRegister java_thread) {
    write_inst("ldr %s, [%s, #%d]", oop_result, java_thread, in_bytes(JavaThread::vm_result_offset()));
    write_inst("str %s, [%s, #%d]", xzr, java_thread, in_bytes(JavaThread::vm_result_offset()));
    write_insts_verify_oop(oop_result, "broken oop in call_VM_base");
    return current_pc();
}

address YuhuMacroAssembler::write_insts_get_vm_result_2(YuhuRegister metadata_result, YuhuRegister java_thread) {
    write_inst_ldr(metadata_result, YuhuAddress(java_thread, JavaThread::vm_result_2_offset()));
    write_inst_str(xzr, YuhuAddress(java_thread, JavaThread::vm_result_2_offset()));
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
        write_insts_atomic_incw(YuhuAddress((address)counters->biased_lock_entry_count_addr()), tmp_reg, x8, x9);
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
            write_insts_atomic_incw(YuhuAddress((address)counters->anonymously_biased_lock_entry_count_addr()), tmp_reg, x8, x9);
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
            write_insts_atomic_incw(YuhuAddress((address)counters->rebiased_lock_entry_count_addr()), tmp_reg, x8, x9);
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
            write_insts_atomic_incw(YuhuAddress((address)counters->revoked_lock_entry_count_addr()), tmp_reg, x8, x9);
        }
        pin_label(nope);
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
        write_inst_regs("ldadd %s, %s, [%s]", w_reg(tmp), wzr, counter_addr);
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

address YuhuMacroAssembler::write_insts_atomic_incw(YuhuAddress counter_addr, YuhuRegister tmp1, YuhuRegister tmp2, YuhuRegister tmp3) {
    write_insts_lea(tmp1, counter_addr);
    return write_insts_atomic_incw(tmp1, tmp2, tmp3);
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

void YuhuMacroAssembler::write_insts_push(YuhuRegSet regs, YuhuRegister stack) {
    if (regs.bits()) write_insts_push(regs.bits(), stack);
}
void YuhuMacroAssembler::write_insts_pop(YuhuRegSet regs, YuhuRegister stack) {
    if (regs.bits()) write_insts_pop(regs.bits(), stack);
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
        write_insts_load_heap_oop(pre_val, YuhuAddress(obj, 0));
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

address YuhuMacroAssembler::write_insts_g1_write_barrier_post(YuhuRegister store_addr,
                                                              YuhuRegister new_val,
                                                              YuhuRegister thread,
                                                              YuhuRegister tmp,
                                                              YuhuRegister tmp2) {
#ifdef _LP64
    assert(thread == x28, "must be");
#endif // _LP64
//    assert_different_registers(store_addr, new_val, thread, tmp, tmp2,
//                               rscratch1);
    assert(store_addr != noreg && new_val != noreg && tmp != noreg
           && tmp2 != noreg, "expecting a register");

//    Address queue_index(thread, in_bytes(JavaThread::dirty_card_queue_offset() +
//                                         PtrQueue::byte_offset_of_index()));
//    Address buffer(thread, in_bytes(JavaThread::dirty_card_queue_offset() +
//                                    PtrQueue::byte_offset_of_buf()));

    BarrierSet* bs = Universe::heap()->barrier_set();
    CardTableModRefBS* ct = (CardTableModRefBS*)bs;
    assert(sizeof(*ct->byte_map_base) == sizeof(jbyte), "adjust this code");

    YuhuLabel done;
    YuhuLabel runtime;

    // Does store cross heap regions?

    write_inst_regs("eor %s, %s, %s", tmp, store_addr, new_val);
    write_inst("lsr %s, %s, #%d", tmp, tmp, HeapRegion::LogOfHRGrainBytes);
    write_inst_cbz(tmp, done);

    // crosses regions, storing NULL?

    write_inst_cbz(new_val, done);

    // storing region crossing non-NULL, is card already dirty?

    ExternalAddress cardtable((address) ct->byte_map_base);
    assert(sizeof(*ct->byte_map_base) == sizeof(jbyte), "adjust this code");
    const YuhuRegister card_addr = tmp;

    write_inst("lsr %s, %s, #%d", card_addr, store_addr, CardTableModRefBS::card_shift);

    // get the address of the card
    write_insts_load_byte_map_base(tmp2);
    write_inst_regs("add %s, %s, %s", card_addr, card_addr, tmp2);
    write_inst_regs("ldrb %s, [%s]", w_reg(tmp2), card_addr);
    write_inst("cmp %s, #%d", w_reg(tmp2), (int)G1SATBCardTableModRefBS::g1_young_card_val());
    write_inst_b(eq, done);

    assert((int)CardTableModRefBS::dirty_card_val() == 0, "must be 0");

    write_inst("dmb ish");

    write_inst_regs("ldrb %s, [%s]", w_reg(tmp2), card_addr);
    write_inst_cbz(w_reg(tmp2), done);

    // storing a region crossing, non-NULL oop, card is clean.
    // dirty card and log.
    write_inst_regs("strb %s, [%s]", wzr, card_addr);

    write_inst("ldr x8, [%s, #%d]", thread, in_bytes(JavaThread::dirty_card_queue_offset() +
                                                     PtrQueue::byte_offset_of_index()));
    write_inst_cbz(x8, runtime);
    write_inst("sub x8, x8, #%d", wordSize);
    write_inst("str x8, [%s, #%d]", thread, in_bytes(JavaThread::dirty_card_queue_offset() +
                                                     PtrQueue::byte_offset_of_index()));

    write_inst("ldr %s, [%s, #%d]", tmp2, thread, in_bytes(JavaThread::dirty_card_queue_offset() +
                                                           PtrQueue::byte_offset_of_buf()));
    write_inst_regs("str %s, [%s, x8]", card_addr, tmp2);
    write_inst_b(done);

    pin_label(runtime);
    // save the live input values
    write_insts_push(bit(store_addr, true) | bit(new_val, true), sp);
    write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::g1_wb_post), card_addr, thread);
    write_insts_pop(bit(store_addr, true) | bit(new_val, true), sp);

    pin_label(done);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_load_heap_oop(YuhuRegister dst, YuhuAddress src) {
    // TODO
//    if (UseCompressedOops) {
//        ldrw(dst, src);
//        decode_heap_oop(dst);
//    } else {
        write_inst_ldr(dst, src);
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

address YuhuMacroAssembler::write_insts_get_cache_and_index_at_bcp(YuhuRegister cache,
                                                           YuhuRegister index,
                                                           int bcp_offset,
                                                           size_t index_size) {
//    assert_different_registers(cache, index);
//    assert_different_registers(cache, rcpool);
    write_insts_get_cache_index_at_bcp(index, bcp_offset, index_size);
    assert(sizeof(ConstantPoolCacheEntry) == 4 * wordSize, "adjust code below");
    // convert from field index to ConstantPoolCacheEntry
    // aarch64 already has the cache in rcpool so there is no need to
    // install it in cache. instead we pre-add the indexed offset to
    // rcpool and return it in cache. All clients of this method need to
    // be modified accordingly.
    write_inst_regs("add %s, x26, %s, lsl #5", cache, index);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_get_cache_and_index_and_bytecode_at_bcp(YuhuRegister cache,
                                                                        YuhuRegister index,
                                                                        YuhuRegister bytecode,
                                                                        int byte_no,
                                                                        int bcp_offset,
                                                                        size_t index_size) {
    write_insts_get_cache_and_index_at_bcp(cache, index, bcp_offset, index_size);
    // We use a 32-bit load here since the layout of 64-bit words on
    // little-endian machines allow us that.
    // n.b. unlike x86 cache already includes the index offset
    write_insts_lea(bytecode, YuhuAddress(cache, ConstantPoolCache::base_offset() + ConstantPoolCacheEntry::indices_offset()));
    write_inst_regs("ldar %s, [%s]", w_reg(bytecode), bytecode);
    const int shift_count = (1 + byte_no) * BitsPerByte;
    write_inst_imms("ubfx %s, %s, #%d, #%d", bytecode, bytecode, shift_count, BitsPerByte);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_get_cache_index_at_bcp(YuhuRegister index,
                                                       int bcp_offset,
                                                       size_t index_size) {
    assert(bcp_offset > 0, "bcp is still pointing to start of bytecode");
    if (index_size == sizeof(u2)) {
        write_insts_load_unsigned_short(w_reg(index), YuhuAddress(x22, bcp_offset));
    } else if (index_size == sizeof(u4)) {
        assert(EnableInvokeDynamic, "giant index used only for JSR 292");
        write_inst("ldr %s, [x22, #%d]", w_reg(index), bcp_offset);
        // Check if the secondary index definition is still ~x, otherwise
        // we have to change the following assembler code to calculate the
        // plain index.
        assert(ConstantPool::decode_invokedynamic_index(~123) == 123, "else change next line");
        write_inst_regs("eon %s, %s, %s", w_reg(index), w_reg(index), wzr); // convert to plain index
    } else if (index_size == sizeof(u1)) {
        write_insts_load_unsigned_byte(w_reg(index), YuhuAddress(x22, bcp_offset));
    } else {
        ShouldNotReachHere();
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_c2bool(YuhuRegister x) {
    // implements x == 0 ? 0 : 1
    // note: must only look at least-significant byte of x
    //       since C-style booleans are stored in one byte
    //       only! (was bug)
    write_inst("tst %s, #%d", x, 0xff);
    write_inst_cset(x, YuhuMacroAssembler::ne);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_remove_activation(
        TosState state,
        bool throw_monitor_exception,
        bool install_monitor_exception,
        bool notify_jvmdi) {
    // Note: Registers r3 xmm0 may be in use for the
    // result check if synchronized method
    YuhuLabel unlocked, unlock, no_unlock;

    // get the value of _do_not_unlock_if_synchronized into r3
//    const Address do_not_unlock_if_synchronized(rthread,
//                                                in_bytes(JavaThread::do_not_unlock_if_synchronized_offset()));
    write_inst("ldrb w3, [x28, #%d]", in_bytes(JavaThread::do_not_unlock_if_synchronized_offset()));
    write_inst("strb wzr, [x28, #%d]", in_bytes(JavaThread::do_not_unlock_if_synchronized_offset())); // reset the flag

    // get method access flags
    write_inst("ldr x1, [x29, #%d]", frame::interpreter_frame_method_offset * wordSize);
    write_inst("ldr x2, [x1, #%d]", in_bytes(Method::access_flags_offset()));
    write_inst("tst x2, #%d", JVM_ACC_SYNCHRONIZED);
    write_inst_b(eq, unlocked);

    // Don't unlock anything if the _do_not_unlock_if_synchronized flag
    // is set.
    write_inst_cbnz(x3, no_unlock);

    // unlock monitor
    write_insts_push(state); // save result

    // BasicObjectLock will be first in list, since this is a
    // synchronized method. However, need to check that the object has
    // not been unlocked by an explicit monitorexit bytecode.
    const YuhuAddress monitor(x29, frame::interpreter_frame_initial_sp_offset *
                               wordSize - (int) sizeof(BasicObjectLock));
    // We use c_rarg1 so that if we go slow path it will be the correct
    // register for unlock_object to pass to VM directly
    write_insts_lea(x1, monitor); // address of first monitor

    write_inst("ldr x0, [x1, #%d]", BasicObjectLock::obj_offset_in_bytes());
    write_inst_cbnz(x0, unlock);

    write_insts_pop(state);
    if (throw_monitor_exception) {
        // Entry already unlocked, need to throw exception
        write_insts_final_call_VM(noreg, CAST_FROM_FN_PTR(address,
                                        InterpreterRuntime::throw_illegal_monitor_state_exception));
        write_insts_stop("should not reach here");
    } else {
        // Monitor already unlocked during a stack unroll. If requested,
        // install an illegal_monitor_state_exception.  Continue with
        // stack unrolling.
        if (install_monitor_exception) {
            write_insts_final_call_VM(noreg, CAST_FROM_FN_PTR(address,
                                            InterpreterRuntime::new_illegal_monitor_state_exception));
        }
        write_inst_b(unlocked);
    }

    pin_label(unlock);
    write_insts_unlock_object(x1);
    write_insts_pop(state);

    // Check that for block-structured locking (i.e., that all locked
    // objects has been unlocked)
    pin_label(unlocked);

    // r0: Might contain return value

    // Check that all monitors are unlocked
    {
        YuhuLabel loop, exception, entry, restart;
        const int entry_size = frame::interpreter_frame_monitor_size() * wordSize;
//        const Address monitor_block_top(
//                rfp, frame::interpreter_frame_monitor_block_top_offset * wordSize);
        const YuhuAddress monitor_block_bot(
                x29, frame::interpreter_frame_initial_sp_offset * wordSize);

        pin_label(restart);
        // We use c_rarg1 so that if we go slow path it will be the correct
        // register for unlock_object to pass to VM directly
        write_inst("ldr x1, [x29, #%d]", frame::interpreter_frame_monitor_block_top_offset * wordSize); // points to current entry, starting
        // with top-most entry
        write_insts_lea(x19, monitor_block_bot);  // points to word before bottom of
        // monitor block
        write_inst_b(entry);

        // Entry already locked, need to throw exception
        pin_label(exception);

        if (throw_monitor_exception) {
            // Throw exception
            write_insts_final_call_VM(noreg,
                                    CAST_FROM_FN_PTR(address, InterpreterRuntime::
                                            throw_illegal_monitor_state_exception));
            write_insts_stop("should not reach here");
        } else {
            // Stack unrolling. Unlock object and install illegal_monitor_exception.
            // Unlock does not block, so don't have to worry about the frame.
            // We don't have to preserve c_rarg1 since we are going to throw an exception.

            write_insts_push(state);
            write_insts_unlock_object(x1);
            write_insts_pop(state);

            if (install_monitor_exception) {
                write_insts_final_call_VM(noreg, CAST_FROM_FN_PTR(address,
                                                InterpreterRuntime::
                                                        new_illegal_monitor_state_exception));
            }

            write_inst_b(restart);
        }

        pin_label(loop);
        // check if current entry is used
        write_inst("ldr x8, [x1, #%d]", BasicObjectLock::obj_offset_in_bytes());
        write_inst_cbnz(x8, exception);

        write_inst("add x1, x1, #%d", entry_size); // otherwise advance to next entry
        pin_label(entry);
        write_inst("cmp x1, x19"); // check if bottom reached
        write_inst_b(ne, loop); // if not at bottom then check this entry
    }

    pin_label(no_unlock);

    // TODO
    // jvmti support
//    if (notify_jvmdi) {
//        notify_method_exit(state, NotifyJVMTI);    // preserve TOSCA
//    } else {
//        notify_method_exit(state, SkipNotifyJVMTI); // preserve TOSCA
//    }

    // remove activation
    // get sender esp
    write_inst("ldr x20, [x29, #%d]", frame::interpreter_frame_sender_sp_offset * wordSize);
    // remove frame anchor
    write_insts_leave();
    // If we're returning to interpreted code we will shortly be
    // adjusting SP to allow some space for ESP.  If we're returning to
    // compiled code the saved sender SP was saved in sender_sp, so this
    // restores it.
    write_inst("and sp, x20, #-16");
    return current_pc();
}

address YuhuMacroAssembler::write_insts_get_thread(YuhuRegister dst) {
    YuhuRegSet saved_regs = YuhuRegSet::range(x0, x17) + YuhuRegSet::range(x19, x20) + lr - dst;
    write_insts_push(saved_regs, sp);
    write_insts_mov_imm64(x0, ThreadLocalStorage::thread_index());
    write_insts_mov_imm64(x19, (uint64_t) CAST_FROM_FN_PTR(address, pthread_getspecific));
    write_inst_blr(x19);
    if (dst != x0) {
        write_inst_mov_reg(dst, x0);
    }
    // restore pushed registers
    write_insts_pop(saved_regs, sp);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_null_check(YuhuRegister reg, int offset) {
    if (needs_explicit_null_check(offset)) {
        // provoke OS NULL exception if reg = NULL by
        // accessing M[reg] w/o changing any registers
        // NOTE: this is plenty to provoke a segv
        write_inst("ldr xzr, [%s, #%d]", reg, 0);
    } else {
        // nothing to do, (later) access of M[reg + offset]
        // will provoke OS NULL exception if reg = NULL
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_gen_subtype_check(YuhuRegister Rsub_klass,
                                                  YuhuLabel& ok_is_subtype) {
    assert(Rsub_klass != x0, "r0 holds superklass");
    assert(Rsub_klass != x2, "r2 holds 2ndary super array length");
    assert(Rsub_klass != x5, "r5 holds 2ndary super array scan ptr");

    // Profile the not-null value's klass.
    // TODO
//    profile_typecheck(r2, Rsub_klass, r5); // blows r2, reloads r5

    // Do the check.
    write_insts_check_klass_subtype(Rsub_klass, x0, x2, ok_is_subtype); // blows r2

    // Profile the failure of the check.
    // TODO
//    profile_typecheck_failed(r2); // blows r2

    return current_pc();
}

address YuhuMacroAssembler::write_insts_check_klass_subtype(YuhuRegister sub_klass,
                                         YuhuRegister super_klass,
                                         YuhuRegister temp_reg,
                                         YuhuLabel& L_success) {
    YuhuLabel L_failure;
    write_insts_check_klass_subtype_fast_path(sub_klass, super_klass, temp_reg,        &L_success, &L_failure, NULL);
    write_insts_check_klass_subtype_slow_path(sub_klass, super_klass, temp_reg, noreg, &L_success, NULL);
    pin_label(L_failure);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_check_klass_subtype_fast_path(YuhuRegister sub_klass,
                                                   YuhuRegister super_klass,
                                                   YuhuRegister temp_reg,
                                                   YuhuLabel* L_success,
                                                   YuhuLabel* L_failure,
                                                   YuhuLabel* L_slow_path,
                                                   YuhuRegisterOrConstant super_check_offset) {
//    assert_different_registers(sub_klass, super_klass, temp_reg);
    bool must_load_sco = (super_check_offset.constant_or_zero() == -1);
    if (super_check_offset.is_register()) {
//        assert_different_registers(sub_klass, super_klass,
//                                   super_check_offset.as_register());
    } else if (must_load_sco) {
        assert(temp_reg != noreg, "supply either a temp or a register offset");
    }

    YuhuLabel L_fallthrough;
    int label_nulls = 0;
    if (L_success == NULL)   { L_success   = &L_fallthrough; label_nulls++; }
    if (L_failure == NULL)   { L_failure   = &L_fallthrough; label_nulls++; }
    if (L_slow_path == NULL) { L_slow_path = &L_fallthrough; label_nulls++; }
    assert(label_nulls <= 1, "at most one NULL in the batch");

    int sc_offset = in_bytes(Klass::secondary_super_cache_offset());
    int sco_offset = in_bytes(Klass::super_check_offset_offset());
    YuhuAddress super_check_offset_addr(super_klass, sco_offset);

    // Hacked jmp, which may only be used just before L_fallthrough.
#define final_jmp(label)                                                \
  if (&(label) == &L_fallthrough) { /*do nothing*/ }                    \
  else                            write_inst_b(label)                /*omit semi*/

    // If the pointers are equal, we are done (e.g., String[] elements).
    // This self-check enables sharing of secondary supertype arrays among
    // non-primary types such as array-of-interface.  Otherwise, each such
    // type would need its own customized SSA.
    // We move this check to the front of the fast path because many
    // type checks are in fact trivially successful in this manner,
    // so we get a nicely predicted branch right at the start of the check.
    write_inst_regs("cmp %s, %s", sub_klass, super_klass);
    write_inst_b(eq, *L_success);

    // Check the supertype display:
    if (must_load_sco) {
        // Positive movl does right thing on LP64.
        write_inst_ldr(w_reg(temp_reg), super_check_offset_addr);
        super_check_offset = YuhuRegisterOrConstant(temp_reg);
    }
    YuhuAddress super_check_addr(sub_klass, super_check_offset);
    write_inst_ldr(x8, super_check_addr);
    write_inst_regs("cmp %s, %s", super_klass, x8);  // load displayed supertype

    // This check has worked decisively for primary supers.
    // Secondary supers are sought in the super_cache ('super_cache_addr').
    // (Secondary supers are interfaces and very deeply nested subtypes.)
    // This works in the same check above because of a tricky aliasing
    // between the super_cache and the primary super display elements.
    // (The 'super_check_addr' can address either, as the case requires.)
    // Note that the cache is updated below if it does not help us find
    // what we need immediately.
    // So if it was a primary super, we can just fail immediately.
    // Otherwise, it's the slow path for us (no success at this point).

    if (super_check_offset.is_register()) {
        write_inst_b(eq, *L_success);
        write_inst("cmp %s, #%d", super_check_offset.as_register(), sc_offset);
        if (L_failure == &L_fallthrough) {
            write_inst_b(eq, *L_slow_path);
        } else {
            write_inst_b(ne, *L_failure);
            final_jmp(*L_slow_path);
        }
    } else if (super_check_offset.as_constant() == sc_offset) {
        // Need a slow path; fast failure is impossible.
        if (L_slow_path == &L_fallthrough) {
            write_inst_b(eq, *L_success);
        } else {
            write_inst_b(ne, *L_slow_path);
            final_jmp(*L_success);
        }
    } else {
        // No slow path; it's a fast decision.
        if (L_failure == &L_fallthrough) {
            write_inst_b(eq, *L_success);
        } else {
            write_inst_b(ne, *L_failure);
            final_jmp(*L_success);
        }
    }

    pin_label(L_fallthrough);

#undef final_jmp
    return current_pc();
}

address YuhuMacroAssembler::write_insts_check_klass_subtype_slow_path(YuhuRegister sub_klass,
                                                   YuhuRegister super_klass,
                                                   YuhuRegister temp_reg,
                                                   YuhuRegister temp2_reg,
                                                   YuhuLabel* L_success,
                                                   YuhuLabel* L_failure,
                                                   bool set_cond_codes) {
//    assert_different_registers(sub_klass, super_klass, temp_reg);
//    if (temp2_reg != noreg)
//        assert_different_registers(sub_klass, super_klass, temp_reg, temp2_reg, rscratch1);
#define IS_A_TEMP(reg) ((reg) == temp_reg || (reg) == temp2_reg)

    YuhuLabel L_fallthrough;
    int label_nulls = 0;
    if (L_success == NULL)   { L_success   = &L_fallthrough; label_nulls++; }
    if (L_failure == NULL)   { L_failure   = &L_fallthrough; label_nulls++; }
    assert(label_nulls <= 1, "at most one NULL in the batch");

    // a couple of useful fields in sub_klass:
    int ss_offset = in_bytes(Klass::secondary_supers_offset());
    int sc_offset = in_bytes(Klass::secondary_super_cache_offset());
//    Address secondary_supers_addr(sub_klass, ss_offset);
//    Address super_cache_addr(     sub_klass, sc_offset);

//    BLOCK_COMMENT("check_klass_subtype_slow_path");

    // Do a linear scan of the secondary super-klass chain.
    // This code is rarely used, so simplicity is a virtue here.
    // The repne_scan instruction uses fixed registers, which we must spill.
    // Don't worry too much about pre-existing connections with the input regs.

    assert(sub_klass != x0, "killed reg"); // killed by mov(r0, super)
    assert(sub_klass != x2, "killed reg"); // killed by lea(r2, &pst_counter)

    YuhuRegSet pushed_registers;
    if (!IS_A_TEMP(x2))    pushed_registers += x2;
    if (!IS_A_TEMP(x5))    pushed_registers += x5;

    if (super_klass != x0 || UseCompressedOops) {
        if (!IS_A_TEMP(x0))   pushed_registers += x0;
    }

    write_insts_push(pushed_registers, sp);

    // Get super_klass value into r0 (even if it was in r5 or r2).
    if (super_klass != x0) {
        write_inst_mov_reg(x0, super_klass);
    }

#ifndef PRODUCT
    write_insts_mov_imm64(x9, (uint64_t)(address)&SharedRuntime::_partial_subtype_ctr);
//    Address pst_counter_addr(rscratch2);
    write_inst("ldr x8, [x9]");
    write_inst("add x8, x8, #1");
    write_inst("str x8, [x9]");
#endif //PRODUCT

    // We will consult the secondary-super array.
    write_inst("ldr x5, [%s, #%d]", sub_klass, ss_offset);
    // Load the array length.  (Positive movl does right thing on LP64.)
    write_inst("ldr w2, [x5, #%d]", Array<Klass*>::length_offset_in_bytes());
    // Skip to start of data.
    write_inst("add x5, x5, #%d", Array<Klass*>::base_offset_in_bytes());

    write_inst("cmp sp, xzr");  // Clear Z flag; SP is never zero
    // Scan R2 words at [R5] for an occurrence of R0.
    // Set NZ/Z based on last compare.
    write_insts_repne_scan(x5, x0, x2, x8);

    // Unspill the temp. registers:
    write_insts_pop(pushed_registers, sp);

    write_inst_b(ne, *L_failure);

    // Success.  Cache the super we found and proceed in triumph.
    write_inst("str %s, [%s, #%d]", super_klass, sub_klass, sc_offset);

    if (L_success != &L_fallthrough) {
        write_inst_b(*L_success);
    }

#undef IS_A_TEMP

    pin_label(L_fallthrough);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_repne_scan(YuhuRegister addr, YuhuRegister value, YuhuRegister count,
                                YuhuRegister scratch) {
    YuhuLabel Lloop, Lexit;
    write_inst_cbz(count, Lexit);
    pin_label(Lloop);
    write_inst("ldr %s, [%s], #%d", scratch, addr, wordSize);
    write_inst_regs("cmp %s, %s", value, scratch);
    write_inst_b(eq, Lexit);
    write_inst_regs("sub %s, %s, #1", count, count);
    write_inst_cbnz(count, Lloop);
    pin_label(Lexit);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_lea(YuhuRegister reg, YuhuAddress addr) {
    addr.lea(this, reg);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_encode_heap_oop(YuhuRegister d, YuhuRegister s) {
    // TODO
//#ifdef ASSERT
//    verify_heapbase("MacroAssembler::encode_heap_oop: heap base corrupted?");
//#endif
    write_insts_verify_oop(s, "broken oop in encode_heap_oop");
    if (Universe::narrow_oop_base() == NULL) {
        if (Universe::narrow_oop_shift() != 0) {
            assert (LogMinObjAlignmentInBytes == Universe::narrow_oop_shift(), "decode alg wrong");
            write_inst("lsr %s, %s, #%d", d, s, LogMinObjAlignmentInBytes);
        } else {
            write_inst_mov_reg(d, s);
        }
    } else {
        write_inst_regs("subs %s, %s, x27", d, s);
        write_inst_csel(d, d, xzr, hs);
        write_inst("lsr %s, %s, #%d", d, d, LogMinObjAlignmentInBytes);

        /*  Old algorithm: is this any worse?
        Label nonnull;
        cbnz(r, nonnull);
        sub(r, r, rheapbase);
        bind(nonnull);
        lsr(r, r, LogMinObjAlignmentInBytes);
        */
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_store_heap_oop(YuhuAddress dst, YuhuRegister src) {
    if (UseCompressedOops) {
        assert(!dst.uses(src), "not enough registers");
        write_insts_encode_heap_oop(src);
        write_inst_str(w_reg(src), dst);
    } else
        write_inst_str(src, dst);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_store_heap_oop_null(YuhuAddress dst) {
    if (UseCompressedOops) {
        write_inst_str(wzr, dst);
    } else
        write_inst_str(xzr, dst);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_load_byte_map_base(YuhuRegister reg) {
    jbyte *byte_map_base =
            ((CardTableModRefBS*)(Universe::heap()->barrier_set()))->byte_map_base;

    if (is_valid_AArch64_address((address)byte_map_base)) {
        // Strictly speaking the byte_map_base isn't an address at all,
        // and it might even be negative.
        uint64_t offset;
        write_insts_adrp(reg, (address)byte_map_base, offset);
        // We expect offset to be zero with most collectors.
        if (offset != 0) {
            write_inst("add %s, %s, #%d", reg, reg, offset);
        }
    } else {
        write_insts_mov_imm64(reg, (uint64_t)byte_map_base);
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_store_check(YuhuRegister obj) {
    // Does a store check for the oop in register obj. The content of
    // register obj is destroyed afterwards.
    write_insts_store_check_part_1(obj);
    write_insts_store_check_part_2(obj);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_store_check(YuhuRegister obj, YuhuAddress dst) {
    return write_insts_store_check(obj);
}

address YuhuMacroAssembler::write_insts_store_check_part_1(YuhuRegister obj) {
    BarrierSet* bs = Universe::heap()->barrier_set();
    assert(bs->kind() == BarrierSet::CardTableModRef, "Wrong barrier set kind");
    write_inst("lsr %s, %s, #%d", obj, obj, CardTableModRefBS::card_shift);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_store_check_part_2(YuhuRegister obj) {
    BarrierSet* bs = Universe::heap()->barrier_set();
    assert(bs->kind() == BarrierSet::CardTableModRef, "Wrong barrier set kind");
    CardTableModRefBS* ct = (CardTableModRefBS*)bs;
    assert(sizeof(*ct->byte_map_base) == sizeof(jbyte), "adjust this code");

    // The calculation for byte_map_base is as follows:
    // byte_map_base = _byte_map - (uintptr_t(low_bound) >> card_shift);
    // So this essentially converts an address to a displacement and
    // it will never need to be relocated.

    // FIXME: It's not likely that disp will fit into an offset so we
    // don't bother to check, but it could save an instruction.
    intptr_t disp = (intptr_t) ct->byte_map_base;
    write_insts_load_byte_map_base(x8);

    if (UseConcMarkSweepGC && CMSPrecleaningEnabled) {
        write_inst("dmb ishst");
    }
    write_inst_regs("strb wzr, [%s, %s]", obj, x8);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_corrected_idivl(YuhuRegister result, YuhuRegister ra, YuhuRegister rb,
                                    bool want_remainder, YuhuRegister scratch)
{
    // Full implementation of Java idiv and irem.  The function
    // returns the (pc) offset of the div instruction - may be needed
    // for implicit exceptions.
    //
    // constraint : ra/rb =/= scratch
    //         normal case
    //
    // input : ra: dividend
    //         rb: divisor
    //
    // result: either
    //         quotient  (= ra idiv rb)
    //         remainder (= ra irem rb)

    assert(ra != scratch && rb != scratch, "reg cannot be scratch");

//    int idivl_offset = offset();
    if (! want_remainder) {
        write_inst_regs("sdiv %s, %s, %s", w_reg(result), w_reg(ra), w_reg(rb));
    } else {
        write_inst_regs("sdiv %s, %s, %s", w_reg(scratch), w_reg(ra), w_reg(rb));
        write_inst_regs("msub %s, %s, %s, %s", w_reg(result), w_reg(scratch), w_reg(rb), w_reg(ra));
    }

    return current_pc();
}

address YuhuMacroAssembler::write_insts_corrected_idivq(YuhuRegister result, YuhuRegister ra, YuhuRegister rb,
                                    bool want_remainder, YuhuRegister scratch)
{
    // Full implementation of Java ldiv and lrem.  The function
    // returns the (pc) offset of the div instruction - may be needed
    // for implicit exceptions.
    //
    // constraint : ra/rb =/= scratch
    //         normal case
    //
    // input : ra: dividend
    //         rb: divisor
    //
    // result: either
    //         quotient  (= ra idiv rb)
    //         remainder (= ra irem rb)

    assert(ra != scratch && rb != scratch, "reg cannot be scratch");

//    int idivq_offset = offset();
    if (! want_remainder) {
        write_inst_regs("sdiv %s, %s, %s", result, ra, rb);
    } else {
        write_inst_regs("sdiv %s, %s, %s", scratch, ra, rb);
        write_inst_regs("msub %s, %s, %s, %s", result, scratch, rb, ra);
    }

    return current_pc();
}

address YuhuMacroAssembler::write_insts_narrow(YuhuRegister result) {

    // Get method->_constMethod->_result_type
    write_inst_ldr(x8, YuhuAddress(x29, frame::interpreter_frame_method_offset * wordSize));
    write_inst_ldr(x8, YuhuAddress(x8, Method::const_offset()));
    write_inst_ldrb(w8, YuhuAddress(x8, ConstMethod::result_type_offset()));

    YuhuLabel done, notBool, notByte, notChar;

    // common case first
    write_inst("cmp w8, #%d", T_INT);
    write_inst_b(eq, done);

    // mask integer result to narrower return type.
    write_inst("cmp w8, #%d", T_BOOLEAN);
    write_inst_b(ne, notBool);
    write_inst("and %s, %s, #%d", w_reg(result), w_reg(result), 0x1);
    write_inst_b(done);

    pin_label(notBool);
    write_inst("cmp w8, #%d", T_BYTE);
    write_inst_b(ne, notByte);
    write_inst_imms("sbfx %s, %s, #%d, #%d", result, result, 0, 8);
    write_inst_b(done);

    pin_label(notByte);
    write_inst("cmp w8, #%d", T_CHAR);
    write_inst_b(ne, notChar);
    write_inst_imms("ubfx %s, %s, #%d, #%d", result, result, 0, 16); // truncate upper 16 bits
    write_inst_b(done);

    pin_label(notChar);
    write_inst_imms("sbfx %s, %s, #%d, #%d", result, result, 0, 16); // sign-extend short

    // Nothing to do for T_INT
    pin_label(done);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_load_resolved_reference_at_index(
        YuhuRegister result, YuhuRegister index) {
//    assert_different_registers(result, index);
    // convert from field index to resolved_references() index and from
    // word index to byte offset. Since this is a java object, it can be compressed
    YuhuRegister tmp = index;  // reuse
    write_inst("lsl %s, %s, #%d", w_reg(tmp), w_reg(tmp), LogBytesPerHeapOop);

    write_insts_get_constant_pool(result);
    // load pointer for resolved_references[] objArray
    write_inst_ldr(result, YuhuAddress(result, ConstantPool::resolved_references_offset_in_bytes()));
    // JNIHandles::resolve(obj);
    write_inst_ldr(result, YuhuAddress(result, 0));
    // Add in the index
    write_inst_regs("add %s, %s, %s", result, result, tmp);
    write_insts_load_heap_oop(result, YuhuAddress(result, arrayOopDesc::base_offset_in_bytes(T_OBJECT)));
    return current_pc();
}

address YuhuMacroAssembler::write_insts_prepare_to_jump_from_interpreted() {
    // set sender sp
    write_inst("mov x13, sp");
    // record last_sp
    write_inst_str(x20, YuhuAddress(x29, frame::interpreter_frame_last_sp_offset * wordSize));
    return current_pc();
}

address YuhuMacroAssembler::write_insts_jump_from_interpreted(YuhuRegister method, YuhuRegister temp) {
    write_insts_prepare_to_jump_from_interpreted();

    if (JvmtiExport::can_post_interpreter_events()) {
        YuhuLabel run_compiled_code;
        // JVMTI events, such as single-stepping, are implemented partly by avoiding running
        // compiled code in threads for which the event is enabled.  Check here for
        // interp_only_mode if these events CAN be enabled.
        // interp_only is an int, on little endian it is sufficient to test the byte only
        // Is a cmpl faster?
        write_inst_ldr(x8, YuhuAddress(x28, JavaThread::interp_only_mode_offset()));
        write_inst_cbz(x8, run_compiled_code);
        write_inst_ldr(x8, YuhuAddress(method, Method::interpreter_entry_offset()));
        write_inst_br(x8);
        pin_label(run_compiled_code);
    }

    write_inst_ldr(x8, YuhuAddress(method, Method::from_interpreted_offset()));
    write_inst_br(x8);
    return current_pc();
}

YuhuAddress YuhuMacroAssembler::form_address(YuhuRegister Rd, YuhuRegister base, int64_t byte_offset, int shift) {
    // TODO
//    if (Address::offset_ok_for_immed(byte_offset, shift))
//        // It fits; no need for any heroics
//        return Address(base, byte_offset);
//
//    // Don't do anything clever with negative or misaligned offsets
//    unsigned mask = (1 << shift) - 1;
//    if (byte_offset < 0 || byte_offset & mask) {
//        mov(Rd, byte_offset);
//        add(Rd, base, Rd);
//        return Address(Rd);
//    }
//
//    // See if we can do this with two 12-bit offsets
//    {
//        uint64_t word_offset = byte_offset >> shift;
//        uint64_t masked_offset = word_offset & 0xfff000;
//        if (Address::offset_ok_for_immed(word_offset - masked_offset, 0)
//            && Assembler::operand_valid_for_add_sub_immediate(masked_offset << shift)) {
//            add(Rd, base, masked_offset << shift);
//            word_offset -= masked_offset;
//            return Address(Rd, word_offset << shift);
//        }
//    }

    // Do it the hard way
    write_insts_mov_imm64(Rd, byte_offset);
    write_inst_regs("add %s, %s, %s", Rd, base, Rd);
    return YuhuAddress(Rd);
}

address YuhuMacroAssembler::write_insts_lookup_virtual_method(YuhuRegister recv_klass,
                                                              YuhuRegisterOrConstant vtable_index,
                                                              YuhuRegister method_result) {
    const int base = InstanceKlass::vtable_start_offset() * wordSize;
    assert(vtableEntry::size() * wordSize == 8,
           "adjust the scaling in the code below");
    int vtable_offset_in_bytes = base + vtableEntry::method_offset_in_bytes();

    if (vtable_index.is_register()) {
        write_insts_lea(method_result,
                        YuhuAddress(recv_klass, vtable_index.as_register(), YuhuAddress::lsl(LogBytesPerWord)));
        write_inst_ldr(method_result, YuhuAddress(method_result, vtable_offset_in_bytes));
    } else {
        vtable_offset_in_bytes += vtable_index.as_constant() * wordSize;
        write_inst_ldr(method_result, form_address(x8, recv_klass, vtable_offset_in_bytes, 0));
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_lookup_interface_method(YuhuRegister recv_klass,
                                                                YuhuRegister intf_klass,
                                                                YuhuRegisterOrConstant itable_index,
                                                                YuhuRegister method_result,
                                                                YuhuRegister scan_temp,
                                                                YuhuLabel& L_no_such_interface,
                                                                bool return_method) {
//    assert_different_registers(recv_klass, intf_klass, scan_temp);
//    assert_different_registers(method_result, intf_klass, scan_temp);
    assert(recv_klass != method_result || !return_method,
           "recv_klass can be destroyed when method isn't needed");

    assert(itable_index.is_constant() || itable_index.as_register() == method_result,
           "caller must use same register for non-constant itable index as for method");

    // Compute start of first itableOffsetEntry (which is at the end of the vtable)
    int vtable_base = InstanceKlass::vtable_start_offset() * wordSize;
    int itentry_off = itableMethodEntry::method_offset_in_bytes();
    int scan_step   = itableOffsetEntry::size() * wordSize;
    int vte_size    = vtableEntry::size() * wordSize;
    assert(vte_size == wordSize, "else adjust times_vte_scale");

    write_inst_ldr(w_reg(scan_temp), YuhuAddress(recv_klass, InstanceKlass::vtable_length_offset() * wordSize));

    // %%% Could store the aligned, prescaled offset in the klassoop.
    // lea(scan_temp, Address(recv_klass, scan_temp, times_vte_scale, vtable_base));
    write_insts_lea(scan_temp, YuhuAddress(recv_klass, scan_temp, YuhuAddress::lsl(3)));
    write_inst("add %s, %s, #%d", scan_temp, scan_temp, vtable_base);
    if (HeapWordsPerLong > 1) {
        // Round up to align_object_offset boundary
        // see code for instanceKlass::start_of_itable!
        // TODO
//        round_to(scan_temp, BytesPerLong);
    }

    if (return_method) {
        // Adjust recv_klass by scaled itable_index, so we can free itable_index.
        assert(itableMethodEntry::size() * wordSize == wordSize, "adjust the scaling in the code below");
        // lea(recv_klass, Address(recv_klass, itable_index, Address::times_ptr, itentry_off));
        write_insts_lea(recv_klass,
                        YuhuAddress(recv_klass, itable_index, YuhuAddress::lsl(3)));
        if (itentry_off)
            write_inst("add %s, %s, #%d", recv_klass, recv_klass, itentry_off);
    }

    // for (scan = klass->itable(); scan->interface() != NULL; scan += scan_step) {
    //   if (scan->interface() == intf) {
    //     result = (klass + scan->offset() + itable_index);
    //   }
    // }
    YuhuLabel search, found_method;

    for (int peel = 1; peel >= 0; peel--) {
        write_inst_ldr(method_result, YuhuAddress(scan_temp, itableOffsetEntry::interface_offset_in_bytes()));
        write_inst_regs("cmp %s, %s", intf_klass, method_result);

        if (peel) {
            write_inst_b(eq, found_method);
        } else {
            write_inst_b(ne, search);
            // (invert the test to fall through to found_method...)
        }

        if (!peel)  break;

        pin_label(search);

        // Check that the previous entry is non-null.  A null entry means that
        // the receiver class doesn't implement the interface, and wasn't the
        // same as when the caller was compiled.
        write_inst_cbz(method_result, L_no_such_interface);
        write_inst("add %s, %s, #%d", scan_temp, scan_temp, scan_step);
    }

    pin_label(found_method);

    if (return_method) {
        // Got a hit.
        write_inst_ldr(w_reg(scan_temp), YuhuAddress(scan_temp, itableOffsetEntry::offset_offset_in_bytes()));
        write_inst_ldr(method_result, YuhuAddress(recv_klass, w_reg(scan_temp), YuhuAddress::uxtw(0)));
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_tlab_allocate(YuhuRegister obj,
                                                      YuhuRegister var_size_in_bytes,
                                                      int con_size_in_bytes,
                                                      YuhuRegister t1,
                                                      YuhuRegister t2,
                                                      YuhuLabel& slow_case) {
//    assert_different_registers(obj, t2);
//    assert_different_registers(obj, var_size_in_bytes);
    YuhuRegister end = t2;

    // verify_tlab();

    write_inst_ldr(obj, YuhuAddress(x28, JavaThread::tlab_top_offset()));
    if (var_size_in_bytes == noreg) {
        write_insts_lea(end, YuhuAddress(obj, con_size_in_bytes));
    } else {
        write_insts_lea(end, YuhuAddress(obj, var_size_in_bytes));
    }
    write_inst_ldr(x8, YuhuAddress(x28, JavaThread::tlab_end_offset()));
    write_inst_regs("cmp %s, %s", end, x8);
    write_inst_b(hi, slow_case);

    // update the tlab top pointer
    write_inst_str(end, YuhuAddress(x28, JavaThread::tlab_top_offset()));

    // recover var_size_in_bytes if necessary
    if (var_size_in_bytes == end) {
        write_inst_regs("sub %s, %s, %s", var_size_in_bytes, var_size_in_bytes, obj);
    }
    // verify_tlab();
    return current_pc();
}

address YuhuMacroAssembler::write_insts_eden_allocate(YuhuRegister obj,
                                                      YuhuRegister var_size_in_bytes,
                                                      int con_size_in_bytes,
                                                      YuhuRegister t1,
                                                      YuhuLabel& slow_case) {
//    assert_different_registers(obj, var_size_in_bytes, t1);
    if (CMSIncrementalMode || !Universe::heap()->supports_inline_contig_alloc()) {
        write_inst_b(slow_case);
    } else {
        YuhuRegister end = t1;
        YuhuRegister heap_end = x9;
        YuhuLabel retry;
        pin_label(retry);
        {
            uint64_t offset;
            write_insts_adrp(x8, (address) Universe::heap()->end_addr(), offset);
            write_inst_ldr(heap_end, YuhuAddress(x8, offset));
        }

        ExternalAddress heap_top((address) Universe::heap()->top_addr());

        // Get the current top of the heap
        {
            uint64_t offset;
            write_insts_adrp(x8, (address) Universe::heap()->top_addr(), offset);
            // Use add() here after ARDP, rather than lea().
            // lea() does not generate anything if its offset is zero.
            // However, relocs expect to find either an ADD or a load/store
            // insn after an ADRP.  add() always generates an ADD insn, even
            // for add(Rn, Rn, 0).
            write_inst("add x8, x8, #%d", offset);
            write_inst_regs("ldaxr %s, [%s]", obj, x8);
        }

        // Adjust it my the size of our new object
        if (var_size_in_bytes == noreg) {
            write_insts_lea(end, YuhuAddress(obj, con_size_in_bytes));
        } else {
            write_insts_lea(end, YuhuAddress(obj, var_size_in_bytes));
        }

        // if end < obj then we wrapped around high memory
        write_inst_regs("cmp %s, %s", end, obj);
        write_inst_b(lo, slow_case);

        write_inst_regs("cmp %s, %s", end, heap_end);
        write_inst_b(hi, slow_case);

        // If heap_top hasn't been changed by some other thread, update it.
        write_inst_regs("stlxr w9, %s, [x8]", end);
        write_inst_cbnz(w9, retry);
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_incr_allocated_bytes(YuhuRegister thread,
                                                             YuhuRegister var_size_in_bytes,
                                                             int con_size_in_bytes,
                                                             YuhuRegister t1) {
    if (thread == noreg) {
        thread = x28;
    }
    assert(t1 != noreg, "need temp reg");

    write_inst_ldr(t1, YuhuAddress(thread, in_bytes(JavaThread::allocated_bytes_offset())));
    if (var_size_in_bytes != noreg) {
        write_inst_regs("add %s, %s, %s", t1, t1, var_size_in_bytes);
    } else {
        write_inst("add %s, %s, #%d", t1, t1, con_size_in_bytes);
    }
    write_inst_str(t1, YuhuAddress(thread, in_bytes(JavaThread::allocated_bytes_offset())));
    return current_pc();
}

address YuhuMacroAssembler::write_insts_store_klass_gap(YuhuRegister dst, YuhuRegister src) {
    if (UseCompressedClassPointers) {
        // Store to klass gap in destination
        write_inst_str(w_reg(src), YuhuAddress(dst, oopDesc::klass_gap_offset_in_bytes()));
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_store_klass(YuhuRegister dst, YuhuRegister src) {
    // FIXME: Should this be a store release?  concurrent gcs assumes
    // klass length is valid if klass field is not null.
    if (UseCompressedClassPointers) {
        write_insts_encode_klass_not_null(src);
        write_inst_str(w_reg(src), YuhuAddress(dst, oopDesc::klass_offset_in_bytes()));
    } else {
        write_inst_str(src, YuhuAddress(dst, oopDesc::klass_offset_in_bytes()));
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_encode_klass_not_null(YuhuRegister r) {
    return write_insts_encode_klass_not_null(r, r);
}

address YuhuMacroAssembler::write_insts_encode_klass_not_null(YuhuRegister dst, YuhuRegister src) {
    if (Universe::narrow_klass_base() == NULL) {
        if (Universe::narrow_klass_shift() != 0) {
            assert (LogKlassAlignmentInBytes == Universe::narrow_klass_shift(), "decode alg wrong");
            write_inst("lsr %s, %s, #%d", dst, src, LogKlassAlignmentInBytes);
        } else {
            if (dst != src) write_inst_mov_reg(dst, src);
        }
        return current_pc();
    }

    if (use_XOR_for_compressed_class_base) {
        if (Universe::narrow_klass_shift() != 0) {
            write_inst("eor %s, %s, #%d", dst, src, (uint64_t)Universe::narrow_klass_base());
            write_inst("lsr %s, %s, #%d", dst, dst, LogKlassAlignmentInBytes);
        } else {
            write_inst("eor %s, %s, #%d", dst, src, (uint64_t)Universe::narrow_klass_base());
        }
        return current_pc();
    }

    if (((uint64_t)Universe::narrow_klass_base() & 0xffffffff) == 0
        && Universe::narrow_klass_shift() == 0) {
        write_inst_mov_reg(w_reg(dst), w_reg(src));
        return current_pc();
    }

#ifdef ASSERT
    // TODO
//    verify_heapbase("MacroAssembler::encode_klass_not_null2: heap base corrupted?");
#endif

    YuhuRegister rbase = dst;
    if (dst == src) rbase = x27;
    write_insts_mov_imm64(rbase, (uint64_t)Universe::narrow_klass_base());
    write_inst_regs("sub %s, %s, %s", dst, src, rbase);
    if (Universe::narrow_klass_shift() != 0) {
        assert (LogKlassAlignmentInBytes == Universe::narrow_klass_shift(), "decode alg wrong");
        write_inst("lsr, %s, %s, #%d", dst, dst, LogKlassAlignmentInBytes);
    }
    if (dst == src) write_insts_reinit_heapbase();
    return current_pc();
}

address YuhuMacroAssembler::write_insts_reinit_heapbase()
{
    if (UseCompressedOops) {
        if (Universe::is_fully_initialized()) {
            write_insts_mov_imm64(x27, (uint64_t)Universe::narrow_ptrs_base());
        } else {
            write_insts_lea(x27, (address)Universe::narrow_ptrs_base_addr());
            write_inst_ldr(x27, YuhuAddress(x27));
        }
    }
    return current_pc();
}

address YuhuMacroAssembler::write_insts_decrement(YuhuRegister reg, int value)
{
    if (value < 0)  { write_insts_increment(reg, -value);      return current_pc(); }
    if (value == 0) {                              return current_pc(); }
    if (value < (1 << 12)) { write_inst("sub %s, %s, #%d", reg, reg, value); return current_pc(); }
    /* else */ {
        assert(reg != x9, "invalid dst for register decrement");
        write_insts_mov_imm64(x9, (uint64_t)value);
        write_inst_regs("sub %s, %s, %s", reg, reg, x9);
        return current_pc();
    }
}

address YuhuMacroAssembler::write_insts_decrement(YuhuAddress dst, int value)
{
    assert(!dst.uses(x8), "invalid address for decrement");
    write_inst_ldr(x8, dst);
    write_insts_decrement(x8, value);
    write_inst_str(x8, dst);
    return current_pc();
}


address YuhuMacroAssembler::write_insts_increment(YuhuRegister reg, int value)
{
    if (value < 0)  { write_insts_decrement(reg, -value);      return current_pc(); }
    if (value == 0) {                              return current_pc(); }
    if (value < (1 << 12)) { write_inst("add %s, %s, #%d", reg, reg, value); return current_pc(); }
    /* else */ {
        assert(reg != x9, "invalid dst for register increment");
        write_insts_mov_imm32(w9, (unsigned)value);
        write_inst_regs("add %s, %s, %s", reg, reg, x9);
        return current_pc();
    }
}

address YuhuMacroAssembler::write_insts_increment(YuhuAddress dst, int value)
{
    assert(!dst.uses(x8), "invalid dst for address increment");
    write_inst_ldr(x8, dst);
    write_insts_increment(x8, value);
    write_inst_str(x8, dst);
    return current_pc();
}

address YuhuMacroAssembler::write_insts_generate_stack_overflow_check( int frame_size_in_bytes) {
    if (UseStackBanging) {
        // Each code entry causes one stack bang n pages down the stack where n
        // is configurable by StackShadowPages.  The setting depends on the maximum
        // depth of VM call stack or native before going back into java code,
        // since only java code can raise a stack overflow exception using the
        // stack banging mechanism.  The VM and native code does not detect stack
        // overflow.
        // The code in JavaCalls::call() checks that there is at least n pages
        // available, so all entry code needs to do is bang once for the end of
        // this shadow zone.
        // The entry code may need to bang additional pages if the framesize
        // is greater than a page.

        const int page_size = os::vm_page_size();
        int bang_end = StackShadowPages*page_size;

        // This is how far the previous frame's stack banging extended.
        const int bang_end_safe = bang_end;

        if (frame_size_in_bytes > page_size) {
            bang_end += frame_size_in_bytes;
        }

        int bang_offset = bang_end_safe;
        while (bang_offset <= bang_end) {
            // Need at least one stack bang at end of shadow zone.
            // stack grows down, caller passes positive offset
            int offset = bang_offset;

            assert(offset > 0, "must bang with negative offset");
            write_insts_mov_imm64(x9, -offset);
            write_inst_str(xzr, YuhuAddress(sp, x9));

            bang_offset += page_size;
        }
    } // end (UseStackBanging)
    return current_pc();
}

#define __ _masm->

YuhuSkipIfEqual::YuhuSkipIfEqual(
        YuhuMacroAssembler* masm, const bool* flag_addr, bool value) {
    _masm = masm;
    uint64_t offset;
    __ write_insts_adrp(__ x8, (address)flag_addr, offset);
    __ write_inst_ldrb(__ w8, YuhuAddress(__ x8, offset));
    __ write_inst_cbz(__ w8, _label);
}

#undef __

YuhuSkipIfEqual::~YuhuSkipIfEqual() {
    _masm->pin_label(_label);
}

#define __ as->

YuhuAddress::YuhuAddress(address target, relocInfo::relocType rtype) : _mode(literal){
    _is_lval = false;
    _target = target;
    switch (rtype) {
        case relocInfo::oop_type:
        case relocInfo::metadata_type:
            // Oops are a special case. Normally they would be their own section
            // but in cases like icBuffer they are literals in the code stream that
            // we don't have a section for. We use none so that we get a literal address
            // which is always patchable.
            break;
        case relocInfo::external_word_type:
            _rspec = external_word_Relocation::spec(target);
            break;
        case relocInfo::internal_word_type:
            _rspec = internal_word_Relocation::spec(target);
            break;
        case relocInfo::opt_virtual_call_type:
            _rspec = opt_virtual_call_Relocation::spec();
            break;
        case relocInfo::static_call_type:
            _rspec = static_call_Relocation::spec();
            break;
        case relocInfo::runtime_call_type:
            _rspec = runtime_call_Relocation::spec();
            break;
        case relocInfo::poll_type:
        case relocInfo::poll_return_type:
            _rspec = Relocation::spec_simple(rtype);
            break;
        case relocInfo::none:
            _rspec = RelocationHolder::none;
            break;
        default:
            ShouldNotReachHere();
            break;
    }
}

void YuhuAddress::lea(YuhuMacroAssembler *as, YuhuMacroAssembler::YuhuRegister r) const {
    // TODO
//    Relocation* reloc = _rspec.reloc();
//    relocInfo::relocType rtype = (relocInfo::relocType) reloc->type();

    switch(_mode) {
        case base_plus_offset: {
            if (_offset == 0 && _base == r) // it's a nop
                break;
            if (_offset > 0)
                __ write_inst("add %s, %s, #%d", r, _base, _offset);
            else
                __ write_inst("sub %s, %s, #%d", r, _base, -_offset);
            break;
        }
        case base_plus_offset_reg: {
            __ write_inst_add(r, _base, _index, _ext.op(), MAX(_ext.shift(), 0));
            break;
        }
        case literal: {
            __ write_insts_mov_imm64(r, (uint64_t)target());
            break;
        }
        default:
            ShouldNotReachHere();
    }
}

#undef __
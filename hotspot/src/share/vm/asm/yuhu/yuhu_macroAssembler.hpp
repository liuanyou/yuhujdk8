//
// Created by Anyou Liu on 2025/4/28.
//

#ifndef JDK8_YUHU_MACROASSEMBLER_HPP
#define JDK8_YUHU_MACROASSEMBLER_HPP

#include "asm/macroAssembler.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "interpreter/invocationCounter.hpp"
#include "runtime/frame.hpp"
#include "asm/codeBuffer.hpp"
#include "code/oopRecorder.hpp"
#include "code/relocInfo.hpp"
#include "memory/allocation.hpp"
#include "utilities/debug.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/top.hpp"
#include <keystone/keystone.h>

class YuhuLabel;
class YuhuRegSet;
class YuhuAddress;

class YuhuMacroAssembler: public MacroAssembler {
private:
    ks_engine *ks;
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
    // compute target according label and given address
    address target(YuhuLabel& L, address branch_pc);
public:
    enum YuhuRegister {
        w0, w1, w2, w3, w4, w5, w6, w7,
        w8,
        w9, w10, w11, w12, w13, w14, w15,
        w16, w17, w18,
        w19, w20, w21, w22, w23, w24, w25, w26, w27, w28,
        w29, w30, wzr,
        x0, x1, x2, x3, x4, x5, x6, x7,
        x8,
        x9, x10, x11, x12, x13, x14, x15, // Caller saved
        x16, x17, x18,
        x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, // Callee saved
        x29, fp = x29, x30, lr = x30, xzr, sp, noreg
    };
    enum YuhuFloatRegister {
        s0, s1, s2, s3, s4, s5, s6, s7,
        s8,
        s9, s10, s11, s12, s13, s14, s15,
        s16, s17, s18,
        s19, s20, s21, s22, s23, s24, s25, s26, s27, s28,
        s29, s30, s31,
        d0, d1, d2, d3, d4, d5, d6, d7,
        d8,
        d9, d10, d11, d12, d13, d14, d15, // Caller saved
        d16, d17, d18,
        d19, d20, d21, d22, d23, d24, d25, d26, d27, d28, // Callee saved
        d29, d30, d31, fnoreg
    };
    // condition kind for b.cond instruction
    enum YuhuCond {
        gt, ne, al, ls, hi, le, eq, hs, lo
    };
    enum YuhuOperation {
        lsl, uxtb, uxth, uxtw, uxtx, sxtb, sxth, sxtw, sxtx
    };
private:
    const char* reg_name(YuhuRegister reg) {
        static const char* register_names[] = {
            // w0-w30
            "w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7",
            "w8", "w9", "w10", "w11", "w12", "w13", "w14", "w15",
            "w16", "w17", "w18", "w19", "w20", "w21", "w22", "w23",
            "w24", "w25", "w26", "w27", "w28", "w29", "w30",
            // wzr
            "wzr",

            // x0-x30
            "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
            "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
            "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
            "x24", "x25", "x26", "x27", "x28", "x29", "x30",

            // xzr, sp
            "xzr", "sp"
        };

        // check array index
        size_t count = sizeof(register_names)/sizeof(register_names[0]);
        assert((int)reg >= 0 && (int)reg < (int)count, "Unknown register");
        return register_names[(int)reg];
    }

    const char* cond_name(YuhuCond cond) {
        static const char* cond_names[] = {
                "gt", "ne", "al", "ls", "hi", "le", "eq", "hs", "lo"
        };

        // check array index
        size_t count = sizeof(cond_names)/sizeof(cond_names[0]);
        assert((int)cond >=0 && (int)cond < (int)count, "Unknown cond");
        return cond_names[(int)cond];
    }

    const char* f_reg_name(YuhuFloatRegister reg) {
        static const char* register_names[] = {
                // s0-s31
                "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
                "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15",
                "s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
                "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31",
                // d0-d31
                "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
                "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15",
                "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23",
                "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31"
        };

        // check array index
        size_t count = sizeof(register_names)/sizeof(register_names[0]);
        assert((int)reg >= 0 && (int)reg < (int)count, "Unknown register");
        return register_names[(int)reg];
    }

    const char* op_name(YuhuOperation op) {
        static const char* op_names[] = {
                "ls", "uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx"
        };

        // check array index
        size_t count = sizeof(op_names)/sizeof(op_names[0]);
        assert((int)op >=0 && (int)op < (int)count, "Unknown op");
        return op_names[(int)op];
    }
public:
    YuhuRegister w_reg(YuhuRegister reg) {
        assert(reg >= x0 && reg <= xzr, "Unknown register");
        return (YuhuRegister) (reg - (xzr - wzr));
    }
    static uint64_t bit(YuhuRegister reg, bool should_set = true) {
        assert(reg >= x0 && reg <= x30, "Unknown register");
        return should_set ? 1 << encoding(reg) : 0;
    }
    static int encoding(YuhuRegister reg) {
        assert(reg >= x0 && reg <= x30, "Unknown register");
        return (int) (reg - x0);
    }
public:
    YuhuMacroAssembler(CodeBuffer* code) : MacroAssembler(code) {
        ks_err err = ks_open(KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN, &ks);
        assert(err == KS_ERR_OK, "Failed to initialize Keystone!");
    }

    ~YuhuMacroAssembler() {
        if (ks != nullptr) {
            ks_close(ks);
            ks = nullptr;
        }
    }

    address write_inst(uint32_t value);

    address current_pc();

    address write_inst(const char* assembly);

    address write_inst(const char* assembly_format, YuhuRegister reg, int imm32);

    address write_inst(const char* assembly_format, YuhuFloatRegister reg, int imm32);

    address write_inst(const char* assembly_format, YuhuRegister reg1, YuhuRegister reg2, int imm32);

    address write_inst(const char* assembly_format, YuhuRegister reg1, YuhuRegister reg2, YuhuRegister reg3, int imm32);

    address write_inst(const char* assembly_format, int imm32);

    address write_inst(const char* assembly_format, YuhuRegister reg, YuhuAddress addr);

    address write_inst(const char* assembly_format, YuhuFloatRegister reg, YuhuAddress addr);

    address write_inst_regs(const char* assembly_format, YuhuRegister reg1);

    address write_inst_regs(const char* assembly_format, YuhuRegister reg1, YuhuRegister reg2);

    address write_inst_regs(const char* assembly_format, YuhuRegister reg1, YuhuRegister reg2, YuhuRegister reg3);

    address write_inst_regs(const char* assembly_format, YuhuRegister reg1, YuhuRegister reg2, YuhuRegister reg3, YuhuRegister reg4);

    address write_inst_imms(const char* assembly_format, YuhuRegister reg1, YuhuRegister reg2, int imm1, int imm2);

    address write_inst_add(YuhuRegister reg, YuhuRegister base, YuhuRegister index, YuhuOperation op, int shift);

    address write_inst_str(YuhuRegister reg, YuhuAddress addr);

    address write_inst_str(YuhuFloatRegister reg, YuhuAddress addr);

    address write_inst_ldr(YuhuRegister reg, YuhuAddress addr);

    address write_inst_ldr(YuhuFloatRegister reg, YuhuAddress addr);

    address write_inst_ldrh(YuhuRegister reg, YuhuAddress addr);

    address write_inst_ldrb(YuhuRegister reg, YuhuAddress addr);

    address write_inst_ldrsb(YuhuRegister reg, YuhuAddress addr);

    address write_inst_ldrsh(YuhuRegister reg, YuhuAddress addr);

    address write_inst_mov_reg(YuhuRegister reg1, YuhuRegister reg2);

    address write_inst_fmov_reg(YuhuFloatRegister reg1, YuhuRegister reg2);

    address write_inst_br(YuhuRegister reg);

    address write_inst_blr(YuhuRegister reg);

    address write_inst_b(address target);

    address write_inst_b(YuhuLabel& label);

    address write_inst_b(YuhuCond cond, address target);

    address write_inst_b(YuhuCond cond, YuhuLabel& label);

    address write_inst_cbz(YuhuRegister reg, address target);

    address write_inst_cbz(YuhuRegister reg, YuhuLabel& label);

    address write_inst_cbnz(YuhuRegister reg, address target);

    address write_inst_cbnz(YuhuRegister reg, YuhuLabel& label);

    address write_inst_tbz(YuhuRegister reg, int bitpos, address target);

    address write_inst_tbz(YuhuRegister reg, int bitpos, YuhuLabel &label);

    address write_inst_tbnz(YuhuRegister reg, int bitpos, address target);

    address write_inst_tbnz(YuhuRegister reg, int bitpos, YuhuLabel &label);

    address write_inst_adrp(YuhuRegister reg, address target);

    address write_inst_adr(YuhuRegister reg, address target);

    address write_inst_adr(YuhuRegister reg, YuhuLabel& label);

    void pin_label(YuhuLabel& label);

    address write_inst_pop_ptr(YuhuRegister r = x0);
    address write_inst_pop_i(YuhuRegister r = x0);
    address write_inst_pop_l(YuhuRegister r = x0);
    address write_inst_pop_f(YuhuFloatRegister r = s0);
    address write_inst_pop_d(YuhuFloatRegister r = d0);
    address write_inst_pop(YuhuRegister dst);
    address write_inst_push_ptr(YuhuRegister r = x0);
    address write_inst_push_i(YuhuRegister r = x0);
    address write_inst_push_l(YuhuRegister r = x0);
    address write_inst_push_f(YuhuFloatRegister r = s0);
    address write_inst_push_d(YuhuFloatRegister r = d0);
    address write_inst_push(YuhuRegister src);

    address write_inst_cset(YuhuRegister reg, YuhuCond cond);

    address write_inst_csel(YuhuRegister reg1, YuhuRegister reg2, YuhuRegister reg3, YuhuCond cond);

    address write_inst_get_fpsr(YuhuRegister reg) {
        return write_inst_regs("mrs %s, fpsr", reg);
    }

    address write_inst_clear_fpsr() {
        return write_inst("msr fpsr, xzr");
    }

    address write_insts_load_unsigned_short(YuhuRegister dst, YuhuAddress src);
    address write_insts_load_unsigned_byte(YuhuRegister dst, YuhuAddress src);
    address write_insts_get_unsigned_2_byte_index_at_bcp(YuhuRegister reg, int bcp_offset);

    address write_insts_load_signed_byte(YuhuRegister dst, YuhuAddress src);
    address write_insts_load_signed_short(YuhuRegister dst, YuhuAddress src);

    address write_insts_load_signed_byte32(YuhuRegister dst, YuhuAddress src);
    address write_insts_load_signed_short32(YuhuRegister dst, YuhuAddress src);

    address write_insts_enter();

    address write_insts_leave();

    address write_insts_stop(const char* msg);

    /**
     * push all registers into sp
     *
     * @return
     */
    address write_insts_pusha();

    address write_insts_popa();

    address write_insts_mov_ptr(YuhuRegister reg, uintptr_t imm64);

    address write_insts_mov_imm64(YuhuRegister reg, uint64_t imm64);

    address write_insts_mov_imm32(YuhuRegister reg, uint32_t imm32);

    address write_insts_dispatch_next(TosState state, int step = 0);

    address write_insts_dispatch_via(TosState state, address* table);

    address write_insts_dispatch_base(TosState state, address* table, bool verifyoop = true);

    address write_insts_get_dispatch();

    address write_insts_far_jump(address entry, CodeBuffer *cbuf = NULL, YuhuRegister tmp = x8);

    address write_insts_adrp(YuhuRegister reg, const address &dest, uint64_t &byte_offset);

    address write_insts_set_last_java_frame(YuhuRegister last_java_sp, YuhuRegister last_java_fp, address  last_java_pc, YuhuRegister scratch);

    address write_insts_set_last_java_frame(YuhuRegister last_java_sp, YuhuRegister last_java_fp, YuhuLabel &last_java_pc, YuhuRegister scratch);

    address write_insts_set_last_java_frame(YuhuRegister last_java_sp, YuhuRegister last_java_fp, YuhuRegister last_java_pc, YuhuRegister scratch);

    address write_insts_reset_last_java_frame(bool clear_fp);

    address write_insts_call_VM_base(YuhuRegister oop_result, YuhuRegister java_thread, YuhuRegister last_java_sp, address  entry_point, int number_of_arguments, bool check_exceptions);

    address write_insts_call_VM_leaf_base(address entry_point, int number_of_arguments, YuhuLabel *retaddr = NULL);

    address write_insts_verify_oop(YuhuRegister reg, const char* s = "broken oop");

    address write_insts_verify_oop(YuhuRegister reg, TosState state = atos);

    address write_insts_load_klass(YuhuRegister dst, YuhuRegister src);

    address write_insts_lock_object(YuhuRegister lock_reg);

    address write_insts_unlock_object(YuhuRegister lock_reg);

    address write_insts_save_bcp() {
        return write_inst("str x22, [x29, #%d]", frame::interpreter_frame_bcx_offset * wordSize);
    }

    address write_insts_restore_bcp() {
        return write_inst("ldr x22, [x29, #%d]", frame::interpreter_frame_bcx_offset * wordSize);
    }

    address write_insts_restore_locals() {
        return write_inst("ldr x24, [x29, #%d]", frame::interpreter_frame_locals_offset * wordSize);
    }

    address write_insts_final_call_VM(YuhuRegister oop_result, address entry_point, bool check_exceptions = true);

    address write_insts_final_call_VM(YuhuRegister oop_result, address entry_point, YuhuRegister arg_1, bool check_exceptions = true);

    address write_insts_final_call_VM(YuhuRegister oop_result, address entry_point, YuhuRegister arg_1, YuhuRegister arg_2, YuhuRegister arg_3, bool check_exceptions = true);

    address write_insts_final_call_VM_helper(YuhuRegister oop_result, address entry_point, int number_of_arguments, bool check_exceptions = true);

    address write_insts_final_call_VM_base(YuhuRegister oop_result, YuhuRegister java_thread, YuhuRegister last_java_sp, address  entry_point, int number_of_arguments, bool check_exceptions);

    address write_insts_final_call_VM_leaf_base(address entry_point, int number_of_arguments, YuhuLabel *retaddr = NULL);

    address write_insts_final_call_VM_leaf(address entry_point, int number_of_arguments = 0);

    address write_insts_final_call_VM_leaf(address entry_point, YuhuRegister arg_0);

    address write_insts_final_call_VM_leaf(address entry_point, YuhuRegister arg_0, YuhuRegister arg_1);

    address write_insts_final_call_VM_leaf(address entry_point, YuhuRegister arg_0, YuhuRegister arg_1, YuhuRegister arg_2);

    address write_insts_get_vm_result(YuhuRegister oop_result, YuhuRegister thread);

    int write_insts_biased_locking_enter(YuhuRegister lock_reg, YuhuRegister obj_reg,
                             YuhuRegister swap_reg, YuhuRegister tmp_reg,
                             bool swap_reg_contains_mark,
                             YuhuLabel& done, YuhuLabel* slow_case = NULL,
                             BiasedLockingCounters* counters = NULL);

    void write_insts_biased_locking_exit(YuhuRegister obj_reg, YuhuRegister temp_reg, YuhuLabel& done);

    address write_insts_load_prototype_header(YuhuRegister dst, YuhuRegister src);

    address write_insts_cmpxchgptr(YuhuRegister oldv, YuhuRegister newv, YuhuRegister addr, YuhuRegister tmp,
                    YuhuLabel &suceed, YuhuLabel *fail);

    address write_insts_atomic_incw(YuhuRegister counter_addr, YuhuRegister tmp, YuhuRegister tmp2);

    address write_insts_atomic_incw(YuhuAddress counter_addr, YuhuRegister tmp1, YuhuRegister tmp2, YuhuRegister tmp3);

    address write_insts_pop(TosState state); // transition vtos -> state
    address write_insts_push(TosState state); // transition state -> vtos

    void write_insts_push(YuhuRegSet regs, YuhuRegister stack);
    void write_insts_pop(YuhuRegSet regs, YuhuRegister stack);

    int write_insts_push(unsigned int bitset, YuhuRegister stack);
    int write_insts_pop(unsigned int bitset, YuhuRegister stack);

    address write_insts_g1_write_barrier_pre(YuhuRegister obj,
                                             YuhuRegister pre_val,
                                             YuhuRegister thread,
                                             YuhuRegister tmp,
                                             bool tosca_live,
                                             bool expand_call);

    address write_insts_g1_write_barrier_post(YuhuRegister store_addr,
                                              YuhuRegister new_val,
                                              YuhuRegister thread,
                                              YuhuRegister tmp,
                                              YuhuRegister tmp2);

    address write_insts_load_heap_oop(YuhuRegister dst, YuhuAddress src);

    address write_insts_empty_expression_stack() {
        write_inst("ldr x20, [x29, #%d]", frame::interpreter_frame_monitor_block_top_offset * wordSize);
        // NULL last_sp until next java call
        write_inst("str xzr, [x29, #%d]", frame::interpreter_frame_last_sp_offset * wordSize);
        return current_pc();
    }

    address write_insts_update_byte_crc32(YuhuRegister crc, YuhuRegister val, YuhuRegister table);

    address write_insts_restore_constant_pool_cache() {
        write_inst("ldr x26, [x29, #%d]", frame::interpreter_frame_cache_offset * wordSize);
        return current_pc();
    }

    address write_insts_get_cache_and_index_at_bcp(YuhuRegister cache, YuhuRegister index, int bcp_offset, size_t index_size = sizeof(u2));

    address write_insts_get_cache_and_index_and_bytecode_at_bcp(YuhuRegister cache, YuhuRegister index, YuhuRegister bytecode, int byte_no, int bcp_offset, size_t index_size = sizeof(u2));

    address write_insts_get_cache_index_at_bcp(YuhuRegister index, int bcp_offset, size_t index_size = sizeof(u2));

    address write_insts_c2bool(YuhuRegister x);

    address write_insts_remove_activation(TosState state,
                           bool throw_monitor_exception = true,
                           bool install_monitor_exception = true,
                           bool notify_jvmdi = true);

    address write_insts_get_thread(YuhuRegister thread);

    address write_insts_get_method(YuhuRegister reg) {
        write_inst("ldr %s, [x29, #%d]", reg, frame::interpreter_frame_method_offset * wordSize);
        return current_pc();
    }

    address write_insts_get_const(YuhuRegister reg) {
        write_insts_get_method(reg);
        write_inst("ldr %s, [%s, #%d]", reg, reg, in_bytes(Method::const_offset()));
        return current_pc();
    }

    address write_insts_get_constant_pool(YuhuRegister reg) {
        write_insts_get_const(reg);
        write_inst("ldr %s, [%s, #%d]", reg, reg, in_bytes(ConstMethod::constants_offset()));
        return current_pc();
    }

    address write_insts_get_cpool_and_tags(YuhuRegister cpool, YuhuRegister tags) {
        write_insts_get_constant_pool(cpool);
        write_inst("ldr %s, [%s, #%d]", tags, cpool, ConstantPool::tags_offset_in_bytes());
        return current_pc();
    }

    address write_insts_null_check(YuhuRegister reg, int offset = -1);

    // Generate a subtype check: branch to ok_is_subtype if sub_klass is
    // a subtype of super_klass.
    address write_insts_gen_subtype_check(YuhuRegister sub_klass, YuhuLabel &ok_is_subtype );

    // Simplified, combined version, good for typical uses.
    // Falls through on failure.
    address write_insts_check_klass_subtype(YuhuRegister sub_klass,
                             YuhuRegister super_klass,
                             YuhuRegister temp_reg,
                             YuhuLabel& L_success);

    address write_insts_check_klass_subtype_fast_path(YuhuRegister sub_klass,
                                       YuhuRegister super_klass,
                                       YuhuRegister temp_reg,
                                       YuhuLabel* L_success,
                                       YuhuLabel* L_failure,
                                       YuhuLabel* L_slow_path,
                                       YuhuRegister super_check_offset_r = noreg,
                                       intptr_t super_check_offset_c = -1);

    // The rest of the type check; must be wired to a corresponding fast path.
    // It does not repeat the fast path logic, so don't use it standalone.
    // The temp_reg and temp2_reg can be noreg, if no temps are available.
    // Updates the sub's secondary super cache as necessary.
    // If set_cond_codes, condition codes will be Z on success, NZ on failure.
    address write_insts_check_klass_subtype_slow_path(YuhuRegister sub_klass,
                                       YuhuRegister super_klass,
                                       YuhuRegister temp_reg,
                                       YuhuRegister temp2_reg,
                                       YuhuLabel* L_success,
                                       YuhuLabel* L_failure,
                                       bool set_cond_codes = false);

    address write_insts_repne_scan(YuhuRegister addr, YuhuRegister value, YuhuRegister count,
                    YuhuRegister scratch);

    address write_insts_lea(YuhuRegister reg, YuhuAddress addr);

    address write_insts_encode_heap_oop(YuhuRegister d, YuhuRegister s);
    address write_insts_encode_heap_oop(YuhuRegister r) { return write_insts_encode_heap_oop(r, r); }

    address write_insts_store_heap_oop(YuhuAddress dst, YuhuRegister src);

    address write_insts_store_heap_oop_null(YuhuAddress dst);

    address write_insts_load_byte_map_base(YuhuRegister reg);

    address write_insts_store_check(YuhuRegister obj);
    address write_insts_store_check(YuhuRegister obj, YuhuAddress dst);

    address write_insts_store_check_part_1(YuhuRegister obj);
    address write_insts_store_check_part_2(YuhuRegister obj);

    address write_insts_corrected_idivl(YuhuRegister result, YuhuRegister ra, YuhuRegister rb,
                        bool want_remainder, YuhuRegister tmp = x8);

    address write_insts_corrected_idivq(YuhuRegister result, YuhuRegister ra, YuhuRegister rb,
                        bool want_remainder, YuhuRegister tmp = x8);
};

class YuhuLabel VALUE_OBJ_CLASS_SPEC {
private:
    enum { PatchCacheSize = 4 };

    // _loc encodes both the binding state (via its sign)
    // and the binding locator (via its value) of a label.
    //
    // _loc >= 0   bound label, loc() encodes the target (jump) position
    // _loc == -1  unbound label
    int _loc;

    // References to instructions that jump to this unresolved label.
    // These instructions need to be patched when the label is bound
    // using the platform-specific patchInstruction() method.
    //
    // To avoid having to allocate from the C-heap each time, we provide
    // a local cache and use the overflow only if we exceed the local cache
    int _patches[PatchCacheSize];
    int _patch_index;
    GrowableArray<int>* _patch_overflow;

    YuhuLabel(const YuhuLabel&) { ShouldNotReachHere(); }

public:

    /**
     * After binding, be sure 'patch_instructions' is called later to link
     */
    void bind_loc(int loc) {
        assert(loc >= 0, "illegal locator");
        assert(_loc == -1, "already bound");
        _loc = loc;
    }
    void bind_loc(int pos, int sect) { bind_loc(CodeBuffer::locator(pos, sect)); }

#ifndef PRODUCT
    // Iterates over all unresolved instructions for printing
    void print_instructions(YuhuMacroAssembler* masm) const;
#endif // PRODUCT

    /**
     * Returns the position of the the Label in the code buffer
     * The position is a 'locator', which encodes both offset and section.
     */
    int loc() const {
        assert(_loc >= 0, "unbound label");
        return _loc;
    }
    int loc_pos()  const { return CodeBuffer::locator_pos(loc()); }
    int loc_sect() const { return CodeBuffer::locator_sect(loc()); }

    bool is_bound() const    { return _loc >=  0; }
    bool is_unbound() const  { return _loc == -1 && _patch_index > 0; }
    bool is_unused() const   { return _loc == -1 && _patch_index == 0; }

    /**
     * Adds a reference to an unresolved displacement instruction to
     * this unbound label
     *
     * @param cb         the code buffer being patched
     * @param branch_loc the locator of the branch instruction in the code buffer
     */
    void add_patch_at(CodeBuffer* cb, int branch_loc);

    /**
     * Iterate over the list of patches, resolving the instructions
     * Call patch_instruction on each 'branch_loc' value
     */
    void patch_instructions(YuhuMacroAssembler* masm);

    void init() {
        _loc = -1;
        _patch_index = 0;
        _patch_overflow = NULL;
    }

    YuhuLabel() {
        init();
    }
};

// A set of registers
class YuhuRegSet {
    uint32_t _bitset;

    YuhuRegSet(uint32_t bitset) : _bitset(bitset) { }

public:

    YuhuRegSet() : _bitset(0) { }

    YuhuRegSet(YuhuMacroAssembler::YuhuRegister r1) : _bitset(YuhuMacroAssembler::bit(r1)) { }

    YuhuRegSet operator+(const YuhuRegSet aSet) const {
        YuhuRegSet result(_bitset | aSet._bitset);
        return result;
    }

    YuhuRegSet operator-(const YuhuRegSet aSet) const {
        YuhuRegSet result(_bitset & ~aSet._bitset);
        return result;
    }

    YuhuRegSet &operator+=(const YuhuRegSet aSet) {
        *this = *this + aSet;
        return *this;
    }

    static YuhuRegSet of(YuhuMacroAssembler::YuhuRegister r1) {
        return YuhuRegSet(r1);
    }

    static YuhuRegSet of(YuhuMacroAssembler::YuhuRegister r1, YuhuMacroAssembler::YuhuRegister r2) {
        return of(r1) + r2;
    }

    static YuhuRegSet of(YuhuMacroAssembler::YuhuRegister r1, YuhuMacroAssembler::YuhuRegister r2, YuhuMacroAssembler::YuhuRegister r3) {
        return of(r1, r2) + r3;
    }

    static YuhuRegSet of(YuhuMacroAssembler::YuhuRegister r1, YuhuMacroAssembler::YuhuRegister r2, YuhuMacroAssembler::YuhuRegister r3, YuhuMacroAssembler::YuhuRegister r4) {
        return of(r1, r2, r3) + r4;
    }

    static YuhuRegSet range(YuhuMacroAssembler::YuhuRegister start, YuhuMacroAssembler::YuhuRegister end) {
        uint32_t bits = ~0;
        bits <<= YuhuMacroAssembler::encoding(start);
        bits <<= 31 - YuhuMacroAssembler::encoding(end);
        bits >>= 31 - YuhuMacroAssembler::encoding(end);

        return YuhuRegSet(bits);
    }

    uint32_t bits() const { return _bitset; }
};

class YuhuPrePost {
    int _offset;
    YuhuMacroAssembler::YuhuRegister _r;
public:
    YuhuPrePost(YuhuMacroAssembler::YuhuRegister reg, int o) : _r(reg), _offset(o) { }
    int offset() { return _offset; }
    YuhuMacroAssembler::YuhuRegister reg() { return _r; }
};

class YuhuPre : public YuhuPrePost {
public:
    YuhuPre(YuhuMacroAssembler::YuhuRegister reg, int o) : YuhuPrePost(reg, o) { }
};

class YuhuPost : public YuhuPrePost {
public:
    YuhuPost(YuhuMacroAssembler::YuhuRegister reg, int o) : YuhuPrePost(reg, o) { }
};

class YuhuAddress VALUE_OBJ_CLASS_SPEC {
public:
    enum mode { no_mode, base_plus_offset, pre, post, pcrel,
                base_plus_offset_reg, literal };

    class extend {
        int _shift;
        YuhuMacroAssembler::YuhuOperation _op;
    public:
        extend() { }
        extend(int s, YuhuMacroAssembler::YuhuOperation op) : _shift(s), _op(op) { }
        int shift() const { return _shift; }
        YuhuMacroAssembler::YuhuOperation op() const { return _op; }
    };
    class uxtw : public extend {
        public:
        uxtw(int shift = -1): extend(shift, YuhuMacroAssembler::uxtw) { }
    };
    class lsl : public extend {
        public:
        lsl(int shift = -1): extend(shift, YuhuMacroAssembler::lsl) { }
    };
    class sxtw : public extend {
        public:
        sxtw(int shift = -1): extend(shift, YuhuMacroAssembler::sxtw) { }
    };
    class sxtx : public extend {
        public:
        sxtx(int shift = -1): extend(shift, YuhuMacroAssembler::sxtx) { }
    };
private:
    YuhuMacroAssembler::YuhuRegister _base;
    YuhuMacroAssembler::YuhuRegister _index;
    int64_t _offset;
    enum mode _mode;
    extend _ext;

    bool _is_lval;

    address          _target;
public:
    YuhuAddress()
    : _mode(no_mode) { }
    YuhuAddress(YuhuMacroAssembler::YuhuRegister r)
    : _mode(base_plus_offset), _base(r), _offset(0), _index(YuhuMacroAssembler::noreg), _target(0) { }
    YuhuAddress(YuhuMacroAssembler::YuhuRegister r, int o)
    : _mode(base_plus_offset), _base(r), _offset(o), _index(YuhuMacroAssembler::noreg), _target(0) { }
    YuhuAddress(YuhuMacroAssembler::YuhuRegister r, long o)
    : _mode(base_plus_offset), _base(r), _offset(o), _index(YuhuMacroAssembler::noreg), _target(0) { }
    YuhuAddress(YuhuMacroAssembler::YuhuRegister r, long long o)
    : _mode(base_plus_offset), _base(r), _offset(o), _index(YuhuMacroAssembler::noreg), _target(0) { }
    YuhuAddress(YuhuMacroAssembler::YuhuRegister r, unsigned int o)
    : _mode(base_plus_offset), _base(r), _offset(o), _index(YuhuMacroAssembler::noreg), _target(0) { }
    YuhuAddress(YuhuMacroAssembler::YuhuRegister r, unsigned long o)
    : _mode(base_plus_offset), _base(r), _offset(o), _index(YuhuMacroAssembler::noreg), _target(0) { }
    YuhuAddress(YuhuMacroAssembler::YuhuRegister r, unsigned long long o)
    : _mode(base_plus_offset), _base(r), _offset(o), _index(YuhuMacroAssembler::noreg), _target(0) { }
#ifdef ASSERT
    YuhuAddress(YuhuMacroAssembler::YuhuRegister r, ByteSize disp)
    : _mode(base_plus_offset), _base(r), _offset(in_bytes(disp)),
    _index(YuhuMacroAssembler::noreg), _target(0) { }
#endif
    YuhuAddress(YuhuMacroAssembler::YuhuRegister r, YuhuMacroAssembler::YuhuRegister r1, extend ext = lsl())
    : _mode(base_plus_offset_reg), _base(r), _index(r1),
    _ext(ext), _offset(0), _target(0) { }
    YuhuAddress(YuhuPre p)
    : _mode(pre), _base(p.reg()), _offset(p.offset()) { }
    YuhuAddress(YuhuPost p)
    : _mode(post), _base(p.reg()), _offset(p.offset()), _target(0) { }
    YuhuAddress(address target)
    : _mode(literal), _is_lval(false), _target(target)  { }

    YuhuMacroAssembler::YuhuRegister base() const {
        guarantee((_mode == base_plus_offset | _mode == base_plus_offset_reg
                   | _mode == post),
                  "wrong mode");
        return _base;
    }
    int64_t offset() const {
        return _offset;
    }
    YuhuMacroAssembler::YuhuRegister index() const {
        return _index;
    }
    extend ext() const {
        return _ext;
    }
    mode getMode() const {
        return _mode;
    }
    bool uses(YuhuMacroAssembler::YuhuRegister reg) const { return _base == reg || _index == reg; }
    address target() const { return _target; }

    void lea(YuhuMacroAssembler *, YuhuMacroAssembler::YuhuRegister) const;
};

#endif //JDK8_YUHU_MACROASSEMBLER_HPP

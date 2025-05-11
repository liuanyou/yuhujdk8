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
        w29, w30,
        x0, x1, x2, x3, x4, x5, x6, x7,
        x8,
        x9, x10, x11, x12, x13, x14, x15, // Caller saved
        x16, x17, x18,
        x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, // Callee saved
        x29, x30, x31, sp = x31, xzr = x31
    };
    // condition kind for b.cond instruction
    enum YuhuCond {
        gt, ne, al, ls, hi
    };
private:
    const char* reg_name(YuhuRegister reg) {
        static const char* register_names[] = {
            // w0-w30
            "w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7",
            "w8", "w9", "w10", "w11", "w12", "w13", "w14", "w15",
            "w16", "w17", "w18", "w19", "w20", "w21", "w22", "w23",
            "w24", "w25", "w26", "w27", "w28", "w29", "w30",

            // x0-x31
            "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
            "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
            "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
            "x24", "x25", "x26", "x27", "x28", "x29", "x30", "x31",

            // alias sp and xzr is mapped to x31
            "sp", "xzr"
        };

        // check array index
        size_t count = sizeof(register_names)/sizeof(register_names[0]);
        assert((int)reg >= 0 && (int)reg < (int)count, "Unknown register");
        return register_names[(int)reg];
    }
    const char* cond_name(YuhuCond cond) {
        static const char* cond_names[] = {
                "gt", "ne", "al", "ls", "hi"
        };

        // check array index
        size_t count = sizeof(cond_names)/sizeof(cond_names[0]);
        assert((int)cond >=0 && (int)cond < (int)count, "Unknown cond");
        return cond_names[(int)cond];
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

    address write_inst(const char* assembly_format, YuhuRegister reg, unsigned int imm32);

    address write_inst(const char* assembly_format, YuhuRegister reg1, YuhuRegister reg2, unsigned int imm32);

    address write_inst(const char* assembly_format, unsigned int imm32);

    address write_inst_br(YuhuRegister reg);

    address write_inst_b(address target);

    address write_inst_b(YuhuLabel& label);

    address write_inst_b(YuhuCond cond, address target);

    address write_inst_b(YuhuCond cond, YuhuLabel& label);

    address write_inst_cbz(YuhuRegister reg, address target);

    address write_inst_cbz(YuhuRegister reg, YuhuLabel& label);

    address write_inst_cbnz(YuhuRegister reg, address target);

    address write_inst_cbnz(YuhuRegister reg, YuhuLabel& label);

    address write_inst_adrp(YuhuRegister reg, address target);

    void pin_label(YuhuLabel& label);

    address write_insts_stop(const char* msg);

    /**
     * push all registers into sp
     *
     * @return
     */
    address write_insts_pusha();

    address write_insts_mov_ptr(YuhuRegister reg, uintptr_t imm64);

    address write_insts_mov_imm64(YuhuRegister reg, uint64_t imm64);

    address write_insts_mov_imm32(YuhuRegister reg, uint32_t imm32);

    address write_insts_dispatch_next(TosState state, int step = 0);

    address write_insts_dispatch_base(TosState state, address* table, bool verifyoop = true);

    address write_insts_far_jump(address entry, CodeBuffer *cbuf = NULL, YuhuRegister tmp = x8);

    address write_insts_adrp(YuhuRegister reg, const address &dest, uint64_t &byte_offset);
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

#endif //JDK8_YUHU_MACROASSEMBLER_HPP

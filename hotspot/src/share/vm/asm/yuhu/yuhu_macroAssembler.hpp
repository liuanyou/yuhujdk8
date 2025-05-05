//
// Created by Anyou Liu on 2025/4/28.
//

#ifndef JDK8_YUHU_MACROASSEMBLER_HPP
#define JDK8_YUHU_MACROASSEMBLER_HPP

#include "asm/macroAssembler.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "interpreter/invocationCounter.hpp"
#include "runtime/frame.hpp"
#include <keystone/keystone.h>

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

    address write_inst(const char* assembly_format, YuhuRegister reg, unsigned int imm16);

    address write_inst_b(long offset);

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
};

#endif //JDK8_YUHU_MACROASSEMBLER_HPP

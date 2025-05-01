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

    address write_inst(const char* head, unsigned int tail);

    address write_inst(const char* head, unsigned int mid, const char* tail);

    address write_inst_b(long offset);
};

#endif //JDK8_YUHU_MACROASSEMBLER_HPP

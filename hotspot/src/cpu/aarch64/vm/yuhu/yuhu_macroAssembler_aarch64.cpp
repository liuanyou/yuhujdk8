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

address YuhuMacroAssembler::write_inst(const char* head, unsigned int tail) {
    char buffer[50];
    snprintf(buffer, sizeof(buffer), "%s%d", head, tail);
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst(const char* head, unsigned int mid, const char* tail) {
    char buffer[50];
    snprintf(buffer, sizeof(buffer), "%s%d%s", head, mid, tail);
    return write_inst(machine_code(buffer));
}

address YuhuMacroAssembler::write_inst_b(long offset) {
    char buffer[50];
    snprintf(buffer, sizeof(buffer), "b #%d", offset);
    return write_inst(machine_code(buffer));
}
//
// Created by Anyou Liu on 2025/4/24.
//

#ifndef JDK8_YUHU_INTERPRETERGENERATOR_HPP
#define JDK8_YUHU_INTERPRETERGENERATOR_HPP

#include "code/stubs.hpp"
#include "interpreter/bytecodes.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/top.hpp"
#include "asm/yuhu/yuhu_macroAssembler.hpp"

class YuhuInterpreterGenerator: public StackObj {
protected:
    YuhuMacroAssembler* _masm;
    address _unimplemented_bytecode;
    address _illegal_bytecode_sequence;
    void generate_all();
    address generate_error_exit(const char* msg);
public:
    YuhuInterpreterGenerator();
};

#endif //JDK8_YUHU_INTERPRETERGENERATOR_HPP

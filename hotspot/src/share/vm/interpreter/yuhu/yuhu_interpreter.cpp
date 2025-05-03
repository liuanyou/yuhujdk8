//
// Created by Anyou Liu on 2025/4/23.
//
#include "precompiled.hpp"
#include "interpreter/yuhu/yuhu_interpreter.hpp"
#include "interpreter/yuhu/yuhu_interpreterGenerator.hpp"

StubQueue* YuhuInterpreter::_code = NULL;

void YuhuInterpreterCodelet::verify() {
}

void YuhuInterpreterCodelet::print_on(outputStream* st) const {
}

void YuhuInterpreterCodelet::initialize(const char* description, Bytecodes::Code bytecode) {
    _description       = description;
    _bytecode          = bytecode;
}

void yuhuInterpreter_init() {
    YuhuInterpreter::initialize();
}

void YuhuInterpreter::initialize() {
    if (_code != NULL) return;
    {
        ResourceMark rm;
        TraceTime timer("yuhuInterpreter generation", TraceStartupTime);
        int code_size = InterpreterCodeSize;
        NOT_PRODUCT(code_size *= 4;)  // debug uses extra interpreter code space
        _code = new StubQueue(new YuhuInterpreterCodeletInterface, code_size, NULL,
                              "yuhInterpreter");
        YuhuInterpreterGenerator g;
    }
}


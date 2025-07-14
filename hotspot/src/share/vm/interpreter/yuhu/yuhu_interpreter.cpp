//
// Created by Anyou Liu on 2025/4/23.
//
#include "precompiled.hpp"
#include "interpreter/yuhu/yuhu_interpreter.hpp"
#include "interpreter/yuhu/yuhu_interpreterGenerator.hpp"

StubQueue* YuhuInterpreter::_code = NULL;
address YuhuInterpreter::_native_entry_begin = NULL;
address YuhuInterpreter::_native_entry_end = NULL;
YuhuEntryPoint YuhuInterpreter::_return_entry[number_of_states];
YuhuEntryPoint YuhuInterpreter::_safept_entry;
YuhuDispatchTable YuhuInterpreter::_active_table;
YuhuDispatchTable YuhuInterpreter::_safept_table;
YuhuDispatchTable YuhuInterpreter::_normal_table;
address    YuhuInterpreter::_wentry_point[YuhuDispatchTable::length];

address    YuhuInterpreter::_entry_table            [YuhuInterpreter::number_of_method_entries];

YuhuEntryPoint::YuhuEntryPoint() {
    assert(number_of_states == 9, "check the code below");
    _entry[btos] = NULL;
    _entry[ctos] = NULL;
    _entry[stos] = NULL;
    _entry[atos] = NULL;
    _entry[itos] = NULL;
    _entry[ltos] = NULL;
    _entry[ftos] = NULL;
    _entry[dtos] = NULL;
    _entry[vtos] = NULL;
}

YuhuEntryPoint::YuhuEntryPoint(address bentry, address centry, address sentry, address aentry, address ientry, address lentry, address fentry, address dentry, address ventry) {
    assert(number_of_states == 9, "check the code below");
    _entry[btos] = bentry;
    _entry[ctos] = centry;
    _entry[stos] = sentry;
    _entry[atos] = aentry;
    _entry[itos] = ientry;
    _entry[ltos] = lentry;
    _entry[ftos] = fentry;
    _entry[dtos] = dentry;
    _entry[vtos] = ventry;
}

void YuhuEntryPoint::set_entry(TosState state, address entry) {
    assert(0 <= state && state < number_of_states, "state out of bounds");
    _entry[state] = entry;
}


address YuhuEntryPoint::entry(TosState state) const {
    assert(0 <= state && state < number_of_states, "state out of bounds");
    return _entry[state];
}


void YuhuEntryPoint::print() {
    tty->print("[");
    for (int i = 0; i < number_of_states; i++) {
        if (i > 0) tty->print(", ");
        tty->print(INTPTR_FORMAT, _entry[i]);
    }
    tty->print("]");
}

YuhuEntryPoint YuhuDispatchTable::entry(int i) const {
    assert(0 <= i && i < length, "index out of bounds");
    return
            YuhuEntryPoint(
                    _table[btos][i],
                    _table[ctos][i],
                    _table[stos][i],
                    _table[atos][i],
                    _table[itos][i],
                    _table[ltos][i],
                    _table[ftos][i],
                    _table[dtos][i],
                    _table[vtos][i]
            );
}

void YuhuDispatchTable::set_entry(int i, YuhuEntryPoint& entry) {
    assert(0 <= i && i < length, "index out of bounds");
    assert(number_of_states == 9, "check the code below");
    _table[btos][i] = entry.entry(btos);
    _table[ctos][i] = entry.entry(ctos);
    _table[stos][i] = entry.entry(stos);
    _table[atos][i] = entry.entry(atos);
    _table[itos][i] = entry.entry(itos);
    _table[ltos][i] = entry.entry(ltos);
    _table[ftos][i] = entry.entry(ftos);
    _table[dtos][i] = entry.entry(dtos);
    _table[vtos][i] = entry.entry(vtos);
}

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

    // initialize dispatch table
    _active_table = _normal_table;
}


//
// Created by Anyou Liu on 2025/5/4.
//
#include "precompiled.hpp"
#include "gc_interface/collectedHeap.hpp"
#include "runtime/timer.hpp"
#include "interpreter/yuhu/yuhu_templateTable.hpp"

bool                       YuhuTemplateTable::_is_initialized = false;
YuhuTemplate               YuhuTemplateTable::_template_table     [Bytecodes::number_of_codes];
YuhuTemplate               YuhuTemplateTable::_template_table_wide[Bytecodes::number_of_codes];

YuhuTemplate*              YuhuTemplateTable::_desc;
YuhuMacroAssembler*        YuhuTemplateTable::_masm;
BarrierSet*                YuhuTemplateTable::_bs;

Bytecodes::Code YuhuTemplate::bytecode() const {
    int i = this - YuhuTemplateTable::_template_table;
    if (i < 0 || i >= Bytecodes::number_of_codes) i = this - YuhuTemplateTable::_template_table_wide;
    return Bytecodes::cast(i);
}

void YuhuTemplate::generate(YuhuMacroAssembler* masm) {
    // parameter passing
    YuhuTemplateTable::_desc = this;
    YuhuTemplateTable::_masm = masm;
    // code generation
    _gen(_arg);
    masm->flush();
}
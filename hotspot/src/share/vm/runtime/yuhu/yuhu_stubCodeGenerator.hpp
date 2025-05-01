//
// Created by Anyou Liu on 2025/4/27.
//

#ifndef JDK8_YUHU_STUBCODEGENERATOR_HPP
#define JDK8_YUHU_STUBCODEGENERATOR_HPP

#include "asm/assembler.hpp"
#include "memory/allocation.hpp"
#include "asm/yuhu/yuhu_macroAssembler.hpp"

class YuhuStubCodeGenerator: public StackObj {
protected:
    YuhuMacroAssembler*  _masm;

//    StubCodeDesc* _first_stub;
//    StubCodeDesc* _last_stub;
    bool _print_code;

public:
    YuhuStubCodeGenerator(CodeBuffer* code, bool print_code = false);
    ~YuhuStubCodeGenerator();

    YuhuMacroAssembler* assembler() const              { return _masm; }

//    virtual void stub_prolog(StubCodeDesc* cdesc); // called by StubCodeMark constructor
//    virtual void stub_epilog(StubCodeDesc* cdesc); // called by StubCodeMark destructor
};

#endif //JDK8_YUHU_STUBCODEGENERATOR_HPP

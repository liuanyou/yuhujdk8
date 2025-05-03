//
// Created by Anyou Liu on 2025/4/23.
//

#ifndef JDK8_YUHU_INTERPRETER_HPP
#define JDK8_YUHU_INTERPRETER_HPP

#include "code/stubs.hpp"
#include "interpreter/bytecodes.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/top.hpp"
#include "asm/yuhu/yuhu_macroAssembler.hpp"

class YuhuInterpreter : AllStatic {
protected:
    static StubQueue* _code;
public:
    static StubQueue* code() { return _code; }
    static void initialize();

#ifdef TARGET_ARCH_aarch64
# include "yuhu_interpreter_aarch64.hpp"
#endif
};

class YuhuInterpreterCodelet: public Stub {
    friend class VMStructs;
private:
    int         _size;                             // the size in bytes
    const char* _description;                      // a description of the codelet, for debugging & printing
    Bytecodes::Code _bytecode;                     // associated bytecode if any
    DEBUG_ONLY(CodeStrings _strings;)              // Comments for annotating assembler output.

public:
    // Initialization/finalization
    void    initialize(int size,
                       CodeStrings& strings)       { _size = size; DEBUG_ONLY(_strings.assign(strings);) }
    void    finalize()                             { ShouldNotCallThis(); }

    // General info/converters
    int     size() const                           { return _size; }
    static  int code_size_to_size(int code_size)   { return round_to(sizeof(YuhuInterpreterCodelet), CodeEntryAlignment) + code_size; }

    // Code info
    address code_begin() const                     { return (address)this + round_to(sizeof(YuhuInterpreterCodelet), CodeEntryAlignment); }
    address code_end() const                       { return (address)this + size(); }

    // Debugging
    void    verify();
    void    print_on(outputStream* st) const;
    void    print() const { print_on(tty); }

    // Interpreter-specific initialization
    void    initialize(const char* description, Bytecodes::Code bytecode);

    // Interpreter-specific attributes
    int         code_size() const                  { return code_end() - code_begin(); }
    const char* description() const                { return _description; }
    Bytecodes::Code bytecode() const               { return _bytecode; }
};

// Define a prototype interface
DEF_STUB_INTERFACE(YuhuInterpreterCodelet);

class YuhuCodeletMark: ResourceMark {
private:
    YuhuInterpreterCodelet*         _clet;
    YuhuMacroAssembler** _masm;
    CodeBuffer                  _cb;

    int codelet_size() {
        // Request the whole code buffer (minus a little for alignment).
        // The commit call below trims it back for each codelet.
        int codelet_size = YuhuInterpreter::code()->available_space() - 2*K;

        // Guarantee there's a little bit of code space left.
        guarantee (codelet_size > 0 && (size_t)codelet_size >  2*K,
                   "not enough space for interpreter generation");

        return codelet_size;
    }

public:
    YuhuCodeletMark(
            YuhuMacroAssembler*& masm,
            const char* description,
            Bytecodes::Code bytecode = Bytecodes::_illegal):
            _clet((YuhuInterpreterCodelet*)YuhuInterpreter::code()->request(codelet_size())),
            _cb(_clet->code_begin(), _clet->code_size())

    { // request all space (add some slack for Codelet data)
        assert (_clet != NULL, "we checked not enough space already");

        // initialize Codelet attributes
        _clet->initialize(description, bytecode);
        // create assembler for code generation
        masm  = new YuhuMacroAssembler(&_cb);
        _masm = &masm;
    }

    ~YuhuCodeletMark() {
        // align so printing shows nop's instead of random code at the end (Codelets are aligned)
        (*_masm)->align(wordSize);
        // make sure all code is in code buffer
        (*_masm)->flush();


        // commit Codelet
        YuhuInterpreter::code()->commit((*_masm)->code()->pure_insts_size(), (*_masm)->code()->strings());
        // make sure nobody can use _masm outside a CodeletMark lifespan
        *_masm = NULL;
    }
};

#endif //JDK8_YUHU_INTERPRETER_HPP

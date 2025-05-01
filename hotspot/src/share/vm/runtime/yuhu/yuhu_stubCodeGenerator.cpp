//
// Created by Anyou Liu on 2025/4/29.
//
#include "precompiled.hpp"
#include "asm/yuhu/yuhu_macroAssembler.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "code/codeCache.hpp"
#include "compiler/disassembler.hpp"
#include "oops/oop.inline.hpp"
#include "prims/forte.hpp"
#include "runtime/yuhu/yuhu_stubCodeGenerator.hpp"

YuhuStubCodeGenerator::YuhuStubCodeGenerator(CodeBuffer* code, bool print_code) {
    _masm = new YuhuMacroAssembler(code);
//    _first_stub = _last_stub = NULL;
//    _print_code = print_code;
}

YuhuStubCodeGenerator::~YuhuStubCodeGenerator() {
    if (PrintStubCode || _print_code) {
        CodeBuffer* cbuf = _masm->code();
        CodeBlob*   blob = CodeCache::find_blob_unsafe(cbuf->insts()->start());
        if (blob != NULL) {
            blob->set_strings(cbuf->strings());
        }
//        bool saw_first = false;
//        StubCodeDesc* toprint[1000];
//        int toprint_len = 0;
//        for (StubCodeDesc* cdesc = _last_stub; cdesc != NULL; cdesc = cdesc->_next) {
//            toprint[toprint_len++] = cdesc;
//            if (cdesc == _first_stub) { saw_first = true; break; }
//        }
//        assert(saw_first, "must get both first & last");
        // Print in reverse order:
//        qsort(toprint, toprint_len, sizeof(toprint[0]), compare_cdesc);
//        for (int i = 0; i < toprint_len; i++) {
//            StubCodeDesc* cdesc = toprint[i];
//            cdesc->print();
//            tty->cr();
//            Disassembler::decode(cdesc->begin(), cdesc->end());
//            tty->cr();
//        }
    }
}
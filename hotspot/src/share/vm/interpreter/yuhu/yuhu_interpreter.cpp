//
// Created by Anyou Liu on 2025/4/23.
//
#include "precompiled.hpp"
#include "interpreter/yuhu/yuhu_interpreter.hpp"
#include "interpreter/yuhu/yuhu_interpreterGenerator.hpp"

StubQueue* YuhuInterpreter::_code = NULL;
address YuhuInterpreter::_native_entry_begin = NULL;
address YuhuInterpreter::_native_entry_end = NULL;
YuhuEntryPoint YuhuInterpreter::_return_entry[number_of_return_entries];
YuhuEntryPoint YuhuInterpreter::_deopt_entry[number_of_deopt_entries];
YuhuEntryPoint YuhuInterpreter::_continuation_entry;
YuhuEntryPoint YuhuInterpreter::_safept_entry;

address YuhuInterpreter::_invoke_return_entry[number_of_return_addrs];
address YuhuInterpreter::_invokeinterface_return_entry[number_of_return_addrs];
address YuhuInterpreter::_invokedynamic_return_entry[number_of_return_addrs];

YuhuDispatchTable YuhuInterpreter::_active_table;
YuhuDispatchTable YuhuInterpreter::_safept_table;
YuhuDispatchTable YuhuInterpreter::_normal_table;
address    YuhuInterpreter::_wentry_point[YuhuDispatchTable::length];

address    YuhuInterpreter::_entry_table            [YuhuInterpreter::number_of_method_entries];
address    YuhuInterpreter::_native_abi_to_tosca[number_of_result_handlers];

address YuhuInterpreter::_rethrow_exception_entry = NULL;
address YuhuInterpreter::_throw_exception_entry = NULL;
address YuhuInterpreter::_remove_activation_preserving_args_entry    = NULL;
address YuhuInterpreter::_remove_activation_entry = NULL;

address    YuhuInterpreter::_throw_ArrayIndexOutOfBoundsException_entry = NULL;
address    YuhuInterpreter::_throw_ArrayStoreException_entry = NULL;
address    YuhuInterpreter::_throw_ArithmeticException_entry = NULL;
address    YuhuInterpreter::_throw_ClassCastException_entry = NULL;
address    YuhuInterpreter::_throw_NullPointerException_entry = NULL;
address    YuhuInterpreter::_throw_StackOverflowError_entry = NULL;

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

    YuhuTemplateTable::initialize();

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

int YuhuInterpreter::BasicType_as_index(BasicType type) {
    int i = 0;
    switch (type) {
        case T_BOOLEAN: i = 0; break;
        case T_CHAR   : i = 1; break;
        case T_BYTE   : i = 2; break;
        case T_SHORT  : i = 3; break;
        case T_INT    : i = 4; break;
        case T_LONG   : i = 5; break;
        case T_VOID   : i = 6; break;
        case T_FLOAT  : i = 7; break;
        case T_DOUBLE : i = 8; break;
        case T_OBJECT : i = 9; break;
        case T_ARRAY  : i = 9; break;
        default       : ShouldNotReachHere();
    }
    assert(0 <= i && i < YuhuInterpreter::number_of_result_handlers,
           "index out of bounds");
    return i;
}

address* YuhuInterpreter::invoke_return_entry_table_for(Bytecodes::Code code) {
    switch (code) {
        case Bytecodes::_invokestatic:
        case Bytecodes::_invokespecial:
        case Bytecodes::_invokevirtual:
        case Bytecodes::_invokehandle:
            return YuhuInterpreter::invoke_return_entry_table();
        case Bytecodes::_invokeinterface:
            return YuhuInterpreter::invokeinterface_return_entry_table();
        case Bytecodes::_invokedynamic:
            return YuhuInterpreter::invokedynamic_return_entry_table();
        default:
            fatal(err_msg("invalid bytecode: %s", Bytecodes::name(code)));
            return NULL;
    }
}

YuhuInterpreter::MethodKind YuhuInterpreter::method_kind(methodHandle m) {
    // Abstract method?
    if (m->is_abstract()) return abstract;

    // Method handle primitive?
    if (m->is_method_handle_intrinsic()) {
        vmIntrinsics::ID id = m->intrinsic_id();
        assert(MethodHandles::is_signature_polymorphic(id), "must match an intrinsic");
        MethodKind kind = (MethodKind)( method_handle_invoke_FIRST +
                                        ((int)id - vmIntrinsics::FIRST_MH_SIG_POLY) );
        assert(kind <= method_handle_invoke_LAST, "parallel enum ranges");
        return kind;
    }

#ifndef CC_INTERP
    if (UseCRC32Intrinsics && m->is_native()) {
        // Use optimized stub code for CRC32 native methods.
        switch (m->intrinsic_id()) {
            case vmIntrinsics::_updateCRC32            : return java_util_zip_CRC32_update;
            case vmIntrinsics::_updateBytesCRC32       : return java_util_zip_CRC32_updateBytes;
            case vmIntrinsics::_updateByteBufferCRC32  : return java_util_zip_CRC32_updateByteBuffer;
        }
    }
#endif

    // Native method?
    // Note: This test must come _before_ the test for intrinsic
    //       methods. See also comments below.
    if (m->is_native()) {
        assert(!m->is_method_handle_intrinsic(), "overlapping bits here, watch out");
        return m->is_synchronized() ? native_synchronized : native;
    }

    // Synchronized?
    if (m->is_synchronized()) {
        return zerolocals_synchronized;
    }

    if (RegisterFinalizersAtInit && m->code_size() == 1 &&
        m->intrinsic_id() == vmIntrinsics::_Object_init) {
        // We need to execute the special return bytecode to check for
        // finalizer registration so create a normal frame.
        return zerolocals;
    }

    // Empty method?
    if (m->is_empty_method()) {
        return empty;
    }

    // Special intrinsic method?
    // Note: This test must come _after_ the test for native methods,
    //       otherwise we will run into problems with JDK 1.2, see also
    //       AbstractInterpreterGenerator::generate_method_entry() for
    //       for details.
    switch (m->intrinsic_id()) {
        case vmIntrinsics::_dsin  : return java_lang_math_sin  ;
        case vmIntrinsics::_dcos  : return java_lang_math_cos  ;
        case vmIntrinsics::_dtan  : return java_lang_math_tan  ;
        case vmIntrinsics::_dabs  : return java_lang_math_abs  ;
        case vmIntrinsics::_dsqrt : return java_lang_math_sqrt ;
        case vmIntrinsics::_dlog  : return java_lang_math_log  ;
        case vmIntrinsics::_dlog10: return java_lang_math_log10;
        case vmIntrinsics::_dpow  : return java_lang_math_pow  ;
        case vmIntrinsics::_dexp  : return java_lang_math_exp  ;

        case vmIntrinsics::_Reference_get:
            return java_lang_ref_reference_get;
    }

    // Accessor method?
    if (m->is_accessor()) {
        assert(m->size_of_parameters() == 1, "fast code for accessors assumes parameter size = 1");
        return accessor;
    }

    // Note: for now: zero locals for all non-empty methods
    return zerolocals;
}
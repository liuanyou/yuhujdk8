//
// Created by Anyou Liu on 2025/4/24.
//

#include "interpreter/interpreterRuntime.hpp"
#include "interpreter/yuhu/yuhu_interpreterGenerator.hpp"
#include "interpreter/yuhu/yuhu_templateTable.hpp"

static const BasicType types[YuhuInterpreter::number_of_result_handlers] = {
        T_BOOLEAN,
        T_CHAR   ,
        T_BYTE   ,
        T_SHORT  ,
        T_INT    ,
        T_LONG   ,
        T_VOID   ,
        T_FLOAT  ,
        T_DOUBLE ,
        T_OBJECT
};

void YuhuInterpreterGenerator::generate_all() {
    {
        YuhuCodeletMark cm(_masm, "yuhu error exits");
        _unimplemented_bytecode    = generate_error_exit("yuhu unimplemented bytecode");
        _illegal_bytecode_sequence = generate_error_exit("yuhu illegal bytecode sequence - method not verified");
    }
    {
        YuhuCodeletMark cm(_masm, "yuhu return entry points");
        const int index_size = sizeof(u2);
        for (int i = 0; i < YuhuInterpreter::number_of_return_entries; i++) {
            YuhuInterpreter::_return_entry[i] =
                YuhuEntryPoint(
                    generate_return_entry_for(itos, i, index_size),
                    generate_return_entry_for(itos, i, index_size),
                    generate_return_entry_for(itos, i, index_size),
                    generate_return_entry_for(atos, i, index_size),
                    generate_return_entry_for(itos, i, index_size),
                    generate_return_entry_for(ltos, i, index_size),
                    generate_return_entry_for(ftos, i, index_size),
                    generate_return_entry_for(dtos, i, index_size),
                    generate_return_entry_for(vtos, i, index_size)
                );
        }
    }

    { YuhuCodeletMark cm(_masm, "yuhu invoke return entry points");
        const TosState states[] = {itos, itos, itos, itos, ltos, ftos, dtos, atos, vtos};
        const int invoke_length = Bytecodes::length_for(Bytecodes::_invokestatic);
        const int invokeinterface_length = Bytecodes::length_for(Bytecodes::_invokeinterface);
        const int invokedynamic_length = Bytecodes::length_for(Bytecodes::_invokedynamic);

        for (int i = 0; i < YuhuInterpreter::number_of_return_addrs; i++) {
            TosState state = states[i];
            YuhuInterpreter::_invoke_return_entry[i] = generate_return_entry_for(state, invoke_length, sizeof(u2));
            YuhuInterpreter::_invokeinterface_return_entry[i] = generate_return_entry_for(state, invokeinterface_length, sizeof(u2));
            YuhuInterpreter::_invokedynamic_return_entry[i] = generate_return_entry_for(state, invokedynamic_length, sizeof(u4));
        }
    }

    // TODO
//    { CodeletMark cm(_masm, "earlyret entry points");
//        Interpreter::_earlyret_entry =
//                EntryPoint(
//                        generate_earlyret_entry_for(btos),
//                        generate_earlyret_entry_for(ctos),
//                        generate_earlyret_entry_for(stos),
//                        generate_earlyret_entry_for(atos),
//                        generate_earlyret_entry_for(itos),
//                        generate_earlyret_entry_for(ltos),
//                        generate_earlyret_entry_for(ftos),
//                        generate_earlyret_entry_for(dtos),
//                        generate_earlyret_entry_for(vtos)
//                );
//    }

    { YuhuCodeletMark cm(_masm, "yuhu deoptimization entry points");
        for (int i = 0; i < YuhuInterpreter::number_of_deopt_entries; i++) {
            YuhuInterpreter::_deopt_entry[i] =
                    YuhuEntryPoint(
                            generate_deopt_entry_for(itos, i),
                            generate_deopt_entry_for(itos, i),
                            generate_deopt_entry_for(itos, i),
                            generate_deopt_entry_for(atos, i),
                            generate_deopt_entry_for(itos, i),
                            generate_deopt_entry_for(ltos, i),
                            generate_deopt_entry_for(ftos, i),
                            generate_deopt_entry_for(dtos, i),
                            generate_deopt_entry_for(vtos, i)
                    );
        }
    }

    { YuhuCodeletMark cm(_masm, "yuhu result handlers for native calls");
        // The various result converter stublets.
        int is_generated[YuhuInterpreter::number_of_result_handlers];
        memset(is_generated, 0, sizeof(is_generated));

        for (int i = 0; i < YuhuInterpreter::number_of_result_handlers; i++) {
            BasicType type = types[i];
            if (!is_generated[YuhuInterpreter::BasicType_as_index(type)]++) {
                YuhuInterpreter::_native_abi_to_tosca[YuhuInterpreter::BasicType_as_index(type)] = generate_result_handler_for(type);
            }
        }
    }

    { YuhuCodeletMark cm(_masm, "yuhu continuation entry points");
        YuhuInterpreter::_continuation_entry =
                YuhuEntryPoint(
                        generate_continuation_for(btos),
                        generate_continuation_for(ctos),
                        generate_continuation_for(stos),
                        generate_continuation_for(atos),
                        generate_continuation_for(itos),
                        generate_continuation_for(ltos),
                        generate_continuation_for(ftos),
                        generate_continuation_for(dtos),
                        generate_continuation_for(vtos)
                );
    }

    { YuhuCodeletMark cm(_masm, "yuhu safepoint entry points");
        YuhuInterpreter::_safept_entry =
                YuhuEntryPoint(
                        generate_safept_entry_for(btos, CAST_FROM_FN_PTR(address, InterpreterRuntime::at_safepoint)),
                        generate_safept_entry_for(ctos, CAST_FROM_FN_PTR(address, InterpreterRuntime::at_safepoint)),
                        generate_safept_entry_for(stos, CAST_FROM_FN_PTR(address, InterpreterRuntime::at_safepoint)),
                        generate_safept_entry_for(atos, CAST_FROM_FN_PTR(address, InterpreterRuntime::at_safepoint)),
                        generate_safept_entry_for(itos, CAST_FROM_FN_PTR(address, InterpreterRuntime::at_safepoint)),
                        generate_safept_entry_for(ltos, CAST_FROM_FN_PTR(address, InterpreterRuntime::at_safepoint)),
                        generate_safept_entry_for(ftos, CAST_FROM_FN_PTR(address, InterpreterRuntime::at_safepoint)),
                        generate_safept_entry_for(dtos, CAST_FROM_FN_PTR(address, InterpreterRuntime::at_safepoint)),
                        generate_safept_entry_for(vtos, CAST_FROM_FN_PTR(address, InterpreterRuntime::at_safepoint))
                );
    }

    { YuhuCodeletMark cm(_masm, "yuhu exception handling");
        // (Note: this is not safepoint safe because thread may return to compiled code)
        generate_throw_exception();
    }

    { YuhuCodeletMark cm(_masm, "yuhu throw exception entrypoints");
        YuhuInterpreter::_throw_ArrayIndexOutOfBoundsException_entry = generate_ArrayIndexOutOfBounds_handler("java/lang/ArrayIndexOutOfBoundsException");
        YuhuInterpreter::_throw_ArrayStoreException_entry            = generate_klass_exception_handler("java/lang/ArrayStoreException"                 );
        YuhuInterpreter::_throw_ArithmeticException_entry            = generate_exception_handler("java/lang/ArithmeticException"           , "/ by zero");
        YuhuInterpreter::_throw_ClassCastException_entry             = generate_ClassCastException_handler();
        YuhuInterpreter::_throw_NullPointerException_entry           = generate_exception_handler("java/lang/NullPointerException"          , NULL       );
        YuhuInterpreter::_throw_StackOverflowError_entry             = generate_StackOverflowError_handler();
    }

#define method_entry(kind)                                                                    \
    {                                                                                             \
        YuhuCodeletMark cm(_masm, "yuhu method entry point (kind = " #kind ")");                    \
        YuhuInterpreter::_entry_table[Interpreter::kind] = generate_method_entry(YuhuInterpreter::kind);  \
    }

    // all non-native method kinds
    method_entry(zerolocals)
    method_entry(zerolocals_synchronized)
    method_entry(empty)
    method_entry(accessor)
    method_entry(abstract)
    method_entry(java_lang_math_sin  )
    method_entry(java_lang_math_cos  )
    method_entry(java_lang_math_tan  )
    method_entry(java_lang_math_abs  )
    method_entry(java_lang_math_sqrt )
    method_entry(java_lang_math_log  )
    method_entry(java_lang_math_log10)
    method_entry(java_lang_math_exp  )
    method_entry(java_lang_math_pow  )
    method_entry(java_lang_ref_reference_get)

    if (UseCRC32Intrinsics) {
        method_entry(java_util_zip_CRC32_update)
        method_entry(java_util_zip_CRC32_updateBytes)
        method_entry(java_util_zip_CRC32_updateByteBuffer)
    }

    initialize_method_handle_entries();

    // all native method kinds (must be one contiguous block)
    YuhuInterpreter::_native_entry_begin = YuhuInterpreter::code()->code_end();
    method_entry(native)
    method_entry(native_synchronized)
    YuhuInterpreter::_native_entry_end = YuhuInterpreter::code()->code_end();

#undef method_entry

    // Bytecodes
    set_entry_points_for_all_bytes();

    set_safepoints_for_all_bytes();
}

void YuhuInterpreterGenerator::set_entry_points(Bytecodes::Code code) {
    YuhuCodeletMark cm(_masm, Bytecodes::name(code), code);
    // initialize entry points
    assert(_unimplemented_bytecode    != NULL, "should have been generated before");
    assert(_illegal_bytecode_sequence != NULL, "should have been generated before");
    address bep = _illegal_bytecode_sequence;
    address cep = _illegal_bytecode_sequence;
    address sep = _illegal_bytecode_sequence;
    address aep = _illegal_bytecode_sequence;
    address iep = _illegal_bytecode_sequence;
    address lep = _illegal_bytecode_sequence;
    address fep = _illegal_bytecode_sequence;
    address dep = _illegal_bytecode_sequence;
    address vep = _unimplemented_bytecode;
    address wep = _unimplemented_bytecode;
    // code for short & wide version of bytecode
    if (Bytecodes::is_defined(code)) {
        YuhuTemplate* t = YuhuTemplateTable::template_for(code);
        assert(t->is_valid(), "just checking");
        set_short_entry_points(t, bep, cep, sep, aep, iep, lep, fep, dep, vep);
    }
    if (Bytecodes::wide_is_defined(code)) {
        YuhuTemplate* t = YuhuTemplateTable::template_for_wide(code);
        assert(t->is_valid(), "just checking");
        set_wide_entry_point(t, wep);
    }
    // set entry points
    YuhuEntryPoint entry(bep, cep, sep, aep, iep, lep, fep, dep, vep);
    YuhuInterpreter::_normal_table.set_entry(code, entry);
    YuhuInterpreter::_wentry_point[code] = wep;
}

void YuhuInterpreterGenerator::set_unimplemented(int i) {
    address e = _unimplemented_bytecode;
    YuhuEntryPoint entry(e, e, e, e, e, e, e, e, e);
    YuhuInterpreter::_normal_table.set_entry(i, entry);
    YuhuInterpreter::_wentry_point[i] = _unimplemented_bytecode;
}

void YuhuInterpreterGenerator::set_entry_points_for_all_bytes() {
    for (int i = 0; i < YuhuDispatchTable::length; i++) {
        Bytecodes::Code code = (Bytecodes::Code)i;
        if (Bytecodes::is_defined(code)) {
            set_entry_points(code);
        } else {
            set_unimplemented(i);
        }
    }
}

void YuhuInterpreterGenerator::set_safepoints_for_all_bytes() {
    for (int i = 0; i < DispatchTable::length; i++) {
        Bytecodes::Code code = (Bytecodes::Code)i;
        if (Bytecodes::is_defined(code)) YuhuInterpreter::_safept_table.set_entry(code, YuhuInterpreter::_safept_entry);
    }
}

void YuhuInterpreterGenerator::initialize_method_handle_entries() {
    // method handle entry kinds are generated later in MethodHandlesAdapterGenerator::generate:
    for (int i = YuhuInterpreter::method_handle_invoke_FIRST; i <= YuhuInterpreter::method_handle_invoke_LAST; i++) {
        YuhuInterpreter::MethodKind kind = (YuhuInterpreter::MethodKind) i;
        YuhuInterpreter::_entry_table[kind] = YuhuInterpreter::_entry_table[Interpreter::abstract];
    }
}

YuhuInterpreterGenerator::YuhuInterpreterGenerator() {
    _unimplemented_bytecode    = NULL;
    _illegal_bytecode_sequence = NULL;
    generate_all();
}
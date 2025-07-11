//
// Created by Anyou Liu on 2025/4/24.
//

#include "interpreter/yuhu/yuhu_interpreterGenerator.hpp"
#include "interpreter/yuhu/yuhu_templateTable.hpp"

void YuhuInterpreterGenerator::generate_all() {
    {
        YuhuCodeletMark cm(_masm, "yuhu error exits");
        _unimplemented_bytecode    = generate_error_exit("yuhu unimplemented bytecode");
        _illegal_bytecode_sequence = generate_error_exit("yuhu illegal bytecode sequence - method not verified");
    }
    {
        YuhuCodeletMark cm(_masm, "yuhu return entry points");
        const int index_size = sizeof(u2);
        for (int i = 0; i < number_of_states; i++) {
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

//    initialize_method_handle_entries();

    // all native method kinds (must be one contiguous block)
//    Interpreter::_native_entry_begin = Interpreter::code()->code_end();
//    method_entry(native)
//    method_entry(native_synchronized)
//    Interpreter::_native_entry_end = Interpreter::code()->code_end();

#undef method_entry

    // Bytecodes
    set_entry_points_for_all_bytes();
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

YuhuInterpreterGenerator::YuhuInterpreterGenerator() {
    _unimplemented_bytecode    = NULL;
    _illegal_bytecode_sequence = NULL;
    generate_all();
}
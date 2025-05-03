//
// Created by Anyou Liu on 2025/4/24.
//

#include "interpreter/yuhu/yuhu_interpreterGenerator.hpp"

# define __ _masm->

void YuhuInterpreterGenerator::generate_all() {
    {
        YuhuCodeletMark cm(_masm, "yuhu error exits");
        _unimplemented_bytecode    = generate_error_exit("yuhu unimplemented bytecode");
        _illegal_bytecode_sequence = generate_error_exit("yuhu illegal bytecode sequence - method not verified");
    }
}

address YuhuInterpreterGenerator::generate_error_exit(const char* msg) {
    address entry = __ current_pc();
    __ write_insts_stop(msg);
    return entry;
}

YuhuInterpreterGenerator::YuhuInterpreterGenerator() {
    _unimplemented_bytecode    = NULL;
    _illegal_bytecode_sequence = NULL;
    generate_all();
}
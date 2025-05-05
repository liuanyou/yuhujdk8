//
// Created by Anyou Liu on 2025/4/24.
//

#include "interpreter/yuhu/yuhu_interpreterGenerator.hpp"
#include "interpreter/yuhu/yuhu_templateTable.hpp"

# define __ _masm->

void YuhuInterpreterGenerator::generate_all() {
    {
        YuhuCodeletMark cm(_masm, "yuhu error exits");
        _unimplemented_bytecode    = generate_error_exit("yuhu unimplemented bytecode");
        _illegal_bytecode_sequence = generate_error_exit("yuhu illegal bytecode sequence - method not verified");
    }
    {
        YuhuCodeletMark cm(_masm, "return entry points");
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

    // Bytecodes
    set_entry_points_for_all_bytes();
}

address YuhuInterpreterGenerator::generate_error_exit(const char* msg) {
    address entry = __ current_pc();
    __ write_insts_stop(msg);
    return entry;
}

address YuhuInterpreterGenerator::generate_return_entry_for(TosState state, int step, size_t index_size) {
    address entry = __ pc();
    // TODO
//    // Restore stack bottom in case i2c adjusted stack
//    __ ldr(esp, Address(rfp, frame::interpreter_frame_last_sp_offset * wordSize));
//    // and NULL it as marker that esp is now tos until next java call
//    __ str(zr, Address(rfp, frame::interpreter_frame_last_sp_offset * wordSize));
//    __ restore_bcp();
//    __ restore_locals();
//    __ restore_constant_pool_cache();
//    __ get_method(rmethod);
//
//    // Pop N words from the stack
//    __ get_cache_and_index_at_bcp(r1, r2, 1, index_size);
//    __ ldr(r1, Address(r1, ConstantPoolCache::base_offset() + ConstantPoolCacheEntry::flags_offset()));
//    __ andr(r1, r1, ConstantPoolCacheEntry::parameter_size_mask);
//
//    __ add(esp, esp, r1, Assembler::LSL, 3);
//
//    // Restore machine SP
//    __ ldr(rscratch1, Address(rmethod, Method::const_offset()));
//    __ ldrh(rscratch1, Address(rscratch1, ConstMethod::max_stack_offset()));
//    __ add(rscratch1, rscratch1, frame::interpreter_frame_monitor_size() + 2);
//    __ ldr(rscratch2,
//           Address(rfp, frame::interpreter_frame_initial_sp_offset * wordSize));
//    __ sub(rscratch1, rscratch2, rscratch1, ext::uxtw, 3);
//    __ andr(sp, rscratch1, -16);
//
//    __ get_dispatch();
//    __ dispatch_next(state, step);

    return entry;
}

void YuhuInterpreterGenerator::generate_and_dispatch(YuhuTemplate* t, TosState tos_out) {
//    if (PrintBytecodeHistogram)                                    histogram_bytecode(t);
//#ifndef PRODUCT
//    // debugging code
//    if (CountBytecodes || TraceBytecodes || StopInterpreterAt > 0) count_bytecode();
//    if (PrintBytecodePairHistogram)                                histogram_bytecode_pair(t);
//    if (TraceBytecodes)                                            trace_bytecode(t);
//    if (StopInterpreterAt > 0)                                     stop_interpreter_at();
//    __ verify_FPU(1, t->tos_in());
//#endif // !PRODUCT
    int step;
    if (!t->does_dispatch()) {
        step = t->is_wide() ? Bytecodes::wide_length_for(t->bytecode()) : Bytecodes::length_for(t->bytecode());
        if (tos_out == ilgl) tos_out = t->tos_out();
        // compute bytecode size
        assert(step > 0, "just checkin'");
        // setup stuff for dispatching next bytecode
//        if (ProfileInterpreter && VerifyDataPointer
//            && MethodData::bytecode_has_profile(t->bytecode())) {
//            __ verify_method_data_pointer();
//        }
        // do nothing
    }
    // generate template
    t->generate(_masm);
    // advance
    if (t->does_dispatch()) {
#ifdef ASSERT
        // make sure execution doesn't go beyond this point if code is broken
        __ write_insts_stop("should not reach here");
#endif // ASSERT
    } else {
        // dispatch to next bytecode
        __ write_insts_dispatch_next(tos_out, step);
    }
}

void YuhuInterpreterGenerator::set_vtos_entry_points(YuhuTemplate* t,
                                                         address& bep,
                                                         address& cep,
                                                         address& sep,
                                                         address& aep,
                                                         address& iep,
                                                         address& lep,
                                                         address& fep,
                                                         address& dep,
                                                         address& vep) {
    assert(t->is_valid() && t->tos_in() == vtos, "illegal template");

    aep = __ current_pc();
    // pre mode
    __ write_inst("str x0, [x20, #-8]!");
    __ write_inst("b #32");

    fep = __ current_pc();
    __ write_inst("str s0, [x20, #-8]!");
    __ write_inst("b #24");

    dep = __ current_pc();
    __ write_inst("str d0, [x20, #-16]!");
    __ write_inst("b #16");

    lep = __ current_pc();
    __ write_inst("str x0, [x20, #-16]!");
    __ write_inst("b #8");

    bep = cep = sep =
    iep = __ current_pc();
    __ write_inst("str w0, [x20, #-8]!");

    vep = __ current_pc();
    generate_and_dispatch(t);
}

void YuhuInterpreterGenerator::set_short_entry_points(YuhuTemplate* t, address& bep, address& cep, address& sep, address& aep, address& iep, address& lep, address& fep, address& dep, address& vep) {
    assert(t->is_valid(), "template must exist");
    switch (t->tos_in()) {
        case btos:
        case ctos:
        case stos:
            ShouldNotReachHere();  // btos/ctos/stos should use itos.
            break;
        case atos:
            vep = __ current_pc();
            __ write_inst("ldr x0, [x20], #8");
            aep = __ current_pc();
            generate_and_dispatch(t);
            break;
        case itos:
            vep = __ current_pc();
            __ write_inst("ldr w0, [x20], #8");
            iep = __ current_pc();
            generate_and_dispatch(t);
            break;
        case ltos:
            vep = __ current_pc();
            __ write_inst("ldr x0, [x20], #16");
            lep = __ current_pc();
            generate_and_dispatch(t);
            break;
        case ftos:
            vep = __ current_pc();
            __ write_inst("ldr s0, [x20], #8");
            fep = __ current_pc();
            generate_and_dispatch(t);
            break;
        case dtos:
            vep = __ current_pc();
            __ write_inst("ldr d0, [x20], #16");
            dep = __ current_pc();
            generate_and_dispatch(t);
            break;
        case vtos:
            set_vtos_entry_points(t, bep, cep, sep, aep, iep, lep, fep, dep, vep);
            break;
        default  :
            ShouldNotReachHere();
            break;
    }
}

void YuhuInterpreterGenerator::set_wide_entry_point(YuhuTemplate* t, address& wep) {
    assert(t->is_valid(), "template must exist");
    assert(t->tos_in() == vtos, "only vtos tos_in supported for wide instructions");
    wep = __ current_pc();
    generate_and_dispatch(t);
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
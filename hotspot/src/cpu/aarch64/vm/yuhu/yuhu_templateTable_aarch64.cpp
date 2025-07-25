//
// Created by Anyou Liu on 2025/7/22.
//
#include "precompiled.hpp"
#include "asm/yuhu/yuhu_macroAssembler.hpp"
#include "interpreter/yuhu/yuhu_interpreter.hpp"
#include "interpreter/interpreterRuntime.hpp"
#include "interpreter/yuhu/yuhu_templateTable.hpp"
#include "memory/universe.inline.hpp"
#include "oops/methodData.hpp"
#include "oops/method.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/oop.inline.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/synchronizer.hpp"

#define __ _masm->

// Individual instructions

void YuhuTemplateTable::nop() {
    transition(vtos, vtos);
    // nothing to do
}

void YuhuTemplateTable::aconst_null() {
    transition(vtos, atos);

    __ write_insts_mov_imm64(__ x0, 0);
}

void YuhuTemplateTable::iconst(int value)
{
    transition(vtos, itos);

    __ write_insts_mov_imm64(__ x0, value);
}

void YuhuTemplateTable::lconst(int value)
{
    __ write_insts_mov_imm64(__ x0, value);
}

void YuhuTemplateTable::fconst(int value)
{
    transition(vtos, ftos);
    switch (value) {
        case 0:
            __ write_inst_fmov_reg(__ s0, __ wzr);
            break;
        case 1:
            __ write_inst("fmov s0, #1.0");
            break;
        case 2:
            __ write_inst("fmov s0, #2.0");
            break;
        default:
            ShouldNotReachHere();
            break;
    }
}

void YuhuTemplateTable::dconst(int value)
{
    transition(vtos, dtos);
    switch (value) {
        case 0:
            __ write_inst_fmov_reg(__ d0, __ xzr);
            break;
        case 1:
            __ write_inst("fmov d0, #1.0");
            break;
        case 2:
            __ write_inst("fmov d0, #2.0");
            break;
        default:
            ShouldNotReachHere();
            break;
    }
}

void YuhuTemplateTable::bipush()
{
    transition(vtos, itos);
    __ write_inst("ldrsb w0, [x22, #%d]", 1);
}

void YuhuTemplateTable::sipush()
{
    transition(vtos, itos);
    __ write_inst("ldrh w0, [x22, #%d]", 1);
    __ write_inst("rev w0, w0");
    __ write_inst("asr w0, w0, #%d", 16);
}

void YuhuTemplateTable::ldc(bool wide)
{
    transition(vtos, vtos);
    YuhuLabel call_ldc, notFloat, notClass, Done;

    if (wide) {
        __ write_insts_get_unsigned_2_byte_index_at_bcp(__ w1, 1);
    } else {
        __ write_insts_load_unsigned_byte(__ w1, __ x22, 1);
    }
    __ write_insts_get_cpool_and_tags(__ x2, __ x0);

    const int base_offset = ConstantPool::header_size() * wordSize;
    const int tags_offset = Array<u1>::base_offset_in_bytes();

    // get type
    __ write_inst("add x3, x1, #%d", tags_offset);
    __ write_inst("add x3, x0, x3"); // __ lea(r3, Address(r0, r3));
    __ write_inst("ldarb w3, [x3]");

    // unresolved class - get the resolved class
    __ write_inst("cmp x3, #%d", JVM_CONSTANT_UnresolvedClass);
    __ write_inst_b(__ eq, call_ldc);

    // unresolved class in error state - call into runtime to throw the error
    // from the first resolution attempt
    __ write_inst("cmp x3, #%d", JVM_CONSTANT_UnresolvedClassInError);
    __ write_inst_b(__ eq, call_ldc);

    // resolved class - need to call vm to get java mirror of the class
    __ write_inst("cmp x3, #%d", JVM_CONSTANT_Class);
    __ write_inst_b(__ ne, notClass);

    __ pin_label(call_ldc);
    __ write_insts_mov_imm64(__ x1, wide);
    __ write_insts_final_call_VM(__ x0, CAST_FROM_FN_PTR(address, InterpreterRuntime::ldc), __ x1);
    __ write_inst_push_ptr(__ x0);
    __ write_insts_verify_oop(__ x0, "broken oop");
    __ write_inst_b(Done);

    __ pin_label(notClass);
    __ write_inst("cmp x3, #%d", JVM_CONSTANT_Float);
    __ write_inst_b(__ ne, notFloat);
    // ftos
    __ write_inst("adds x1, x2, x1, lsl #3");
    __ write_inst("ldr s0, [x1, #%d]", base_offset);
    __ write_inst_push_f();
    __ write_inst_b(Done);

    __ pin_label(notFloat);
#ifdef ASSERT
    {
        YuhuLabel L;
        __ write_inst("cmp x3, #%d", JVM_CONSTANT_Integer);
        __ write_inst_b(__ eq, L);
        // String and Object are rewritten to fast_aldc
        __ write_insts_stop("unexpected tag type in ldc");
        __ pin_label(L);
    }
#endif
    // itos JVM_CONSTANT_Integer only
    __ write_inst("adds x1, x2, x1, lsl #3");
    __ write_inst("ldr w0, [x1, #%d]", base_offset);
    __ write_inst_push_i(__ x0);
    __ pin_label(Done);
}

void YuhuTemplateTable::ldc2_w()
{
    transition(vtos, vtos);
    YuhuLabel Long, Done;
    __ write_insts_get_unsigned_2_byte_index_at_bcp(__ w0, 1);

    __ write_insts_get_cpool_and_tags(__ x1, __ x2);
    const int base_offset = ConstantPool::header_size() * wordSize;
    const int tags_offset = Array<u1>::base_offset_in_bytes();

    // get type
    __ write_inst("add x2, x2, x0, lsl #0"); // __ lea(r2, Address(r2, r0, Address::lsl(0)));
    __ write_insts_load_unsigned_byte(__ w2, __ x2, tags_offset);
    __ write_inst("cmp w2, #%d", (int)JVM_CONSTANT_Double);
    __ write_inst_b(__ ne, Long);
    // dtos
    __ write_inst("add x2, x1, x0, lsl #3"); // __ lea (r2, Address(r1, r0, Address::lsl(3)));
    __ write_inst("ldr d0, [x2, #%d]", base_offset);
    __ write_inst_push_d();
    __ write_inst_b(Done);

    __ pin_label(Long);
    // ltos
    __ write_inst("add x0, x1, x0, lsl #3"); // __ lea(r0, Address(r1, r0, Address::lsl(3)));
    __ write_inst("ldr x0, [x0, #%d]", base_offset);
    __ write_inst_push_l();

    __ pin_label(Done);
}
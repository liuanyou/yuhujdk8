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

// Forward declarations
static void do_oop_store(YuhuMacroAssembler* _masm,
                         YuhuAddress obj,
                         YuhuMacroAssembler::YuhuRegister val,
                         BarrierSet::Name barrier,
                         bool precise);

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

void YuhuTemplateTable::iload()
{
    transition(vtos, itos);
    if (RewriteFrequentPairs) {
        YuhuLabel rewrite, done;
        YuhuMacroAssembler::YuhuRegister bc = __ x4;

        // get next bytecode
        __ write_insts_load_unsigned_byte(__ w1, __ x22, Bytecodes::length_for(Bytecodes::_iload));

        // if _iload, wait to rewrite to iload2.  We only want to rewrite the
        // last two iloads in a pair.  Comparing against fast_iload means that
        // the next bytecode is neither an iload or a caload, and therefore
        // an iload pair.
        __ write_inst("cmp w1, #%d", Bytecodes::_iload);
        __ write_inst_b(__ eq, done);

        // if _fast_iload rewrite to _fast_iload2
        __ write_inst("cmp w1, #%d", Bytecodes::_fast_iload);
        __ write_insts_mov_imm32(__ w_reg(bc), Bytecodes::_fast_iload2);
        __ write_inst_b(__ eq, rewrite);

        // if _caload rewrite to _fast_icaload
        __ write_inst("cmp w1, #%d", Bytecodes::_caload);
        __ write_insts_mov_imm32(__ w_reg(bc), Bytecodes::_fast_icaload);
        __ write_inst_b(__ eq, rewrite);

        // else rewrite to _fast_iload
        __ write_insts_mov_imm32(__ w_reg(bc), Bytecodes::_fast_iload);

        // rewrite
        // bc: new bytecode
        __ pin_label(rewrite);
        patch_bytecode(Bytecodes::_iload, bc, __ x1, false);
        __ pin_label(done);

    }

    // do iload, get the local value into tos
    locals_index(__ x1);
    __ write_inst("ldr x0, [x24, x1, lsl #3]");
}

void YuhuTemplateTable::lload()
{
    transition(vtos, ltos);
    __ write_inst("ldrb w1, [x22, #%s]", 1);
    __ write_inst("sub x1, x24, w1, uxtw #%d", LogBytesPerWord);
    __ write_inst("ldr x0, [x1, #%d]", YuhuInterpreter::local_offset_in_bytes(1));
}

void YuhuTemplateTable::fload()
{
    transition(vtos, ftos);
    locals_index(__ x1);
    // n.b. we use ldrd here because this is a 64 bit slot
    // this is comparable to the iload case
    __ write_inst("ldr d0, [x24, x1, lsl #3]");
}

void YuhuTemplateTable::dload()
{
    transition(vtos, dtos);
    __ write_inst("ldrb w1, [x22, #%d]", 1);
    __ write_inst("sub x1, x24, w1, uxtw #%d", LogBytesPerWord);
    __ write_inst("ldr d0, [x1, #%d]", YuhuInterpreter::local_offset_in_bytes(1));
}

void YuhuTemplateTable::aload()
{
    transition(vtos, atos);
    locals_index(__ x1);
    __ write_inst("ldr x0, [x24, x1, lsl #3]");
}

void YuhuTemplateTable::iload(int n)
{
    transition(vtos, itos);
    __ write_inst("ldr x0, [x24, #%d]", YuhuInterpreter::local_offset_in_bytes(n));
}

void YuhuTemplateTable::lload(int n)
{
    transition(vtos, ltos);
    __ write_inst("ldr x0, [x24, #%d]", YuhuInterpreter::local_offset_in_bytes(n + 1));
}

void YuhuTemplateTable::fload(int n)
{
    transition(vtos, ftos);
    __ write_inst("ldr s0, [x24, #%d]", YuhuInterpreter::local_offset_in_bytes(n));
}

void YuhuTemplateTable::dload(int n)
{
    transition(vtos, dtos);
    __ write_inst("ldr d0, [x24, #%d]", YuhuInterpreter::local_offset_in_bytes(n + 1));
}

void YuhuTemplateTable::aload_0()
{
    // According to bytecode histograms, the pairs:
    //
    // _aload_0, _fast_igetfield
    // _aload_0, _fast_agetfield
    // _aload_0, _fast_fgetfield
    //
    // occur frequently. If RewriteFrequentPairs is set, the (slow)
    // _aload_0 bytecode checks if the next bytecode is either
    // _fast_igetfield, _fast_agetfield or _fast_fgetfield and then
    // rewrites the current bytecode into a pair bytecode; otherwise it
    // rewrites the current bytecode into _fast_aload_0 that doesn't do
    // the pair check anymore.
    //
    // Note: If the next bytecode is _getfield, the rewrite must be
    //       delayed, otherwise we may miss an opportunity for a pair.
    //
    // Also rewrite frequent pairs
    //   aload_0, aload_1
    //   aload_0, iload_1
    // These bytecodes with a small amount of code are most profitable
    // to rewrite
    if (RewriteFrequentPairs) {
        YuhuLabel rewrite, done;
        const YuhuMacroAssembler::YuhuRegister bc = __ x4;

        // get next bytecode
        __ write_insts_load_unsigned_byte(__ w1, __ x22, Bytecodes::length_for(Bytecodes::_aload_0));

        // do actual aload_0
        aload(0);

        // if _getfield then wait with rewrite
        __ write_inst("cmp w1, #%d", Bytecodes::Bytecodes::_getfield);
        __ write_inst_b(__ eq, done);

        // if _igetfield then reqrite to _fast_iaccess_0
        assert(Bytecodes::java_code(Bytecodes::_fast_iaccess_0) == Bytecodes::_aload_0, "fix bytecode definition");
        __ write_inst("cmp w1, #%d", Bytecodes::_fast_igetfield);
        __ write_insts_mov_imm32(__ w_reg(bc), Bytecodes::_fast_iaccess_0);
        __ write_inst_b(__ eq, rewrite);

        // if _agetfield then reqrite to _fast_aaccess_0
        assert(Bytecodes::java_code(Bytecodes::_fast_aaccess_0) == Bytecodes::_aload_0, "fix bytecode definition");
        __ write_inst("cmp w1, #%d", Bytecodes::_fast_agetfield);
        __ write_insts_mov_imm32(__ w_reg(bc), Bytecodes::_fast_aaccess_0);
        __ write_inst_b(__ eq, rewrite);

        // if _fgetfield then reqrite to _fast_faccess_0
        assert(Bytecodes::java_code(Bytecodes::_fast_faccess_0) == Bytecodes::_aload_0, "fix bytecode definition");
        __ write_inst("cmp w1, #%d", Bytecodes::_fast_fgetfield);
        __ write_insts_mov_imm32(__ w_reg(bc), Bytecodes::_fast_faccess_0);
        __ write_inst_b(__ eq, rewrite);

        // else rewrite to _fast_aload0
        assert(Bytecodes::java_code(Bytecodes::_fast_aload_0) == Bytecodes::_aload_0, "fix bytecode definition");
        __ write_insts_mov_imm32(__ w_reg(bc), Bytecodes::Bytecodes::_fast_aload_0);

        // rewrite
        // bc: new bytecode
        __ pin_label(rewrite);
        patch_bytecode(Bytecodes::_aload_0, bc, __ x1, false);

        __ pin_label(done);
    } else {
        aload(0);
    }
}

void YuhuTemplateTable::aload(int n)
{
    transition(vtos, atos);
    __ write_inst("ldr x0, [x24, #%d]", YuhuInterpreter::local_offset_in_bytes(n));
}

void YuhuTemplateTable::iaload()
{
    transition(itos, itos);
    __ write_inst("mov x1, x0");
    __ write_inst_pop_ptr(__ x0);
    // r0: array
    // r1: index
    index_check(__ x0, __ x1); // leaves index in r1, kills rscratch1
    __ write_inst("add x1, x0, w1, uxtw #2"); // __ lea(r1, Address(r0, r1, Address::uxtw(2)));
    __ write_inst("ldr w0, [x1, #%d]", arrayOopDesc::base_offset_in_bytes(T_INT));
}

void YuhuTemplateTable::laload()
{
    transition(itos, ltos);
    __ write_inst("mov x1, x0");
    __ write_inst_pop_ptr(__ x0);
    // r0: array
    // r1: index
    index_check(__ x0, __ x1); // leaves index in r1, kills rscratch1
    __ write_inst("add x1, x0, w1, uxtw #3"); // __ lea(r1, Address(r0, r1, Address::uxtw(3)));
    __ write_inst("ldr x0, [x1, #%d]", arrayOopDesc::base_offset_in_bytes(T_LONG));
}

void YuhuTemplateTable::faload()
{
    transition(itos, ftos);
    __ write_inst("mov x1, x0");
    __ write_inst_pop_ptr(__ x0);
    // r0: array
    // r1: index
    index_check(__ x0, __ x1); // leaves index in r1, kills rscratch1
    __ write_inst("add x1, x0, w1, uxtw #3"); // __ lea(r1,  Address(r0, r1, Address::uxtw(2)));
    __ write_inst("ldr s0, [x1, #%d]", arrayOopDesc::base_offset_in_bytes(T_FLOAT));
}

void YuhuTemplateTable::daload()
{
    transition(itos, dtos);
    __ write_inst("mov x1, x0");
    __ write_inst_pop_ptr(__ x0);
    // r0: array
    // r1: index
    index_check(__ x0, __ x1); // leaves index in r1, kills rscratch1
    __ write_inst("add x1, x0, w1, uxtw #3"); // __ lea(r1,  Address(r0, r1, Address::uxtw(3)));
    __ write_inst("ldr s0, [x1, #%d]", arrayOopDesc::base_offset_in_bytes(T_DOUBLE));
}

void YuhuTemplateTable::aaload()
{
    transition(itos, atos);
    __ write_inst("mov x1, x0");
    __ write_inst_pop_ptr(__ x0);
    // r0: array
    // r1: index
    index_check(__ x0, __ x1); // leaves index in r1, kills rscratch1
    int s = (UseCompressedOops ? 2 : 3);
    __ write_inst("add x1, x0, w1, uxtw #%d", s); // __ lea(r1, Address(r0, r1, Address::uxtw(s)));
    __ write_insts_load_heap_oop(__ x0, __ x1, arrayOopDesc::base_offset_in_bytes(T_OBJECT));
}

void YuhuTemplateTable::baload()
{
    transition(itos, itos);
    __ write_inst("mov x1, x0");
    __ write_inst_pop_ptr(__ x0);
    // r0: array
    // r1: index
    index_check(__ x0, __ x1); // leaves index in r1, kills rscratch1
    __ write_inst("add x1, x0, w1, uxtw #0"); // __ lea(r1,  Address(r0, r1, Address::uxtw(0)));
    __ write_insts_load_signed_byte(__ x0, __ x1, arrayOopDesc::base_offset_in_bytes(T_BYTE));
}

void YuhuTemplateTable::caload()
{
    transition(itos, itos);
    __ write_inst("mov x1, x0");
    __ write_inst_pop_ptr(__ x0);
    // r0: array
    // r1: index
    index_check(__ x0, __ x1); // leaves index in r1, kills rscratch1
    __ write_inst("add x1, x0, w1, uxtw #1"); // __ lea(r1,  Address(r0, r1, Address::uxtw(1)));
    __ write_insts_load_unsigned_short(__ w0, __ x1, arrayOopDesc::base_offset_in_bytes(T_CHAR));
}

void YuhuTemplateTable::saload()
{
    transition(itos, itos);
    __ write_inst("mov x1, x0");
    __ write_inst_pop_ptr(__ x0);
    // r0: array
    // r1: index
    index_check(__ x0, __ x1); // leaves index in r1, kills rscratch1
    __ write_inst("add x1, x0, w1, uxtw #1"); // __ lea(r1,  Address(r0, r1, Address::uxtw(1)));
    __ write_insts_load_unsigned_short(__ w0, __ x1, arrayOopDesc::base_offset_in_bytes(T_SHORT));
}

void YuhuTemplateTable::istore()
{
    transition(itos, vtos);
    locals_index(__ x1);
    // FIXME: We're being very pernickerty here storing a jint in a
    // local with strw, which costs an extra instruction over what we'd
    // be able to do with a simple str.  We should just store the whole
    // word.
    __ write_inst("add x8, x24, x1, lsl #3"); // __ lea(rscratch1, iaddress(r1));
    __ write_inst("str w0, [x8]");
}

void YuhuTemplateTable::lstore()
{
    transition(ltos, vtos);
    locals_index(__ x1);
    __ write_inst("add x8, x24, x1, lsl #3");
    __ write_inst("str x0, [x8, #%d]", YuhuInterpreter::local_offset_in_bytes(1));
}

void YuhuTemplateTable::fstore() {
    transition(ftos, vtos);
    locals_index(__ x1);
    __ write_inst("add x8, x24, x1, lsl #3"); // __ lea(rscratch1, iaddress(r1));
    __ write_inst("str s0, [x8]");
}

void YuhuTemplateTable::dstore() {
    transition(dtos, vtos);
    locals_index(__ x1);
    __ write_inst("add x8, x24, x1, lsl #3");
    __ write_inst("str d0, [x8, #%d]", YuhuInterpreter::local_offset_in_bytes(1));
}

void YuhuTemplateTable::astore()
{
    transition(vtos, vtos);
    __ write_inst_pop_ptr(__ x0);
    locals_index(__ x1);
    __ write_inst("add x8, x24, x1, lsl #3");
    __ write_inst("str x0, [x8]");
}

void YuhuTemplateTable::istore(int n)
{
    transition(itos, vtos);
    __ write_inst("str x0, [x24, #%d]", YuhuInterpreter::local_offset_in_bytes(n));
}

void YuhuTemplateTable::lstore(int n)
{
    transition(ltos, vtos);
    __ write_inst("str x0, [x24, #%d]", YuhuInterpreter::local_offset_in_bytes(n+1));
}

void YuhuTemplateTable::fstore(int n)
{
    transition(ftos, vtos);
    __ write_inst("str s0, [x24, #%d]", YuhuInterpreter::local_offset_in_bytes(n));
}

void YuhuTemplateTable::dstore(int n)
{
    transition(dtos, vtos);
    __ write_inst("str d0, [x24, #%d]", YuhuInterpreter::local_offset_in_bytes(n+1));
}

void YuhuTemplateTable::astore(int n)
{
    transition(vtos, vtos);
    __ write_inst_pop_ptr(__ x0);
    __ write_inst("str x0, [x24, #%d]", YuhuInterpreter::local_offset_in_bytes(n));
}

void YuhuTemplateTable::iastore() {
    transition(itos, vtos);
    __ write_inst_pop_i(__ x1);
    __ write_inst_pop_ptr(__ x3);
    // r0: value
    // r1: index
    // r3: array
    index_check(__ x3, __ x1); // prefer index in r1
    __ write_inst("add x8, x3, w1, uxtw #2"); // __ lea(rscratch1, Address(r3, r1, Address::uxtw(2)));
    __ write_inst("str w0, [x8, #%d]", arrayOopDesc::base_offset_in_bytes(T_INT));
}

void YuhuTemplateTable::lastore() {
    transition(ltos, vtos);
    __ write_inst_pop_i(__ x1);
    __ write_inst_pop_ptr(__ x3);
    // r0: value
    // r1: index
    // r3: array
    index_check(__ x3, __ x1); // prefer index in r1
    __ write_inst("add x8, x3, w1, uxtw #3"); // __ lea(rscratch1, Address(r3, r1, Address::uxtw(3)));
    __ write_inst("str x0, [x8, #%d]", arrayOopDesc::base_offset_in_bytes(T_LONG));
}

void YuhuTemplateTable::fastore() {
    transition(ftos, vtos);
    __ write_inst_pop_i(__ x1);
    __ write_inst_pop_ptr(__ x3);
    // v0: value
    // r1:  index
    // r3:  array
    index_check(__ x3, __ x1); // prefer index in r1
    __ write_inst("add x8, x3, w1, uxtw #2"); // __ lea(rscratch1, Address(r3, r1, Address::uxtw(2)));
    __ write_inst("str s0, [x8, #%d]", arrayOopDesc::base_offset_in_bytes(T_FLOAT));
}

void YuhuTemplateTable::dastore() {
    transition(dtos, vtos);
    __ write_inst_pop_i(__ x1);
    __ write_inst_pop_ptr(__ x3);
    // v0: value
    // r1:  index
    // r3:  array
    index_check(__ x3, __ x1); // prefer index in r1
    __ write_inst("add x8, x3, w1, uxtw #3"); // __ lea(rscratch1, Address(r3, r1, Address::uxtw(3)));
    __ write_inst("str d0, [x8, #%d]", arrayOopDesc::base_offset_in_bytes(T_DOUBLE));
}

void YuhuTemplateTable::aastore() {
    YuhuLabel is_null, ok_is_subtype, done;
    transition(vtos, vtos);
    // stack: ..., array, index, value
    __ write_inst("ldr x0, [x20, #%d]", YuhuInterpreter::expr_offset_in_bytes(0)); // value
    __ write_inst("ldr x2, [x20, #%d]", YuhuInterpreter::expr_offset_in_bytes(1)); // index
    __ write_inst("ldr x3, [x20, #%d]", YuhuInterpreter::expr_offset_in_bytes(2)); // array

    YuhuAddress element_address(__ x4, arrayOopDesc::base_offset_in_bytes(T_OBJECT));

    index_check(__ x3, __ x2);     // kills r1
    __ write_inst("add x4, x3, w2, uxtw #%d", UseCompressedOops? 2 : 3); // __ lea(r4, Address(r3, r2, Address::uxtw(UseCompressedOops? 2 : 3)));

    // do array store check - check for NULL value first
    __ write_inst_cbz(__ x0, is_null);

    // Move subklass into r1
    __ write_insts_load_klass(__ x1, __ x0);
    // Move superklass into r0
    __ write_insts_load_klass(__ x0, __ x3);
    __ write_inst("ldr x0, [x0, #%d]", in_bytes(ObjArrayKlass::element_klass_offset()));
    // Compress array + index*oopSize + 12 into a single register.  Frees r2.

    // Generate subtype check.  Blows r2, r5
    // Superklass in r0.  Subklass in r1.
    __ write_insts_gen_subtype_check(__ x1, ok_is_subtype);

    // Come here on failure
    // object is at TOS
    __ write_inst_b(YuhuInterpreter::_throw_ArrayStoreException_entry);

    // Come here on success
    __ pin_label(ok_is_subtype);

    // Get the value we will store
    __ write_inst("ldr x0, [x20, #%d]", YuhuInterpreter::expr_offset_in_bytes(0));
    // Now store using the appropriate barrier
    do_oop_store(_masm, element_address, __ x0, _bs->kind(), true);
    __ write_inst_b(done);

    // Have a NULL in r0, r3=array, r2=index.  Store NULL at ary[idx]
    __ pin_label(is_null);
    // TODO
//    __ profile_null_seen(r2);

    // Store a NULL
    do_oop_store(_masm, element_address, __ noreg, _bs->kind(), true);

    // Pop stack arguments
    __ pin_label(done);
    __ write_inst("add x20, x20, #%d", 3 * YuhuInterpreter::stackElementSize);
}

static void do_oop_store(YuhuMacroAssembler* _masm,
                         YuhuAddress obj,
                         YuhuMacroAssembler::YuhuRegister val,
                         BarrierSet::Name barrier,
                         bool precise) {
    assert(val == __ noreg || val == __ x0, "parameter is just for looks");
    switch (barrier) {
#if INCLUDE_ALL_GCS
        case BarrierSet::G1SATBCT:
        case BarrierSet::G1SATBCTLogging:
        {
            // flatten object address if needed
            if (obj.index() == __ noreg && obj.offset() == 0) {
                if (obj.base() != __ x3) {
                    __ write_inst_mov_reg(__ x3, obj.base());
                }
            } else {
                __ write_insts_lea(__ x3, obj);
            }
            __ write_insts_g1_write_barrier_pre(__ x3 /* obj */,
                                    __ x1 /* pre_val */,
                                    __ x28 /* thread */,
                                    __ x10  /* tmp */,
                                    val != __ noreg /* tosca_live */,
                                    false /* expand_call */);
            if (val == __ noreg) {
                __ write_insts_store_heap_oop_null(YuhuAddress(__ x3, 0));
            } else {
                // G1 barrier needs uncompressed oop for region cross check.
                YuhuMacroAssembler::YuhuRegister new_val = val;
                if (UseCompressedOops) {
                    new_val = __ x9;
                    __ write_inst_mov_reg(new_val, val);
                }
                __ write_insts_store_heap_oop(YuhuAddress(__ x3, 0), val);
                __ write_insts_g1_write_barrier_post(__ x3 /* store_adr */,
                                         new_val /* new_val */,
                                         __ x28 /* thread */,
                                         __ x10 /* tmp */,
                                         __ x1 /* tmp2 */);
            }

        }
            break;
#endif // INCLUDE_ALL_GCS
        case BarrierSet::CardTableModRef:
        case BarrierSet::CardTableExtension:
        {
            if (val == __ noreg) {
                __ write_insts_store_heap_oop_null(obj);
            } else {
                __ write_insts_store_heap_oop(obj, val);
                // flatten object address if needed
                if (!precise || (obj.index() == __ noreg && obj.offset() == 0)) {
                    __ write_insts_store_check(obj.base());
                } else {
                    __ write_insts_lea(__ x3, obj);
                    __ write_insts_store_check(__ x3);
                }
            }
        }
            break;
        case BarrierSet::ModRef:
        case BarrierSet::Other:
            if (val == __ noreg) {
                __ write_insts_store_heap_oop_null(obj);
            } else {
                __ write_insts_store_heap_oop(obj, val);
            }
            break;
        default      :
            ShouldNotReachHere();

    }
}

void YuhuTemplateTable::patch_bytecode(Bytecodes::Code bc, YuhuMacroAssembler::YuhuRegister bc_reg,
                                   YuhuMacroAssembler::YuhuRegister temp_reg, bool load_bc_into_bc_reg/*=true*/,
                                   int byte_no)
{
    if (!RewriteBytecodes)  return;
    YuhuLabel L_patch_done;

    switch (bc) {
        case Bytecodes::_fast_aputfield:
        case Bytecodes::_fast_bputfield:
//  case Bytecodes::_fast_zputfield:
        case Bytecodes::_fast_cputfield:
        case Bytecodes::_fast_dputfield:
        case Bytecodes::_fast_fputfield:
        case Bytecodes::_fast_iputfield:
        case Bytecodes::_fast_lputfield:
        case Bytecodes::_fast_sputfield:
        {
            // We skip bytecode quickening for putfield instructions when
            // the put_code written to the constant pool cache is zero.
            // This is required so that every execution of this instruction
            // calls out to InterpreterRuntime::resolve_get_put to do
            // additional, required work.
            assert(byte_no == f1_byte || byte_no == f2_byte, "byte_no out of range");
            assert(load_bc_into_bc_reg, "we use bc_reg as temp");
            __ write_insts_get_cache_and_index_and_bytecode_at_bcp(temp_reg, bc_reg, temp_reg, byte_no, 1);
            __ write_insts_mov_imm32(__ w_reg(bc_reg), bc);
            __ write_inst("cmp %s, #%d", __ w_reg(temp_reg), (unsigned) 0);
            __ write_inst_b(__ eq, L_patch_done); // don't patch
        }
            break;
        default:
            assert(byte_no == -1, "sanity");
            // the pair bytecodes have already done the load.
            if (load_bc_into_bc_reg) {
                __ write_insts_mov_imm32(__ w_reg(bc_reg), bc);
            }
    }

    if (JvmtiExport::can_post_breakpoint()) {
        YuhuLabel L_fast_patch;
        // if a breakpoint is present we can't rewrite the stream directly
        __ write_insts_load_unsigned_byte(__ w_reg(temp_reg), __ x22, 0);
        __ write_inst("cmp %s, #%d", __ w_reg(temp_reg), Bytecodes::_breakpoint);
        __ write_inst_b(__ ne, L_fast_patch);
        // Let breakpoint table handling rewrite to quicker bytecode
        __ write_insts_final_call_VM(__ noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::set_original_bytecode_at), __ x12, __ x22, bc_reg);
        __ write_inst_b(L_patch_done);
        __ pin_label(L_fast_patch);
    }

#ifdef ASSERT
    YuhuLabel L_okay;
    __ write_insts_load_unsigned_byte(__ w_reg(temp_reg), __ x22, 0);
    __ write_inst("cmp %s, #%d", __ w_reg(temp_reg), (int) Bytecodes::java_code(bc));
    __ write_inst_b(__ eq, L_okay);
    __ write_inst_regs("cmp %s, %s", temp_reg, bc_reg);
    __ write_inst_b(__ eq, L_okay);
    __ write_insts_stop("patching the wrong bytecode");
    __ pin_label(L_okay);
#endif

    // patch bytecode
    __ write_inst("strb %s, [x22, #%d]", __ w_reg(bc_reg), 0);
    __ pin_label(L_patch_done);
}

void YuhuTemplateTable::locals_index(YuhuMacroAssembler::YuhuRegister reg, int offset)
{
    __ write_inst("ldrb %s, [x22, #%d]", __ w_reg(reg), offset);
    __ write_inst_regs("neg %s, %s", reg, reg);
}

void YuhuTemplateTable::index_check(YuhuMacroAssembler::YuhuRegister array, YuhuMacroAssembler::YuhuRegister index)
{
    // destroys r1, rscratch1
    // check array
    __ write_insts_null_check(array, arrayOopDesc::length_offset_in_bytes());
    // sign extend index for use by indexed load
    // __ movl2ptr(index, index);
    // check index
    YuhuMacroAssembler::YuhuRegister length = __ x8;
    __ write_inst("ldr %s, [%s, #%d]", __ w_reg(length), array, arrayOopDesc::length_offset_in_bytes());
    __ write_inst_regs("cmp %s, %s", __ w_reg(index), __ w_reg(length));
    if (index != __ x1) {
        // ??? convention: move aberrant index into r1 for exception message
        assert(__ x1 != array, "different registers");
        __ write_inst_mov_reg(__ x1, index);
    }
    YuhuLabel ok;
    __ write_inst_b(__ lo, ok);
    __ write_insts_mov_imm64(__ x8, (uint64_t)YuhuInterpreter::_throw_ArrayIndexOutOfBoundsException_entry);
    __ write_inst_br(__ x8);
    __ pin_label(ok);
}
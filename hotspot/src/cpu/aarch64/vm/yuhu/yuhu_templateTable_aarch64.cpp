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

static inline YuhuAddress iaddress(int n) {
    return YuhuAddress(YuhuMacroAssembler::x24, YuhuInterpreter::local_offset_in_bytes(n));
}

static inline YuhuAddress laddress(int n) {
    return iaddress(n + 1);
}

static inline YuhuAddress faddress(int n) {
    return iaddress(n);
}

static inline YuhuAddress daddress(int n) {
    return laddress(n);
}

static inline YuhuAddress aaddress(int n) {
    return iaddress(n);
}

static inline YuhuAddress iaddress(YuhuMacroAssembler::YuhuRegister r) {
    return YuhuAddress(YuhuMacroAssembler::x24, r, YuhuAddress::lsl(3));
}

static inline YuhuAddress laddress(YuhuMacroAssembler::YuhuRegister r, YuhuMacroAssembler::YuhuRegister scratch,
                               YuhuMacroAssembler* _masm) {
    __ write_insts_lea(scratch, YuhuAddress(YuhuMacroAssembler::x24, r, YuhuAddress::lsl(3)));
    return YuhuAddress(scratch, YuhuInterpreter::local_offset_in_bytes(1));
}

static inline YuhuAddress faddress(YuhuMacroAssembler::YuhuRegister r) {
    return iaddress(r);
}

static inline YuhuAddress daddress(YuhuMacroAssembler::YuhuRegister r, YuhuMacroAssembler::YuhuRegister scratch,
                               YuhuMacroAssembler* _masm) {
    return laddress(r, scratch, _masm);
}

static inline YuhuAddress aaddress(YuhuMacroAssembler::YuhuRegister r) {
    return iaddress(r);
}

static inline YuhuAddress at_tos   () {
    return YuhuAddress(YuhuMacroAssembler::x20,  YuhuInterpreter::expr_offset_in_bytes(0));
}

static inline YuhuAddress at_tos_p1() {
    return YuhuAddress(YuhuMacroAssembler::x20,  YuhuInterpreter::expr_offset_in_bytes(1));
}

static inline YuhuAddress at_tos_p2() {
    return YuhuAddress(YuhuMacroAssembler::x20,  YuhuInterpreter::expr_offset_in_bytes(2));
}

static inline YuhuAddress at_tos_p3() {
    return YuhuAddress(YuhuMacroAssembler::x20,  YuhuInterpreter::expr_offset_in_bytes(3));
}

static inline YuhuAddress at_tos_p4() {
    return YuhuAddress(YuhuMacroAssembler::x20,  YuhuInterpreter::expr_offset_in_bytes(4));
}

static inline YuhuAddress at_tos_p5() {
    return YuhuAddress(YuhuMacroAssembler::x20,  YuhuInterpreter::expr_offset_in_bytes(5));
}

static YuhuMacroAssembler::YuhuCond j_not(YuhuTemplateTable::Condition cc) {
    switch (cc) {
        case YuhuTemplateTable::equal        : return YuhuMacroAssembler::ne;
        case YuhuTemplateTable::not_equal    : return YuhuMacroAssembler::eq;
        case YuhuTemplateTable::less         : return YuhuMacroAssembler::ge;
        case YuhuTemplateTable::less_equal   : return YuhuMacroAssembler::gt;
        case YuhuTemplateTable::greater      : return YuhuMacroAssembler::le;
        case YuhuTemplateTable::greater_equal: return YuhuMacroAssembler::lt;
    }
    ShouldNotReachHere();
    return YuhuMacroAssembler::eq;
}

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
    __ write_insts_load_signed_byte32(__ w0, at_bcp(1));
}

void YuhuTemplateTable::sipush()
{
    transition(vtos, itos);
    __ write_insts_load_unsigned_short(__ w0, at_bcp(1));
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
        __ write_insts_load_unsigned_byte(__ w1, YuhuAddress(__ x22, 1));
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
    __ write_insts_lea(__ x2, YuhuAddress(__ x2, __ x0, YuhuAddress::lsl(0)));
    __ write_insts_load_unsigned_byte(__ w2, YuhuAddress( __ x2, tags_offset));
    __ write_inst("cmp w2, #%d", (int)JVM_CONSTANT_Double);
    __ write_inst_b(__ ne, Long);
    // dtos
    __ write_insts_lea(__ x2, YuhuAddress(__ x1, __ x0, YuhuAddress::lsl(3)));
    __ write_inst("ldr d0, [x2, #%d]", base_offset);
    __ write_inst_push_d();
    __ write_inst_b(Done);

    __ pin_label(Long);
    // ltos
    __ write_insts_lea(__ x0, YuhuAddress(__ x1, __ x0, YuhuAddress::lsl(3)));
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
        __ write_insts_load_unsigned_byte(__ w1, YuhuAddress(__ x22, Bytecodes::length_for(Bytecodes::_iload)));

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
    __ write_inst_ldr(__ x0, iaddress(__ x1));
}

void YuhuTemplateTable::lload()
{
    transition(vtos, ltos);
    __ write_inst_ldrb(__ w1, at_bcp(1));
    __ write_inst("sub x1, x24, w1, uxtw #%d", LogBytesPerWord);
    __ write_inst("ldr x0, [x1, #%d]", YuhuInterpreter::local_offset_in_bytes(1));
}

void YuhuTemplateTable::fload()
{
    transition(vtos, ftos);
    locals_index(__ x1);
    // n.b. we use ldrd here because this is a 64 bit slot
    // this is comparable to the iload case
    __ write_inst_ldr(__ d0, faddress(__ x1));
}

void YuhuTemplateTable::dload()
{
    transition(vtos, dtos);
    __ write_inst_ldrb(__ w1, at_bcp(1));
    __ write_inst("sub x1, x24, w1, uxtw #%d", LogBytesPerWord);
    __ write_inst("ldr d0, [x1, #%d]", YuhuInterpreter::local_offset_in_bytes(1));
}

void YuhuTemplateTable::aload()
{
    transition(vtos, atos);
    locals_index(__ x1);
    __ write_inst_ldr(__ x0, iaddress(__ x1));
}

void YuhuTemplateTable::iload(int n)
{
    transition(vtos, itos);
    __ write_inst_ldr(__ x0, iaddress(n));
}

void YuhuTemplateTable::lload(int n)
{
    transition(vtos, ltos);
    __ write_inst_ldr(__ x0, laddress(n));
}

void YuhuTemplateTable::fload(int n)
{
    transition(vtos, ftos);
    __ write_inst_ldr(__ s0, faddress(n));
}

void YuhuTemplateTable::dload(int n)
{
    transition(vtos, dtos);
    __ write_inst_ldr(__ d0, daddress(n));
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
        __ write_insts_load_unsigned_byte(__ w1, YuhuAddress(__ x22, Bytecodes::length_for(Bytecodes::_aload_0)));

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
    __ write_inst_ldr(__ x0, iaddress(n));
}

void YuhuTemplateTable::iaload()
{
    transition(itos, itos);
    __ write_inst("mov x1, x0");
    __ write_inst_pop_ptr(__ x0);
    // r0: array
    // r1: index
    index_check(__ x0, __ x1); // leaves index in r1, kills rscratch1
    __ write_insts_lea(__ x1, YuhuAddress(__ x0, __ w1, YuhuAddress::uxtw(2)));
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
    __ write_insts_lea(__ x1, YuhuAddress(__ x0, __ w1, YuhuAddress::uxtw(3)));
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
    __ write_insts_lea(__ x1, YuhuAddress(__ x0, __ w1, YuhuAddress::uxtw(2)));
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
    __ write_insts_lea(__ x0, YuhuAddress(__ x0, __ w1, YuhuAddress::uxtw(3)));
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
    __ write_insts_lea(__ x1, YuhuAddress(__ x0, __ w1, YuhuAddress::uxtw(s)));
    __ write_insts_load_heap_oop(__ x0, YuhuAddress(__ x1, arrayOopDesc::base_offset_in_bytes(T_OBJECT)));
}

void YuhuTemplateTable::baload()
{
    transition(itos, itos);
    __ write_inst("mov x1, x0");
    __ write_inst_pop_ptr(__ x0);
    // r0: array
    // r1: index
    index_check(__ x0, __ x1); // leaves index in r1, kills rscratch1
    __ write_insts_lea(__ x1, YuhuAddress(__ x0, __ w1, YuhuAddress::uxtw(0)));
    __ write_insts_load_signed_byte(__ w0, YuhuAddress(__ x1, arrayOopDesc::base_offset_in_bytes(T_BYTE)));
}

void YuhuTemplateTable::caload()
{
    transition(itos, itos);
    __ write_inst("mov x1, x0");
    __ write_inst_pop_ptr(__ x0);
    // r0: array
    // r1: index
    index_check(__ x0, __ x1); // leaves index in r1, kills rscratch1
    __ write_insts_lea(__ x1, YuhuAddress(__ x0, __ w1, YuhuAddress::uxtw(1)));
    __ write_insts_load_unsigned_short(__ w0, YuhuAddress(__ x1, arrayOopDesc::base_offset_in_bytes(T_CHAR)));
}

void YuhuTemplateTable::saload()
{
    transition(itos, itos);
    __ write_inst("mov x1, x0");
    __ write_inst_pop_ptr(__ x0);
    // r0: array
    // r1: index
    index_check(__ x0, __ x1); // leaves index in r1, kills rscratch1
    __ write_insts_lea(__ x1, YuhuAddress(__ x0, __ w1, YuhuAddress::uxtw(1)));
    __ write_insts_load_unsigned_short(__ w0, YuhuAddress(__ x1, arrayOopDesc::base_offset_in_bytes(T_SHORT)));
}

void YuhuTemplateTable::istore()
{
    transition(itos, vtos);
    locals_index(__ x1);
    // FIXME: We're being very pernickerty here storing a jint in a
    // local with strw, which costs an extra instruction over what we'd
    // be able to do with a simple str.  We should just store the whole
    // word.
    __ write_insts_lea(__ x8, iaddress(__ x1));
    __ write_inst("str w0, [x8]");
}

void YuhuTemplateTable::lstore()
{
    transition(ltos, vtos);
    locals_index(__ x1);
    __ write_inst_str(__ x0, laddress(__ x1, __ x8, _masm));
}

void YuhuTemplateTable::fstore() {
    transition(ftos, vtos);
    locals_index(__ x1);
    __ write_insts_lea(__ x8, iaddress(__ x1));
    __ write_inst("str s0, [x8]");
}

void YuhuTemplateTable::dstore() {
    transition(dtos, vtos);
    locals_index(__ x1);
    __ write_inst_str(__ d0, daddress(__ x1, __ x8, _masm));
}

void YuhuTemplateTable::astore()
{
    transition(vtos, vtos);
    __ write_inst_pop_ptr(__ x0);
    locals_index(__ x1);
    __ write_inst_str(__ x0, aaddress(__ x1));
}

void YuhuTemplateTable::istore(int n)
{
    transition(itos, vtos);
    __ write_inst_str(__ x0, iaddress(n));
}

void YuhuTemplateTable::lstore(int n)
{
    transition(ltos, vtos);
    __ write_inst_str(__ x0, laddress(n));
}

void YuhuTemplateTable::fstore(int n)
{
    transition(ftos, vtos);
    __ write_inst_str(__ s0, faddress(n));
}

void YuhuTemplateTable::dstore(int n)
{
    transition(dtos, vtos);
    __ write_inst_str(__ d0, daddress(n));
}

void YuhuTemplateTable::astore(int n)
{
    transition(vtos, vtos);
    __ write_inst_pop_ptr(__ x0);
    __ write_inst_str(__ x0, iaddress(n));
}

void YuhuTemplateTable::iastore() {
    transition(itos, vtos);
    __ write_inst_pop_i(__ x1);
    __ write_inst_pop_ptr(__ x3);
    // r0: value
    // r1: index
    // r3: array
    index_check(__ x3, __ x1); // prefer index in r1
    __ write_insts_lea(__ x8, YuhuAddress(__ x3, __ w1, YuhuAddress::uxtw(2)));
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
    __ write_insts_lea(__ x8, YuhuAddress(__ x3, __ w1, YuhuAddress::uxtw(3)));
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
    __ write_insts_lea(__ x8, YuhuAddress(__ x3, __ w1, YuhuAddress::uxtw(2)));
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
    __ write_insts_lea(__ x8, YuhuAddress(__ x3, __ w1, YuhuAddress::uxtw(3)));
    __ write_inst("str d0, [x8, #%d]", arrayOopDesc::base_offset_in_bytes(T_DOUBLE));
}

void YuhuTemplateTable::aastore() {
    YuhuLabel is_null, ok_is_subtype, done;
    transition(vtos, vtos);
    // stack: ..., array, index, value
    __ write_inst_ldr(__ x0, at_tos());    // value
    __ write_inst_ldr(__ x2, at_tos_p1()); // index
    __ write_inst_ldr(__ x3, at_tos_p2()); // array

    YuhuAddress element_address(__ x4, arrayOopDesc::base_offset_in_bytes(T_OBJECT));

    index_check(__ x3, __ x2);     // kills r1
    __ write_insts_lea(__ x4, YuhuAddress(__ x3, __ w2, YuhuAddress::uxtw(UseCompressedOops? 2 : 3)));

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

void YuhuTemplateTable::bastore()
{
    transition(itos, vtos);
    __ write_inst_pop_i(__ x1);
    __ write_inst_pop_ptr(__ x3);
    // r0: value
    // r1: index
    // r3: array
    index_check(__ x3, __ x1); // prefer index in r1

    // Need to check whether array is boolean or byte
    // since both types share the bastore bytecode.
    __ write_insts_load_klass(__ x2, __ x3);
    __ write_inst("ldr w2, [x2, #%d]", in_bytes(Klass::layout_helper_offset()));
    int diffbit = Klass::layout_helper_boolean_diffbit();
    __ write_inst("and w8, w2, #%d", diffbit);
    YuhuLabel L_skip;
    __ write_inst_cbz(__ w8, L_skip);
    __ write_inst("and w0, w0, #%d", 1); // if it is a T_BOOLEAN array, mask the stored value to 0/1
    __ pin_label(L_skip);

    __ write_insts_lea(__ x8, YuhuAddress(__ x3, __ w1, YuhuAddress::uxtw(0)));
    __ write_inst("strb w0, [x8, #%d]", arrayOopDesc::base_offset_in_bytes(T_BYTE));
}

void YuhuTemplateTable::castore()
{
    transition(itos, vtos);
    __ write_inst_pop_i(__ x1);
    __ write_inst_pop_ptr(__ x3);
    // r0: value
    // r1: index
    // r3: array
    index_check(__ x3, __ x1); // prefer index in r1
    __ write_insts_lea(__ x8, YuhuAddress(__ x3, __ w1, YuhuAddress::uxtw(1)));
    __ write_inst("strh w0, [x8, #%d]", arrayOopDesc::base_offset_in_bytes(T_CHAR));
}

void YuhuTemplateTable::sastore()
{
    castore();
}

void YuhuTemplateTable::pop()
{
    transition(vtos, vtos);
    __ write_inst("add x20, x20, #%d", YuhuInterpreter::stackElementSize);
}

void YuhuTemplateTable::pop2()
{
    transition(vtos, vtos);
    __ write_inst("add x20, x20, #%d", 2 * YuhuInterpreter::stackElementSize);
}

void YuhuTemplateTable::dup()
{
    transition(vtos, vtos);
    __ write_inst_ldr(__ x0, YuhuAddress(__ x20, 0));
    __ write_inst_push(__ x0);
    // stack: ..., a, a
}

void YuhuTemplateTable::dup_x1()
{
    transition(vtos, vtos);
    // stack: ..., a, b
    __ write_inst_ldr(__ x0, at_tos());  // load b
    __ write_inst_ldr(__ x2, at_tos_p1());  // load a
    __ write_inst_str(__ x0, at_tos_p1());  // store b
    __ write_inst_str(__ x2, at_tos());  // store a
    __ write_inst_push(__ x0);                  // push b
    // stack: ..., b, a, b
}

void YuhuTemplateTable::dup_x2()
{
    transition(vtos, vtos);
    // stack: ..., a, b, c
    __ write_inst_ldr(__ x0, at_tos());  // load c
    __ write_inst_ldr(__ x2, at_tos_p2());  // load a
    __ write_inst_str(__ x0, at_tos_p2());  // store c in a
    __ write_inst_push(__ x0);      // push c
    // stack: ..., c, b, c, c
    __ write_inst_ldr(__ x0, at_tos_p2());  // load b
    __ write_inst_str(__ x2, at_tos_p2());  // store a in b
    // stack: ..., c, a, c, c
    __ write_inst_str(__ x0, at_tos_p1());  // store b in c
    // stack: ..., c, a, b, c
}

void YuhuTemplateTable::dup2()
{
    transition(vtos, vtos);
    // stack: ..., a, b
    __ write_inst_ldr(__ x0, at_tos_p1());  // load a
    __ write_inst_push(__ x0);                  // push a
    __ write_inst_ldr(__ x0, at_tos_p1());  // load b
    __ write_inst_push(__ x0);                  // push b
    // stack: ..., a, b, a, b
}

void YuhuTemplateTable::dup2_x1()
{
    transition(vtos, vtos);
    // stack: ..., a, b, c
    __ write_inst_ldr(__ x2, at_tos());  // load c
    __ write_inst_ldr(__ x0, at_tos_p1());  // load b
    __ write_inst_push(__ x0);                  // push b
    __ write_inst_push(__ x2);                  // push c
    // stack: ..., a, b, c, b, c
    __ write_inst_str(__ x2, at_tos_p3());  // store c in b
    // stack: ..., a, c, c, b, c
    __ write_inst_ldr(__ x2, at_tos_p4());  // load a
    __ write_inst_str(__ x2, at_tos_p2());  // store a in 2nd c
    // stack: ..., a, c, a, b, c
    __ write_inst_str(__ x0, at_tos_p4());  // store b in a
    // stack: ..., b, c, a, b, c
}

void YuhuTemplateTable::dup2_x2()
{
    transition(vtos, vtos);
    // stack: ..., a, b, c, d
    __ write_inst_ldr(__ x2, at_tos());  // load d
    __ write_inst_ldr(__ x0, at_tos_p1());  // load c
    __ write_inst_push(__ x0)            ;      // push c
    __ write_inst_push(__ x2);                  // push d
    // stack: ..., a, b, c, d, c, d
    __ write_inst_ldr(__ x0, at_tos_p4());  // load b
    __ write_inst_str(__ x0, at_tos_p2());  // store b in d
    __ write_inst_str(__ x2, at_tos_p4());  // store d in b
    // stack: ..., a, d, c, b, c, d
    __ write_inst_ldr(__ x2, at_tos_p5());  // load a
    __ write_inst_ldr(__ x0, at_tos_p3());  // load c
    __ write_inst_str(__ x2, at_tos_p3());  // store a in c
    __ write_inst_str(__ x0, at_tos_p5());  // store c in a
    // stack: ..., c, d, a, b, c, d
}

void YuhuTemplateTable::swap()
{
    transition(vtos, vtos);
    // stack: ..., a, b
    __ write_inst_ldr(__ x2, at_tos_p1());  // load a
    __ write_inst_ldr(__ x0, at_tos());  // load b
    __ write_inst_str(__ x2, at_tos());  // store a in b
    __ write_inst_str(__ x0, at_tos_p1());  // store b in a
    // stack: ..., b, a
}

void YuhuTemplateTable::iop2(Operation op)
{
    transition(itos, itos);
    // r0 <== r1 op r0
    __ write_inst_pop_i(__ x1);
    switch (op) {
        case add  : __ write_inst("add w0, w1, w0"); break;
        case sub  : __ write_inst("sub w0, w1, w0"); break;
        case mul  : __ write_inst("mul w0, w1, w0"); break;
        case _and : __ write_inst("and w0, w1, w0"); break;
        case _or  : __ write_inst("orr w0, w1, w0"); break;
        case _xor : __ write_inst("eor w0, w1, w0"); break;
        case shl  : __ write_inst("lslv w0, w1, w0"); break;
        case shr  : __ write_inst("asrv w0, w1, w0"); break;
        case ushr : __ write_inst("lsrv w0, w1, w0"); break;
        default   : ShouldNotReachHere();
    }
}

void YuhuTemplateTable::lop2(Operation op)
{
    transition(ltos, ltos);
    // r0 <== r1 op r0
    __ write_inst_pop_l(__ x1);
    switch (op) {
        case add  : __ write_inst("add x0, x1, x0"); break;
        case sub  : __ write_inst("sub x0, x1, x0"); break;
        case mul  : __ write_inst("mul x0, x1, x0"); break;
        case _and : __ write_inst("and x0, x1, x0"); break;
        case _or  : __ write_inst("orr x0, x1, x0"); break;
        case _xor : __ write_inst("eor x0, x1, x0"); break;
        default   : ShouldNotReachHere();
    }
}

void YuhuTemplateTable::fop2(Operation op)
{
    transition(ftos, ftos);
    switch (op) {
        case add:
            // n.b. use ldrd because this is a 64 bit slot
            __ write_inst_pop_f(__ s1);
            __ write_inst("fadd s0, s1, s0");
            break;
        case sub:
            __ write_inst_pop_f(__ s1);
            __ write_inst("fsub s0, s1, s0");
            break;
        case mul:
            __ write_inst_pop_f(__ s1);
            __ write_inst("fmul s0, s1, s0");
            break;
        case div:
            __ write_inst_pop_f(__ s1);
            __ write_inst("fdiv s0, s1, s0");
            break;
        case rem:
            __ write_inst("fmov s1, s0");
            __ write_inst_pop_f(__ s0);
            __ write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::frem));
            break;
        default:
            ShouldNotReachHere();
            break;
    }
}

void YuhuTemplateTable::dop2(Operation op)
{
    transition(dtos, dtos);
    switch (op) {
        case add:
            // n.b. use ldrd because this is a 64 bit slot
            __ write_inst_pop_d(__ d1);
            __ write_inst("fadd d0, d1, d0");
            break;
        case sub:
            __ write_inst_pop_d(__ d1);
            __ write_inst("fsub d0, d1, d0");
            break;
        case mul:
            __ write_inst_pop_d(__ d1);
            __ write_inst("fmul d0, d1, d0");
            break;
        case div:
            __ write_inst_pop_d(__ d1);
            __ write_inst("fdiv d0, d1, d0");
            break;
        case rem:
            __ write_inst("fmov d1, d0");
            __ write_inst_pop_d(__ d0);
            __ write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::drem));
            break;
        default:
            ShouldNotReachHere();
            break;
    }
}

void YuhuTemplateTable::lmul()
{
    transition(ltos, ltos);
    __ write_inst_pop_l(__ x1);
    __ write_inst("mul x0, x0, x1");
}

void YuhuTemplateTable::idiv()
{
    transition(itos, itos);
    // explicitly check for div0
    YuhuLabel no_div0;
    __ write_inst_cbnz(__ w0, no_div0);
    __ write_insts_mov_imm64(__ x8, (uint64_t) YuhuInterpreter::_throw_ArithmeticException_entry);
    __ write_inst_br(__ x8);
    __ pin_label(no_div0);
    __ write_inst_pop_i(__ x1);
    // r0 <== r1 idiv r0
    __ write_insts_corrected_idivl(__ x0, __ x1, __ x0, /* want_remainder */ false);
}

void YuhuTemplateTable::ldiv()
{
    transition(ltos, ltos);
    // explicitly check for div0
    YuhuLabel no_div0;
    __ write_inst_cbnz(__ x0, no_div0);
    __ write_insts_mov_imm64(__ x8, (uint64_t) YuhuInterpreter::_throw_ArithmeticException_entry);
    __ write_inst_br(__ x8);
    __ pin_label(no_div0);
    __ write_inst_pop_l(__ x1);
    // r0 <== r1 ldiv r0
    __ corrected_idivq(r0, r1, r0, /* want_remainder */ false);
}

void YuhuTemplateTable::irem()
{
    transition(itos, itos);
    // explicitly check for div0
    YuhuLabel no_div0;
    __ write_inst_cbnz(__ w0, no_div0);
    __ write_insts_mov_imm64(__ x8, (uint64_t)YuhuInterpreter::_throw_ArithmeticException_entry);
    __ write_inst_br(__ x8);
    __ pin_label(no_div0);
    __ write_inst_pop_i(__ x1);
    // r0 <== r1 irem r0
    __ write_insts_corrected_idivl(__ x0, __ x1, __ x0, /* want_remainder */ true);
}

void YuhuTemplateTable::lrem()
{
    transition(ltos, ltos);
    // explicitly check for div0
    YuhuLabel no_div0;
    __ write_inst_cbnz(__ x0, no_div0);
    __ write_insts_mov_imm64(__ x8, (uint64_t)YuhuInterpreter::_throw_ArithmeticException_entry);
    __ write_inst_br(__ x8);
    __ pin_label(no_div0);
    __ write_inst_pop_l(__ x1);
    // r0 <== r1 lrem r0
    __ write_insts_corrected_idivq(__ x0, __ x1, __ x0, /* want_remainder */ true);
}

void YuhuTemplateTable::ineg()
{
    transition(itos, itos);
    __ write_inst("neg w0, w0");

}

void YuhuTemplateTable::lneg()
{
    transition(ltos, ltos);
    __ write_inst("neg x0, x0");
}

void YuhuTemplateTable::fneg()
{
    transition(ftos, ftos);
    __ write_inst("fneg s0, s0");
}

void YuhuTemplateTable::dneg()
{
    transition(dtos, dtos);
    __ write_inst("fneg d0, d0");
}

void YuhuTemplateTable::lshl()
{
    transition(itos, ltos);
    // shift count is in r0
    __ write_inst_pop_l(__ x1);
    __ write_inst("lslv x0, x1, x0");
}

void YuhuTemplateTable::lshr()
{
    transition(itos, ltos);
    // shift count is in r0
    __ write_inst_pop_l(__ x1);
    __ write_inst("asrv x0, x1, x0");
}

void YuhuTemplateTable::lushr()
{
    transition(itos, ltos);
    // shift count is in r0
    __ write_inst_pop_l(__ x1);
    __ write_inst("lsrv x0, x1, x0");
}

void YuhuTemplateTable::iinc()
{
    transition(vtos, vtos);
    __ write_insts_load_signed_byte(__ w1, at_bcp(2)); // get constant
    locals_index(__ x2);
    __ write_inst_ldr(__ x0, iaddress(__ x2));
    __ write_inst("add w0, w0, w1");
    __ write_inst_str(__ x0, iaddress(__ x2));
}

void YuhuTemplateTable::convert()
{
    // Checking
#ifdef ASSERT
    {
        TosState tos_in  = ilgl;
        TosState tos_out = ilgl;
        switch (bytecode()) {
            case Bytecodes::_i2l: // fall through
            case Bytecodes::_i2f: // fall through
            case Bytecodes::_i2d: // fall through
            case Bytecodes::_i2b: // fall through
            case Bytecodes::_i2c: // fall through
            case Bytecodes::_i2s: tos_in = itos; break;
            case Bytecodes::_l2i: // fall through
            case Bytecodes::_l2f: // fall through
            case Bytecodes::_l2d: tos_in = ltos; break;
            case Bytecodes::_f2i: // fall through
            case Bytecodes::_f2l: // fall through
            case Bytecodes::_f2d: tos_in = ftos; break;
            case Bytecodes::_d2i: // fall through
            case Bytecodes::_d2l: // fall through
            case Bytecodes::_d2f: tos_in = dtos; break;
            default             : ShouldNotReachHere();
        }
        switch (bytecode()) {
            case Bytecodes::_l2i: // fall through
            case Bytecodes::_f2i: // fall through
            case Bytecodes::_d2i: // fall through
            case Bytecodes::_i2b: // fall through
            case Bytecodes::_i2c: // fall through
            case Bytecodes::_i2s: tos_out = itos; break;
            case Bytecodes::_i2l: // fall through
            case Bytecodes::_f2l: // fall through
            case Bytecodes::_d2l: tos_out = ltos; break;
            case Bytecodes::_i2f: // fall through
            case Bytecodes::_l2f: // fall through
            case Bytecodes::_d2f: tos_out = ftos; break;
            case Bytecodes::_i2d: // fall through
            case Bytecodes::_l2d: // fall through
            case Bytecodes::_f2d: tos_out = dtos; break;
            default             : ShouldNotReachHere();
        }
        transition(tos_in, tos_out);
    }
#endif // ASSERT
    // static const int64_t is_nan = 0x8000000000000000L;

    // Conversion
    switch (bytecode()) {
        case Bytecodes::_i2l:
            __ write_inst("sxtw x0, w0");
            break;
        case Bytecodes::_i2f:
            __ write_inst("scvtf s0, w0");
            break;
        case Bytecodes::_i2d:
            __ write_inst("scvtf d0, w0");
            break;
        case Bytecodes::_i2b:
            __ write_inst("sxtb w0, w0");
            break;
        case Bytecodes::_i2c:
            __ write_inst("uxth w0, w0");
            break;
        case Bytecodes::_i2s:
            __ write_inst("sxth w0, w0");
            break;
        case Bytecodes::_l2i:
            __ write_inst("ubfm x0, x0, #0, #31 ");
            break;
        case Bytecodes::_l2f:
            __ write_inst("scvtf s0, x0");
            break;
        case Bytecodes::_l2d:
            __ write_inst("scvtf d0, x0");
            break;
        case Bytecodes::_f2i:
        {
            YuhuLabel L_Okay;
            __ write_inst_clear_fpsr();
            __ write_inst("fcvtzs w0, s0");
            __ write_inst_get_fpsr(__ x1);
            __ write_inst_cbz(__ w1, L_Okay);
            __ write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::f2i));
            __ pin_label(L_Okay);
        }
            break;
        case Bytecodes::_f2l:
        {
            YuhuLabel L_Okay;
            __ write_inst_clear_fpsr();
            __ write_inst("fcvtzs x0, s0");
            __ write_inst_get_fpsr(__ x1);
            __ write_inst_cbz(__ w1, L_Okay);
            __ write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::f2l));
            __ pin_label(L_Okay);
        }
            break;
        case Bytecodes::_f2d:
            __ write_inst("fcvt d0, s0");
            break;
        case Bytecodes::_d2i:
        {
            YuhuLabel L_Okay;
            __ write_inst_clear_fpsr();
            __ write_inst("fcvtzs w0, d0");
            __ write_inst_get_fpsr(__ x1);
            __ write_inst_cbz(__ w1, L_Okay);
            __ write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::d2i));
            __ pin_label(L_Okay);
        }
            break;
        case Bytecodes::_d2l:
        {
            YuhuLabel L_Okay;
            __ write_inst_clear_fpsr();
            __ write_inst("fcvtzs x0, d0");
            __ write_inst_get_fpsr(__ x1);
            __ write_inst_cbz(__ w1, L_Okay);
            __ write_insts_final_call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::d2l));
            __ pin_label(L_Okay);
        }
            break;
        case Bytecodes::_d2f:
            __ write_inst("fcvt s0, d0");
            break;
        default:
            ShouldNotReachHere();
    }
}

void YuhuTemplateTable::lcmp()
{
    transition(ltos, itos);
    YuhuLabel done;
    __ write_inst_pop_l(__ x1);
    __ write_inst("cmp x1, x0");
    __ write_insts_mov_imm64(__ x0, (u_int64_t)-1L);
    __ write_inst_b(__ lt, done);
    // __ mov(r0, 1UL);
    // __ csel(r0, r0, zr, Assembler::NE);
    // and here is a faster way
    __ write_inst_csinc(__ x0, __ xzr, __ xzr, __ eq);
    __ pin_label(done);
}

void YuhuTemplateTable::float_cmp(bool is_float, int unordered_result)
{
    YuhuLabel done;
    if (is_float) {
        // XXX get rid of pop here, use ... reg, mem32
        __ write_inst_pop_f(__ s1);
        __ write_inst("fcmp s1, s0");
    } else {
        // XXX get rid of pop here, use ... reg, mem64
        __ write_inst_pop_d(__ d0);
        __ write_inst("fcmp d1, d0");
    }
    if (unordered_result < 0) {
        // we want -1 for unordered or less than, 0 for equal and 1 for
        // greater than.
        __ write_insts_mov_imm64(__ x0, (u_int64_t)-1L);
        // for FP LT tests less than or unordered
        __ write_inst_b(__ lt, done);
        // install 0 for EQ otherwise 1
        __ write_inst_csinc(__ x0, __ xzr, __ xzr, __ eq);
    } else {
        // we want -1 for less than, 0 for equal and 1 for unordered or
        // greater than.
        __ write_insts_mov_imm64(__ x0, 1L);
        // for FP HI tests greater than or unordered
        __ write_inst_b(__ hi, done);
        // install 0 for EQ otherwise ~0
        __ write_inst_csinv(__ x0, __ xzr, __ xzr, __ eq);

    }
    __ pin_label(done);
}

void YuhuTemplateTable::branch(bool is_jsr, bool is_wide)
{
    // We might be moving to a safepoint.  The thread which calls
    // Interpreter::notice_safepoints() will effectively flush its cache
    // when it makes a system call, but we need to do something to
    // ensure that we see the changed dispatch table.
    __ write_inst("dmb ishld");

    // TODO
//    __ profile_taken_branch(r0, r1);
    const ByteSize be_offset = MethodCounters::backedge_counter_offset() +
                               InvocationCounter::counter_offset();
    const ByteSize inv_offset = MethodCounters::invocation_counter_offset() +
                                InvocationCounter::counter_offset();

    // load branch displacement
    if (!is_wide) {
        __ write_inst_ldrh(__ w2, at_bcp(1));
        __ write_inst("rev16 x2, x2");
        // sign extend the 16 bit value in r2
        __ write_inst("sbfm x2, x2, #0, #15");
    } else {
        __ write_inst_ldr(__ w2, at_bcp(1));
        __ write_inst("rev w2, w2");
        // sign extend the 32 bit value in r2
        __ write_inst("sbfm x2, x2, #0, #31");
    }

    // Handle all the JSR stuff here, then exit.
    // It's much shorter and cleaner than intermingling with the non-JSR
    // normal-branch stuff occurring below.

    if (is_jsr) {
        // Pre-load the next target bytecode into rscratch1
        __ write_insts_load_unsigned_byte(__ w8, YuhuAddress(__ x22, __ x2));
        // compute return address as bci
        __ write_inst_ldr(__ x9, YuhuAddress(__ x12, Method::const_offset()));
        __ write_inst("add x9, x9, #%d", in_bytes(ConstMethod::codes_offset()) - (is_wide ? 5 : 3));
        __ write_inst("sub x1, x22, x9");
        __ write_inst_push_i(__ x1);
        // Adjust the bcp by the 16-bit displacement in r2
        __ write_inst("add x22, x22, x2");
        __ write_insts_dispatch_only(vtos);
        return;
    }

    // Normal (non-jsr) branch handling

    // Adjust the bcp by the displacement in r2
    __ write_inst("add x22, x22, x2");

    assert(UseLoopCounter || !UseOnStackReplacement,
           "on-stack-replacement requires loop counters");
    // TODO
//    Label backedge_counter_overflow;
//    Label profile_method;
//    Label dispatch;
//    if (UseLoopCounter) {
//        // increment backedge counter for backward branches
//        // r0: MDO
//        // w1: MDO bumped taken-count
//        // r2: target offset
//        __ cmp(r2, zr);
//        __ br(Assembler::GT, dispatch); // count only if backward branch
//
//        // ECN: FIXME: This code smells
//        // check if MethodCounters exists
//        Label has_counters;
//        __ ldr(rscratch1, Address(rmethod, Method::method_counters_offset()));
//        __ cbnz(rscratch1, has_counters);
//        __ push(r0);
//        __ push(r1);
//        __ push(r2);
//        __ call_VM(noreg, CAST_FROM_FN_PTR(address,
//                                           InterpreterRuntime::build_method_counters), rmethod);
//        __ pop(r2);
//        __ pop(r1);
//        __ pop(r0);
//        __ ldr(rscratch1, Address(rmethod, Method::method_counters_offset()));
//        __ cbz(rscratch1, dispatch); // No MethodCounters allocated, OutOfMemory
//        __ bind(has_counters);
//
//        if (TieredCompilation) {
//            Label no_mdo;
//            int increment = InvocationCounter::count_increment;
//            int mask = ((1 << Tier0BackedgeNotifyFreqLog) - 1) << InvocationCounter::count_shift;
//            if (ProfileInterpreter) {
//                // Are we profiling?
//                __ ldr(r1, Address(rmethod, in_bytes(Method::method_data_offset())));
//                __ cbz(r1, no_mdo);
//                // Increment the MDO backedge counter
//                const Address mdo_backedge_counter(r1, in_bytes(MethodData::backedge_counter_offset()) +
//                                                       in_bytes(InvocationCounter::counter_offset()));
//                __ increment_mask_and_jump(mdo_backedge_counter, increment, mask,
//                                           r0, rscratch2, false, Assembler::EQ, &backedge_counter_overflow);
//                __ b(dispatch);
//            }
//            __ bind(no_mdo);
//            // Increment backedge counter in MethodCounters*
//            __ ldr(rscratch1, Address(rmethod, Method::method_counters_offset()));
//            __ increment_mask_and_jump(Address(rscratch1, be_offset), increment, mask,
//                                       r0, rscratch2, false, Assembler::EQ, &backedge_counter_overflow);
//        } else {
//            // increment counter
//            __ ldr(rscratch2, Address(rmethod, Method::method_counters_offset()));
//            __ ldrw(r0, Address(rscratch2, be_offset));        // load backedge counter
//            __ addw(rscratch1, r0, InvocationCounter::count_increment); // increment counter
//            __ strw(rscratch1, Address(rscratch2, be_offset));        // store counter
//
//            __ ldrw(r0, Address(rscratch2, inv_offset));    // load invocation counter
//            __ andw(r0, r0, (unsigned)InvocationCounter::count_mask_value); // and the status bits
//            __ addw(r0, r0, rscratch1);        // add both counters
//
//            if (ProfileInterpreter) {
//                // Test to see if we should create a method data oop
//                __ lea(rscratch1, ExternalAddress((address) &InvocationCounter::InterpreterProfileLimit));
//                __ ldrw(rscratch1, rscratch1);
//                __ cmpw(r0, rscratch1);
//                __ br(Assembler::LT, dispatch);
//
//                // if no method data exists, go to profile method
//                __ test_method_data_pointer(r0, profile_method);
//
//                if (UseOnStackReplacement) {
//                    // check for overflow against w1 which is the MDO taken count
//                    __ lea(rscratch1, ExternalAddress((address) &InvocationCounter::InterpreterBackwardBranchLimit));
//                    __ ldrw(rscratch1, rscratch1);
//                    __ cmpw(r1, rscratch1);
//                    __ br(Assembler::LO, dispatch); // Intel == Assembler::below
//
//                    // When ProfileInterpreter is on, the backedge_count comes
//                    // from the MethodData*, which value does not get reset on
//                    // the call to frequency_counter_overflow().  To avoid
//                    // excessive calls to the overflow routine while the method is
//                    // being compiled, add a second test to make sure the overflow
//                    // function is called only once every overflow_frequency.
//                    const int overflow_frequency = 1024;
//                    __ andsw(r1, r1, overflow_frequency - 1);
//                    __ br(Assembler::EQ, backedge_counter_overflow);
//
//                }
//            } else {
//                if (UseOnStackReplacement) {
//                    // check for overflow against w0, which is the sum of the
//                    // counters
//                    __ lea(rscratch1, ExternalAddress((address) &InvocationCounter::InterpreterBackwardBranchLimit));
//                    __ ldrw(rscratch1, rscratch1);
//                    __ cmpw(r0, rscratch1);
//                    __ br(Assembler::HS, backedge_counter_overflow); // Intel == Assembler::aboveEqual
//                }
//            }
//        }
//    }
//    __ bind(dispatch);

    // Pre-load the next target bytecode into rscratch1
    __ write_insts_load_unsigned_byte(__ w8, YuhuAddress(__ x22, 0));

    // continue with the bytecode @ target
    // rscratch1: target bytecode
    // rbcp: target bcp
    __ write_insts_dispatch_only(vtos);

    // TODO
//    if (UseLoopCounter) {
//        if (ProfileInterpreter) {
//            // Out-of-line code to allocate method data oop.
//            __ bind(profile_method);
//            __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::profile_method));
//            __ load_unsigned_byte(r1, Address(rbcp, 0));  // restore target bytecode
//            __ set_method_data_pointer_for_bcp();
//            __ b(dispatch);
//        }
//
//        if (TieredCompilation || UseOnStackReplacement) {
//            // invocation counter overflow
//            __ bind(backedge_counter_overflow);
//            __ neg(r2, r2);
//            __ add(r2, r2, rbcp);     // branch bcp
//            // IcoResult frequency_counter_overflow([JavaThread*], address branch_bcp)
//            __ call_VM(noreg,
//                       CAST_FROM_FN_PTR(address,
//                                        InterpreterRuntime::frequency_counter_overflow),
//                       r2);
//            if (!UseOnStackReplacement)
//                __ b(dispatch);
//        }
//
//        if (UseOnStackReplacement) {
//            __ load_unsigned_byte(r1, Address(rbcp, 0));  // restore target bytecode
//
//            // r0: osr nmethod (osr ok) or NULL (osr not possible)
//            // w1: target bytecode
//            // r2: scratch
//            __ cbz(r0, dispatch);     // test result -- no osr if null
//            // nmethod may have been invalidated (VM may block upon call_VM return)
//            __ ldrw(r2, Address(r0, nmethod::entry_bci_offset()));
//            // InvalidOSREntryBci == -2 which overflows cmpw as unsigned
//            // use cmnw against -InvalidOSREntryBci which does the same thing
//            __ cmn(r2, -InvalidOSREntryBci);
//            __ br(Assembler::EQ, dispatch);
//
//            // We have the address of an on stack replacement routine in r0
//            // We need to prepare to execute the OSR method. First we must
//            // migrate the locals and monitors off of the stack.
//
//            __ mov(r19, r0);                             // save the nmethod
//
//            call_VM(noreg, CAST_FROM_FN_PTR(address, SharedRuntime::OSR_migration_begin));
//
//            // r0 is OSR buffer, move it to expected parameter location
//            __ mov(j_rarg0, r0);
//
//            // remove activation
//            // get sender esp
//            __ ldr(esp,
//                   Address(rfp, frame::interpreter_frame_sender_sp_offset * wordSize));
//            // remove frame anchor
//            __ leave();
//            // Ensure compiled code always sees stack at proper alignment
//            __ andr(sp, esp, -16);
//
//            // and begin the OSR nmethod
//            __ ldr(rscratch1, Address(r19, nmethod::osr_entry_point_offset()));
//            __ br(rscratch1);
//        }
//    }
}

void YuhuTemplateTable::if_0cmp(Condition cc)
{
    transition(itos, vtos);
    // assume branch is more often taken than not (loops use backward branches)
    YuhuLabel not_taken;
    if (cc == equal)
        __ write_inst_cbnz(__ w0, not_taken);
    else if (cc == not_equal)
        __ write_inst_cbz(__ w0, not_taken);
    else {
        __ write_inst("ands wzr, w0, w0");
        __ write_inst_b(j_not(cc), not_taken);
    }

    branch(false, false);
    __ pin_label(not_taken);
    // TODO
//    __ profile_not_taken_branch(r0);
}

void YuhuTemplateTable::if_icmp(Condition cc)
{
    transition(itos, vtos);
    // assume branch is more often taken than not (loops use backward branches)
    YuhuLabel not_taken;
    __ write_inst_pop_i(__ x1);
    __ write_inst("cmp w1, w0, lsl #0");
    __ write_inst_b(j_not(cc), not_taken);
    branch(false, false);
    __ pin_label(not_taken);
    // TODO
//    __ profile_not_taken_branch(r0);
}

void YuhuTemplateTable::if_acmp(Condition cc)
{
    transition(atos, vtos);
    // assume branch is more often taken than not (loops use backward branches)
    YuhuLabel not_taken;
    __ write_inst_pop_ptr(__ x1);
    __ write_inst("cmp x1, x0");
    __ write_inst_b(j_not(cc), not_taken);
    branch(false, false);
    __ pin_label(not_taken);
    // TODO
//    __ profile_not_taken_branch(r0);
}

void YuhuTemplateTable::ret() {
    transition(vtos, vtos);
    // We might be moving to a safepoint.  The thread which calls
    // Interpreter::notice_safepoints() will effectively flush its cache
    // when it makes a system call, but we need to do something to
    // ensure that we see the changed dispatch table.
    __ write_inst("dmb ishld");

    locals_index(__ x1);
    __ write_inst_ldr(__ x1, aaddress(__ x1)); // get return bci, compute return bcp
    // TODO
//    __ profile_ret(r1, r2);
    __ write_inst_ldr(__ x22, YuhuAddress(__ x12, Method::const_offset()));
    __ write_insts_lea(__ x22, YuhuAddress(__ x22, __ x1));
    __ write_inst("add x22, x22, #%d", in_bytes(ConstMethod::codes_offset()));
    __ write_insts_dispatch_next(vtos);
}

void YuhuTemplateTable::tableswitch() {
    YuhuLabel default_case, continue_execution;
    transition(itos, vtos);
    // align rbcp
    __ write_insts_lea(__ x1, at_bcp(BytesPerInt));
    __ write_inst("and x1, x1, #%d", -BytesPerInt);
    // load lo & hi
    __ write_inst_ldr(__ w2, YuhuAddress(__ x1, BytesPerInt));
    __ write_inst_ldr(__ w3, YuhuAddress(__ x1, 2 * BytesPerInt));
    __ write_inst("rev32 x2, x2");
    __ write_inst("rev32 x3, x3");
    // check against lo & hi
    __ write_inst("cmp w0, w2");
    __ write_inst_b(__ lt, default_case);
    __ write_inst("cmp w0, w3");
    __ write_inst_b(__ gt, default_case);
    // lookup dispatch offset
    __ write_inst("sub w0, w0, w2");
    __ write_insts_lea(__ x3, YuhuAddress(__ x1, __ w0, YuhuAddress::uxtw(2)));
    __ write_inst_ldr(__ w3, YuhuAddress(__ x3, 3 * BytesPerInt));
    // TODO
//    __ profile_switch_case(r0, r1, r2);
    // continue execution
    __ pin_label(continue_execution);
    __ write_inst("rev32 x3, x3");
    __ write_insts_load_unsigned_byte(__ w8, YuhuAddress(__ x22, __ w3, YuhuAddress::sxtw(0)));
    __ write_inst("add x22, x22, w3, sxtw #0");
    __ write_insts_dispatch_only(vtos);
    // handle default
    __ pin_label(default_case);
    // TODO
//    __ profile_switch_default(r0);
    __ write_inst_ldr(__ w3, YuhuAddress(__ x1, 0));
    __ write_inst_b(continue_execution);
}

void YuhuTemplateTable::lookupswitch() {
    transition(itos, itos);
    __ write_insts_stop("lookupswitch bytecode should have been rewritten");
}

void YuhuTemplateTable::_return(TosState state)
{
    transition(state, state);
    assert(_desc->calls_vm(),
           "inconsistent calls_vm information"); // call in remove_activation

    if (_desc->bytecode() == Bytecodes::_return_register_finalizer) {
        assert(state == vtos, "only valid state");

        __ write_inst_ldr(__ x1, aaddress(0));
        __ write_insts_load_klass(__ x3, __ x1);
        __ write_inst_ldr(__ w3, YuhuAddress(__ x3, Klass::access_flags_offset()));
        __ write_inst("tst x3, #%d", JVM_ACC_HAS_FINALIZER);
        YuhuLabel skip_register_finalizer;
        __ write_inst_b(__ eq, skip_register_finalizer);

        __ write_insts_final_call_VM(__ noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::register_finalizer), __ x1);

        __ pin_label(skip_register_finalizer);
    }

    // Issue a StoreStore barrier after all stores but before return
    // from any constructor for any class with a final field.  We don't
    // know if this is a finalizer, so we always do so.
    if (_desc->bytecode() == Bytecodes::_return)
        __ write_inst("dmb ishst");

    // Narrow result if state is itos but result type is smaller.
    // Need to narrow in the return bytecode rather than in generate_return_entry
    // since compiled code callers expect the result to already be narrowed.
    if (state == itos) {
        __ write_insts_narrow(__ x0);
    }

    __ write_insts_remove_activation(state);
    __ write_inst("ret");
}

void YuhuTemplateTable::getfield_or_static(int byte_no, bool is_static)
{
    const YuhuMacroAssembler::YuhuRegister cache = __ x2;
    const YuhuMacroAssembler::YuhuRegister index = __ x3;
    const YuhuMacroAssembler::YuhuRegister obj   = __ x4;
    const YuhuMacroAssembler::YuhuRegister off   = __ x19;
    const YuhuMacroAssembler::YuhuRegister flags = __ x0;
    const YuhuMacroAssembler::YuhuRegister raw_flags = __ x6;
    const YuhuMacroAssembler::YuhuRegister bc    = __ x4; // uses same reg as obj, so don't mix them

    resolve_cache_and_index(byte_no, cache, index, sizeof(u2));
    // TODO
//    jvmti_post_field_access(cache, index, is_static, false);
    load_field_cp_cache_entry(obj, cache, index, off, raw_flags, is_static);

    if (!is_static) {
        // obj is on the stack
        pop_and_check_object(obj);
    }

    // 8179954: We need to make sure that the code generated for
    // volatile accesses forms a sequentially-consistent set of
    // operations when combined with STLR and LDAR.  Without a leading
    // membar it's possible for a simple Dekker test to fail if loads
    // use LDR;DMB but stores use STLR.  This can happen if C2 compiles
    // the stores in one method and we interpret the loads in another.
    if (! UseBarriersForVolatile) {
        YuhuLabel notVolatile;
        __ write_inst_tbz(raw_flags, ConstantPoolCacheEntry::is_volatile_shift, notVolatile);
        __ write_inst("dmb ish");
        __ pin_label(notVolatile);
    }

    const YuhuAddress field(obj, off);

    YuhuLabel Done, notByte, notBool, notInt, notShort, notChar,
            notLong, notFloat, notObj, notDouble;

    // x86 uses a shift and mask or wings it with a shift plus assert
    // the mask is not needed. aarch64 just uses bitfield extract
    __ write_inst_imms("ubfx %s, %s, #%d, #%d", __ w_reg(flags), __ w_reg(raw_flags),
                       ConstantPoolCacheEntry::tos_state_shift, ConstantPoolCacheEntry::tos_state_bits);

    assert(btos == 0, "change code, btos != 0");
    __ write_inst_cbnz(flags, notByte);

    // btos
    __ write_insts_load_signed_byte(__ w0, field);
    __ write_insts_push(btos);
    // Rewrite bytecode to be faster
    if (!is_static) {
        patch_bytecode(Bytecodes::_fast_bgetfield, bc, __ x1);
    }
    __ write_inst_b(Done);

    __ pin_label(notByte);
    /*__ cmp(flags, ztos);
    __ br(Assembler::NE, notBool);

    // ztos (same code as btos)
    __ ldrsb(r0, field);
    __ push(ztos);
    // Rewrite bytecode to be faster
    if (!is_static) {
      // use btos rewriting, no truncating to t/f bit is needed for getfield.
      patch_bytecode(Bytecodes::_fast_bgetfield, bc, r1);
    }
    __ b(Done);

    __ bind(notBool);*/
    __ write_inst("cmp %s, #%d", flags, atos);
    __ write_inst_b(__ ne, notObj);
    // atos
    __ write_insts_load_heap_oop(__ x0, field);
    __ write_insts_push(atos);
    if (!is_static) {
        patch_bytecode(Bytecodes::_fast_agetfield, bc, __ x1);
    }
    __ write_inst_b(Done);

    __ pin_label(notObj);
    __ write_inst("cmp %s, #%d", flags, itos);
    __ write_inst_b(__ ne, notInt);
    // itos
    __ write_inst_ldr(__ w0, field);
    __ write_insts_push(itos);
    // Rewrite bytecode to be faster
    if (!is_static) {
        patch_bytecode(Bytecodes::_fast_igetfield, bc, __ x1);
    }
    __ write_inst_b(Done);

    __ pin_label(notInt);
    __ write_inst("cmp %s, #%d", flags, ctos);
    __ write_inst_b(__ ne, notChar);
    // ctos
    __ write_insts_load_unsigned_short(__ w0, field);
    __ write_insts_push(ctos);
    // Rewrite bytecode to be faster
    if (!is_static) {
        patch_bytecode(Bytecodes::_fast_cgetfield, bc, __ x1);
    }
    __ write_inst_b(Done);

    __ pin_label(notChar);
    __ write_inst("cmp %s, #%d", flags, stos);
    __ write_inst_b(__ ne, notShort);
    // stos
    __ write_insts_load_signed_short(__ w0, field);
    __ write_insts_push(stos);
    // Rewrite bytecode to be faster
    if (!is_static) {
        patch_bytecode(Bytecodes::_fast_sgetfield, bc, __ x1);
    }
    __ write_inst_b(Done);

    __ pin_label(notShort);
    __ write_inst("cmp %s, #%d", flags, ltos);
    __ write_inst_b(__ ne, notLong);
    // ltos
    __ write_inst_ldr(__ x0, field);
    __ write_insts_push(ltos);
    // Rewrite bytecode to be faster
    if (!is_static) {
        patch_bytecode(Bytecodes::_fast_lgetfield, bc, __ x1);
    }
    __ write_inst_b(Done);

    __ pin_label(notLong);
    __ write_inst("cmp %s, #%d", flags, ftos);
    __ write_inst_b(__ ne, notFloat);
    // ftos
    __ write_inst_ldr(__ s0, field);
    __ write_insts_push(ftos);
    // Rewrite bytecode to be faster
    if (!is_static) {
        patch_bytecode(Bytecodes::_fast_fgetfield, bc, __ x1);
    }
    __ write_inst_b(Done);

    __ pin_label(notFloat);
#ifdef ASSERT
    __ write_inst("cmp %s, #%d", flags, dtos);
    __ write_inst_b(__ ne, notDouble);
#endif
    // dtos
    __ write_inst_ldr(__ d0, field);
    __ write_insts_push(dtos);
    // Rewrite bytecode to be faster
    if (!is_static) {
        patch_bytecode(Bytecodes::_fast_dgetfield, bc, __ x1);
    }
#ifdef ASSERT
    __ write_inst_b(Done);

    __ pin_label(notDouble);
    __ write_insts_stop("Bad state");
#endif

    __ pin_label(Done);

    YuhuLabel notVolatile;
    __ write_inst_tbz(raw_flags, ConstantPoolCacheEntry::is_volatile_shift, notVolatile);
    __ write_inst("dmb ishld");
    __ pin_label(notVolatile);
}

void YuhuTemplateTable::getstatic(int byte_no)
{
    getfield_or_static(byte_no, true);
}

void YuhuTemplateTable::putfield_or_static(int byte_no, bool is_static) {
    transition(vtos, vtos);

    const YuhuMacroAssembler::YuhuRegister cache = __ x2;
    const YuhuMacroAssembler::YuhuRegister index = __ x3;
    const YuhuMacroAssembler::YuhuRegister obj   = __ x2;
    const YuhuMacroAssembler::YuhuRegister off   = __ x19;
    const YuhuMacroAssembler::YuhuRegister flags = __ x0;
    const YuhuMacroAssembler::YuhuRegister bc    = __ x4;

    resolve_cache_and_index(byte_no, cache, index, sizeof(u2));
    // TODO
//    jvmti_post_field_mod(cache, index, is_static);
    load_field_cp_cache_entry(obj, cache, index, off, flags, is_static);

    YuhuLabel Done;
    __ write_insts_mov_imm64(__ x5, flags);

    {
        YuhuLabel notVolatile;
        __ write_inst_tbz(__ x5, ConstantPoolCacheEntry::is_volatile_shift, notVolatile);
        __ write_inst("dmb ish");
        __ pin_label(notVolatile);
    }

    // field address
    const YuhuAddress field(obj, off);

    YuhuLabel notByte, notBool, notInt, notShort, notChar,
            notLong, notFloat, notObj, notDouble;

    // x86 uses a shift and mask or wings it with a shift plus assert
    // the mask is not needed. aarch64 just uses bitfield extract
    __ write_inst_imms("ubfx %s, %s, #%d, #%d", __ w_reg(flags), __ w_reg(flags),
                       ConstantPoolCacheEntry::tos_state_shift,  ConstantPoolCacheEntry::tos_state_bits);

    assert(btos == 0, "change code, btos != 0");
    __ write_inst_cbnz(flags, notByte);

    // btos
    {
        __ write_insts_pop(btos);
        if (!is_static) pop_and_check_object(obj);
        __ write_inst_strb(__ w0, field);
        if (!is_static) {
            patch_bytecode(Bytecodes::_fast_bputfield, bc, __ x1, true, byte_no);
        }
        __ write_inst_b(Done);
    }

    __ pin_label(notByte);
    /*__ cmp(flags, ztos);
    __ br(Assembler::NE, notBool);

    // ztos
    {
      __ pop(ztos);
      if (!is_static) pop_and_check_object(obj);
      __ andw(r0, r0, 0x1);
      __ strb(r0, field);
      if (!is_static) {
        patch_bytecode(Bytecodes::_fast_zputfield, bc, r1, true, byte_no);
      }
      __ b(Done);
    }

    __ bind(notBool);*/
    __ write_inst("cmp %s, #%d", flags, atos);
    __ write_inst_b(__ ne, notObj);

    // atos
    {
        __ write_insts_pop(atos);
        if (!is_static) pop_and_check_object(obj);
        // Store into the field
        do_oop_store(_masm, field, __ x0, _bs->kind(), false);
        if (!is_static) {
            patch_bytecode(Bytecodes::_fast_aputfield, bc, __ x1, true, byte_no);
        }
        __ write_inst_b(Done);
    }

    __ pin_label(notObj);
    __ write_inst("cmp %s, #%d", flags, itos);
    __ write_inst_b(__ ne, notInt);

    // itos
    {
        __ write_insts_pop(itos);
        if (!is_static) pop_and_check_object(obj);
        __ write_inst_str(__ w0, field);
        if (!is_static) {
            patch_bytecode(Bytecodes::_fast_iputfield, bc, __ x1, true, byte_no);
        }
        __ write_inst_b(Done);
    }

    __ pin_label(notInt);
    __ write_inst("cmp %s, #%d", flags, ctos);
    __ write_inst_b(__ ne, notChar);

    // ctos
    {
        __ write_insts_pop(ctos);
        if (!is_static) pop_and_check_object(obj);
        __ write_inst_strh(__ w0, field);
        if (!is_static) {
            patch_bytecode(Bytecodes::_fast_cputfield, bc, __ x1, true, byte_no);
        }
        __ write_inst_b(Done);
    }

    __ pin_label(notChar);
    __ write_inst("cmp %s, #%d", flags, stos);
    __ write_inst_b(__ ne, notShort);

    // stos
    {
        __ write_insts_pop(stos);
        if (!is_static) pop_and_check_object(obj);
        __ write_inst_strh(__ w0, field);
        if (!is_static) {
            patch_bytecode(Bytecodes::_fast_sputfield, bc, __ x1, true, byte_no);
        }
        __ write_inst_b(Done);
    }

    __ pin_label(notShort);
    __ write_inst("cmp %s, #%d", flags, ltos);
    __ write_inst_b(__ ne, notLong);

    // ltos
    {
        __ write_insts_pop(ltos);
        if (!is_static) pop_and_check_object(obj);
        __ write_inst_str(__ x0, field);
        if (!is_static) {
            patch_bytecode(Bytecodes::_fast_lputfield, bc, __ x1, true, byte_no);
        }
        __ write_inst_b(Done);
    }

    __ pin_label(notLong);
    __ write_inst("cmp %s, #%d", flags, ftos);
    __ write_inst_b(__ ne, notFloat);

    // ftos
    {
        __ write_insts_pop(ftos);
        if (!is_static) pop_and_check_object(obj);
        __ write_inst_str(__ s0, field);
        if (!is_static) {
            patch_bytecode(Bytecodes::_fast_fputfield, bc, __ x1, true, byte_no);
        }
        __ write_inst_b(Done);
    }

    __ pin_label(notFloat);
#ifdef ASSERT
    __ write_inst("cmp %s, #%d", flags, dtos);
    __ write_inst_b(__ ne, notDouble);
#endif

    // dtos
    {
        __ write_insts_pop(dtos);
        if (!is_static) pop_and_check_object(obj);
        __ write_inst_str(__ d0, field);
        if (!is_static) {
            patch_bytecode(Bytecodes::_fast_dputfield, bc, __ x1, true, byte_no);
        }
    }

#ifdef ASSERT
    __ write_inst_b(Done);

    __ pin_label(notDouble);
    __ write_insts_stop("Bad state");
#endif

    __ pin_label(Done);

    {
        YuhuLabel notVolatile;
        __ write_inst_tbz(__ x5, ConstantPoolCacheEntry::is_volatile_shift, notVolatile);
        __ write_inst("dmb ish");
        __ pin_label(notVolatile);
    }
}

void YuhuTemplateTable::putstatic(int byte_no) {
    putfield_or_static(byte_no, true);
}

void YuhuTemplateTable::getfield(int byte_no)
{
    getfield_or_static(byte_no, false);
}

void YuhuTemplateTable::putfield(int byte_no)
{
    putfield_or_static(byte_no, false);
}

void YuhuTemplateTable::prepare_invoke(int byte_no,
                                       YuhuMacroAssembler::YuhuRegister method, // linked method (or i-klass)
                                       YuhuMacroAssembler::YuhuRegister index,  // itable index, MethodType, etc.
                                       YuhuMacroAssembler::YuhuRegister recv,   // if caller wants to see it
                                       YuhuMacroAssembler::YuhuRegister flags   // if caller wants to test it
) {
    // determine flags
    Bytecodes::Code code = bytecode();
    const bool is_invokeinterface  = code == Bytecodes::_invokeinterface;
    const bool is_invokedynamic    = code == Bytecodes::_invokedynamic;
    const bool is_invokehandle     = code == Bytecodes::_invokehandle;
    const bool is_invokevirtual    = code == Bytecodes::_invokevirtual;
    const bool is_invokespecial    = code == Bytecodes::_invokespecial;
    const bool load_receiver       = (recv  != __ noreg);
    const bool save_flags          = (flags != __ noreg);
    assert(load_receiver == (code != Bytecodes::_invokestatic && code != Bytecodes::_invokedynamic), "");
    assert(save_flags    == (is_invokeinterface || is_invokevirtual), "need flags for vfinal");
    assert(flags == __ noreg || flags == __ x3, "");
    assert(recv  == __ noreg || recv  == __ x2, "");

    // setup registers & access constant pool cache
    if (recv  == __ noreg)  recv  = __ x2;
    if (flags == __ noreg)  flags = __ x3;
//    assert_different_registers(method, index, recv, flags);

    // save 'interpreter return address'
    __ write_insts_save_bcp();

    load_invoke_cp_cache_entry(byte_no, method, index, flags, is_invokevirtual, false, is_invokedynamic);

    // maybe push appendix to arguments (just before return address)
    if (is_invokedynamic || is_invokehandle) {
        YuhuLabel L_no_push;
        __ write_inst_tbz(flags, ConstantPoolCacheEntry::has_appendix_shift, L_no_push);
        // Push the appendix as a trailing parameter.
        // This must be done before we get the receiver,
        // since the parameter_size includes it.
        __ write_inst_push(__ x19);
        __ write_insts_mov_imm64(__ x19, index);
        assert(ConstantPoolCacheEntry::_indy_resolved_references_appendix_offset == 0, "appendix expected at index+0");
        __ write_insts_load_resolved_reference_at_index(index, __ x19);
        __ write_inst_pop(__ x19);
        __ write_inst_push(index);  // push appendix (MethodType, CallSite, etc.)
        __ pin_label(L_no_push);
    }

    // load receiver if needed (note: no return address pushed yet)
    if (load_receiver) {
        __ write_inst("and %s, %s, #%d", __ w_reg(recv), __ w_reg(flags), ConstantPoolCacheEntry::parameter_size_mask);
        // FIXME -- is this actually correct? looks like it should be 2
        // const int no_return_pc_pushed_yet = -1;  // argument slot correction before we push return address
        // const int receiver_is_at_end      = -1;  // back off one slot to get receiver
        // Address recv_addr = __ argument_address(recv, no_return_pc_pushed_yet + receiver_is_at_end);
        // __ movptr(recv, recv_addr);
        __ write_inst_add(__ x8, __ x20, recv, __ uxtx, 3); // FIXME: uxtb here?
        __ write_inst_ldr(recv, YuhuAddress(__ x8, -YuhuInterpreter::expr_offset_in_bytes(1)));
        __ write_insts_verify_oop(recv, "broken oop");
    }

    // compute return type
    // x86 uses a shift and mask or wings it with a shift plus assert
    // the mask is not needed. aarch64 just uses bitfield extract
    __ write_inst_imms("ubfx %s, %s, #%d, #%d", __ w9, __ w_reg(flags),
                       ConstantPoolCacheEntry::tos_state_shift,  ConstantPoolCacheEntry::tos_state_bits);
    // load return address
    {
        const address table_addr = (address) YuhuInterpreter::invoke_return_entry_table_for(code);
        __ write_insts_mov_imm64(__ x8, (uint64_t) table_addr);
        __ write_inst_ldr(__ lr, YuhuAddress(__ x8, __ x9, YuhuAddress::lsl(3)));
    }
}

void YuhuTemplateTable::invokevirtual_helper(YuhuMacroAssembler::YuhuRegister index,
                                             YuhuMacroAssembler::YuhuRegister recv,
                                             YuhuMacroAssembler::YuhuRegister flags)
{
    // Uses temporary registers r0, r3
//    assert_different_registers(index, recv, r0, r3);
    // Test for an invoke of a final method
    YuhuLabel notFinal;
    __ write_inst_tbz(flags, ConstantPoolCacheEntry::is_vfinal_shift, notFinal);

    const YuhuMacroAssembler::YuhuRegister method = index;  // method must be rmethod
    assert(method == __ x12,
           "methodOop must be rmethod for interpreter calling convention");

    // do the call - the index is actually the method to call
    // that is, f2 is a vtable index if !is_vfinal, else f2 is a Method*

    // It's final, need a null check here!
    __ write_insts_null_check(recv);

    // profile this call
    // TODO
//    __ profile_final_call(r0);
//    __ profile_arguments_type(r0, method, r4, true);

    __ write_insts_jump_from_interpreted(method, __ x0);

    __ pin_label(notFinal);

    // get receiver klass
    __ write_insts_null_check(recv, oopDesc::klass_offset_in_bytes());
    __ write_insts_load_klass(__ x0, recv);

    // profile this call
    // TODO
//    __ profile_virtual_call(r0, rlocals, r3);

    // get target methodOop & entry point
    __ write_insts_lookup_virtual_method(__ x0, method, index);
    // TODO
//    __ profile_arguments_type(r3, method, r4, true);
    // FIXME -- this looks completely redundant. is it?
    // __ ldr(r3, Address(method, Method::interpreter_entry_offset()));
    __ write_insts_jump_from_interpreted(method, __ x3);
}

void YuhuTemplateTable::invokevirtual(int byte_no)
{
    transition(vtos, vtos);
    assert(byte_no == f2_byte, "use this argument");

    const YuhuMacroAssembler::YuhuRegister t = __ x17;

    prepare_invoke(byte_no, __ x12, __ noreg, __ x2, __ x3);

    // rmethod: index (actually a Method*)
    // r2: receiver
    // r3: flags

    invokevirtual_helper(__ x12, __ x2, __ x3);
}

void YuhuTemplateTable::invokespecial(int byte_no)
{
    transition(vtos, vtos);
    assert(byte_no == f1_byte, "use this argument");

    prepare_invoke(byte_no, __ x12, __ noreg,  // get f1 Method*
                   __ x2);  // get receiver also for null check
    __ write_insts_verify_oop(__ x2, "broken oop");
    __ write_insts_null_check(__ x2);
    // do the call
    // TODO
//    __ profile_call(r0);
//    __ profile_arguments_type(r0, rmethod, rbcp, false);
    __ write_insts_jump_from_interpreted(__ x12, __ x0);
}

void YuhuTemplateTable::invokestatic(int byte_no)
{
    transition(vtos, vtos);
    assert(byte_no == f1_byte, "use this argument");

    prepare_invoke(byte_no, __ x12);  // get f1 Method*
    // do the call
    // TODO
//    __ profile_call(r0);
//    __ profile_arguments_type(r0, rmethod, r4, false);
    __ write_insts_jump_from_interpreted(__ x12, __ x0);
}

void YuhuTemplateTable::invokeinterface(int byte_no) {
    transition(vtos, vtos);
    assert(byte_no == f1_byte, "use this argument");

    prepare_invoke(byte_no, __ x0, __ x12,  // get f1 Klass*, f2 Method*
                   __ x2, __ x3); // recv, flags

    // r0: interface klass (from f1)
    // rmethod: method (from f2)
    // r2: receiver
    // r3: flags

    // Special case of invokeinterface called for virtual method of
    // java.lang.Object.  See cpCacheOop.cpp for details.
    // This code isn't produced by javac, but could be produced by
    // another compliant java compiler.
    YuhuLabel notMethod;
    __ write_inst_tbz(__ x3, ConstantPoolCacheEntry::is_forced_virtual_shift, notMethod);

    invokevirtual_helper(__ x12, __ x2, __ x3);
    __ pin_label(notMethod);

    // Get receiver klass into r3 - also a null check
    __ write_insts_restore_locals();
    __ write_insts_null_check(__ x2, oopDesc::klass_offset_in_bytes());
    __ write_insts_load_klass(__ x3, __ x2);

    YuhuLabel no_such_interface, no_such_method;

    // Receiver subtype check against REFC.
    // Superklass in r0. Subklass in r3. Blows rscratch2, r13.
    __ write_insts_lookup_interface_method(// inputs: rec. class, interface, itable index
            __ x3, __ x0, __ noreg,
            // outputs: scan temp. reg, scan temp. reg
            __ x9, __ x13,
            no_such_interface,
            /*return_method=*/false);

    // profile this call
    // TODO
//    __ profile_virtual_call(r3, r13, r19);

    // Get declaring interface class from method, and itable index
    __ write_inst_ldr(__ x0, YuhuAddress(__ x12, Method::const_offset()));
    __ write_inst_ldr(__ x0, YuhuAddress(__ x0, ConstMethod::constants_offset()));
    __ write_inst_ldr(__ x0, YuhuAddress(__ x0, ConstantPool::pool_holder_offset_in_bytes()));
    __ write_inst_ldr(__ w12, YuhuAddress(__ x12, Method::itable_index_offset()));
    __ write_inst("sub w12, w12, #%d", Method::itable_index_max);
    __ write_inst("neg w12, w12");

    __ write_insts_lookup_interface_method(// inputs: rec. class, interface, itable index
            __ x3, __ x0, __ x12,
            // outputs: method, scan temp. reg
            __ x12, __ x13,
            no_such_interface);

    // rmethod,: methodOop to call
    // r2: receiver
    // Check for abstract method error
    // Note: This should be done more efficiently via a throw_abstract_method_error
    //       interpreter entry point and a conditional jump to it in case of a null
    //       method.
    __ write_inst_cbz(__ x12, no_such_method);

    // TODO
//    __ profile_arguments_type(r3, rmethod, r13, true);

    // do the call
    // r2: receiver
    // rmethod,: methodOop
    __ write_insts_jump_from_interpreted(__ x12, __ x3);
    __ write_insts_stop("should not reach here");

    // exception handling code follows...
    // note: must restore interpreter registers to canonical
    //       state for exception handling to work correctly!

    __ pin_label(no_such_method);
    // throw exception
    __ write_insts_restore_bcp();      // bcp must be correct for exception handler   (was destroyed)
    __ write_insts_restore_locals();   // make sure locals pointer is correct as well (was destroyed)
    __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::throw_AbstractMethodError));
    // the call_VM checks for exception, so we should never return here.
    __ write_insts_stop("should not reach here");

    __ pin_label(no_such_interface);
    // throw exception
    __ write_insts_restore_bcp();      // bcp must be correct for exception handler   (was destroyed)
    __ write_insts_restore_locals();   // make sure locals pointer is correct as well (was destroyed)
    __ write_insts_final_call_VM(__ noreg, CAST_FROM_FN_PTR(address,
                                       InterpreterRuntime::throw_IncompatibleClassChangeError));
    // the call_VM checks for exception, so we should never return here.
    __ write_insts_stop("should not reach here");
    return;
}

void YuhuTemplateTable::invokedynamic(int byte_no) {
    transition(vtos, vtos);
    assert(byte_no == f1_byte, "use this argument");

    if (!EnableInvokeDynamic) {
        // We should not encounter this bytecode if !EnableInvokeDynamic.
        // The verifier will stop it.  However, if we get past the verifier,
        // this will stop the thread in a reasonable way, without crashing the JVM.
        __ write_insts_final_call_VM(__ noreg, CAST_FROM_FN_PTR(address,
                                           InterpreterRuntime::throw_IncompatibleClassChangeError));
        // the call_VM checks for exception, so we should never return here.
        __ write_insts_stop("should not reach here");
        return;
    }

    prepare_invoke(byte_no, __ x12, __ x0);

    // r0: CallSite object (from cpool->resolved_references[])
    // rmethod: MH.linkToCallSite method (from f2)

    // Note:  r0_callsite is already pushed by prepare_invoke

    // %%% should make a type profile for any invokedynamic that takes a ref argument
    // profile this call
    // TODO
//    __ profile_call(rbcp);
//    __ profile_arguments_type(r3, rmethod, r13, false);

    __ write_insts_verify_oop(__ x0, "broken oop");

    __ write_insts_jump_from_interpreted(__ x12, __ x0);
}

void YuhuTemplateTable::_new() {
    transition(vtos, atos);

    __ write_insts_get_unsigned_2_byte_index_at_bcp(__ w3, 1);
    YuhuLabel slow_case;
    YuhuLabel done;
    YuhuLabel initialize_header;
    YuhuLabel initialize_object; // including clearing the fields
    YuhuLabel allocate_shared;

    __ write_insts_get_cpool_and_tags(__ x4, __ x0);
    // Make sure the class we're about to instantiate has been resolved.
    // This is done before loading InstanceKlass to be consistent with the order
    // how Constant Pool is updated (see ConstantPool::klass_at_put)
    const int tags_offset = Array<u1>::base_offset_in_bytes();
    __ write_insts_lea(__ x8, YuhuAddress(__ x0, __ x3, YuhuAddress::lsl(0)));
    __ write_insts_lea(__ x8, YuhuAddress(__ x8, tags_offset));
    __ write_inst("ldarb w8, [x8]");
    __ write_inst("cmp x8, #%d", JVM_CONSTANT_Class);
    __ write_inst_b(__ ne, slow_case);

    // get InstanceKlass
    __ write_insts_lea(__ x4, YuhuAddress(__ x4, __ x3, YuhuAddress::lsl(3)));
    __ write_inst_ldr(__ x4, YuhuAddress(__ x4, sizeof(ConstantPool)));

    // make sure klass is initialized & doesn't have finalizer
    // make sure klass is fully initialized
    __ write_inst_ldrb(__ w8, YuhuAddress(__ x4, InstanceKlass::init_state_offset()));
    __ write_inst("cmp x8, #%d", InstanceKlass::fully_initialized);
    __ write_inst_b(__ ne, slow_case);

    // get instance_size in InstanceKlass (scaled to a count of bytes)
    __ write_inst_ldr(__ w3, YuhuAddress(__ x4, Klass::layout_helper_offset()));
    // test to see if it has a finalizer or is malformed in some way
    __ write_inst_tbnz(__ x3, exact_log2(Klass::_lh_instance_slow_path_bit), slow_case);

    // Allocate the instance
    // 1) Try to allocate in the TLAB
    // 2) if fail and the object is large allocate in the shared Eden
    // 3) if the above fails (or is not applicable), go to a slow case
    // (creates a new TLAB, etc.)

    const bool allow_shared_alloc =
            Universe::heap()->supports_inline_contig_alloc() && !CMSIncrementalMode;

    if (UseTLAB) {
        __ write_insts_tlab_allocate(__ x0, __ x3, 0, __ noreg, __ x1,
                         allow_shared_alloc ? allocate_shared : slow_case);

        if (ZeroTLAB) {
            // the fields have been already cleared
            __ write_inst_b(initialize_header);
        } else {
            // initialize both the header and fields
            __ write_inst_b(initialize_object);
        }
    }

    // Allocation in the shared Eden, if allowed.
    //
    // r3: instance size in bytes
    if (allow_shared_alloc) {
        __ pin_label(allocate_shared);

        __ write_insts_eden_allocate(__ x0, __ x3, 0, __ x10, slow_case);
        __ write_insts_incr_allocated_bytes(__ x28, __ x3, 0, __ x8);
    }

    if (UseTLAB || Universe::heap()->supports_inline_contig_alloc()) {
        // The object is initialized before the header.  If the object size is
        // zero, go directly to the header initialization.
        __ pin_label(initialize_object);
        __ write_inst("sub %s, %s, #%d", __ x3, __ x3, sizeof(oopDesc));
        __ write_inst_cbz(__ x3, initialize_header);

        // Initialize object fields
        {
            __ write_inst("add %s, %s, #%d", __ x2, __ x0, sizeof(oopDesc));
            YuhuLabel loop;
            __ pin_label(loop);
            __ write_inst_str(__ xzr, YuhuAddress(YuhuPost(__ x2, BytesPerLong)));
            __ write_inst("sub x3, x3, #%d", BytesPerLong);
            __ write_inst_cbnz(__ x3, loop);
        }

        // initialize object header only.
        __ pin_label(initialize_header);
        if (UseBiasedLocking) {
            __ write_inst_ldr(__ x8, YuhuAddress(__ x4, Klass::prototype_header_offset()));
        } else {
            __ write_insts_mov_imm64(__ x8, (intptr_t)markOopDesc::prototype());
        }
        __ write_inst_str(__ x8, YuhuAddress(__ x0, oopDesc::mark_offset_in_bytes()));
        __ write_insts_store_klass_gap(__ x0, __ xzr);  // zero klass gap for compressed oops
        __ write_insts_store_klass(__ x0, __ x4);      // store klass last

        {
            YuhuSkipIfEqual skip(_masm, &DTraceAllocProbes, false);
            // Trigger dtrace event for fastpath
            __ write_insts_push(atos); // save the return value
            __ write_insts_final_call_VM_leaf(
                    CAST_FROM_FN_PTR(address, SharedRuntime::dtrace_object_alloc), __ x0);
            __ write_insts_pop(atos); // restore the return value

        }
        __ write_inst_b(done);
    }

    // slow case
    __ pin_label(slow_case);
    __ write_insts_get_constant_pool(__ x1);
    __ write_insts_get_unsigned_2_byte_index_at_bcp(__ w2, 1);
    __ write_insts_final_call_VM(__ x0, CAST_FROM_FN_PTR(address, InterpreterRuntime::_new), __ x1, __ x2);
    __ write_insts_verify_oop(__ x0, "broken oop");

    // continue
    __ pin_label(done);
    // Must prevent reordering of stores for object initialization with stores that publish the new object.
    __ write_inst("dmb ishst");
}

void YuhuTemplateTable::newarray() {
    transition(itos, atos);
    __ write_insts_load_unsigned_byte(__ w1, at_bcp(1));
    __ write_inst("mov x2, x0");
    __ write_insts_final_call_VM(__ x0, CAST_FROM_FN_PTR(address, InterpreterRuntime::newarray),
            __ x1, __ x2);
    // Must prevent reordering of stores for object initialization with stores that publish the new object.
    __ write_inst("dmb ishst");
}

void YuhuTemplateTable::anewarray() {
    transition(itos, atos);
    __ write_insts_get_unsigned_2_byte_index_at_bcp(__ w2, 1);
    __ write_insts_get_constant_pool(__ x1);
    __ write_inst("mov x3, x0");
    __ write_insts_final_call_VM(__ x0, CAST_FROM_FN_PTR(address, InterpreterRuntime::anewarray),
            __ x1, __ x2, __ x3);
    // Must prevent reordering of stores for object initialization with stores that publish the new object.
    __ write_inst("dmb ishst");
}

void YuhuTemplateTable::arraylength() {
    transition(atos, itos);
    __ write_insts_null_check(__ x0, arrayOopDesc::length_offset_in_bytes());
    __ write_inst_ldr(__ w0, YuhuAddress(__ x0, arrayOopDesc::length_offset_in_bytes()));
}

void YuhuTemplateTable::athrow() {
    transition(atos, vtos);
    __ write_insts_null_check(__ x0);
    __ write_inst_b(YuhuInterpreter::throw_exception_entry());
}

void YuhuTemplateTable::checkcast()
{
    transition(atos, atos);
    YuhuLabel done, is_null, ok_is_subtype, quicked, resolved;
    __ write_inst_cbz(__ x0, is_null);

    // Get cpool & tags index
    __ write_insts_get_cpool_and_tags(__ x2, __ x3); // r2=cpool, r3=tags array
    __ write_insts_get_unsigned_2_byte_index_at_bcp(__ w19, 1); // r19=index
    // See if bytecode has already been quicked
    __ write_inst("add x8, x3, #%d", Array<u1>::base_offset_in_bytes());
    __ write_insts_lea(__ x1, YuhuAddress(__ x8, __ x19));
    __ write_inst("ldarb w1, [x1]");
    __ write_inst("cmp x1, #%d", JVM_CONSTANT_Class);
    __ write_inst_b(__ eq, quicked);

    __ write_insts_push(atos); // save receiver for result, and for GC
    __ write_insts_final_call_VM(__ x0, CAST_FROM_FN_PTR(address, InterpreterRuntime::quicken_io_cc));
    // vm_result_2 has metadata result
    __ write_insts_get_vm_result_2(__ x0, __ x28);
    __ write_inst_pop(__ x3); // restore receiver
    __ write_inst_b(resolved);

    // Get superklass in r0 and subklass in r3
    __ pin_label(quicked);
    __ write_inst("mov x3, x0"); // Save object in r3; r0 needed for subtype check
    __ write_insts_lea(__ x0, YuhuAddress(__ x2, __ x19, YuhuAddress::lsl(3)));
    __ write_inst_ldr(__ x0, YuhuAddress(__ x0, sizeof(ConstantPool)));

    __ pin_label(resolved);
    __ write_insts_load_klass(__ x19, __ x3);

    // Generate subtype check.  Blows r2, r5.  Object in r3.
    // Superklass in r0.  Subklass in r19.
    __ write_insts_gen_subtype_check(__ x19, ok_is_subtype);

    // Come here on failure
    __ write_inst_push(__ x3);
    // object is at TOS
    __ write_inst_b(YuhuInterpreter::_throw_ClassCastException_entry);

    // Come here on success
    __ pin_label(ok_is_subtype);
    __ write_inst("mov x0, x3"); // Restore object in r3

    // Collect counts on whether this test sees NULLs a lot or not.
    if (ProfileInterpreter) {
        __ write_inst_b(done);
        __ pin_label(is_null);
        // TODO
//        __ profile_null_seen(r2);
    } else {
        __ pin_label(is_null);   // same as 'done'
    }
    __ pin_label(done);
}

void YuhuTemplateTable::instanceof() {
    transition(atos, itos);
    YuhuLabel done, is_null, ok_is_subtype, quicked, resolved;
    __ write_inst_cbz(__ x0, is_null);

    // Get cpool & tags index
    __ write_insts_get_cpool_and_tags(__ x2, __ x3); // r2=cpool, r3=tags array
    __ write_insts_get_unsigned_2_byte_index_at_bcp(__ w19, 1); // r19=index
    // See if bytecode has already been quicked
    __ write_inst("add x8, x3, #%d", Array<u1>::base_offset_in_bytes());
    __ write_insts_lea(__ x1, YuhuAddress(__ x8, __ x19));
    __ write_inst("ldarb w1, [x1]");
    __ write_inst("cmp x1, #%d", JVM_CONSTANT_Class);
    __ write_inst_b(__ eq, quicked);

    __ write_insts_push(atos); // save receiver for result, and for GC
    __ write_insts_final_call_VM(__ x0, CAST_FROM_FN_PTR(address, InterpreterRuntime::quicken_io_cc));
    // vm_result_2 has metadata result
    __ write_insts_get_vm_result_2(__ x0, __ x28);
    __ write_inst_pop(__ x3); // restore receiver
    __ write_insts_verify_oop(__ x3, "broken oop");
    __ write_insts_load_klass(__ x3, __ x3);
    __ write_inst_b(resolved);

    // Get superklass in r0 and subklass in r3
    __ pin_label(quicked);
    __ write_insts_load_klass(__ x3, __ x0);
    __ write_insts_lea(__ x0, YuhuAddress(__ x2, __ x19, YuhuAddress::lsl(3)));
    __ write_inst_ldr(__ x0, YuhuAddress(__ x0, sizeof(ConstantPool)));

    __ pin_label(resolved);

    // Generate subtype check.  Blows r2, r5
    // Superklass in r0.  Subklass in r3.
    __ write_insts_gen_subtype_check(__ x3, ok_is_subtype);

    // Come here on failure
    __ write_insts_mov_imm64(__ x0, 0);
    __ write_inst_b(done);
    // Come here on success
    __ pin_label(ok_is_subtype);
    __ write_insts_mov_imm64(__ x0, 1);

    // Collect counts on whether this test sees NULLs a lot or not.
    if (ProfileInterpreter) {
        __ write_inst_b(done);
        __ pin_label(is_null);
        // TODO
//        __ profile_null_seen(r2);
    } else {
        __ pin_label(is_null);   // same as 'done'
    }
    __ pin_label(done);
    // r0 = 0: obj == NULL or  obj is not an instanceof the specified klass
    // r0 = 1: obj != NULL and obj is     an instanceof the specified klass
}

void YuhuTemplateTable::monitorenter()
{
    transition(atos, vtos);

    // check for NULL object
    __ write_insts_null_check(__ x0);

    const YuhuAddress monitor_block_top(
            __ x29, frame::interpreter_frame_monitor_block_top_offset * wordSize);
    const YuhuAddress monitor_block_bot(
            __ x29, frame::interpreter_frame_initial_sp_offset * wordSize);
    const int entry_size = frame::interpreter_frame_monitor_size() * wordSize;

    YuhuLabel allocated;

    // initialize entry pointer
    __ write_inst("mov x1, xzr");  // points to free slot or NULL

    // find a free slot in the monitor block (result in c_rarg1)
    {
        YuhuLabel entry, loop, exit;
        __ write_inst_ldr(__ x3, monitor_block_top); // points to current entry,
        // starting with top-most entry
        __ write_insts_lea(__ x2, monitor_block_bot); // points to word before bottom

        __ write_inst_b(entry);

        __ pin_label(loop);
        // check if current entry is used
        // if not used then remember entry in c_rarg1
        __ write_inst_ldr(__ x8, YuhuAddress(__ x3, BasicObjectLock::obj_offset_in_bytes()));
        __ write_inst_regs("cmp %s, %s", __ xzr, __ x8);
        __ write_inst_csel(__ x1, __ x3, __ x1, __ eq);
        // check if current entry is for same object
        __ write_inst("cmp x0, x8");
        // if same object then stop searching
        __ write_inst_b(__ eq, exit);
        // otherwise advance to next entry
        __ write_inst("add x3, x3, #%d", entry_size);
        __ pin_label(entry);
        // check if bottom reached
        __ write_inst("cmp x3, x2");
        // if not at bottom then check this entry
        __ write_inst_b(__ ne, loop);
        __ pin_label(exit);
    }

    __ write_inst_cbnz(__ x1, allocated); // check if a slot has been found and
    // if found, continue with that on

    // allocate one if there's no free slot
    {
        YuhuLabel entry, loop;
        // 1. compute new pointers            // rsp: old expression stack top
        __ write_inst_ldr(__ x1, monitor_block_bot); // c_rarg1: old expression stack bottom
        __ write_inst("sub x20, x20, #%d", entry_size);
        __ write_inst("sub x1, x1, #%d", entry_size); // move expression stack bottom
        __ write_inst("mov x3, x20"); // set start value for copy loop
        __ write_inst_str(__ x1, monitor_block_bot); // set new monitor block bottom

        __ write_inst("sub sp, sp, #%d", entry_size); // make room for the monitor

        __ write_inst_b(entry);
        // 2. move expression stack contents
        __ pin_label(loop);
        __ write_inst_ldr(__ x2, YuhuAddress(__ x3, entry_size)); // load expression stack
        // word from old location
        __ write_inst_str(__ x2, YuhuAddress(__ x3, 0)); // and store it at new location
        __ write_inst("add x3, x3, #%d", wordSize); // advance to next word
        __ pin_label(entry);
        __ write_inst("cmp x3, x1"); // check if bottom reached
        __ write_inst_b(__ ne, loop); // if not at bottom then
        // copy next word
    }

    // call run-time routine
    // c_rarg1: points to monitor entry
    __ pin_label(allocated);

    // Increment bcp to point to the next bytecode, so exception
    // handling for async. exceptions work correctly.
    // The object has already been poped from the stack, so the
    // expression stack looks correct.
    __ write_insts_increment(__ x22);

    // store object
    __ write_inst_str(__ x0, YuhuAddress(__ x1, BasicObjectLock::obj_offset_in_bytes()));
    __ write_insts_lock_object(__ x1);

    // check to make sure this monitor doesn't cause stack overflow after locking
    __ write_insts_save_bcp();  // in case of exception
    __ write_insts_generate_stack_overflow_check(0);

    // The bcp has already been incremented. Just need to dispatch to
    // next instruction.
    __ write_insts_dispatch_next(vtos);
}


void YuhuTemplateTable::monitorexit()
{
    transition(atos, vtos);

    // check for NULL object
    __ write_insts_null_check(__ x0);

    const YuhuAddress monitor_block_top(
            __ x29, frame::interpreter_frame_monitor_block_top_offset * wordSize);
    const YuhuAddress monitor_block_bot(
            __ x29, frame::interpreter_frame_initial_sp_offset * wordSize);
    const int entry_size = frame::interpreter_frame_monitor_size() * wordSize;

    YuhuLabel found;

    // find matching slot
    {
        YuhuLabel entry, loop;
        __ write_inst_ldr(__ x1, monitor_block_top); // points to current entry,
        // starting with top-most entry
        __ write_insts_lea(__ x2, monitor_block_bot); // points to word before bottom
        // of monitor block
        __ write_inst_b(entry);

        __ pin_label(loop);
        // check if current entry is for same object
        __ write_inst_ldr(__ x8, YuhuAddress(__ x1, BasicObjectLock::obj_offset_in_bytes()));
        __ write_inst("cmp x0, x8");
        // if same object then stop searching
        __ write_inst_b(__ eq, found);
        // otherwise advance to next entry
        __ write_inst("add x1, x1, #%d", entry_size);
        __ pin_label(entry);
        // check if bottom reached
        __ write_inst("cmp x1, x2");
        // if not at bottom then check this entry
        __ write_inst_b(__ ne, loop);
    }

    // error handling. Unlocking was not block-structured
    __ write_insts_final_call_VM(__ noreg, CAST_FROM_FN_PTR(address,
                                       InterpreterRuntime::throw_illegal_monitor_state_exception));
    __ write_insts_stop("should not reach here");

    // call run-time routine
    __ pin_label(found);
    __ write_inst_push_ptr(__ x0); // make sure object is on stack (contract with oopMaps)
    __ write_insts_unlock_object(__ x1);
    __ write_inst_pop_ptr(__ x0); // discard object
}

void YuhuTemplateTable::wide()
{
    __ write_insts_load_unsigned_byte(__ w19, at_bcp(1));
    __ write_insts_mov_imm64(__ x8, (uint64_t)(address)YuhuInterpreter::_wentry_point);
    __ write_inst_ldr(__ x8, YuhuAddress(__ x8, __ w19, YuhuAddress::uxtw(3)));
    __ write_inst_br(__ x8);
}


// Multi arrays
void YuhuTemplateTable::multianewarray() {
    transition(vtos, atos);
    __ write_insts_load_unsigned_byte(__ w0, at_bcp(3)); // get number of dimensions
    // last dim is on top of stack; we want address of first one:
    // first_addr = last_addr + (ndims - 1) * wordSize
    __ write_insts_lea(__ x1, YuhuAddress(__ x20, __ w0, YuhuAddress::uxtw(3)));
    __ write_inst("sub x1, x1, #%d", wordSize);
    __ write_insts_final_call_VM(__ x0,
            CAST_FROM_FN_PTR(address, InterpreterRuntime::multianewarray),
            __ x1);
    __ write_insts_load_unsigned_byte(__ w1, at_bcp(3));
    __ write_insts_lea(__ x20, YuhuAddress(__ x20, __ w1, YuhuAddress::uxtw(3)));
}

void YuhuTemplateTable::if_nullcmp(Condition cc)
{
    transition(atos, vtos);
    // assume branch is more often taken than not (loops use backward branches)
    YuhuLabel not_taken;
    if (cc == equal)
        __ write_inst_cbnz(__ x0, not_taken);
    else
        __ write_inst_cbz(__ x0, not_taken);
    branch(false, false);
    __ pin_label(not_taken);
    // TODO
//    __ profile_not_taken_branch(r0);
}

void YuhuTemplateTable::locals_index_wide(YuhuMacroAssembler::YuhuRegister reg) {
    __ write_inst_ldrh(__ w_reg(reg), at_bcp(2));
    __ write_inst_regs("rev16 %s, %s", __ w_reg(reg), __ w_reg(reg));
    __ write_inst_regs("neg %s, %s", reg, reg);
}

void YuhuTemplateTable::wide_iload() {
    transition(vtos, itos);
    locals_index_wide(__ x1);
    __ write_inst_ldr(__ x0, iaddress(__ x1));
}

void YuhuTemplateTable::wide_lload()
{
    transition(vtos, ltos);
    __ write_inst_ldrh(__ w1, at_bcp(2));
    __ write_inst("rev16 w1, w1");
    __ write_inst("sub x1, x24, w1, uxtw #%d", LogBytesPerWord);
    __ write_inst_ldr(__ x0, YuhuAddress(__ x1, YuhuInterpreter::local_offset_in_bytes(1)));
}

void YuhuTemplateTable::wide_fload()
{
    transition(vtos, ftos);
    locals_index_wide(__ x1);
    // n.b. we use ldrd here because this is a 64 bit slot
    // this is comparable to the iload case
    __ write_inst_ldr(__ d0, faddress(__ x1));
}

void YuhuTemplateTable::wide_dload()
{
    transition(vtos, dtos);
    __ write_inst_ldrh(__ w1, at_bcp(2));
    __ write_inst("rev16 w1, w1");
    __ write_inst("sub x1, x24, w1, uxtw #%d", LogBytesPerWord);
    __ write_inst_ldr(__ d0, YuhuAddress(__ x1, YuhuInterpreter::local_offset_in_bytes(1)));
}

void YuhuTemplateTable::wide_aload()
{
    transition(vtos, atos);
    locals_index_wide(__ x1);
    __ write_inst_ldr(__ x0, aaddress(__ x1));
}

void YuhuTemplateTable::wide_istore() {
    transition(vtos, vtos);
    __ write_inst_pop_i();
    locals_index_wide(__ x1);
    __ write_insts_lea(__ x8, iaddress(__ x1));
    __ write_inst_str(__ w0, YuhuAddress(__ x8));
}

void YuhuTemplateTable::wide_lstore() {
    transition(vtos, vtos);
    __ write_inst_pop_l();
    locals_index_wide(__ x1);
    __ write_inst_str(__ x0, laddress(__ x1, __ x8, _masm));
}

void YuhuTemplateTable::wide_fstore() {
    transition(vtos, vtos);
    __ write_inst_pop_f();
    locals_index_wide(__ x1);
    __ write_insts_lea(__ x8, faddress(__ x1));
    __ write_inst_str(__ s0, __ x8);
}

void YuhuTemplateTable::wide_dstore() {
    transition(vtos, vtos);
    __ write_inst_pop_d();
    locals_index_wide(__ x1);
    __ write_inst_str(__ d0, daddress(__ x1, __ x8, _masm));
}

void YuhuTemplateTable::wide_astore() {
    transition(vtos, vtos);
    __ write_inst_pop_ptr(__ x0);
    locals_index_wide(__ x1);
    __ write_inst_str(__ x0, aaddress((__ x1)));
}

void YuhuTemplateTable::wide_iinc()
{
    transition(vtos, vtos);
    // __ mov(r1, zr);
    __ write_inst_ldr(__ w1, at_bcp(2)); // get constant and index
    __ write_inst("rev16 w1, w1");
    __ write_inst("ubfx x2, x1, #0, #16");
    __ write_inst("neg x2, x2");
    __ write_inst("sbfx x1, x1, #16, #16");
    __ write_inst("add w0, w0, w1");
    __ write_inst_str(__ x0, iaddress(__ x2));
}

void YuhuTemplateTable::wide_ret() {
    transition(vtos, vtos);
    locals_index_wide(__ x1);
    __ write_inst_ldr(__ x1, aaddress(__ x1)); // get return bci, compute return bcp
    // TODO
//    __ profile_ret(r1, r2);
    __ write_inst_ldr(__ x22, YuhuAddress(__ x12, Method::const_offset()));
    __ write_insts_lea(__ x22, YuhuAddress(__ x22, __ x1));
    __ write_inst("add x22, x22, #%d", in_bytes(ConstMethod::codes_offset()));
    __ write_insts_dispatch_next(vtos);
}

void YuhuTemplateTable::_breakpoint() {
    // Note: We get here even if we are single stepping..
    // jbug inists on setting breakpoints at every bytecode
    // even if we are in single step mode.

    transition(vtos, vtos);

    // get the unpatched byte code
    __ write_insts_get_method(__ x1);
    __ write_insts_final_call_VM(__ noreg,
               CAST_FROM_FN_PTR(address,
                                InterpreterRuntime::get_original_bytecode_at),
               __ x1, __ x22);
    __ write_inst("mov x19, x0");

    // post the breakpoint event
    __ write_insts_final_call_VM(__ noreg,
               CAST_FROM_FN_PTR(address, InterpreterRuntime::_breakpoint),
               __ x12, __ x22);

    // complete the execution of original bytecode
    __ write_inst("mov x8, x19");
    __ write_insts_dispatch_only_normal(vtos);
}

void YuhuTemplateTable::fast_accessfield(TosState state)
{
    transition(atos, state);
    // Do the JVMTI work here to avoid disturbing the register state below
    // TODO
//    if (JvmtiExport::can_post_field_access()) {
//        // Check to see if a field access watch has been set before we
//        // take the time to call into the VM.
//        Label L1;
//        __ lea(rscratch1, ExternalAddress((address) JvmtiExport::get_field_access_count_addr()));
//        __ ldrw(r2, Address(rscratch1));
//        __ cbzw(r2, L1);
//        // access constant pool cache entry
//        __ get_cache_entry_pointer_at_bcp(c_rarg2, rscratch2, 1);
//        __ verify_oop(r0);
//        __ push_ptr(r0);  // save object pointer before call_VM() clobbers it
//        __ mov(c_rarg1, r0);
//        // c_rarg1: object pointer copied above
//        // c_rarg2: cache entry pointer
//        __ call_VM(noreg,
//                   CAST_FROM_FN_PTR(address,
//                                    InterpreterRuntime::post_field_access),
//                   c_rarg1, c_rarg2);
//        __ pop_ptr(r0); // restore object pointer
//        __ bind(L1);
//    }

    // access constant pool cache
    __ write_insts_get_cache_and_index_at_bcp(__ x2, __ x1, 1);

    // Must prevent reordering of the following cp cache loads with bytecode load
    __ write_inst("dmb ishld");

    __ write_inst_ldr(__ x1, YuhuAddress(__ x2, in_bytes(ConstantPoolCache::base_offset() +
                                                         ConstantPoolCacheEntry::f2_offset())));
    __ write_inst_ldr(__ w3, YuhuAddress(__ x2, in_bytes(ConstantPoolCache::base_offset() +
                                                         ConstantPoolCacheEntry::flags_offset())));

    // r0: object
    __ write_insts_verify_oop(__ x0, "broken oop");
    __ write_insts_null_check(__ x0);
    const YuhuAddress field(__ x0, __ x1);

    // 8179954: We need to make sure that the code generated for
    // volatile accesses forms a sequentially-consistent set of
    // operations when combined with STLR and LDAR.  Without a leading
    // membar it's possible for a simple Dekker test to fail if loads
    // use LDR;DMB but stores use STLR.  This can happen if C2 compiles
    // the stores in one method and we interpret the loads in another.
    if (! UseBarriersForVolatile) {
        YuhuLabel notVolatile;
        __ write_inst_tbz(__ x3, ConstantPoolCacheEntry::is_volatile_shift, notVolatile);
        __ write_inst("dmb ish");
        __ pin_label(notVolatile);
    }

    // access field
    switch (bytecode()) {
        case Bytecodes::_fast_agetfield:
            __ write_insts_load_heap_oop(__ x0, field);
            __ write_insts_verify_oop(__ x0, "broken oop");
            break;
        case Bytecodes::_fast_lgetfield:
            __ write_inst_ldr(__ x0, field);
            break;
        case Bytecodes::_fast_igetfield:
            __ write_inst_ldr(__ w0, field);
            break;
        case Bytecodes::_fast_bgetfield:
            __ write_insts_load_signed_byte(__ w0, field);
            break;
        case Bytecodes::_fast_sgetfield:
            __ write_insts_load_signed_short(__ w0, field);
            break;
        case Bytecodes::_fast_cgetfield:
            __ write_insts_load_unsigned_short(__ w0, field);
            break;
        case Bytecodes::_fast_fgetfield:
            __ write_inst_ldr(__ s0, field);
            break;
        case Bytecodes::_fast_dgetfield:
            __ write_inst_ldr(__ d0, field);
            break;
        default:
            ShouldNotReachHere();
    }
    {
        YuhuLabel notVolatile;
        __ write_inst_tbz(__ x3, ConstantPoolCacheEntry::is_volatile_shift, notVolatile);
        __ write_inst("dmb ishld");
        __ pin_label(notVolatile);
    }
}

void YuhuTemplateTable::fast_storefield(TosState state)
{
    transition(state, vtos);

    ByteSize base = ConstantPoolCache::base_offset();

    // TODO
//    jvmti_post_fast_field_mod();

    // access constant pool cache
    __ write_insts_get_cache_and_index_at_bcp(__ x2, __ x1, 1);

    // Must prevent reordering of the following cp cache loads with bytecode load
    __ write_inst("dmb ishld");

    // test for volatile with r3
    __ write_inst_ldr(__ w3, YuhuAddress(__ x2, in_bytes(base +
                                     ConstantPoolCacheEntry::flags_offset())));

    // replace index with field offset from cache entry
    __ write_inst_ldr(__ x1, YuhuAddress(__ x2, in_bytes(base + ConstantPoolCacheEntry::f2_offset())));

    {
        YuhuLabel notVolatile;
        __ write_inst_tbz(__ x3, ConstantPoolCacheEntry::is_volatile_shift, notVolatile);
        __ write_inst("dmb ish");
        __ pin_label(notVolatile);
    }

    YuhuLabel notVolatile;

    // Get object from stack
    pop_and_check_object(__ x2);

    // field address
    const YuhuAddress field(__ x2, __ x1);

    // access field
    switch (bytecode()) {
        case Bytecodes::_fast_aputfield:
            do_oop_store(_masm, field, __ x0, _bs->kind(), false);
            break;
        case Bytecodes::_fast_lputfield:
            __ write_inst_str(__ x0, field);
            break;
        case Bytecodes::_fast_iputfield:
            __ write_inst_str(__ w0, field);
            break;
            /*case Bytecodes::_fast_zputfield:
              __ andw(r0, r0, 0x1);  // boolean is true if LSB is 1*/
            // fall through to bputfield
        case Bytecodes::_fast_bputfield:
            __ write_inst_strb(__ w0, field);
            break;
        case Bytecodes::_fast_sputfield:
            // fall through
        case Bytecodes::_fast_cputfield:
            __ write_inst_strh(__ w0, field);
            break;
        case Bytecodes::_fast_fputfield:
            __ write_inst_str(__ s0, field);
            break;
        case Bytecodes::_fast_dputfield:
            __ write_inst_str(__ d0, field);
            break;
        default:
            ShouldNotReachHere();
    }

    {
        YuhuLabel notVolatile;
        __ write_inst_tbz(__ x3, ConstantPoolCacheEntry::is_volatile_shift, notVolatile);
        __ write_inst("dmb ish");
        __ pin_label(notVolatile);
    }
}

void YuhuTemplateTable::fast_xaccess(TosState state)
{
    transition(vtos, state);

    // get receiver
    __ write_inst_ldr(__ x0, aaddress(0));
    // access constant pool cache
    __ write_insts_get_cache_and_index_at_bcp(__ x2, __ x3, 2);
    __ write_inst_ldr(__ x1, YuhuAddress(__ x2, in_bytes(ConstantPoolCache::base_offset() +
                                    ConstantPoolCacheEntry::f2_offset())));

    // 8179954: We need to make sure that the code generated for
    // volatile accesses forms a sequentially-consistent set of
    // operations when combined with STLR and LDAR.  Without a leading
    // membar it's possible for a simple Dekker test to fail if loads
    // use LDR;DMB but stores use STLR.  This can happen if C2 compiles
    // the stores in one method and we interpret the loads in another.
    if (! UseBarriersForVolatile) {
        YuhuLabel notVolatile;
        __ write_inst_ldr(__ w3, YuhuAddress(__ x2, in_bytes(ConstantPoolCache::base_offset() +
                                         ConstantPoolCacheEntry::flags_offset())));
        __ write_inst_tbz(__ x3, ConstantPoolCacheEntry::is_volatile_shift, notVolatile);
        __ write_inst("dmb ish");
        __ pin_label(notVolatile);
    }

    // make sure exception is reported in correct bcp range (getfield is
    // next instruction)
    __ write_insts_increment(__ x22);
    __ write_insts_null_check(__ x0);
    switch (state) {
        case itos:
            __ write_inst_ldr(__ w0, YuhuAddress(__ x0, __ x1, YuhuAddress::lsl(0)));
            break;
        case atos:
            __ write_insts_load_heap_oop(__ x0, YuhuAddress(__ x0, __ x1, YuhuAddress::lsl(0)));
            __ write_insts_verify_oop(__ x0, "broken oop");
            break;
        case ftos:
            __ write_inst_ldr(__ s0, YuhuAddress(__ x0, __ x1, YuhuAddress::lsl(0)));
            break;
        default:
            ShouldNotReachHere();
    }

    {
        YuhuLabel notVolatile;
        __ write_inst_ldr(__ w3, YuhuAddress(__ x2, in_bytes(ConstantPoolCache::base_offset() +
                                         ConstantPoolCacheEntry::flags_offset())));
        __ write_inst_tbz(__ x3, ConstantPoolCacheEntry::is_volatile_shift, notVolatile);
        __ write_inst("dmb ishld");
        __ pin_label(notVolatile);
    }

    __ write_insts_decrement(__ x22);
}

void YuhuTemplateTable::fast_iload()
{
    transition(vtos, itos);
    locals_index(__ x1);
    __ write_inst_ldr(__ x0, iaddress(__ x1));
}

void YuhuTemplateTable::fast_iload2()
{
    transition(vtos, itos);
    locals_index(__ x1);
    __ write_inst_ldr(__ x0, iaddress(__ x1));
    __ write_insts_push(itos);
    locals_index(__ x1, 3);
    __ write_inst_ldr(__ x0, iaddress(__ x1));
}

void YuhuTemplateTable::fast_icaload()
{
    transition(vtos, itos);
    // load index out of locals
    locals_index(__ x2);
    __ write_inst_ldr(__ x1, iaddress(__ x2));

    __ write_inst_pop_ptr(__ x0);

    // r0: array
    // r1: index
    index_check(__ x0, __ x1); // leaves index in r1, kills rscratch1
    __ write_insts_lea(__ x1,  YuhuAddress(__ x0, __ w1, YuhuAddress::uxtw(1)));
    __ write_insts_load_unsigned_short(__ w0, YuhuAddress(__ x1,  arrayOopDesc::base_offset_in_bytes(T_CHAR)));
}

void YuhuTemplateTable::fast_invokevfinal(int byte_no)
{
    __ call_Unimplemented();
}

void YuhuTemplateTable::fast_linearswitch() {
    transition(itos, vtos);
    YuhuLabel loop_entry, loop, found, continue_execution;
    // bswap r0 so we can avoid bswapping the table entries
    __ write_inst("rev32 x0, x0");
    // align rbcp
    __ write_insts_lea(__ x19, at_bcp(BytesPerInt)); // btw: should be able to get rid of
    // this instruction (change offsets
    // below)
    __ write_inst("and x19, x19, #%d", -BytesPerInt);
    // set counter
    __ write_inst_ldr(__ w1, YuhuAddress(__ x19, BytesPerInt));
    __ write_inst("rev32 x1, x1");
    __ write_inst_b(loop_entry);
    // table search
    __ pin_label(loop);
    __ write_insts_lea(__ x8, YuhuAddress(__ x19, __ x1, YuhuAddress::lsl(3)));
    __ write_inst_ldr(__ w8, YuhuAddress(__ x8, 2 * BytesPerInt));
    __ write_inst("cmp w0, w8");
    __ write_inst_b(__ eq, found);
    __ pin_label(loop_entry);
    __ write_inst("subs x1, x1, #1");
    __ write_inst_b(__ pl, loop);
    // default case
    // TODO
//    __ profile_switch_default(r0);
    __ write_inst_ldr(__ w3, YuhuAddress(__ x19, 0));
    __ write_inst_b(continue_execution);
    // entry found -> get offset
    __ pin_label(found);
    __ write_insts_lea(__ x8, YuhuAddress(__ x19, __ x1, YuhuAddress::lsl(3)));
    __ write_inst_ldr(__ w3, YuhuAddress(__ x8, 3 * BytesPerInt));
    // TODO
//    __ profile_switch_case(r1, r0, r19);
    // continue execution
    __ pin_label(continue_execution);
    __ write_inst("rev32 x3, x3");
    __ write_inst("add x22, x22, w3, sxtw #0");
    __ write_inst_ldrb(__ w8, YuhuAddress(__ x22, 0));
    __ write_insts_dispatch_only(vtos);
}

void YuhuTemplateTable::fast_binaryswitch() {
    transition(itos, vtos);
    // Implementation using the following core algorithm:
    //
    // int binary_search(int key, LookupswitchPair* array, int n) {
    //   // Binary search according to "Methodik des Programmierens" by
    //   // Edsger W. Dijkstra and W.H.J. Feijen, Addison Wesley Germany 1985.
    //   int i = 0;
    //   int j = n;
    //   while (i+1 < j) {
    //     // invariant P: 0 <= i < j <= n and (a[i] <= key < a[j] or Q)
    //     // with      Q: for all i: 0 <= i < n: key < a[i]
    //     // where a stands for the array and assuming that the (inexisting)
    //     // element a[n] is infinitely big.
    //     int h = (i + j) >> 1;
    //     // i < h < j
    //     if (key < array[h].fast_match()) {
    //       j = h;
    //     } else {
    //       i = h;
    //     }
    //   }
    //   // R: a[i] <= key < a[i+1] or Q
    //   // (i.e., if key is within array, i is the correct index)
    //   return i;
    // }

    // Register allocation
    const YuhuMacroAssembler::YuhuRegister key   = __ x0; // already set (tosca)
    const YuhuMacroAssembler::YuhuRegister array = __ x1;
    const YuhuMacroAssembler::YuhuRegister i     = __ x2;
    const YuhuMacroAssembler::YuhuRegister j     = __ x3;
    const YuhuMacroAssembler::YuhuRegister h     = __ x8;
    const YuhuMacroAssembler::YuhuRegister temp  = __ x9;

    // Find array start
    __ write_insts_lea(array, at_bcp(3 * BytesPerInt)); // btw: should be able to
    // get rid of this
    // instruction (change
    // offsets below)
    __ write_inst("and %s, %s, #%d", array, array, -BytesPerInt);

    // Initialize i & j
    __ write_insts_mov_imm32(i, 0); // i = 0;
    __ write_inst_ldr(__ w_reg(j), YuhuAddress(array, -BytesPerInt)); // j = length(array);

    // Convert j into native byteordering
    __ write_inst_regs("rev32 %s, %s", j, j);

    // And start
    YuhuLabel entry;
    __ write_inst_b(entry);

    // binary search loop
    {
        YuhuLabel loop;
        __ pin_label(loop);
        // int h = (i + j) >> 1;
        __ write_inst_regs("add %s, %s, %s", __ w_reg(h), __ w_reg(i), __ w_reg(j)); // h = i + j;
        __ write_inst("lsr %s, %s, #%d", __ w_reg(h), __ w_reg(h), 1); // h = (i + j) >> 1;
        // if (key < array[h].fast_match()) {
        //   j = h;
        // } else {
        //   i = h;
        // }
        // Convert array[h].match to native byte-ordering before compare
        __ write_inst_ldr(temp, YuhuAddress(array, h, YuhuAddress::lsl(3)));
        __ write_inst_regs("rev32 %s, %s", temp, temp);
        __ write_inst_regs("cmp %s, %s", __ w_reg(key), __ w_reg(temp));
        // j = h if (key <  array[h].fast_match())
        __ write_inst_csel(j, h, j, __ lt);
        // i = h if (key >= array[h].fast_match())
        __ write_inst_csel(i, h, i, __ ge);
        // while (i+1 < j)
        __ pin_label(entry);
        __ write_inst("add %s, %s, #%d", __ w_reg(h), __ w_reg(i), 1); // i+1
        __ write_inst_regs("cmp %s, %s", __ w_reg(h), __ w_reg(j)); // i+1 < j
        __ write_inst_b(__ lt, loop);
    }

    // end of binary search, result index is i (must check again!)
    YuhuLabel default_case;
    // Convert array[i].match to native byte-ordering before compare
    __ write_inst_ldr(temp, YuhuAddress(array, i, YuhuAddress::lsl(3)));
    __ write_inst_regs("rev32 %s, %s", temp, temp);
    __ write_inst_regs("cmp %s, %s", __ w_reg(key), __ w_reg(temp));
    __ write_inst_b(__ ne, default_case);

    // entry found -> j = offset
    __ write_inst_regs("add %s, %s, %s, uxtx #3", j, array, i);
    __ write_inst_ldr(__ w_reg(j), YuhuAddress(j, BytesPerInt));
    // TODO
//    __ profile_switch_case(i, key, array);
    __ write_inst_regs("rev32 %s, %s", j, j);
    __ write_insts_load_unsigned_byte(__ w8, YuhuAddress(__ x22, __ w_reg(j), YuhuAddress::sxtw(0)));
    __ write_insts_lea(__ x22, YuhuAddress(__ x22, __ w_reg(j), YuhuAddress::sxtw(0)));
    __ write_insts_dispatch_only(vtos);

    // default case -> j = default offset
    __ pin_label(default_case);
    // TODO
//    __ profile_switch_default(i);
    __ write_inst_ldr(__ w_reg(j), YuhuAddress(array, -2 * BytesPerInt));
    __ write_inst_regs("rev32 %s, %s", j, j);
    __ write_insts_load_unsigned_byte(__ w8, YuhuAddress(__ x22, __ w_reg(j), YuhuAddress::sxtw(0)));
    __ write_insts_lea(__ x22, YuhuAddress(__ x22, __ w_reg(j), YuhuAddress::sxtw(0)));
    __ write_insts_dispatch_only(vtos);
}

void YuhuTemplateTable::fast_aldc(bool wide)
{
    transition(vtos, atos);

    YuhuMacroAssembler::YuhuRegister result = __ x0;
    YuhuMacroAssembler::YuhuRegister tmp = __ x1;
    int index_size = wide ? sizeof(u2) : sizeof(u1);

    YuhuLabel resolved;

    // We are resolved if the resolved reference cache entry contains a
    // non-null object (String, MethodType, etc.)
//    assert_different_registers(result, tmp);
    __ write_insts_get_cache_index_at_bcp(tmp, 1, index_size);
    __ write_insts_load_resolved_reference_at_index(result, tmp);
    __ write_inst_cbnz(result, resolved);

    address entry = CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_ldc);

    // first time invocation - must resolve first
    __ write_insts_mov_imm32(tmp, (int)bytecode());
    __ write_insts_final_call_VM(result, entry, tmp);

    __ pin_label(resolved);

    if (VerifyOops) {
        __ write_insts_verify_oop(result, "broken oop");
    }
}

void YuhuTemplateTable::invokehandle(int byte_no) {
    transition(vtos, vtos);
    assert(byte_no == f1_byte, "use this argument");

    if (!EnableInvokeDynamic) {
        // rewriter does not generate this bytecode
        __ write_insts_stop("should not reach here");
        return;
    }

    prepare_invoke(byte_no, __ x12, __ x0, __ x2);
    __ verify_method_ptr(r2);
    __ write_insts_verify_oop(__ x2, "broken oop");
    __ write_insts_null_check(__ x2);

    // FIXME: profile the LambdaForm also

    // r13 is safe to use here as a scratch reg because it is about to
    // be clobbered by jump_from_interpreted().
    // TODO
//    __ profile_final_call(r13);
//    __ profile_arguments_type(r13, rmethod, r4, true);

    __ write_insts_jump_from_interpreted(__ x12, __ x0);
}

void YuhuTemplateTable::shouldnotreachhere() {
    transition(vtos, vtos);
    __ write_insts_stop("shouldnotreachhere bytecode");
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

YuhuAddress YuhuTemplateTable::at_bcp(int offset) {
    assert(_desc->uses_bcp(), "inconsistent uses_bcp information");
    return YuhuAddress(__ x22, offset);
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
        __ write_insts_load_unsigned_byte(__ w_reg(temp_reg), YuhuAddress(__ x22, 0));
        __ write_inst("cmp %s, #%d", __ w_reg(temp_reg), Bytecodes::_breakpoint);
        __ write_inst_b(__ ne, L_fast_patch);
        // Let breakpoint table handling rewrite to quicker bytecode
        __ write_insts_final_call_VM(__ noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::set_original_bytecode_at), __ x12, __ x22, bc_reg);
        __ write_inst_b(L_patch_done);
        __ pin_label(L_fast_patch);
    }

#ifdef ASSERT
    YuhuLabel L_okay;
    __ write_insts_load_unsigned_byte(__ w_reg(temp_reg), YuhuAddress(__ x22, 0));
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
    __ write_inst_ldrb( __ w_reg(reg), at_bcp(offset));
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

void YuhuTemplateTable::resolve_cache_and_index(int byte_no,
                                            YuhuMacroAssembler::YuhuRegister Rcache,
                                            YuhuMacroAssembler::YuhuRegister index,
                                            size_t index_size) {
    const YuhuMacroAssembler::YuhuRegister temp = __ x19;
//    assert_different_registers(Rcache, index, temp);

    YuhuLabel resolved;
    assert(byte_no == f1_byte || byte_no == f2_byte, "byte_no out of range");
    __ write_insts_get_cache_and_index_and_bytecode_at_bcp(Rcache, index, temp, byte_no, 1, index_size);
    __ write_inst("cmp %s, #%d", temp, (int) bytecode()); // have we resolved this bytecode?
    __ write_inst_b(__ eq, resolved);

    // resolve first time through
    address entry;
    switch (bytecode()) {
        case Bytecodes::_getstatic:
        case Bytecodes::_putstatic:
        case Bytecodes::_getfield:
        case Bytecodes::_putfield:
            entry = CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_get_put);
            break;
        case Bytecodes::_invokevirtual:
        case Bytecodes::_invokespecial:
        case Bytecodes::_invokestatic:
        case Bytecodes::_invokeinterface:
            entry = CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_invoke);
            break;
        case Bytecodes::_invokehandle:
            entry = CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_invokehandle);
            break;
        case Bytecodes::_invokedynamic:
            entry = CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_invokedynamic);
            break;
        default:
            fatal(err_msg("unexpected bytecode: %s", Bytecodes::name(bytecode())));
            break;
    }
    __ write_insts_mov_imm32(temp, (int) bytecode());
    __ write_insts_final_call_VM(__ noreg, entry, temp);

    // Update registers with resolved info
    __ write_insts_get_cache_and_index_at_bcp(Rcache, index, 1, index_size);
    // n.b. unlike x86 Rcache is now rcpool plus the indexed offset
    // so all clients ofthis method must be modified accordingly
    __ pin_label(resolved);
}

void YuhuTemplateTable::load_invoke_cp_cache_entry(int byte_no,
                                                   YuhuMacroAssembler::YuhuRegister method,
                                                   YuhuMacroAssembler::YuhuRegister itable_index,
                                                   YuhuMacroAssembler::YuhuRegister flags,
                                                   bool is_invokevirtual,
                                                   bool is_invokevfinal, /*unused*/
                                                   bool is_invokedynamic) {
    // setup registers
    const YuhuMacroAssembler::YuhuRegister cache = __ x9;
    const YuhuMacroAssembler::YuhuRegister index = __ x4;
//    assert_different_registers(method, flags);
//    assert_different_registers(method, cache, index);
//    assert_different_registers(itable_index, flags);
//    assert_different_registers(itable_index, cache, index);
    // determine constant pool cache field offsets
    assert(is_invokevirtual == (byte_no == f2_byte), "is_invokevirtual flag redundant");
    const int method_offset = in_bytes(
            ConstantPoolCache::base_offset() +
            (is_invokevirtual
             ? ConstantPoolCacheEntry::f2_offset()
             : ConstantPoolCacheEntry::f1_offset()));
    const int flags_offset = in_bytes(ConstantPoolCache::base_offset() +
                                      ConstantPoolCacheEntry::flags_offset());
    // access constant pool cache fields
    const int index_offset = in_bytes(ConstantPoolCache::base_offset() +
                                      ConstantPoolCacheEntry::f2_offset());

    size_t index_size = (is_invokedynamic ? sizeof(u4) : sizeof(u2));
    resolve_cache_and_index(byte_no, cache, index, index_size);
    __ write_inst_ldr(method, YuhuAddress(cache, method_offset));

    if (itable_index != __ noreg) {
        __ write_inst_ldr(itable_index, YuhuAddress(cache, index_offset));
    }
    __ write_inst_ldr(__ w_reg(flags), YuhuAddress(cache, flags_offset));
}

void YuhuTemplateTable::load_field_cp_cache_entry(YuhuMacroAssembler::YuhuRegister obj,
                                                  YuhuMacroAssembler::YuhuRegister cache,
                                                  YuhuMacroAssembler::YuhuRegister index,
                                                  YuhuMacroAssembler::YuhuRegister off,
                                                  YuhuMacroAssembler::YuhuRegister flags,
                                              bool is_static = false) {
//    assert_different_registers(cache, index, flags, off);

    ByteSize cp_base_offset = ConstantPoolCache::base_offset();
    // Field offset
    __ write_inst_ldr(off, YuhuAddress(cache, in_bytes(cp_base_offset +
                                                       ConstantPoolCacheEntry::f2_offset())));
    // Flags
    __ write_inst_ldr(__ w_reg(flags), YuhuAddress(cache, in_bytes(cp_base_offset +
                                                                   ConstantPoolCacheEntry::flags_offset())));

    // klass overwrite register
    if (is_static) {
        __ write_inst_ldr(obj, YuhuAddress(cache, in_bytes(cp_base_offset +
                                                           ConstantPoolCacheEntry::f1_offset())));
        const int mirror_offset = in_bytes(Klass::java_mirror_offset());
        __ write_inst_ldr(obj, YuhuAddress(obj, mirror_offset));
    }
}

void YuhuTemplateTable::pop_and_check_object(YuhuMacroAssembler::YuhuRegister r)
{
    __ write_inst_pop_ptr(r);
    __ write_insts_null_check(r);  // for field access must check obj.
    __ write_insts_verify_oop(r, "broken oop");
}
/*
 * Copyright (c) 1999, 2013, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2008, 2009, 2010 Red Hat, Inc.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "interpreter/bytecodes.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/llvmValue.hpp"
#include "yuhu/yuhuBlock.hpp"
#include "yuhu/yuhuBuilder.hpp"
#include "yuhu/yuhuConstant.hpp"
#include "yuhu/yuhuState.hpp"
#include "yuhu/yuhuValue.hpp"
#include "yuhu/yuhu_globals.hpp"
#include "utilities/debug.hpp"

using namespace llvm;

void YuhuBlock::parse_bytecode(int start, int limit) {
  YuhuValue *a, *b, *c, *d;
  int i;

  // Ensure the current state is initialized before we emit any code,
  // so that any setup code for the state is at the start of the block
  current_state();

  // Parse the bytecodes
  iter()->reset_to_bci(start);
  while (iter()->next_bci() < limit) {
    NOT_PRODUCT(a = b = c = d = NULL);
    iter()->next();

    if (YuhuTraceBytecodes)
      tty->print_cr("%4d: %s", bci(), Bytecodes::name(bc()));

    if (has_trap() && trap_bci() == bci()) {
      do_trap(trap_request());
      return;
    }

    if (UseLoopSafepoints) {
      // XXX if a lcmp is followed by an if_?? then C2 maybe-inserts
      // the safepoint before the lcmp rather than before the if.
      // Maybe we should do this too.  See parse2.cpp for details.
      switch (bc()) {
      case Bytecodes::_goto:
      case Bytecodes::_ifnull:
      case Bytecodes::_ifnonnull:
      case Bytecodes::_if_acmpeq:
      case Bytecodes::_if_acmpne:
      case Bytecodes::_ifeq:
      case Bytecodes::_ifne:
      case Bytecodes::_iflt:
      case Bytecodes::_ifle:
      case Bytecodes::_ifgt:
      case Bytecodes::_ifge:
      case Bytecodes::_if_icmpeq:
      case Bytecodes::_if_icmpne:
      case Bytecodes::_if_icmplt:
      case Bytecodes::_if_icmple:
      case Bytecodes::_if_icmpgt:
      case Bytecodes::_if_icmpge:
        if (iter()->get_dest() <= bci())
          maybe_add_backedge_safepoint();
        break;

      case Bytecodes::_goto_w:
        if (iter()->get_far_dest() <= bci())
          maybe_add_backedge_safepoint();
        break;

      case Bytecodes::_tableswitch:
      case Bytecodes::_lookupswitch:
        if (switch_default_dest() <= bci()) {
          maybe_add_backedge_safepoint();
          break;
        }
        int len = switch_table_length();
        for (int i = 0; i < len; i++) {
          if (switch_dest(i) <= bci()) {
            maybe_add_backedge_safepoint();
            break;
          }
        }
        break;
      }
    }

    switch (bc()) {
    case Bytecodes::_nop:
      break;

    case Bytecodes::_aconst_null:
      push(YuhuValue::null());
      break;

    case Bytecodes::_iconst_m1:
      push(YuhuValue::jint_constant(-1));
      break;
    case Bytecodes::_iconst_0:
      push(YuhuValue::jint_constant(0));
      break;
    case Bytecodes::_iconst_1:
      push(YuhuValue::jint_constant(1));
      break;
    case Bytecodes::_iconst_2:
      push(YuhuValue::jint_constant(2));
      break;
    case Bytecodes::_iconst_3:
      push(YuhuValue::jint_constant(3));
      break;
    case Bytecodes::_iconst_4:
      push(YuhuValue::jint_constant(4));
      break;
    case Bytecodes::_iconst_5:
      push(YuhuValue::jint_constant(5));
      break;

    case Bytecodes::_lconst_0:
      push(YuhuValue::jlong_constant(0));
      break;
    case Bytecodes::_lconst_1:
      push(YuhuValue::jlong_constant(1));
      break;

    case Bytecodes::_fconst_0:
      push(YuhuValue::jfloat_constant(0));
      break;
    case Bytecodes::_fconst_1:
      push(YuhuValue::jfloat_constant(1));
      break;
    case Bytecodes::_fconst_2:
      push(YuhuValue::jfloat_constant(2));
      break;

    case Bytecodes::_dconst_0:
      push(YuhuValue::jdouble_constant(0));
      break;
    case Bytecodes::_dconst_1:
      push(YuhuValue::jdouble_constant(1));
      break;

    case Bytecodes::_bipush:
      push(YuhuValue::jint_constant(iter()->get_constant_u1()));
      break;
    case Bytecodes::_sipush:
      push(YuhuValue::jint_constant(iter()->get_constant_u2()));
      break;

    case Bytecodes::_ldc:
    case Bytecodes::_ldc_w:
    case Bytecodes::_ldc2_w: {
      YuhuConstant* constant = YuhuConstant::for_ldc(iter());
      assert(constant->is_loaded(), "trap should handle unloaded classes");
      push(constant->value(builder()));
      break;
    }
    case Bytecodes::_iload_0:
    case Bytecodes::_lload_0:
    case Bytecodes::_fload_0:
    case Bytecodes::_dload_0:
    case Bytecodes::_aload_0:
      push(local(0));
      break;
    case Bytecodes::_iload_1:
    case Bytecodes::_lload_1:
    case Bytecodes::_fload_1:
    case Bytecodes::_dload_1:
    case Bytecodes::_aload_1:
      push(local(1));
      break;
    case Bytecodes::_iload_2:
    case Bytecodes::_lload_2:
    case Bytecodes::_fload_2:
    case Bytecodes::_dload_2:
    case Bytecodes::_aload_2:
      push(local(2));
      break;
    case Bytecodes::_iload_3:
    case Bytecodes::_lload_3:
    case Bytecodes::_fload_3:
    case Bytecodes::_dload_3:
    case Bytecodes::_aload_3:
      push(local(3));
      break;
    case Bytecodes::_iload:
    case Bytecodes::_lload:
    case Bytecodes::_fload:
    case Bytecodes::_dload:
    case Bytecodes::_aload:
      push(local(iter()->get_index()));
      break;

    case Bytecodes::_baload:
      do_aload(T_BYTE);
      break;
    case Bytecodes::_caload:
      do_aload(T_CHAR);
      break;
    case Bytecodes::_saload:
      do_aload(T_SHORT);
      break;
    case Bytecodes::_iaload:
      do_aload(T_INT);
      break;
    case Bytecodes::_laload:
      do_aload(T_LONG);
      break;
    case Bytecodes::_faload:
      do_aload(T_FLOAT);
      break;
    case Bytecodes::_daload:
      do_aload(T_DOUBLE);
      break;
    case Bytecodes::_aaload:
      do_aload(T_OBJECT);
      break;

    case Bytecodes::_istore_0:
    case Bytecodes::_lstore_0:
    case Bytecodes::_fstore_0:
    case Bytecodes::_dstore_0:
    case Bytecodes::_astore_0:
      set_local(0, pop());
      break;
    case Bytecodes::_istore_1:
    case Bytecodes::_lstore_1:
    case Bytecodes::_fstore_1:
    case Bytecodes::_dstore_1:
    case Bytecodes::_astore_1:
      set_local(1, pop());
      break;
    case Bytecodes::_istore_2:
    case Bytecodes::_lstore_2:
    case Bytecodes::_fstore_2:
    case Bytecodes::_dstore_2:
    case Bytecodes::_astore_2:
      set_local(2, pop());
      break;
    case Bytecodes::_istore_3:
    case Bytecodes::_lstore_3:
    case Bytecodes::_fstore_3:
    case Bytecodes::_dstore_3:
    case Bytecodes::_astore_3:
      set_local(3, pop());
      break;
    case Bytecodes::_istore:
    case Bytecodes::_lstore:
    case Bytecodes::_fstore:
    case Bytecodes::_dstore:
    case Bytecodes::_astore:
      set_local(iter()->get_index(), pop());
      break;

    case Bytecodes::_bastore:
      do_astore(T_BYTE);
      break;
    case Bytecodes::_castore:
      do_astore(T_CHAR);
      break;
    case Bytecodes::_sastore:
      do_astore(T_SHORT);
      break;
    case Bytecodes::_iastore:
      do_astore(T_INT);
      break;
    case Bytecodes::_lastore:
      do_astore(T_LONG);
      break;
    case Bytecodes::_fastore:
      do_astore(T_FLOAT);
      break;
    case Bytecodes::_dastore:
      do_astore(T_DOUBLE);
      break;
    case Bytecodes::_aastore:
      do_astore(T_OBJECT);
      break;

    case Bytecodes::_pop:
      xpop();
      break;
    case Bytecodes::_pop2:
      xpop();
      xpop();
      break;
    case Bytecodes::_swap:
      a = xpop();
      b = xpop();
      xpush(a);
      xpush(b);
      break;
    case Bytecodes::_dup:
      a = xpop();
      xpush(a);
      xpush(a);
      break;
    case Bytecodes::_dup_x1:
      a = xpop();
      b = xpop();
      xpush(a);
      xpush(b);
      xpush(a);
      break;
    case Bytecodes::_dup_x2:
      a = xpop();
      b = xpop();
      c = xpop();
      xpush(a);
      xpush(c);
      xpush(b);
      xpush(a);
      break;
    case Bytecodes::_dup2:
      a = xpop();
      b = xpop();
      xpush(b);
      xpush(a);
      xpush(b);
      xpush(a);
      break;
    case Bytecodes::_dup2_x1:
      a = xpop();
      b = xpop();
      c = xpop();
      xpush(b);
      xpush(a);
      xpush(c);
      xpush(b);
      xpush(a);
      break;
    case Bytecodes::_dup2_x2:
      a = xpop();
      b = xpop();
      c = xpop();
      d = xpop();
      xpush(b);
      xpush(a);
      xpush(d);
      xpush(c);
      xpush(b);
      xpush(a);
      break;

    case Bytecodes::_arraylength:
      do_arraylength();
      break;

    case Bytecodes::_getfield:
      do_getfield();
      break;
    case Bytecodes::_getstatic:
      do_getstatic();
      break;
    case Bytecodes::_putfield:
      do_putfield();
      break;
    case Bytecodes::_putstatic:
      do_putstatic();
      break;

    case Bytecodes::_iadd:
      b = pop();
      a = pop();
      push(YuhuValue::create_jint(
        builder()->CreateAdd(a->jint_value(), b->jint_value()), false));
      break;
    case Bytecodes::_isub:
      b = pop();
      a = pop();
      push(YuhuValue::create_jint(
        builder()->CreateSub(a->jint_value(), b->jint_value()), false));
      break;
    case Bytecodes::_imul:
      b = pop();
      a = pop();
      push(YuhuValue::create_jint(
        builder()->CreateMul(a->jint_value(), b->jint_value()), false));
      break;
    case Bytecodes::_idiv:
      do_idiv();
      break;
    case Bytecodes::_irem:
      do_irem();
      break;
    case Bytecodes::_ineg:
      a = pop();
      push(YuhuValue::create_jint(
        builder()->CreateNeg(a->jint_value()), a->zero_checked()));
      break;
    case Bytecodes::_ishl:
      b = pop();
      a = pop();
      push(YuhuValue::create_jint(
        builder()->CreateShl(
          a->jint_value(),
          builder()->CreateAnd(
            b->jint_value(), LLVMValue::jint_constant(0x1f))), false));
      break;
    case Bytecodes::_ishr:
      b = pop();
      a = pop();
      push(YuhuValue::create_jint(
        builder()->CreateAShr(
          a->jint_value(),
          builder()->CreateAnd(
            b->jint_value(), LLVMValue::jint_constant(0x1f))), false));
      break;
    case Bytecodes::_iushr:
      b = pop();
      a = pop();
      push(YuhuValue::create_jint(
        builder()->CreateLShr(
          a->jint_value(),
          builder()->CreateAnd(
            b->jint_value(), LLVMValue::jint_constant(0x1f))), false));
      break;
    case Bytecodes::_iand:
      b = pop();
      a = pop();
      push(YuhuValue::create_jint(
        builder()->CreateAnd(a->jint_value(), b->jint_value()), false));
      break;
    case Bytecodes::_ior:
      b = pop();
      a = pop();
      push(YuhuValue::create_jint(
        builder()->CreateOr(a->jint_value(), b->jint_value()),
        a->zero_checked() && b->zero_checked()));
      break;
    case Bytecodes::_ixor:
      b = pop();
      a = pop();
      push(YuhuValue::create_jint(
        builder()->CreateXor(a->jint_value(), b->jint_value()), false));
      break;

    case Bytecodes::_ladd:
      b = pop();
      a = pop();
      push(YuhuValue::create_jlong(
        builder()->CreateAdd(a->jlong_value(), b->jlong_value()), false));
      break;
    case Bytecodes::_lsub:
      b = pop();
      a = pop();
      push(YuhuValue::create_jlong(
        builder()->CreateSub(a->jlong_value(), b->jlong_value()), false));
      break;
    case Bytecodes::_lmul:
      b = pop();
      a = pop();
      push(YuhuValue::create_jlong(
        builder()->CreateMul(a->jlong_value(), b->jlong_value()), false));
      break;
    case Bytecodes::_ldiv:
      do_ldiv();
      break;
    case Bytecodes::_lrem:
      do_lrem();
      break;
    case Bytecodes::_lneg:
      a = pop();
      push(YuhuValue::create_jlong(
        builder()->CreateNeg(a->jlong_value()), a->zero_checked()));
      break;
    case Bytecodes::_lshl:
      b = pop();
      a = pop();
      push(YuhuValue::create_jlong(
        builder()->CreateShl(
          a->jlong_value(),
          builder()->CreateIntCast(
            builder()->CreateAnd(
              b->jint_value(), LLVMValue::jint_constant(0x3f)),
            YuhuType::jlong_type(), true)), false));
      break;
    case Bytecodes::_lshr:
      b = pop();
      a = pop();
      push(YuhuValue::create_jlong(
        builder()->CreateAShr(
          a->jlong_value(),
          builder()->CreateIntCast(
            builder()->CreateAnd(
              b->jint_value(), LLVMValue::jint_constant(0x3f)),
            YuhuType::jlong_type(), true)), false));
      break;
    case Bytecodes::_lushr:
      b = pop();
      a = pop();
      push(YuhuValue::create_jlong(
        builder()->CreateLShr(
          a->jlong_value(),
          builder()->CreateIntCast(
            builder()->CreateAnd(
              b->jint_value(), LLVMValue::jint_constant(0x3f)),
            YuhuType::jlong_type(), true)), false));
      break;
    case Bytecodes::_land:
      b = pop();
      a = pop();
      push(YuhuValue::create_jlong(
        builder()->CreateAnd(a->jlong_value(), b->jlong_value()), false));
      break;
    case Bytecodes::_lor:
      b = pop();
      a = pop();
      push(YuhuValue::create_jlong(
        builder()->CreateOr(a->jlong_value(), b->jlong_value()),
        a->zero_checked() && b->zero_checked()));
      break;
    case Bytecodes::_lxor:
      b = pop();
      a = pop();
      push(YuhuValue::create_jlong(
        builder()->CreateXor(a->jlong_value(), b->jlong_value()), false));
      break;

    case Bytecodes::_fadd:
      b = pop();
      a = pop();
      push(YuhuValue::create_jfloat(
        builder()->CreateFAdd(a->jfloat_value(), b->jfloat_value())));
      break;
    case Bytecodes::_fsub:
      b = pop();
      a = pop();
      push(YuhuValue::create_jfloat(
        builder()->CreateFSub(a->jfloat_value(), b->jfloat_value())));
      break;
    case Bytecodes::_fmul:
      b = pop();
      a = pop();
      push(YuhuValue::create_jfloat(
        builder()->CreateFMul(a->jfloat_value(), b->jfloat_value())));
      break;
    case Bytecodes::_fdiv:
      b = pop();
      a = pop();
      push(YuhuValue::create_jfloat(
        builder()->CreateFDiv(a->jfloat_value(), b->jfloat_value())));
      break;
    case Bytecodes::_frem:
      b = pop();
      a = pop();
      push(YuhuValue::create_jfloat(
        builder()->CreateFRem(a->jfloat_value(), b->jfloat_value())));
      break;
    case Bytecodes::_fneg:
      a = pop();
      push(YuhuValue::create_jfloat(
        builder()->CreateFNeg(a->jfloat_value())));
      break;

    case Bytecodes::_dadd:
      b = pop();
      a = pop();
      push(YuhuValue::create_jdouble(
        builder()->CreateFAdd(a->jdouble_value(), b->jdouble_value())));
      break;
    case Bytecodes::_dsub:
      b = pop();
      a = pop();
      push(YuhuValue::create_jdouble(
        builder()->CreateFSub(a->jdouble_value(), b->jdouble_value())));
      break;
    case Bytecodes::_dmul:
      b = pop();
      a = pop();
      push(YuhuValue::create_jdouble(
        builder()->CreateFMul(a->jdouble_value(), b->jdouble_value())));
      break;
    case Bytecodes::_ddiv:
      b = pop();
      a = pop();
      push(YuhuValue::create_jdouble(
        builder()->CreateFDiv(a->jdouble_value(), b->jdouble_value())));
      break;
    case Bytecodes::_drem:
      b = pop();
      a = pop();
      push(YuhuValue::create_jdouble(
        builder()->CreateFRem(a->jdouble_value(), b->jdouble_value())));
      break;
    case Bytecodes::_dneg:
      a = pop();
      push(YuhuValue::create_jdouble(
        builder()->CreateFNeg(a->jdouble_value())));
      break;

    case Bytecodes::_iinc:
      i = iter()->get_index();
      set_local(
        i,
        YuhuValue::create_jint(
          builder()->CreateAdd(
            LLVMValue::jint_constant(iter()->get_iinc_con()),
            local(i)->jint_value()), false));
      break;

    case Bytecodes::_lcmp:
      do_lcmp();
      break;

    case Bytecodes::_fcmpl:
      do_fcmp(false, false);
      break;
    case Bytecodes::_fcmpg:
      do_fcmp(false, true);
      break;
    case Bytecodes::_dcmpl:
      do_fcmp(true, false);
      break;
    case Bytecodes::_dcmpg:
      do_fcmp(true, true);
      break;

    case Bytecodes::_i2l:
      a = pop();
      push(YuhuValue::create_jlong(
        builder()->CreateIntCast(
          a->jint_value(), YuhuType::jlong_type(), true), a->zero_checked()));
      break;
    case Bytecodes::_i2f:
      push(YuhuValue::create_jfloat(
        builder()->CreateSIToFP(
          pop()->jint_value(), YuhuType::jfloat_type())));
      break;
    case Bytecodes::_i2d:
      push(YuhuValue::create_jdouble(
        builder()->CreateSIToFP(
          pop()->jint_value(), YuhuType::jdouble_type())));
      break;

    case Bytecodes::_l2i:
      push(YuhuValue::create_jint(
        builder()->CreateIntCast(
          pop()->jlong_value(), YuhuType::jint_type(), true), false));
      break;
    case Bytecodes::_l2f:
      push(YuhuValue::create_jfloat(
        builder()->CreateSIToFP(
          pop()->jlong_value(), YuhuType::jfloat_type())));
      break;
    case Bytecodes::_l2d:
      push(YuhuValue::create_jdouble(
        builder()->CreateSIToFP(
          pop()->jlong_value(), YuhuType::jdouble_type())));
      break;

    case Bytecodes::_f2i: {
      Value* callee = builder()->f2i();
      Value* arg = pop()->jfloat_value();
      // LLVM 20+ uses opaque pointer types, reconstruct FunctionType from signature "f" -> "i"
      llvm::FunctionType* func_type = YuhuBuilder::make_ftype("f", "i");
      push(YuhuValue::create_jint(
        builder()->CreateCall(func_type, callee, arg), false));
      break;
    }
    case Bytecodes::_f2l: {
      Value* callee = builder()->f2l();
      Value* arg = pop()->jfloat_value();
      // LLVM 20+ uses opaque pointer types, reconstruct FunctionType from signature "f" -> "l"
      llvm::FunctionType* func_type = YuhuBuilder::make_ftype("f", "l");
      push(YuhuValue::create_jlong(
        builder()->CreateCall(func_type, callee, arg), false));
      break;
    }
    case Bytecodes::_f2d:
      push(YuhuValue::create_jdouble(
        builder()->CreateFPExt(
          pop()->jfloat_value(), YuhuType::jdouble_type())));
      break;

    case Bytecodes::_d2i: {
      Value* callee = builder()->d2i();
      Value* arg = pop()->jdouble_value();
      // LLVM 20+ uses opaque pointer types, reconstruct FunctionType from signature "d" -> "i"
      llvm::FunctionType* func_type = YuhuBuilder::make_ftype("d", "i");
      push(YuhuValue::create_jint(
        builder()->CreateCall(func_type, callee, arg), false));
      break;
    }
    case Bytecodes::_d2l: {
      Value* callee = builder()->d2l();
      Value* arg = pop()->jdouble_value();
      // LLVM 20+ uses opaque pointer types, reconstruct FunctionType from signature "d" -> "l"
      llvm::FunctionType* func_type = YuhuBuilder::make_ftype("d", "l");
      push(YuhuValue::create_jlong(
        builder()->CreateCall(func_type, callee, arg), false));
      break;
    }
    case Bytecodes::_d2f:
      push(YuhuValue::create_jfloat(
        builder()->CreateFPTrunc(
          pop()->jdouble_value(), YuhuType::jfloat_type())));
      break;

    case Bytecodes::_i2b:
      push(YuhuValue::create_jint(
        builder()->CreateAShr(
          builder()->CreateShl(
            pop()->jint_value(),
            LLVMValue::jint_constant(24)),
          LLVMValue::jint_constant(24)), false));
      break;
    case Bytecodes::_i2c:
      push(YuhuValue::create_jint(
        builder()->CreateAnd(
            pop()->jint_value(),
            LLVMValue::jint_constant(0xffff)), false));
      break;
    case Bytecodes::_i2s:
      push(YuhuValue::create_jint(
        builder()->CreateAShr(
          builder()->CreateShl(
            pop()->jint_value(),
            LLVMValue::jint_constant(16)),
          LLVMValue::jint_constant(16)), false));
      break;

    case Bytecodes::_return:
      do_return(T_VOID);
      break;
    case Bytecodes::_ireturn:
      do_return(T_INT);
      break;
    case Bytecodes::_lreturn:
      do_return(T_LONG);
      break;
    case Bytecodes::_freturn:
      do_return(T_FLOAT);
      break;
    case Bytecodes::_dreturn:
      do_return(T_DOUBLE);
      break;
    case Bytecodes::_areturn:
      do_return(T_OBJECT);
      break;

    case Bytecodes::_athrow:
      do_athrow();
      break;

    case Bytecodes::_goto:
    case Bytecodes::_goto_w:
      do_goto();
      break;

    case Bytecodes::_jsr:
    case Bytecodes::_jsr_w:
      do_jsr();
      break;

    case Bytecodes::_ret:
      do_ret();
      break;

    case Bytecodes::_ifnull:
      do_if(ICmpInst::ICMP_EQ, YuhuValue::null(), pop());
      break;
    case Bytecodes::_ifnonnull:
      do_if(ICmpInst::ICMP_NE, YuhuValue::null(), pop());
      break;
    case Bytecodes::_if_acmpeq:
      b = pop();
      a = pop();
      do_if(ICmpInst::ICMP_EQ, b, a);
      break;
    case Bytecodes::_if_acmpne:
      b = pop();
      a = pop();
      do_if(ICmpInst::ICMP_NE, b, a);
      break;
    case Bytecodes::_ifeq:
      do_if(ICmpInst::ICMP_EQ, YuhuValue::jint_constant(0), pop());
      break;
    case Bytecodes::_ifne:
      do_if(ICmpInst::ICMP_NE, YuhuValue::jint_constant(0), pop());
      break;
    case Bytecodes::_iflt:
      do_if(ICmpInst::ICMP_SLT, YuhuValue::jint_constant(0), pop());
      break;
    case Bytecodes::_ifle:
      do_if(ICmpInst::ICMP_SLE, YuhuValue::jint_constant(0), pop());
      break;
    case Bytecodes::_ifgt:
      do_if(ICmpInst::ICMP_SGT, YuhuValue::jint_constant(0), pop());
      break;
    case Bytecodes::_ifge:
      do_if(ICmpInst::ICMP_SGE, YuhuValue::jint_constant(0), pop());
      break;
    case Bytecodes::_if_icmpeq:
      b = pop();
      a = pop();
      do_if(ICmpInst::ICMP_EQ, b, a);
      break;
    case Bytecodes::_if_icmpne:
      b = pop();
      a = pop();
      do_if(ICmpInst::ICMP_NE, b, a);
      break;
    case Bytecodes::_if_icmplt:
      b = pop();
      a = pop();
      do_if(ICmpInst::ICMP_SLT, b, a);
      break;
    case Bytecodes::_if_icmple:
      b = pop();
      a = pop();
      do_if(ICmpInst::ICMP_SLE, b, a);
      break;
    case Bytecodes::_if_icmpgt:
      b = pop();
      a = pop();
      do_if(ICmpInst::ICMP_SGT, b, a);
      break;
    case Bytecodes::_if_icmpge:
      b = pop();
      a = pop();
      do_if(ICmpInst::ICMP_SGE, b, a);
      break;

    case Bytecodes::_tableswitch:
    case Bytecodes::_lookupswitch:
      do_switch();
      break;

    case Bytecodes::_invokestatic:
    case Bytecodes::_invokespecial:
    case Bytecodes::_invokevirtual:
    case Bytecodes::_invokeinterface:
      do_call();
      break;

    case Bytecodes::_instanceof:
      // This is a very common construct:
      //
      //  if (object instanceof Klass) {
      //    something = (Klass) object;
      //    ...
      //  }
      //
      // which gets compiled to something like this:
      //
      //  28: aload 9
      //  30: instanceof <Class Klass>
      //  33: ifeq 52
      //  36: aload 9
      //  38: checkcast <Class Klass>
      //
      // Handling both bytecodes at once allows us
      // to eliminate the checkcast.
      if (iter()->next_bci() < limit &&
          (iter()->next_bc() == Bytecodes::_ifeq ||
           iter()->next_bc() == Bytecodes::_ifne) &&
          (!UseLoopSafepoints ||
           iter()->next_get_dest() > iter()->next_bci())) {
        if (maybe_do_instanceof_if()) {
          iter()->next();
          if (YuhuTraceBytecodes)
            tty->print_cr("%4d: %s", bci(), Bytecodes::name(bc()));
          break;
        }
      }
      // fall through
    case Bytecodes::_checkcast:
      do_instance_check();
      break;

    case Bytecodes::_new:
      do_new();
      break;
    case Bytecodes::_newarray:
      do_newarray();
      break;
    case Bytecodes::_anewarray:
      do_anewarray();
      break;
    case Bytecodes::_multianewarray:
      do_multianewarray();
      break;

    case Bytecodes::_monitorenter:
      do_monitorenter();
      break;
    case Bytecodes::_monitorexit:
      do_monitorexit();
      break;

    default:
      ShouldNotReachHere();
    }
  }
}

YuhuState* YuhuBlock::initial_current_state() {
  return entry_state()->copy();
}

int YuhuBlock::switch_default_dest() {
  return iter()->get_dest_table(0);
}

int YuhuBlock::switch_table_length() {
  switch(bc()) {
  case Bytecodes::_tableswitch:
    return iter()->get_int_table(2) - iter()->get_int_table(1) + 1;

  case Bytecodes::_lookupswitch:
    return iter()->get_int_table(1);

  default:
    ShouldNotReachHere();
    return 0;
  }
}

int YuhuBlock::switch_key(int i) {
  switch(bc()) {
  case Bytecodes::_tableswitch:
    return iter()->get_int_table(1) + i;

  case Bytecodes::_lookupswitch:
    return iter()->get_int_table(2 + 2 * i);

  default:
    ShouldNotReachHere();
    return 0;
  }
}

int YuhuBlock::switch_dest(int i) {
  switch(bc()) {
  case Bytecodes::_tableswitch:
    return iter()->get_dest_table(i + 3);

  case Bytecodes::_lookupswitch:
    return iter()->get_dest_table(2 + 2 * i + 1);

  default:
    ShouldNotReachHere();
    return 0;
  }
}

void YuhuBlock::do_div_or_rem(bool is_long, bool is_rem) {
  YuhuValue *sb = pop();
  YuhuValue *sa = pop();

  check_divide_by_zero(sb);

  Value *a, *b, *p, *q;
  if (is_long) {
    a = sa->jlong_value();
    b = sb->jlong_value();
    p = LLVMValue::jlong_constant(0x8000000000000000LL);
    q = LLVMValue::jlong_constant(-1);
  }
  else {
    a = sa->jint_value();
    b = sb->jint_value();
    p = LLVMValue::jint_constant(0x80000000);
    q = LLVMValue::jint_constant(-1);
  }

  BasicBlock *ip           = builder()->GetBlockInsertionPoint();
  BasicBlock *special_case = builder()->CreateBlock(ip, "special_case");
  BasicBlock *general_case = builder()->CreateBlock(ip, "general_case");
  BasicBlock *done         = builder()->CreateBlock(ip, "done");

  builder()->CreateCondBr(
    builder()->CreateAnd(
      builder()->CreateICmpEQ(a, p),
      builder()->CreateICmpEQ(b, q)),
    special_case, general_case);

  builder()->SetInsertPoint(special_case);
  Value *special_result;
  if (is_rem) {
    if (is_long)
      special_result = LLVMValue::jlong_constant(0);
    else
      special_result = LLVMValue::jint_constant(0);
  }
  else {
    special_result = a;
  }
  builder()->CreateBr(done);

  builder()->SetInsertPoint(general_case);
  Value *general_result;
  if (is_rem)
    general_result = builder()->CreateSRem(a, b);
  else
    general_result = builder()->CreateSDiv(a, b);
  builder()->CreateBr(done);

  builder()->SetInsertPoint(done);
  PHINode *result;
  if (is_long)
    result = builder()->CreatePHI(YuhuType::jlong_type(), 0, "result");
  else
    result = builder()->CreatePHI(YuhuType::jint_type(), 0, "result");
  result->addIncoming(special_result, special_case);
  result->addIncoming(general_result, general_case);

  if (is_long)
    push(YuhuValue::create_jlong(result, false));
  else
    push(YuhuValue::create_jint(result, false));
}

void YuhuBlock::do_field_access(bool is_get, bool is_field) {
  bool will_link;
  ciField *field = iter()->get_field(will_link);
  assert(will_link, "typeflow responsibility");
  assert(is_field != field->is_static(), "mismatch");

  // Pop the value off the stack where necessary
  YuhuValue *value = NULL;
  if (!is_get)
    value = pop();

  // Find the object we're accessing, if necessary
  Value *object = NULL;
  if (is_field) {
    YuhuValue *obj_value = pop();
    // Explicit null check for field access - always perform the check
    // to ensure Java semantics (NPE instead of segfault)
    check_null(obj_value);
    object = obj_value->generic_value();
  }
  
  // Check if this is a constant static field
  if (is_get && field->is_constant() && field->is_static()) {
    YuhuConstant *constant = YuhuConstant::for_field(iter());
    if (constant->is_loaded())
      value = constant->value(builder());
  }
  
  // Perform the actual field access
  if (!is_get || value == NULL) {
    if (!is_field) {
      // === GETSTATIC: Call runtime helper to get static field value ===
      int cp_index = iter()->get_field_index();
      Value* field_result = builder()->CreateInlineOopForStaticField(cp_index);
      
      // yuhu_resolve_static_field returns jlong (64-bit container)
      // We need to convert it to the actual field type
      BasicType basic_type = field->type()->basic_type();
      llvm::Type* field_type = YuhuType::to_arrayType(basic_type);
      
      Value* field_value;
      if (basic_type == T_OBJECT || basic_type == T_ARRAY) {
        // For static field object references, yuhu_resolve_static_field returns
        // the decoded oop pointer directly (not compressed), so use oop_type()
        field_value = builder()->CreateIntToPtr(field_result, YuhuType::oop_type());
      } else if (basic_type == T_LONG || basic_type == T_DOUBLE) {
        // Already jlong/jdouble (64-bit), may need bitcast for double
        if (basic_type == T_DOUBLE) {
          field_value = builder()->CreateBitCast(field_result, field_type);
        } else {
          field_value = field_result;
        }
      } else {
        // Truncate jlong to smaller types (int, short, byte, char, boolean, float)
        if (basic_type == T_FLOAT) {
          field_value = builder()->CreateBitCast(
            builder()->CreateTrunc(field_result, YuhuType::jint_type()), 
            field_type);
        } else {
          field_value = builder()->CreateTrunc(field_result, field_type);
        }
      }
      
      value = YuhuValue::create_generic(field->type(), field_value, false);
    } else {
      // === GETFIELD: Access instance field from object ===
      BasicType   basic_type = field->type()->basic_type();
      llvm::Type *stack_type = YuhuType::to_stackType(basic_type);
      llvm::Type *field_type = YuhuType::to_arrayType(basic_type);
      llvm::Type *type = field_type;
      if (field->is_volatile()) {
        if (field_type == YuhuType::jfloat_type()) {
          type = YuhuType::jint_type();
        } else if (field_type == YuhuType::jdouble_type()) {
          type = YuhuType::jlong_type();
        }
      }
      Value *addr = builder()->CreateAddressOfStructEntry(
        object, in_ByteSize(field->offset_in_bytes()),
        PointerType::getUnqual(type),
        "addr");

      // Do the load
      Value* field_value;
      if (field->is_volatile()) {
        field_value = builder()->CreateAtomicLoad(addr);
        field_value = builder()->CreateBitCast(field_value, field_type);
      } else {
        // LLVM 20+ requires explicit type parameter for CreateLoad
        field_value = builder()->CreateLoad(field_type, addr);
      }
      
      // Handle compressed oops for object references
      if (!field->type()->is_primitive_type() && field_type != stack_type) {
        // For object references, we need to handle compressed oops
        // If field_type is jint (compressed oop) and stack_type is pointer (full oop),
        // we need to call decode_heap_oop to expand the compressed pointer
        if (field_type == YuhuType::jint_type() && 
            stack_type->isPointerTy() &&
            UseCompressedOops) {
          // Call decode_heap_oop to convert narrowOop to oop
          field_value = builder()->CreateDecodeHeapOop(field_value);
        }
      }
      
      if (field_type != stack_type) {
        field_value = builder()->CreateIntCast(
          field_value, stack_type, basic_type != T_CHAR);
      }

      value = YuhuValue::create_generic(field->type(), field_value, false);
    }
    
    // === PUTFIELD: Store value into instance field ===
    if (!is_get) {
      Value *field_value = value->generic_value();
      BasicType   basic_type = field->type()->basic_type();
      llvm::Type *stack_type = YuhuType::to_stackType(basic_type);
      llvm::Type *field_type = YuhuType::to_arrayType(basic_type);
      
      if (field_type != stack_type) {
        field_value = builder()->CreateIntCast(
          field_value, field_type, basic_type != T_CHAR);
      }

      llvm::Type *type = field_type;
      if (field->is_volatile()) {
        if (field_type == YuhuType::jfloat_type()) {
          type = YuhuType::jint_type();
        } else if (field_type == YuhuType::jdouble_type()) {
          type = YuhuType::jlong_type();
        }
      }
      Value *addr = builder()->CreateAddressOfStructEntry(
        object, in_ByteSize(field->offset_in_bytes()),
        PointerType::getUnqual(type),
        "addr");
      
      if (field->is_volatile()) {
        field_value = builder()->CreateBitCast(field_value, type);
        builder()->CreateAtomicStore(field_value, addr);
      } else {
        builder()->CreateStore(field_value, addr);
      }

      if (!field->type()->is_primitive_type()) {
        builder()->CreateUpdateBarrierSet(oopDesc::bs(), addr);
      }
    }
  }

  // Push the value onto the stack where necessary
  if (is_get)
    push(value);
}

void YuhuBlock::do_lcmp() {
  Value *b = pop()->jlong_value();
  Value *a = pop()->jlong_value();

  BasicBlock *ip   = builder()->GetBlockInsertionPoint();
  BasicBlock *ne   = builder()->CreateBlock(ip, "lcmp_ne");
  BasicBlock *lt   = builder()->CreateBlock(ip, "lcmp_lt");
  BasicBlock *gt   = builder()->CreateBlock(ip, "lcmp_gt");
  BasicBlock *done = builder()->CreateBlock(ip, "done");

  BasicBlock *eq = builder()->GetInsertBlock();
  builder()->CreateCondBr(builder()->CreateICmpEQ(a, b), done, ne);

  builder()->SetInsertPoint(ne);
  builder()->CreateCondBr(builder()->CreateICmpSLT(a, b), lt, gt);

  builder()->SetInsertPoint(lt);
  builder()->CreateBr(done);

  builder()->SetInsertPoint(gt);
  builder()->CreateBr(done);

  builder()->SetInsertPoint(done);
  PHINode *result = builder()->CreatePHI(YuhuType::jint_type(), 0, "result");
  result->addIncoming(LLVMValue::jint_constant(-1), lt);
  result->addIncoming(LLVMValue::jint_constant(0),  eq);
  result->addIncoming(LLVMValue::jint_constant(1),  gt);

  push(YuhuValue::create_jint(result, false));
}

void YuhuBlock::do_fcmp(bool is_double, bool unordered_is_greater) {
  Value *a, *b;
  if (is_double) {
    b = pop()->jdouble_value();
    a = pop()->jdouble_value();
  }
  else {
    b = pop()->jfloat_value();
    a = pop()->jfloat_value();
  }

  BasicBlock *ip      = builder()->GetBlockInsertionPoint();
  BasicBlock *ordered = builder()->CreateBlock(ip, "ordered");
  BasicBlock *ge      = builder()->CreateBlock(ip, "fcmp_ge");
  BasicBlock *lt      = builder()->CreateBlock(ip, "fcmp_lt");
  BasicBlock *eq      = builder()->CreateBlock(ip, "fcmp_eq");
  BasicBlock *gt      = builder()->CreateBlock(ip, "fcmp_gt");
  BasicBlock *done    = builder()->CreateBlock(ip, "done");

  builder()->CreateCondBr(
    builder()->CreateFCmpUNO(a, b),
    unordered_is_greater ? gt : lt, ordered);

  builder()->SetInsertPoint(ordered);
  builder()->CreateCondBr(builder()->CreateFCmpULT(a, b), lt, ge);

  builder()->SetInsertPoint(ge);
  builder()->CreateCondBr(builder()->CreateFCmpUGT(a, b), gt, eq);

  builder()->SetInsertPoint(lt);
  builder()->CreateBr(done);

  builder()->SetInsertPoint(gt);
  builder()->CreateBr(done);

  builder()->SetInsertPoint(eq);
  builder()->CreateBr(done);

  builder()->SetInsertPoint(done);
  PHINode *result = builder()->CreatePHI(YuhuType::jint_type(), 0, "result");
  result->addIncoming(LLVMValue::jint_constant(-1), lt);
  result->addIncoming(LLVMValue::jint_constant(0),  eq);
  result->addIncoming(LLVMValue::jint_constant(1),  gt);

  push(YuhuValue::create_jint(result, false));
}

void YuhuBlock::emit_IR() {
  ShouldNotCallThis();
}

YuhuState* YuhuBlock::entry_state() {
  ShouldNotCallThis();
  return NULL;
}

void YuhuBlock::do_zero_check(YuhuValue* value) {
  ShouldNotCallThis();
}

void YuhuBlock::maybe_add_backedge_safepoint() {
  ShouldNotCallThis();
}

bool YuhuBlock::has_trap() {
  return false;
}

int YuhuBlock::trap_request() {
  ShouldNotCallThis();
  return 0;
}

int YuhuBlock::trap_bci() {
  ShouldNotCallThis();
  return 0;
}

void YuhuBlock::do_trap(int trap_request) {
  ShouldNotCallThis();
}

void YuhuBlock::do_arraylength() {
  ShouldNotCallThis();
}

void YuhuBlock::do_aload(BasicType basic_type) {
  ShouldNotCallThis();
}

void YuhuBlock::do_astore(BasicType basic_type) {
  ShouldNotCallThis();
}

void YuhuBlock::do_return(BasicType type) {
  ShouldNotCallThis();
}

void YuhuBlock::do_athrow() {
  ShouldNotCallThis();
}

void YuhuBlock::do_goto() {
  ShouldNotCallThis();
}

void YuhuBlock::do_jsr() {
  ShouldNotCallThis();
}

void YuhuBlock::do_ret() {
  ShouldNotCallThis();
}

void YuhuBlock::do_if(ICmpInst::Predicate p, YuhuValue* b, YuhuValue* a) {
  ShouldNotCallThis();
}

void YuhuBlock::do_switch() {
  ShouldNotCallThis();
}

void YuhuBlock::do_call() {
  ShouldNotCallThis();
}

void YuhuBlock::do_instance_check() {
  ShouldNotCallThis();
}

bool YuhuBlock::maybe_do_instanceof_if() {
  ShouldNotCallThis();
  return false;
}

void YuhuBlock::do_new() {
  ShouldNotCallThis();
}

void YuhuBlock::do_newarray() {
  ShouldNotCallThis();
}

void YuhuBlock::do_anewarray() {
  ShouldNotCallThis();
}

void YuhuBlock::do_multianewarray() {
  ShouldNotCallThis();
}

void YuhuBlock::do_monitorenter() {
  ShouldNotCallThis();
}

void YuhuBlock::do_monitorexit() {
  ShouldNotCallThis();
}

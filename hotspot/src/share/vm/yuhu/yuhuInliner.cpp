/*
 * Copyright (c) 1999, 2013, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2009 Red Hat, Inc.
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
#include "ci/ciConstant.hpp"
#include "ci/ciField.hpp"
#include "ci/ciMethod.hpp"
#include "ci/ciStreams.hpp"
#include "interpreter/bytecodes.hpp"
#include "memory/allocation.hpp"
#include "yuhu/yuhuBlock.hpp"
#include "yuhu/yuhuCacheDecache.hpp"
#include "yuhu/yuhuConstant.hpp"
#include "yuhu/yuhuInliner.hpp"
#include "yuhu/yuhuInlineTree.hpp"
#include "yuhu/yuhuIntrinsics.hpp"
#include "yuhu/yuhuState.hpp"
#include "yuhu/yuhuTopLevelBlock.hpp"
#include "yuhu/yuhuValue.hpp"
#include "yuhu/yuhu_globals.hpp"

using namespace llvm;

class YuhuInlineBlock : public YuhuBlock {
 public:
  YuhuInlineBlock(ciMethod* target, YuhuState* state)
    : YuhuBlock(state, new YuhuInlineNodeOrMethod(target)),
      _outer_state(state),
      _entry_state(new YuhuState(this)) {
    for (int i = target->max_locals() - 1; i >= 0; i--) {
      YuhuValue *value = NULL;
      if (i < target->arg_size())
        value = outer_state()->pop();
      entry_state()->set_local(i, value);
    }
  }

 private:
  YuhuState* _outer_state;
  YuhuState* _entry_state;

 private:
  YuhuState* outer_state() {
    return _outer_state;
  }
  YuhuState* entry_state() {
    return _entry_state;
  }

 public:
  void emit_IR() {
    parse_bytecode(0, target()->code_size());
  }

 private:
  void do_return(BasicType type) {
    if (type != T_VOID) {
      YuhuValue *result = pop_result(type);
      outer_state()->push(result);
      if (result->is_two_word())
        outer_state()->push(NULL);
    }
  }
};

class YuhuInlinerHelper : public StackObj {
 public:
  YuhuInlinerHelper(ciMethod* target, YuhuState* entry_state)
    : _target(target),
      _entry_state(entry_state),
      _iter(target) {}

 private:
  ciBytecodeStream _iter;
  YuhuState*      _entry_state;
  ciMethod*        _target;

 public:
  ciBytecodeStream* iter() {
    return &_iter;
  }
  YuhuState* entry_state() const {
    return _entry_state;
  }
  ciMethod* target() const {
    return _target;
  }

 public:
  Bytecodes::Code bc() {
    return iter()->cur_bc();
  }
  int max_locals() const {
    return target()->max_locals();
  }
  int max_stack() const {
    return target()->max_stack();
  }

  // Inlinability check
 public:
  bool is_inlinable();

 private:
  void initialize_for_check();

  bool do_getstatic() {
    return do_field_access(true, false);
  }
  bool do_getfield() {
    return do_field_access(true, true);
  }
  bool do_putfield() {
    return do_field_access(false, true);
  }
  bool do_field_access(bool is_get, bool is_field);

  // Local variables for inlinability check
 private:
  bool* _locals;

 public:
  bool* local_addr(int index) const {
    assert(index >= 0 && index < max_locals(), "bad local variable index");
    return &_locals[index];
  }
  bool local(int index) const {
    return *local_addr(index);
  }
  void set_local(int index, bool value) {
    *local_addr(index) = value;
  }

  // Expression stack for inlinability check
 private:
  bool* _stack;
  bool* _sp;

 public:
  int stack_depth() const {
    return _sp - _stack;
  }
  bool* stack_addr(int slot) const {
    assert(slot >= 0 && slot < stack_depth(), "bad stack slot");
    return &_sp[-(slot + 1)];
  }
  void push(bool value) {
    assert(stack_depth() < max_stack(), "stack overrun");
    *(_sp++) = value;
  }
  bool pop() {
    assert(stack_depth() > 0, "stack underrun");
    return *(--_sp);
  }

  // Methods for two-word locals
 public:
  void push_pair_local(int index) {
    push(local(index));
    push(local(index + 1));
  }
  void pop_pair_local(int index) {
    set_local(index + 1, pop());
    set_local(index, pop());
  }

  // Code generation
 public:
  void do_inline() {
    (new YuhuInlineBlock(target(), entry_state()))->emit_IR();
  }
};

// Quick checks so we can bail out before doing too much
bool YuhuInliner::may_be_inlinable(ciMethod *target, bool allow_exception_handlers) {
  // Guard against triggering constant-pool resolution on a klass that
  // has been loaded but not yet linked.  Downstream, both Path A's
  // YuhuInlineBlock::emit_IR and Path B's is_llvm_inlineable eventually
  // run the callee's ciTypeFlow (via target->get_flow_analysis()), whose
  // do_getfield/do_getstatic call ciEnv::get_field_by_index -> ciField's
  // ctor.  That ctor asserts klass->get_instanceKlass()->is_linked() at
  // hotspot/src/share/vm/ci/ciField.cpp:76; hitting the assert crashes
  // the compiler thread (see test_yuhu/hs_err_pid22469.log where
  // java/lang/Class$EnclosingMethodInfo had just been loaded and Yuhu
  // eagerly compiled java.lang.Class::getEnclosingClass at tier 6).
  // C2's InlineTree uses is_initialized(); is_linked() is the strict
  // minimum matching ciField's assertion and sufficient to unblock the
  // Path B inline tree.  An unlinked holder also means its bytecodes
  // haven't been verified, so refusing to inline is the safe choice
  // anyway -- the callee will become inlineable on a subsequent compile
  // once the klass transitions through the verify/prepare phase.
  if (!target->holder()->is_linked())
    return false;

  // We can't inline native methods
  if (target->is_native())
    return false;

  // Not much point inlining abstract ones, and in any
  // case we'd need a stack frame to throw the exception
  if (target->is_abstract())
    return false;

  // Don't inline anything huge
  if (target->code_size() > YuhuMaxInlineSize)
    return false;

  // Monitors aren't allowed without a frame to put them in
  if (target->is_synchronized() || target->has_monitor_bytecodes())
    return false;

  // Path A cannot dispatch an inlinee's try/catch or athrow, so any
  // handler table (or jsr/ret subroutine) disqualifies the callee.
  // Path B gives the callee its own llvm::Function + its own ciMethod-
  // driven ciExceptionHandlerStream, so local try/catch works: only
  // jsrs remain disqualifying ( subroutine-call stack layout is not
  // modelled by Yuhu's inlinee frame).
  if (target->has_jsrs())
    return false;
  if (!allow_exception_handlers && target->has_exception_handlers())
    return false;

  // Don't try to inline constructors, as they must
  // eventually call Object.<init> which we can't inline.
  // Note that this catches <clinit> too, but why would
  // we be compiling that?
  if (target->is_initializer())
    return false;

  // Mustn't inline Object.<init>
  // Should be caught by the above, but just in case...
  if (target->intrinsic_id() == vmIntrinsics::_Object_init)
    return false;

  return true;
}

// Detailed eligibility check for the LLVM-IR inlining path (M0/M1/M2).
//
// A method qualifies only if:
//   - it passes the quick may_be_inlinable() screen with allow_exception_
//     handlers=true (holder klass already linked; size limit, no monitors
//     / jsrs / native / abstract / initializer; exception handler tables
//     are permitted under Path B),
//   - it actually has control flow (at least one branch instruction); a
//     straight-line method should go through the existing bytecode inliner,
//     which never builds a separate LLVM Function for it,
//   - every one of its bytecodes is in the whitelist below: constants,
//     local/stack manipulation, arithmetic (including the trapping
//     idiv/ldiv/irem/lrem), conversions, comparisons, branches (forward
//     or back), the invoke family (invokestatic / invokespecial /
//     invokevirtual / invokeinterface / invokedynamic), field access
//     (getstatic / putstatic / getfield / putfield), arraylength, array
//     load and store (iaload / iastore / aaload / aastore / baload /
//     bastore / etc.), allocation (new / newarray / anewarray /
//     multianewarray), and type checks (instanceof / checkcast).
//     Still rejected: monitorenter / monitorexit (need per-inlinee BasicObjectLock
//     slots in the caller's frame, deferred), and invokehandle (bytecode
//     0xf8, produced by rewriter.cpp when a call targets MethodHandle.
//     invoke / invokeExact; its polymorphic signature and appendix
//     handling require a DeoptMH entry point incompatible with Yuhu's
//     stub-based frame layout).  athrow is permitted since M2 (see below).
//
// Loops ARE permitted since M1.  A backedge inside the inlinee triggers
// YuhuTopLevelBlock::maybe_add_backedge_safepoint on the inlinee's own
// block hierarchy; the resulting statepoint inherits the inlinee's
// YuhuInlineNodeOrMethod scope chain (installed by build_inlinee via
// YuhuFunction's inline_node ctor arg) and travels through AlwaysInlinerPass
// into the caller's merged IR with its statepoint-id preserved intact.
//
// Invokes ARE permitted since M1 (including invokedynamic).  Inside a
// Path B inlinee, do_invoke runs the same routing as for a top-level
// block: if the target is marked for depth-2 LLVM-inline in the caller's
// inline tree, do_llvm_inline_call handles it; if it is a straight-line
// trivial method, Path A's attempt_inline bytecode-inlines it into the
// inlinee's own llvm::Function; otherwise do_call emits a normal stub-
// based compiled call whose call-site statepoint carries the inlinee's
// YuhuInlineNodeOrMethod.  All three flows survive AlwaysInlinerPass.
// For virtual / interface sites, check_null on the receiver emits a
// null-check trap inside the inlinee's IR; post-inline that trap lives in
// the caller's function and its statepoint-id still resolves through the
// inlinee's scope chain.  For invokedynamic sites, the CallSite target
// MethodHandle is dispatched at runtime via Yuhu's dynamic-call stub
// path; scan_for_traps treats invokedynamic separately from the four
// resolved-target invokes (does not apply the compiled-lambda-form / MH-
// intrinsic bail to an indy site).  invokehandle (bytecode 0xf8) remains
// rejected -- polymorphic-signature dispatch requires DeoptMH entry-point
// machinery incompatible with Yuhu's stub-based frame layout.
//
// Field access, arraylength, array load/store, allocation, and type
// checks are permitted since the M1+ extension.  Each uses Yuhu's
// existing do_field_access / do_array_load / do_array_store / do_new_
// object / do_type_check infrastructure; that IR-emission code is
// shared with Path A and top-level compiles and does not need any Path
// B-specific change.  The constant-pool resolution checks in
// scan_for_traps (holder-linked, field-will-link, klass-loaded) already
// run against the inlinee's ciMethod during its own parse, and any trap
// they install carries the inlinee's YuhuInlineNode in the sender chain.
// Post-inline, all of the IR travels through AlwaysInlinerPass into the
// caller's merged function with statepoint-ids intact.
//
// Trapping division (idiv / ldiv / irem / lrem) is permitted since M1+.
// Yuhu's do_div emits the JVM's INT_MIN / -1 = INT_MIN overflow rule
// (AArch64 sdiv traps or returns 0 for that encoding) plus an explicit
// zero-divisor guard that fires an ArithmeticException trap; both paths
// are frame-aware via the same statepoint-id + inline-node mechanism as
// null-check traps.
//
// Known M1 gap (now significantly more load-bearing): the deopt bundle
// attached to any safepoint or trap inside an inlinee captures only the
// inlinee's current_state(), not the caller's virtual state at the
// inline call site.  This is now triggered by any null-check (getfield /
// putfield / arraylength / *aload / *astore / invokevirtual receiver),
// bounds-check (*aload / *astore), class-check (checkcast), zero-divisor
// (idiv / ldiv / irem / lrem), or allocation slow path.  Non-trap paths
// (safepoint stop-and-resume, RS4GC gc-live reporting on the merged
// frame, ordinary control flow) are correct.  Trap paths still resolve
// to the correct bytecode-level bci via the statepoint-id + inline-node
// sender chain, and the caller's compile succeeds (the trap is emitted
// as an ordinary branch to the deopt handler), but at runtime the
// interpreter re-execution sees a partial virtual frame (inlinee
// locals/stack present, caller locals/stack at the inline call site
// missing).  Production safety REQUIRES closing this gap before relying
// on any trap-emitting opcode inside a Path B inlinee.  Full multi-level
// bundle capture is the immediate follow-up subtask.
bool YuhuInliner::is_llvm_inlineable(ciMethod *target) {
  if (!may_be_inlinable(target, /* allow_exception_handlers = */ true))
    return false;

  ciTypeFlow* flow = target->get_flow_analysis();
  if (flow->failing())
    return false;

  if (YuhuCompiler::contains_incomplete_state_analysis(flow)) {
      return false;
  }

  bool saw_branch = false;

  ciBytecodeStream iter(target);
  while (iter.next() != ciBytecodeStream::EOBC()) {
    switch (iter.cur_bc()) {
      // ---- constants ----
    case Bytecodes::_nop:
    case Bytecodes::_aconst_null:
    case Bytecodes::_iconst_m1:
    case Bytecodes::_iconst_0:
    case Bytecodes::_iconst_1:
    case Bytecodes::_iconst_2:
    case Bytecodes::_iconst_3:
    case Bytecodes::_iconst_4:
    case Bytecodes::_iconst_5:
    case Bytecodes::_lconst_0:
    case Bytecodes::_lconst_1:
    case Bytecodes::_fconst_0:
    case Bytecodes::_fconst_1:
    case Bytecodes::_fconst_2:
    case Bytecodes::_dconst_0:
    case Bytecodes::_dconst_1:
    case Bytecodes::_bipush:
    case Bytecodes::_sipush:
      break;

      // ---- ldc family ----
      // Only primitive numeric constants are safe: a String or Class constant
      // can require a VM call (lazy class loading, StringTable interning) and
      // can throw -- neither of which an inlinee can do since it has no frame.
    case Bytecodes::_ldc:
    case Bytecodes::_ldc_w:
    case Bytecodes::_ldc2_w: {
      BasicType bt = iter.get_constant().basic_type();
      if (bt != T_INT && bt != T_LONG && bt != T_FLOAT && bt != T_DOUBLE)
        return false;
      break;
    }

      // ---- loads ----
    case Bytecodes::_iload: case Bytecodes::_iload_0: case Bytecodes::_iload_1:
    case Bytecodes::_iload_2: case Bytecodes::_iload_3:
    case Bytecodes::_lload: case Bytecodes::_lload_0: case Bytecodes::_lload_1:
    case Bytecodes::_lload_2: case Bytecodes::_lload_3:
    case Bytecodes::_fload: case Bytecodes::_fload_0: case Bytecodes::_fload_1:
    case Bytecodes::_fload_2: case Bytecodes::_fload_3:
    case Bytecodes::_dload: case Bytecodes::_dload_0: case Bytecodes::_dload_1:
    case Bytecodes::_dload_2: case Bytecodes::_dload_3:
    case Bytecodes::_aload: case Bytecodes::_aload_0: case Bytecodes::_aload_1:
    case Bytecodes::_aload_2: case Bytecodes::_aload_3:
      break;

      // ---- stores ----
    case Bytecodes::_istore: case Bytecodes::_istore_0: case Bytecodes::_istore_1:
    case Bytecodes::_istore_2: case Bytecodes::_istore_3:
    case Bytecodes::_lstore: case Bytecodes::_lstore_0: case Bytecodes::_lstore_1:
    case Bytecodes::_lstore_2: case Bytecodes::_lstore_3:
    case Bytecodes::_fstore: case Bytecodes::_fstore_0: case Bytecodes::_fstore_1:
    case Bytecodes::_fstore_2: case Bytecodes::_fstore_3:
    case Bytecodes::_dstore: case Bytecodes::_dstore_0: case Bytecodes::_dstore_1:
    case Bytecodes::_dstore_2: case Bytecodes::_dstore_3:
    case Bytecodes::_astore: case Bytecodes::_astore_0: case Bytecodes::_astore_1:
    case Bytecodes::_astore_2: case Bytecodes::_astore_3:
      break;

      // ---- stack manipulation ----
    case Bytecodes::_pop: case Bytecodes::_pop2:
    case Bytecodes::_dup: case Bytecodes::_dup_x1: case Bytecodes::_dup_x2:
    case Bytecodes::_dup2: case Bytecodes::_dup2_x1: case Bytecodes::_dup2_x2:
    case Bytecodes::_swap:
      break;

      // ---- int/long arithmetic (no idiv/irem/ldiv/lrem: those trap on 0) ----
    case Bytecodes::_iadd: case Bytecodes::_isub: case Bytecodes::_imul:
    case Bytecodes::_iand: case Bytecodes::_ior: case Bytecodes::_ixor:
    case Bytecodes::_ishl: case Bytecodes::_ishr: case Bytecodes::_iushr:
    case Bytecodes::_ineg:
    case Bytecodes::_ladd: case Bytecodes::_lsub: case Bytecodes::_lmul:
    case Bytecodes::_land: case Bytecodes::_lor: case Bytecodes::_lxor:
    case Bytecodes::_lshl: case Bytecodes::_lshr: case Bytecodes::_lushr:
    case Bytecodes::_lneg:
      break;

      // ---- fp arithmetic: IEEE division/remainder never trap ----
    case Bytecodes::_fadd: case Bytecodes::_fsub: case Bytecodes::_fmul:
    case Bytecodes::_fdiv: case Bytecodes::_frem: case Bytecodes::_fneg:
    case Bytecodes::_dadd: case Bytecodes::_dsub: case Bytecodes::_dmul:
    case Bytecodes::_ddiv: case Bytecodes::_drem: case Bytecodes::_dneg:
      break;

    case Bytecodes::_iinc:
      break;

      // ---- comparisons ----
    case Bytecodes::_lcmp:
    case Bytecodes::_fcmpl: case Bytecodes::_fcmpg:
    case Bytecodes::_dcmpl: case Bytecodes::_dcmpg:
      break;

      // ---- conversions ----
    case Bytecodes::_i2l: case Bytecodes::_i2f: case Bytecodes::_i2d:
    case Bytecodes::_l2i: case Bytecodes::_l2f: case Bytecodes::_l2d:
    case Bytecodes::_f2i: case Bytecodes::_f2l: case Bytecodes::_f2d:
    case Bytecodes::_d2i: case Bytecodes::_d2f: case Bytecodes::_d2l:
    case Bytecodes::_i2b: case Bytecodes::_i2c: case Bytecodes::_i2s:
      break;

      // ---- branches (forward AND back): this is what makes the method
      //      interesting for the LLVM-IR path rather than the bytecode
      //      path.  Backedges are permitted since M1 -- the backedge
      //      safepoint poll inside an inlinee inherits the inlinee's
      //      YuhuInlineNodeOrMethod scope chain (see the function-level
      //      doc above for the M1 deopt-bundle caveat). ----
    case Bytecodes::_ifeq: case Bytecodes::_ifne: case Bytecodes::_iflt:
    case Bytecodes::_ifge: case Bytecodes::_ifgt: case Bytecodes::_ifle:
    case Bytecodes::_if_icmpeq: case Bytecodes::_if_icmpne: case Bytecodes::_if_icmplt:
    case Bytecodes::_if_icmpge: case Bytecodes::_if_icmpgt: case Bytecodes::_if_icmple:
    case Bytecodes::_if_acmpeq: case Bytecodes::_if_acmpne:
    case Bytecodes::_ifnull: case Bytecodes::_ifnonnull:
    case Bytecodes::_goto: case Bytecodes::_tableswitch: case Bytecodes::_lookupswitch:
      saw_branch = true;
      break;

      // ---- returns ----
    case Bytecodes::_return:
    case Bytecodes::_ireturn: case Bytecodes::_lreturn: case Bytecodes::_freturn:
    case Bytecodes::_dreturn: case Bytecodes::_areturn:
      break;

      // ---- invokes (M1) ----
      // YuhuTopLevelBlock::do_invoke runs the same routing here as for
      // a top-level block's own calls: depth-2 LLVM-inline target
      // (find_child_by_bci) -> do_llvm_inline_call; Path A straight-line
      // target -> attempt_inline; anything else -> do_call.  Call-site
      // safepoint polls and pending-exception checks are registered
      // against the inlinee's YuhuInlineNodeOrMethod and survive
      // AlwaysInlinerPass as ordinary CallInst instructions with their
      // statepoint-id attribute intact.
      // invokespecial to <init> is filtered upstream by may_be_inlinable
      // (constructor-target check).
      // invokedynamic dispatches through the CallSite's target Method-
      // Handle at runtime; scan_for_traps treats it separately from the
      // four resolved-target invokes (only checks holder-is-linked, does
      // NOT apply the is_method_handle_intrinsic / is_compiled_lambda_form
      // bail at yuhuTopLevelBlock.cpp:103).  do_call at yuhuTopLevelBlock
      // handles the is_dynamic case (appendix push + CallSite-based
      // dispatch) identically whether the site lives in a top-level
      // function or in a Path B inlinee; post-inline the call survives
      // AlwaysInlinerPass as an ordinary CallInst.
      // invokehandle (bytecode 0xf8, produced by rewriter.cpp when a call
      // targets MethodHandle.invoke / invokeExact) stays rejected: its
      // polymorphic signature + DeoptMH entry-point requirement is not
      // compatible with Yuhu's stub-based frame layout (see memory
      // "DeoptMH handler requirement for nmethods with method handle
      // invokes").
    case Bytecodes::_invokestatic:
    case Bytecodes::_invokespecial:
    case Bytecodes::_invokevirtual:
    case Bytecodes::_invokeinterface:
    case Bytecodes::_invokedynamic:
      break;

      // ---- field access (M1+) ----
      // do_field_access handles getstatic / putstatic / getfield /
      // putfield including holder-linked check, constant-pool resolution
      // and receiver null-check (for getfield / putfield).  Static
      // initialisation not yet complete dispatches via Yuhu's variant-2
      // Reason_uninitialized reinterpret trap (see scan_for_traps),
      // which falls back to interpreter re-execution.
    case Bytecodes::_getstatic: case Bytecodes::_putstatic:
    case Bytecodes::_getfield:  case Bytecodes::_putfield:
      break;

      // ---- arraylength (M1+) ----
      // Emits a check_null on the receiver and a load from the array's
      // length field.  Null receiver deopts; no bounds concern.
    case Bytecodes::_arraylength:
      break;

      // ---- array load / store (M1+) ----
      // do_array_load / do_array_store emit a null-check + bounds-check
      // pair; on bounds failure an implicit ArrayIndexOutOfBounds trap
      // fires; aastore additionally runs the array-klass store-type
      // check and may throw ArrayStoreException.  See the "Traps" note
      // in the function-level doc for the M1 deopt-bundle caveat.
    case Bytecodes::_iaload: case Bytecodes::_laload: case Bytecodes::_faload:
    case Bytecodes::_daload: case Bytecodes::_aaload: case Bytecodes::_baload:
    case Bytecodes::_caload: case Bytecodes::_saload:
    case Bytecodes::_iastore: case Bytecodes::_lastore: case Bytecodes::_fastore:
    case Bytecodes::_dastore: case Bytecodes::_aastore: case Bytecodes::_bastore:
    case Bytecodes::_castore: case Bytecodes::_sastore:
      break;

      // ---- allocation (M1+) ----
      // new / anewarray / newarray / multianewarray go through Yuhu's
      // slow-path allocation runtime call; fast-path inline TLAB bump-
      // pointer form is emitted when the klass size is a compile
      // constant.  Both flows can trigger GC + safepoint; RS4GC tracks
      // the freshly-allocated oop as gc-live at the statepoint.  Out-of-
      // memory is delivered as a pending_exception rather than a deopt
      // trap, so the M1 multi-level bundle gap does NOT apply here.
    case Bytecodes::_new: case Bytecodes::_anewarray:
    case Bytecodes::_newarray: case Bytecodes::_multianewarray:
      break;

      // ---- type check (M1+) ----
      // instanceof emits a subtype test and pushes 0/1; it cannot trap.
      // checkcast emits a subtype test that throws ClassCastException on
      // failure via the deopt trap path; see the "Traps" note in the
      // function-level doc.  Unloaded klass operand dispatches to the
      // fine-grained Reason_unloaded_object / variant-2 reinterpret
      // trap already used at top-level checkcast sites.
    case Bytecodes::_instanceof:
    case Bytecodes::_checkcast:
      break;

      // ---- trapping integer/long division (M1+) ----
      // Yuhu's do_div emits the JVM's INT_MIN / -1 = INT_MIN overflow
      // rule (AArch64 sdiv traps or returns 0 for that encoding) plus
      // an explicit zero-divisor guard that fires an ArithmeticException
      // trap.  See the "Traps" note in the function-level doc.
    case Bytecodes::_idiv: case Bytecodes::_ldiv:
    case Bytecodes::_irem: case Bytecodes::_lrem:
      break;

      // ---- athrow (M2) ----
      // YuhuTopLevelBlock::do_athrow at yuhuTopLevelBlock.cpp:1126
      // snapshots throw_state, pops the exception oop, null-checks it
      // and calls handle_exception(exception, EX_CHECK_FULL, throw_state).
      // handle_exception consults num_exceptions() against the current
      // target -- inside a Path B inlinee this is the inlinee's own
      // ciMethod -- and dispatches via emit_handler_checks / marshal_
      // exception_fast / marshal_exception_mixed.  Post-inline those
      // handler blocks live inside the caller's llvm::Function as
      // ordinary blocks; statepoint-ids and inline-node references from
      // the inlinee's dispatch survive AlwaysInlinerPass unchanged.
      //
      // Uncaught-by-inlinee path (either the inlinee has no local
      // handler or none of its entries match the exception klass):
      // handle_exception falls through to handle_return(T_VOID, excep-
      // tion).  When called inside a Path B inlinee, handle_return
      // (yuhuTopLevelBlock.cpp:909+) now stores the exception to
      // Thread::pending_exception and branches to the inlinee's
      // unified_exit_block instead of emitting the top-level unwind
      // patchpoint.  Post-AlwaysInlinerPass, that ret becomes a jump
      // into the caller's continuation right after do_llvm_inline_call's
      // CreateCall.  do_llvm_inline_call emits a check_pending_exception
      // at that continuation (yuhuTopLevelBlock.cpp:1453) which runs
      // the CALLER's emit_handler_checks against the caller's own
      // ciExceptionHandlerStream -- so a try/catch wrapping the Java-
      // level inline call site in the caller is dispatched correctly
      // at compiled-code level, using the caller's own statepoint-id +
      // deopt bundle.  If the caller doesn't match either, control
      // falls through to handle_return(T_VOID, exception) at the CALLER
      // level (which is NOT an inlinee, so it does hit the top-level
      // unwind patchpoint); the caller's own bundle reconstructs cor-
      // rectly, sidestepping the multi-level deopt bundle gap.  Note
      // the caller's IR-level fast dispatch is the only path that can
      // fire here (Yuhu still registers an empty ExceptionHandlerTable
      // per memory "Yuhu ExceptionHandlerTable population require-
      // ment"), so callers without a try/catch around the inline call
      // site fall through to interpreter re-execution -- semantically
      // correct, just not fast-pathed.
    case Bytecodes::_athrow:
      break;

      // ---- _goto_w (unreachable in practice) ----
      // Semantically identical to _goto (just a 4-byte unsigned offset).
      // javac emits _goto_w only when the target bci exceeds _goto's
      // 2-byte signed range (±32767).  No method under YuhuMaxInlineSize
      // (32 bytes default) will ever contain one.  Whitelist alongside
      // _goto if the size limit is raised past 64 KB.
    case Bytecodes::_goto_w:

      // ---- _jsr / _jsr_w (unstructured control flow) ----
      // JSR pushes a returnAddress (bci after the jsr) onto the operand
      // stack and branches to a subroutine body; RET performs an indirect
      // jump back via that returnAddress.  This creates multi-entry /
      // multi-exit unstructured CFG requiring LLVM's indirectbr, which
      // AlwaysInlinerPass explicitly refuses to inline (blockaddress
      // references become invalid after merging).  Additionally,
      // may_be_inlinable's has_jsrs() gate already rejects these methods
      // upstream for both Path A and Path B, so whitelisting the opcode
      // here would be a no-op.  Dead since javac stopped emitting JSR/RET
      // in JDK 1.6 (finally-blocks are now duplicated at bytecode level).
    case Bytecodes::_jsr:
    case Bytecodes::_jsr_w:

      // ---- monitorenter / monitorexit ----
      // Require per-inlinee BasicObjectLock slots in the caller's frame
      // (deferred -- need a stack-local monitor array modelled across the
      // inline boundary).
    case Bytecodes::_monitorenter:
    case Bytecodes::_monitorexit:
        return false;
    default:
      return false;
    }
  }

  // Straight-line methods belong to the bytecode inliner's territory.
  if (!saw_branch)
    return false;

  // Loops are permitted since M1 (see function-level doc).  A backedge
  // inside the inlinee causes maybe_add_backedge_safepoint to fire on the
  // inlinee's own block hierarchy; the emitted statepoint call carries
  // the inlinee's YuhuInlineNodeOrMethod, and after AlwaysInlinerPass
  // flattens the callee, the call is relocated into the caller's merged
  // function with its statepoint-id attribute intact so RS4GC and the
  // debug-info recorder keep working.

  return true;
}

// Full-on detailed check for the bytecode-level inliner (Path A).  These
// rules apply to Path A only; the LLVM-IR inliner (Path B, checked by
// is_llvm_inlineable above) is dramatically more permissive since the
// M1+ extension -- it allows loops, the entire invoke family (including
// invokedynamic), field access, arraylength, array load/store, allo-
// cation, type checks (instanceof / checkcast), trapping div/rem, and
// (since M2) athrow and exception handler tables -- because a Path B
// inlinee gets a real llvm::Function with its own block hierarchy and
// its safepoint-poll / call-site / trap metadata flows through Always-
// InlinerPass via the statepoint-id attribute; its local try/catch is
// dispatched via the inlinee's own ciMethod-driven ciExceptionHandler-
// Stream inside its own llvm::Function and merges into the caller post-
// flatten.  invokehandle (bytecode 0xf8) and monitor ops remain
// excluded from Path B (see is_llvm_inlineable's function-level doc for
// the specific reason on each).
//
// Under Path A, an inlined method has no stack frame of its own and its
// IR is emitted directly into the caller's llvm::Function.  That means no
// safepoints (and hence no loops) and no VM calls.  No VM calls means,
// amongst other things, that no exceptions can be created, which means no
// null checks or divide-by-zero checks are allowed.  The lack of null
// checks in particular would eliminate practically everything, but we
// can get around that restriction by relying on the zero-check eliminator
// to strip the checks.  To do that, we need to walk through the method,
// tracking which values are and are not zero-checked.
bool YuhuInlinerHelper::is_inlinable() {
  ResourceMark rm;
  initialize_for_check();

  YuhuConstant *sc;
  bool a, b, c, d;

  iter()->reset_to_bci(0);
  while (iter()->next() != ciBytecodeStream::EOBC()) {
    switch (bc()) {
    case Bytecodes::_nop:
      break;

    case Bytecodes::_aconst_null:
      push(false);
      break;

    case Bytecodes::_iconst_0:
      push(false);
      break;
    case Bytecodes::_iconst_m1:
    case Bytecodes::_iconst_1:
    case Bytecodes::_iconst_2:
    case Bytecodes::_iconst_3:
    case Bytecodes::_iconst_4:
    case Bytecodes::_iconst_5:
      push(true);
      break;

    case Bytecodes::_lconst_0:
      push(false);
      push(false);
      break;
    case Bytecodes::_lconst_1:
      push(true);
      push(false);
      break;

    case Bytecodes::_fconst_0:
    case Bytecodes::_fconst_1:
    case Bytecodes::_fconst_2:
      push(false);
      break;

    case Bytecodes::_dconst_0:
    case Bytecodes::_dconst_1:
      push(false);
      push(false);
      break;

    case Bytecodes::_bipush:
      push(iter()->get_constant_u1() != 0);
      break;
    case Bytecodes::_sipush:
      push(iter()->get_constant_u2() != 0);
      break;

    case Bytecodes::_ldc:
    case Bytecodes::_ldc_w:
    case Bytecodes::_ldc2_w:
      sc = YuhuConstant::for_ldc(iter());
      if (!sc->is_loaded())
        return false;
      push(sc->is_nonzero());
      if (sc->is_two_word())
        push(false);
      break;

    case Bytecodes::_iload_0:
    case Bytecodes::_fload_0:
    case Bytecodes::_aload_0:
      push(local(0));
      break;
    case Bytecodes::_lload_0:
    case Bytecodes::_dload_0:
      push_pair_local(0);
      break;

    case Bytecodes::_iload_1:
    case Bytecodes::_fload_1:
    case Bytecodes::_aload_1:
      push(local(1));
      break;
    case Bytecodes::_lload_1:
    case Bytecodes::_dload_1:
      push_pair_local(1);
      break;

    case Bytecodes::_iload_2:
    case Bytecodes::_fload_2:
    case Bytecodes::_aload_2:
      push(local(2));
      break;
    case Bytecodes::_lload_2:
    case Bytecodes::_dload_2:
      push_pair_local(2);
      break;

    case Bytecodes::_iload_3:
    case Bytecodes::_fload_3:
    case Bytecodes::_aload_3:
      push(local(3));
      break;
    case Bytecodes::_lload_3:
    case Bytecodes::_dload_3:
      push_pair_local(3);
      break;

    case Bytecodes::_iload:
    case Bytecodes::_fload:
    case Bytecodes::_aload:
      push(local(iter()->get_index()));
      break;
    case Bytecodes::_lload:
    case Bytecodes::_dload:
      push_pair_local(iter()->get_index());
      break;

    case Bytecodes::_istore_0:
    case Bytecodes::_fstore_0:
    case Bytecodes::_astore_0:
      set_local(0, pop());
      break;
    case Bytecodes::_lstore_0:
    case Bytecodes::_dstore_0:
      pop_pair_local(0);
      break;

    case Bytecodes::_istore_1:
    case Bytecodes::_fstore_1:
    case Bytecodes::_astore_1:
      set_local(1, pop());
      break;
    case Bytecodes::_lstore_1:
    case Bytecodes::_dstore_1:
      pop_pair_local(1);
      break;

    case Bytecodes::_istore_2:
    case Bytecodes::_fstore_2:
    case Bytecodes::_astore_2:
      set_local(2, pop());
      break;
    case Bytecodes::_lstore_2:
    case Bytecodes::_dstore_2:
      pop_pair_local(2);
      break;

    case Bytecodes::_istore_3:
    case Bytecodes::_fstore_3:
    case Bytecodes::_astore_3:
      set_local(3, pop());
      break;
    case Bytecodes::_lstore_3:
    case Bytecodes::_dstore_3:
      pop_pair_local(3);
      break;

    case Bytecodes::_istore:
    case Bytecodes::_fstore:
    case Bytecodes::_astore:
      set_local(iter()->get_index(), pop());
      break;
    case Bytecodes::_lstore:
    case Bytecodes::_dstore:
      pop_pair_local(iter()->get_index());
      break;

    case Bytecodes::_pop:
      pop();
      break;
    case Bytecodes::_pop2:
      pop();
      pop();
      break;
    case Bytecodes::_swap:
      a = pop();
      b = pop();
      push(a);
      push(b);
      break;
    case Bytecodes::_dup:
      a = pop();
      push(a);
      push(a);
      break;
    case Bytecodes::_dup_x1:
      a = pop();
      b = pop();
      push(a);
      push(b);
      push(a);
      break;
    case Bytecodes::_dup_x2:
      a = pop();
      b = pop();
      c = pop();
      push(a);
      push(c);
      push(b);
      push(a);
      break;
    case Bytecodes::_dup2:
      a = pop();
      b = pop();
      push(b);
      push(a);
      push(b);
      push(a);
      break;
    case Bytecodes::_dup2_x1:
      a = pop();
      b = pop();
      c = pop();
      push(b);
      push(a);
      push(c);
      push(b);
      push(a);
      break;
    case Bytecodes::_dup2_x2:
      a = pop();
      b = pop();
      c = pop();
      d = pop();
      push(b);
      push(a);
      push(d);
      push(c);
      push(b);
      push(a);
      break;

    case Bytecodes::_getfield:
      if (!do_getfield())
        return false;
      break;
    case Bytecodes::_getstatic:
      if (!do_getstatic())
        return false;
      break;
    case Bytecodes::_putfield:
      if (!do_putfield())
        return false;
      break;

    case Bytecodes::_iadd:
    case Bytecodes::_isub:
    case Bytecodes::_imul:
    case Bytecodes::_iand:
    case Bytecodes::_ixor:
    case Bytecodes::_ishl:
    case Bytecodes::_ishr:
    case Bytecodes::_iushr:
      pop();
      pop();
      push(false);
      break;
    case Bytecodes::_ior:
      a = pop();
      b = pop();
      push(a && b);
      break;
    case Bytecodes::_idiv:
    case Bytecodes::_irem:
      if (!pop())
        return false;
      pop();
      push(false);
      break;
    case Bytecodes::_ineg:
      break;

    case Bytecodes::_ladd:
    case Bytecodes::_lsub:
    case Bytecodes::_lmul:
    case Bytecodes::_land:
    case Bytecodes::_lxor:
      pop();
      pop();
      pop();
      pop();
      push(false);
      push(false);
      break;
    case Bytecodes::_lor:
      a = pop();
      b = pop();
      push(a && b);
      break;
    case Bytecodes::_ldiv:
    case Bytecodes::_lrem:
      pop();
      if (!pop())
        return false;
      pop();
      pop();
      push(false);
      push(false);
      break;
    case Bytecodes::_lneg:
      break;
    case Bytecodes::_lshl:
    case Bytecodes::_lshr:
    case Bytecodes::_lushr:
      pop();
      pop();
      pop();
      push(false);
      push(false);
      break;

    case Bytecodes::_fadd:
    case Bytecodes::_fsub:
    case Bytecodes::_fmul:
    case Bytecodes::_fdiv:
    case Bytecodes::_frem:
      pop();
      pop();
      push(false);
      break;
    case Bytecodes::_fneg:
      break;

    case Bytecodes::_dadd:
    case Bytecodes::_dsub:
    case Bytecodes::_dmul:
    case Bytecodes::_ddiv:
    case Bytecodes::_drem:
      pop();
      pop();
      pop();
      pop();
      push(false);
      push(false);
      break;
    case Bytecodes::_dneg:
      break;

    case Bytecodes::_iinc:
      set_local(iter()->get_index(), false);
      break;

    case Bytecodes::_lcmp:
      pop();
      pop();
      pop();
      pop();
      push(false);
      break;

    case Bytecodes::_fcmpl:
    case Bytecodes::_fcmpg:
      pop();
      pop();
      push(false);
      break;

    case Bytecodes::_dcmpl:
    case Bytecodes::_dcmpg:
      pop();
      pop();
      pop();
      pop();
      push(false);
      break;

    case Bytecodes::_i2l:
      push(false);
      break;
    case Bytecodes::_i2f:
      pop();
      push(false);
      break;
    case Bytecodes::_i2d:
      pop();
      push(false);
      push(false);
      break;

    case Bytecodes::_l2i:
    case Bytecodes::_l2f:
      pop();
      pop();
      push(false);
      break;
    case Bytecodes::_l2d:
      pop();
      pop();
      push(false);
      push(false);
      break;

    case Bytecodes::_f2i:
      pop();
      push(false);
      break;
    case Bytecodes::_f2l:
    case Bytecodes::_f2d:
      pop();
      push(false);
      push(false);
      break;

    case Bytecodes::_d2i:
    case Bytecodes::_d2f:
      pop();
      pop();
      push(false);
      break;
    case Bytecodes::_d2l:
      pop();
      pop();
      push(false);
      push(false);
      break;

    case Bytecodes::_i2b:
    case Bytecodes::_i2c:
    case Bytecodes::_i2s:
      pop();
      push(false);
      break;

    case Bytecodes::_return:
    case Bytecodes::_ireturn:
    case Bytecodes::_lreturn:
    case Bytecodes::_freturn:
    case Bytecodes::_dreturn:
    case Bytecodes::_areturn:
      break;

    default:
      return false;
    }
  }

  return true;
}

void YuhuInlinerHelper::initialize_for_check() {
  _locals = NEW_RESOURCE_ARRAY(bool, max_locals());
  _stack = NEW_RESOURCE_ARRAY(bool, max_stack());

  memset(_locals, 0, max_locals() * sizeof(bool));
  for (int i = 0; i < target()->arg_size(); i++) {
    YuhuValue *arg = entry_state()->stack(target()->arg_size() - 1 - i);
    if (arg && arg->zero_checked())
      set_local(i, true);
  }

  _sp = _stack;
}

bool YuhuInlinerHelper::do_field_access(bool is_get, bool is_field) {
  assert(is_get || is_field, "can't inline putstatic");

  // If the holder isn't linked then there isn't a lot we can do
  if (!target()->holder()->is_linked())
    return false;

  // Get the field
  bool will_link;
  ciField *field = iter()->get_field(will_link);
  if (!will_link)
    return false;

  // If the field is mismatched then an exception needs throwing
  if (is_field == field->is_static())
    return false;

  // Pop the value off the stack if necessary
  if (!is_get) {
    pop();
    if (field->type()->is_two_word())
      pop();
  }

  // Pop and null-check the receiver if necessary
  if (is_field) {
    if (!pop())
      return false;
  }

  // Push the result if necessary
  if (is_get) {
    bool result_pushed = false;
    if (field->is_constant() && field->is_static()) {
      YuhuConstant *sc = YuhuConstant::for_field(iter());
      if (sc->is_loaded()) {
        push(sc->is_nonzero());
        result_pushed = true;
      }
    }

    if (!result_pushed)
      push(false);

    if (field->type()->is_two_word())
      push(false);
  }

  return true;
}

bool YuhuInliner::attempt_inline(ciMethod *target, YuhuState *state, YuhuStack *stack, int bci) {
  if (YuhuIntrinsics::is_intrinsic(target)) {
    YuhuIntrinsics::inline_intrinsic(target, state, stack, bci);
    return true;
  }

  // No inline tree - use the old single-level inlining logic
  if (may_be_inlinable(target)) {
    YuhuInlinerHelper inliner(target, state);
    if (inliner.is_inlinable()) {
      inliner.do_inline();
      return true;
    }
  }
  return false;
}

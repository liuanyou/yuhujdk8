/*
 * Copyright (c) 1999, 2010, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_YUHU_YUHUINLINER_HPP
#define SHARE_VM_YUHU_YUHUINLINER_HPP

#include "ci/ciMethod.hpp"
#include "memory/allocation.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/yuhuState.hpp"

class YuhuInlineNode;
class YuhuTopLevelBlock;

class YuhuInliner : public AllStatic {
 public:
  static bool attempt_inline(ciMethod* target, YuhuState* state, YuhuStack* stack, int bci);
  // Path A (bytecode inliner) uses the strict form: any exception handler
  // table disqualifies the callee.  Path B (LLVM-IR inliner) passes
  // allow_exception_handlers=true because a Path B inlinee gets its own
  // llvm::Function with real block structure and its own ciMethod-driven
  // ciExceptionHandlerStream, so its local try/catch dispatches correctly
  // through YuhuTopLevelBlock::compute_exceptions / handle_exception /
  // emit_handler_checks and post-inline those blocks merge into the
  // caller's function.  Exceptions thrown but not caught inside the
  // inlinee travel via handle_return(T_VOID, exception)'s inlinee branch
  // (pending_exception store + ret to unified_exit_block); AlwaysInliner
  // Pass merges that ret into the caller's continuation, where do_llvm_
  // inline_call's newly-added check_pending_exception dispatches the
  // exception against the CALLER's own exception table at compiled-code
  // level.  Only if the caller doesn't match either does control reach
  // the caller-level handle_return (non-inlinee branch) which emits the
  // top-level unwind patchpoint -- and that path uses the caller's own
  // statepoint-id + bundle, so interpreter re-execution reconstructs a
  // full frame.  See is_llvm_inlineable's function-level doc for the
  // full M2 exception flow.
  //
  // allow_monitors follows the exact same reasoning and is set by the
  // same caller (Path B only).  Path A merges the callee's bytecodes
  // into the CALLER's block stream, so a callee monitor would need
  // BasicObjectLock slots in the caller's frame -- slots the caller
  // never allocated, because the caller's own max_monitors() is derived
  // from its own ciTypeFlow and does not fold in callees.  Path A's
  // YuhuBlock::do_monitorenter/do_monitorexit are ShouldNotCallThis()
  // for that reason.  Path B gives the callee its own llvm::Function and
  // its own frame: YuhuStack::initialize sizes the monitor area from the
  // inlinee's own max_monitors(), and YuhuTopLevelBlock::do_monitorenter
  // resolves slots through that per-level frame, so monitors are safe
  // across the inline boundary.
  static bool may_be_inlinable(ciMethod* target,
                               bool allow_exception_handlers = false,
                               bool allow_monitors = false);

  // Eligibility check for the LLVM-IR inlining path (Path B).  Yuhu
  // offers two inliners at each call site: Path A (the bytecode inliner,
  // YuhuInlinerHelper::is_inlinable + do_inline) pastes a callee's IR
  // straight into the caller's block, while Path B builds the callee as a
  // separate alwaysinline llvm::Function that AlwaysInlinerPass flattens
  // into the caller.  Path B is claimed for a callee only when Path A
  // CANNOT inline it -- i.e. the callee contains at least one opcode Path
  // A structurally rejects (branches, the invoke family, putstatic,
  // arraylength, array load/store, allocation, type checks, trapping
  // idiv/ldiv/irem/lrem, athrow, or monitorenter/monitorexit).  A callee
  // made purely of opcodes Path A already handles stays with Path A.
  // Callees NEITHER path can inline (invokehandle / jsr / goto_w) are
  // rejected here and fall through to a normal do_call.
  //
  // Historically (M0) the gate was "has at least one branch", on the
  // theory that Path B existed only to merge a callee's control-flow
  // graph -- which Path A could not do.  That heuristic went stale once
  // M1/M1+/M2 widened Path B's whitelist to absorb invokes, field/array
  // access, allocation, type checks, trapping div, and athrow: a
  // straight-line-with-an-invoke callee fell through BOTH nets un-
  // inlined.  The predicate is now "Path A declines it", checked against
  // the same opcode families the Path B whitelist already admits.  A
  // callee's opcode metadata must survive flattening: the inlinee's
  // allocas (locals, expression stack, monitor area) all collapse into
  // the caller's single LLVM frame once AlwaysInlinerPass runs, so the
  // runtime only ever sees one physical frame -- per-level boundaries are
  // reconstructed from the recorder's frame_holder_chain, not from
  // frame metadata.  Any opcode that survives the whitelist must therefore
  // be one whose safepoint/call/trap metadata can travel through Always-
  // InlinerPass via the statepoint-id attribute.  See is_llvm_inlineable's
  // function-level doc for the full opcode inventory and the M1 deopt-
  // bundle caveat.
  static bool is_llvm_inlineable(ciMethod* target);
};

#endif // SHARE_VM_YUHU_YUHUINLINER_HPP

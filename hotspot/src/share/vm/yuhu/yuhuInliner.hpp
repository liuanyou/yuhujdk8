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
  static bool may_be_inlinable(ciMethod* target, bool allow_exception_handlers = false);

  // M0 of LLVM-IR inlining: a callee is "llvm_inlineable" if it passes the
  // quick may_be_inlinable() checks, has control flow (so the bytecode-level
  // inliner could never handle it), yet is "quiescent": every instruction is
  // straight dataflow (constants, local load/store, stack shuffling, arithmetic,
  // conversions, comparisons) or a forward branch (if*/goto/tableswitch/
  // lookupswitch) that does not form a loop.  In particular it contains no
  // invoke/field-access/array-access/division/throw/new/instanceof/checkcast
  // instruction -- each of those can trap or call into the VM and so would
  // require a real frame, which an inlinee must not have.  See also the
  // "Four things B must not do" list in the inlinee design: this eligibility
  // check is what keeps B from ever reaching those code paths during its own
  // YuhuFunction::initialize().
  static bool is_llvm_inlineable(ciMethod* target);
};

#endif // SHARE_VM_YUHU_YUHUINLINER_HPP

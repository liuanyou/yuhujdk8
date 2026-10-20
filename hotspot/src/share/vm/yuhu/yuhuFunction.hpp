/*
 * Copyright (c) 1999, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2008, 2009 Red Hat, Inc.
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

#ifndef SHARE_VM_YUHU_YUHUFUNCTION_HPP
#define SHARE_VM_YUHU_YUHUFUNCTION_HPP

#include "ci/ciEnv.hpp"
#include "ci/ciStreams.hpp"
#include "ci/ciTypeFlow.hpp"
#include "memory/allocation.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/llvmValue.hpp"
#include "yuhu/yuhuBuilder.hpp"
#include "yuhu/yuhuContext.hpp"
#include "yuhu/yuhuInvariants.hpp"
#include "yuhu/yuhuStack.hpp"
#include "yuhu/yuhuDebugInformationRecorder.hpp"
#include "yuhu/yuhuInlineTree.hpp"

class YuhuTopLevelBlock;
class DeferredZeroCheck;

class YuhuFunction : public YuhuTargetInvariants {
 friend class YuhuStackWithNormalFrame;

 public:
  static llvm::Function* build(ciEnv*        env,
                               YuhuBuilder* builder,
                               ciTypeFlow*   flow,
                               const char*   name,
                               YuhuInlineTree* inline_tree) {
    auto inline_node_or_method = new YuhuInlineNodeOrMethod(flow->method());
    if (inline_tree->llvm_inline_allowed()) {
        inline_node_or_method = new YuhuInlineNodeOrMethod(inline_tree->root());
    }
    YuhuFunction function(env, builder, flow, name, inline_tree, inline_node_or_method, false);
    return function.function();
  }

  // M0 of LLVM-IR inlining: build a callee's IR as a real (but private,
  // alwaysinline) llvm::Function in the same module as the caller, instead
  // of merging its bytecodes into the caller's blocks the way the bytecode
  // inliner does.  The resulting YuhuFunction must outlive this call (it is
  // heap-allocated off the compilation's ResourceArea, not a stack temporary
  // like build()'s), since get_or_build_inlinee() caches it against the invoke
  // site's YuhuInlineNode.  Each distinct call site gets its own inlinee (they
  // are NOT shared by target method any more -- see get_or_build_inlinee) so it
  // owns its statepoint ids, deopt bundle and frame layout.  Unlike build(), this
  // does NOT run any of the four frame/global-registration side effects that
  // only the outermost function is allowed to perform (see the "_is_inlinee"
  // checks sprinkled through initialize()).
  static YuhuFunction* build_inlinee(ciEnv*        env,
                                     YuhuBuilder* builder,
                                     ciTypeFlow*   flow,
                                     const char*   name,
                                     YuhuInlineTree* inline_tree,
                                     YuhuInlineNode* inline_node) {
    return new YuhuFunction(env, builder, flow, name, inline_tree, new YuhuInlineNodeOrMethod(inline_node), true);
  }

 private:
  YuhuFunction(ciEnv*        env,
                YuhuBuilder* builder,
                ciTypeFlow*   flow,
                const char*   name,
                YuhuInlineTree* inline_tree,
                YuhuInlineNodeOrMethod* inline_node_or_method,
                bool          is_inlinee)
    : YuhuTargetInvariants(env, builder, inline_node_or_method, flow),
      _inline_tree(inline_tree),
      _is_inlinee(is_inlinee) { initialize(name); }

 private:
  void initialize(const char* name);

 private:
  llvm::Function*                   _function;
  YuhuTopLevelBlock**              _blocks;
  GrowableArray<DeferredZeroCheck*> _deferred_zero_checks;
  YuhuStack*                       _stack;
  YuhuInlineTree*                  _inline_tree;  // Inline tree for depth-2 inlining
  llvm::BasicBlock*                _unified_exit_block;  // Unified exit block for all returns
  llvm::Value*                     _return_slot;         // Return slot (pc_slot in frame header)
  llvm::Value*                     _x0_slot; // 8th int-like argument
  bool                             _is_inlinee;  // True for a callee built as a separate, alwaysinline llvm::Function

  // Inlinees built for this (non-inlinee) function's call sites, keyed by the
  // invoke-site YuhuInlineNode (NOT by target method), so that two call sites
  // invoking the same method each get their own llvm::Function with per-site
  // statepoint / deopt-bundle / frame-layout metadata; sharing by method caused
  // OopMap over-claims when the same method was inlined at multiple bcis.  The
  // AlwaysInliner clones each inlinee's body into the caller anyway, so this
  // does not grow emitted code size.  Empty on an
  // inlinee itself, since an inlinee is guaranteed (by YuhuInliner::
  // is_llvm_inlineable) to contain no call sites at all.
  GrowableArray<YuhuFunction*> _inlinees;

 public:
  bool is_inlinee() const {
    return _is_inlinee;
  }

  // Find an already-built inlinee for this target, or build and cache a new
  // one.  Must be called with the builder's insert point/position already
  // set up for the eventual `call` instruction; this function saves and
  // restores the builder's global state around the (potentially long-running)
  // nested inlinee build so that the caller's own IR construction can safely
  // resume exactly where it left off afterwards.
  YuhuFunction* get_or_build_inlinee(YuhuInlineTree* inline_tree, YuhuInlineNode* inline_node, const char* name);

 public:
  llvm::Function* function() const {
    return _function;
  }
  // Expose target() for YuhuBuilder to access the method being compiled
  ciMethod* target_method() const {
    return target();
  }
  // Expose env() for YuhuBuilder to access ciEnv
  ciEnv* env() const {
    return YuhuTargetInvariants::env();
  }
  int block_count() const {
    return flow()->block_count();
  }
  YuhuTopLevelBlock* block(int i) const {
    assert(i < block_count(), "should be");
    return _blocks[i];
  }
  llvm::Value* return_slot() const { return _return_slot; }
  GrowableArray<DeferredZeroCheck*>* deferred_zero_checks() {
    return &_deferred_zero_checks;
  }
  YuhuStack* stack() const {
    return _stack;
  }
  YuhuInlineTree* inline_tree() const {
    return _inline_tree;
  }

  llvm::Value* x0_slot() const { return _x0_slot; }
  
  // On-stack replacement
 private:
  bool is_osr() const {
    return flow()->is_osr_flow();
  }
  // Generate function signature dynamically based on Java method parameters
  // Both normal and OSR entry use the same signature:
  //   Instance: (dummy_x0, this, params...) -> return_type
  //   Static:   (dummy_x0, params...) -> return_type
  // The OSR adapter is responsible for extracting locals from the OSR buffer
  // and placing them into the same registers as normal entry.
  //
  // An inlinee is different: it is never entered from generated code directly
  // (the AlwaysInliner pass always eliminates the call before codegen), so it
  // does not need to mirror the AArch64 HotSpot calling convention's dummy-x0
  // slot.  Its signature is just the natural Java-level one.
  llvm::FunctionType* entry_point_type() const {
    return _is_inlinee ? generate_inlinee_entry_point_type()
                        : generate_normal_entry_point_type();
  }
  
 private:
  // Generate function signature for normal entry based on Java method parameters
  llvm::FunctionType* generate_normal_entry_point_type() const;

  // Generate the (simpler) signature used by an alwaysinline callee: no dummy
  // x0 slot, just (this?, params...) -> return_type.  See above.
  llvm::FunctionType* generate_inlinee_entry_point_type() const;

  // Block management
 private:
  llvm::BasicBlock* _block_insertion_point;

  void set_block_insertion_point(llvm::BasicBlock* block_insertion_point) {
    _block_insertion_point = block_insertion_point;
  }
  llvm::BasicBlock* block_insertion_point() const {
    return _block_insertion_point;
  }

 public:
  llvm::BasicBlock* CreateBlock(const char* name = "") const {
    return llvm::BasicBlock::Create(
      YuhuContext::current(), name, function(), block_insertion_point());
  }

  // Deferred zero checks
 public:
  void add_deferred_zero_check(YuhuTopLevelBlock* block,
                               YuhuValue*         value);

  // Unified exit block for all return paths
 public:
  // Get or create the unified exit block where all returns should jump to
  // This block contains the epilogue marker and ret instruction
  llvm::BasicBlock* unified_exit_block();

 private:
  void do_deferred_zero_checks();
};

#endif // SHARE_VM_YUHU_YUHUFUNCTION_HPP

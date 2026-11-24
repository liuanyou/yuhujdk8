/*
 * Copyright (c) 2026, Yuhu Project. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * Inline planner for pre-pass inlining decisions
 */

#ifndef SHARE_VM_YUHU_YUHUINLINEPLANNER_HPP
#define SHARE_VM_YUHU_YUHUINLINEPLANNER_HPP

#include "ci/ciMethod.hpp"
#include "ci/ciStreams.hpp"
#include "ci/ciEnv.hpp"
#include "ci/ciUtilities.hpp"      // CURRENT_ENV
#include "ci/ciMethodData.hpp"
#include "ci/ciInstanceKlass.hpp"
#include "yuhu/yuhuInlineTree.hpp"
#include "yuhu/yuhuInliner.hpp"
#include "yuhu/yuhuCompiler.hpp"
#include "yuhu/yuhu_globals.hpp"

// Planner for inlining decisions - does a pre-pass before IR generation
class YuhuInlinePlanner : public StackObj {
 public:
  // Plan inlining for a method, building an inline tree
  static YuhuInlineTree* plan_inlining(ciMethod* root_method) {
      // Path B is offered for this whole compilation only when the develop flag is
      // on AND the root (caller) method has not been backed off by a previous
      // flatten-miss (see YuhuCompiler::disable_llvm_inline).  When false, no
      // candidate is ever marked llvm_inline, so the caller falls back to the
      // existing bytecode/do_call path on retry.
      bool llvm_inline_allowed = YuhuLLVMInlining && !YuhuCompiler::llvm_inline_disabled(root_method->get_Method());
    YuhuInlineTree* tree = new YuhuInlineTree(root_method, llvm_inline_allowed);
    if (llvm_inline_allowed) {
        YuhuInlinePlanner planner(tree);
        planner.walk_bytecodes(tree->root(), 0);
    }
    
    if (YuhuTraceInlining) {
      tree->print();
    }
    
    return tree;
  }

 private:
  YuhuInlineTree* _tree;
  int _max_inline_depth;

  YuhuInlinePlanner(YuhuInlineTree* tree)
    : _tree(tree),
      _max_inline_depth(YuhuMaxInlineLevel) { }

  // Walk bytecodes of a method to find inlining opportunities
  void walk_bytecodes(YuhuInlineNode* parent_node, int current_depth) {
    if (current_depth >= _max_inline_depth) {
      return;
    }

    ciMethod* method = parent_node->method();
    // Skip if the method's holder class is not yet linked
    // (bytecodes haven't been rewritten, CP cache indices are invalid)
    if (!method->holder()->is_linked()) {
        return;
    }
    ciBytecodeStream iter(method);

    // Iterate through all bytecodes
    while (iter.next() != ciBytecodeStream::EOBC()) {
        int bci = iter.cur_bci();
        Bytecodes::Code bc = iter.cur_bc();

      // Check if this is an invoke bytecode
      if (is_invoke(bc)) {
        // Try to resolve the target method
        bool will_link;
        ciSignature* declared_signature = NULL;
        ciMethod* target = iter.get_method(will_link, &declared_signature);

        // Increment 2: profile-driven devirtualization.  For _invokevirtual
        // at the optimized tier, if the caller's MDO shows a monomorphic
        // receiver at this bci, substitute the profiled concrete method for
        // the CP-cache statically resolved target.  Only affects the inline
        // tree; the runtime receiver-klass guard is emitted at IR generation
        // (YuhuTopLevelBlock::do_call).  A null guard_klass means the site
        // stays on the existing path.
        //
        // The same path applies to _invokeinterface: in JDK8, both
        // _invokevirtual and _invokeinterface lay out their MDO slice as
        // VirtualCallData (see methodData.cpp allocate_data: both branches
        // share the code that picks virtual_call_data_tag).  Receiver rows
        // have identical semantics, and find_monomorphic_target resolves
        // through resolve_invoke against the profiled concrete receiver
        // klass (which walks the itable for interface method holders).
        ciKlass* guard_klass = NULL;
        if (will_link && target != NULL &&
            (bc == Bytecodes::_invokevirtual || bc == Bytecodes::_invokeinterface)) {
          ciMethod* devirt = virtual_call_target(method, bci, target, &guard_klass);
          if (devirt != NULL) {
            target = devirt;
          } else {
            guard_klass = NULL;
          }
        }

        if (will_link && target != NULL) {
          // Check if we should inline this method.  Pass guard_klass so a
          // devirtualized-but-straight-line callee (needs_path_b false) is
          // still claimed by Path B and recorded in the inline tree; its
          // node carries guard_klass for do_call's receiver-klass guard.
          if (should_inline(method, target, current_depth, bci) && YuhuInliner::is_llvm_inlineable(target, guard_klass)) {

            // Create a new inline node for this method
            YuhuInlineNode* child = (guard_klass != NULL)
              ? new YuhuInlineNode(target, bci, current_depth + 1, parent_node, guard_klass)
              : new YuhuInlineNode(target, bci, current_depth + 1, parent_node);
            parent_node->add_child(child);
            
            // Recursively walk the inlined method's bytecodes
            walk_bytecodes(child, current_depth + 1);
          }
        }
      }
    }
  }

  // Check if a bytecode is an invoke
  bool is_invoke(Bytecodes::Code bc) {
    return bc == Bytecodes::_invokestatic ||
           bc == Bytecodes::_invokevirtual ||
           bc == Bytecodes::_invokespecial ||
           bc == Bytecodes::_invokeinterface;
  }

  // Decide whether to inline a method.
  //
  // Increment 1 (profile-driven): at the optimized tier, use the caller's
  // MethodData to skip cold call sites and cold callees.  The fast tier and any
  // compilation without a usable profile keep the previous "accept every
  // depth-eligible callee" behaviour, so nothing regresses when MDO data is
  // absent and level 5 is untouched.
  bool should_inline(ciMethod* caller, ciMethod* target, int current_depth, int bci) {
    // Depth budget (unchanged).
    if (current_depth + 1 >= _max_inline_depth) {
      return false;
    }

    if (CURRENT_ENV == NULL || CURRENT_ENV->comp_level() != CompLevel_yuhu_optimized) {
      return true;   // fast tier / no env -> static behaviour
    }

    // (a) Call-site frequency from the caller's profile at this invoke bci.
    int site_count = call_site_count(caller, bci);

    // (b) Callee frequency: how often the target itself has executed.  An
    //     immature or missing MDO reads as -1 ("unknown").  method_data_or_null()
    //     already snapshots the data under VM_ENTRY_MARK, so we must NOT call
    //     load_data() again here -- it re-translates receiver types via
    //     ciEnv::get_klass, which asserts IS_IN_VM and aborts this native thread.
    int callee_count = -1;
    ciMethodData* callee_mdo = target->method_data_or_null();
    if (callee_mdo != NULL && callee_mdo->is_mature()) {
      callee_count = callee_mdo->invocation_count() + callee_mdo->backedge_count();
    }

    // Reject only when there is positive evidence that BOTH the site and the
    // callee are cold; an unknown (-1) side never vetoes the inline.
    if (site_count >= 0 && callee_count >= 0 &&
        site_count < YuhuInlineHotSiteThreshold &&
        callee_count < YuhuInlineHotSiteThreshold) {
      return false;
    }

    return true;
  }

  // Execution count recorded at invoke bci in the caller's MethodData, or -1
  // when the caller has no profile or the site carries no counter.  Invoke
  // sites are CallTypeData / VirtualCallTypeData, all derived from CounterData,
  // so as_CounterData() reads the shared count cell without a sibling cast.
  // method_data_or_null() has already loaded the snapshot in VM state; reading
  // the copied data below is plain memory access and safe on this native thread.
  int call_site_count(ciMethod* caller, int bci) {
    ciMethodData* caller_mdo = caller->method_data_or_null();
    if (caller_mdo == NULL) {
      return -1;
    }
    if (!caller_mdo->is_mature()) {
      return -1;
    }
    ciProfileData* data = caller_mdo->bci_to_data(bci);
    if (data == NULL || !data->is_CounterData()) {
      return -1;
    }
    return (int)data->as_CounterData()->count();
  }

  // Increment 2: profile-driven devirtualization lookup.
  //
  // Reads the caller's VirtualCallData (i.e., ReceiverTypeData rows) at the
  // given invoke bci and, if the site is monomorphic and hot, returns the
  // concrete method resolved for the profiled receiver klass, together with
  // that klass so IR generation can emit a receiver-klass guard.  Returns
  // NULL whenever the substitution is unsafe or unavailable (flag off, fast
  // tier, missing/immature MDO, non-virtual or polymorphic site, cold count,
  // already statically bound, or CHA could not resolve a unique concrete
  // target).  On success, *out_guard_klass is populated.
  //
  // The concrete target is derived via find_monomorphic_target so the same
  // CHA query that improve_virtual_call uses today also backs the substitution
  // here; the profile is what lets us swap in a target DIFFERENT from the CP
  // entry (which the existing CHA-only path explicitly inhibits).  Because
  // the substitution is profile-backed and NOT a compile-time CHA guarantee,
  // the caller-side guard emission (see YuhuTopLevelBlock::do_call) is
  // mandatory before this target reaches executable IR.
  //
  // Never call ciMethodData::load_data() directly on the compiler thread:
  // method_data_or_null() already snapshots under VM_ENTRY_MARK, so reads on
  // the returned object are pure memory reads in native state.
  ciMethod* virtual_call_target(ciMethod* caller, int bci, ciMethod* dest_method,
                                 ciKlass** out_guard_klass) {
    if (!YuhuDevirtualize) {
      return NULL;
    }
    *out_guard_klass = NULL;
    if (CURRENT_ENV == NULL || CURRENT_ENV->comp_level() != CompLevel_yuhu_optimized) {
      return NULL;
    }
    // Private / final / static targets are already direct; nothing to gain.
    if (dest_method->can_be_statically_bound()) {
      return NULL;
    }

    ciMethodData* caller_mdo = caller->method_data_or_null();
    if (caller_mdo == NULL || !caller_mdo->is_mature()) {
      return NULL;
    }
    ciProfileData* data = caller_mdo->bci_to_data(bci);
    if (data == NULL || !data->is_VirtualCallData()) {
      return NULL;
    }
    // ciProfileData is a typedef of ProfileData; cast the same way
    // ciMethodData.cpp:495 does.  The ci subclass exposes receiver(row)
    // returning ciKlass* rather than the VM Klass*.
    ciVirtualCallData* vcd = (ciVirtualCallData*) data->as_VirtualCallData();
    if (vcd == NULL) {
      return NULL;
    }

    // Monomorphic test: exactly one non-null receiver row, above the hot
    // site threshold.
    ciKlass* recv0 = vcd->receiver(0);
    if (recv0 == NULL || !recv0->is_instance_klass()) {
      return NULL;
    }
    if ((int)vcd->receiver_count(0) < (int)YuhuInlineHotSiteThreshold) {
      return NULL;
    }
    const uint rows = vcd->row_limit();
    for (uint i = 1; i < rows; i++) {
      if (vcd->receiver(i) != NULL) {
        return NULL;   // polymorphic
      }
    }

    ciInstanceKlass* actual = recv0->as_instance_klass();
    // Reuse the CHA query to resolve the concrete method for the profiled
    // receiver klass.  Same call the existing improve_virtual_call uses, so
    // the resolution semantics stay consistent.
    ciMethod* concrete = dest_method->find_monomorphic_target(
        caller->holder(), actual, actual);
    if (concrete == NULL || concrete->is_abstract()) {
      return NULL;
    }
    // If CHA already yields the same target as the CP entry, there's nothing
    // to substitute.
    if (concrete == dest_method) {
      return NULL;
    }
    *out_guard_klass = actual;
    return concrete;
  }
};

#endif // SHARE_VM_YUHU_YUHUINLINEPLANNER_HPP

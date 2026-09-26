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
#include "yuhu/yuhuInlineTree.hpp"
#include "yuhu/yuhuInliner.hpp"
#include "yuhu/yuhu_globals.hpp"

// Planner for inlining decisions - does a pre-pass before IR generation
class YuhuInlinePlanner : public StackObj {
 public:
  // Plan inlining for a method, building an inline tree
  static YuhuInlineTree* plan_inlining(ciMethod* root_method) {
    YuhuInlineTree* tree = new YuhuInlineTree(root_method);
    YuhuInlinePlanner planner(tree);
    planner.walk_bytecodes(tree->root(), 0);
    
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
      _max_inline_depth(YuhuMaxInlineLevel) {}

  // Walk bytecodes of a method to find inlining opportunities
  void walk_bytecodes(YuhuInlineNode* parent_node, int current_depth) {
    if (current_depth >= _max_inline_depth) {
      return;
    }

    ciMethod* method = parent_node->method();
    ciBytecodeStream iter(method);

    // Iterate through all bytecodes
    for (int bci = iter.next(); bci != ciBytecodeStream::EOBC(); bci = iter.next()) {
      Bytecodes::Code bc = iter.cur_bc();

      // Check if this is an invoke bytecode
      if (is_invoke(bc)) {
        // Try to resolve the target method
        bool will_link;
        ciSignature* declared_signature = NULL;
        ciMethod* target = iter.get_method(will_link, &declared_signature);
        
        if (will_link && target != NULL) {
          // Check if we should inline this method
          if (should_inline(target, current_depth, bci)) {
            // Create a new inline node for this method
            YuhuInlineNode* child = new YuhuInlineNode(target, bci, current_depth + 1);
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

  // Decide whether to inline a method
  bool should_inline(ciMethod* target, int current_depth, int bci) {
    // Use existing YuhuInliner checks
    if (!YuhuInliner::may_be_inlinable(target)) {
      return false;
    }

    // For depth 2, we also need to check if the method itself is inlinable
    // This is a simplified check - in the future, we might want more sophisticated logic
    if (current_depth + 1 < _max_inline_depth) {
      // We're inlining at depth 1, so check if the callee can also be inlined
      // For now, just check if it passes the basic eligibility
      return true;
    }

    // At max depth, only inline if the method is simple
    return YuhuInliner::may_be_inlinable(target);
  }
};

#endif // SHARE_VM_YUHU_YUHUINLINEPLANNER_HPP

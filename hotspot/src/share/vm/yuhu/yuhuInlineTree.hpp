/*
 * Copyright (c) 2026, Yuhu Project. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * Inline tree data structure for tracking method inlining decisions
 */

#ifndef SHARE_VM_YUHU_YUHUINLINETREE_HPP
#define SHARE_VM_YUHU_YUHUINLINETREE_HPP

#include "ci/ciMethod.hpp"
#include "memory/allocation.hpp"

// Forward declarations
class YuhuInlineTree;

// Represents a single inlined method in the inline tree
class YuhuInlineNode : public ResourceObj {
 public:
  YuhuInlineNode(ciMethod* method, int bci, int inline_depth, YuhuInlineNode* parent)
    : _method(method),
      _bci(bci),
      _inline_depth(inline_depth),
      _children(NULL),
      _parent(parent),
      _max_locals(method->max_locals()),
      _max_stack(method->max_stack()) {}

 private:
  ciMethod* _method;           // The inlined method
  int _bci;                    // BCI in the caller where this method is inlined
  int _inline_depth;           // Depth of this inlining (0 = root method)
  GrowableArray<YuhuInlineNode*>* _children;  // Methods inlined into this method
  YuhuInlineNode* _parent; // parent node for this inlined node
  int _max_locals;             // Method's max_locals
  int _max_stack;              // Method's max_stack

 public:
  ciMethod* method() const { return _method; }
  int bci() const { return _bci; }
  int inline_depth() const { return _inline_depth; }
  int max_locals() const { return _max_locals; }
  int max_stack() const { return _max_stack; }

  GrowableArray<YuhuInlineNode*>* children() const { return _children; }
  YuhuInlineNode* parent() const { return _parent; }

  void add_child(YuhuInlineNode* child) {
    if (_children == NULL) {
      _children = new GrowableArray<YuhuInlineNode*>(4);
    }
    _children->append(child);
  }

  // Calculate total stack space needed for this node and all children
  int total_locals() const {
    int total = _max_locals;
    if (_children != NULL) {
      for (int i = 0; i < _children->length(); i++) {
        total += _children->at(i)->total_locals();
      }
    }
    return total;
  }

  int max_stack_depth() const {
    int max_child_stack = 0;
    if (_children != NULL) {
      for (int i = 0; i < _children->length(); i++) {
        int child_stack = _children->at(i)->max_stack_depth();
        if (child_stack > max_child_stack) {
          max_child_stack = child_stack;
        }
      }
    }
    return _max_stack > max_child_stack ? _max_stack : max_child_stack;
  }

  // Find a child node by BCI (for inlining decisions during IR generation)
  YuhuInlineNode* find_child_by_bci(int bci) const {
    if (_children == NULL) return NULL;
    for (int i = 0; i < _children->length(); i++) {
      if (_children->at(i)->bci() == bci) {
        return _children->at(i);
      }
    }
    return NULL;
  }

  void print() const {
    tty->print("  ");
    for (int i = 0; i < _inline_depth; i++) {
      tty->print("  ");
    }
    tty->print_cr("depth=%d bci=%d %s.%s (locals=%d, stack=%d)",
                  _inline_depth, _bci,
                  _method->holder()->name()->as_utf8(),
                  _method->name()->as_utf8(),
                  _max_locals, _max_stack);
    if (_children != NULL) {
      for (int i = 0; i < _children->length(); i++) {
        _children->at(i)->print();
      }
    }
  }
};

// Root of the inline tree for a compilation
class YuhuInlineTree : public ResourceObj {
 public:
  YuhuInlineTree(ciMethod* root_method, bool llvm_inline_allowed)
    : _root(new YuhuInlineNode(root_method, 0, 0, NULL)), _llvm_inline_allowed(llvm_inline_allowed) {}

 private:
  YuhuInlineNode* _root;
    // Path B is offered for this whole compilation only when the develop flag is
    // on AND the root (caller) method has not been backed off by a previous
    // flatten-miss (see YuhuCompiler::disable_llvm_inline).  When false, no
    // candidate is ever marked llvm_inline, so the caller falls back to the
    // existing bytecode/do_call path on retry.
    bool _llvm_inline_allowed;

 public:
  YuhuInlineNode* root() const { return _root; }
  bool llvm_inline_allowed() const { return _llvm_inline_allowed; }

  // Calculate total locals needed for all inlined methods
  int total_locals() const {
    return _root->total_locals();
  }

  // Calculate max operand stack depth across all inlined methods
  int max_stack_depth() const {
    return _root->max_stack_depth();
  }

  void print() const {
    tty->print_cr("Inline tree for %s.%s:",
                  _root->method()->holder()->name()->as_utf8(),
                  _root->method()->name()->as_utf8());
    _root->print();
    tty->print_cr("Total locals: %d, Max stack: %d",
                  total_locals(), max_stack_depth());
  }
};

// even though a method can't be created as inline node, but it still can be inlined, like intrinsic method.
class YuhuInlineNodeOrMethod : public ResourceObj {
private:
    YuhuInlineNode* _inline_node;
    ciMethod* _method;

public:
    YuhuInlineNodeOrMethod(YuhuInlineNode* inline_node): _inline_node(inline_node), _method(NULL) {}
    YuhuInlineNodeOrMethod(ciMethod* method): _inline_node(NULL), _method(method) {}

    bool is_inline_node() const {
        return _inline_node != NULL;
    }

    bool is_method() const {
        return _method != NULL;
    }

    YuhuInlineNode* as_inline_node() const {
        assert(is_inline_node(), "");
        return _inline_node;
    }

    ciMethod* as_method() const {
        assert(is_method(), "");
        return _method;
    }

    ciMethod* target() const {
        return is_inline_node() ? _inline_node->method() : _method;
    }
};

#endif // SHARE_VM_YUHU_YUHUINLINETREE_HPP

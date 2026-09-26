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
  YuhuInlineNode(ciMethod* method, int bci, int inline_depth)
    : _method(method),
      _bci(bci),
      _inline_depth(inline_depth),
      _children(NULL),
      _max_locals(method->max_locals()),
      _max_stack(method->max_stack()) {}

 private:
  ciMethod* _method;           // The inlined method
  int _bci;                    // BCI in the caller where this method is inlined
  int _inline_depth;           // Depth of this inlining (0 = root method)
  GrowableArray<YuhuInlineNode*>* _children;  // Methods inlined into this method
  int _max_locals;             // Method's max_locals
  int _max_stack;              // Method's max_stack

 public:
  ciMethod* method() const { return _method; }
  int bci() const { return _bci; }
  int inline_depth() const { return _inline_depth; }
  int max_locals() const { return _max_locals; }
  int max_stack() const { return _max_stack; }

  GrowableArray<YuhuInlineNode*>* children() const { return _children; }

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

  // Calculate the physical slot offset for this node's locals
  // This is the sum of all previous nodes' locals in the inline tree
  int locals_slot_offset() const {
    // For now, return 0 - will be calculated during frame setup
    // The actual offset depends on the traversal order
    return 0;
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
  YuhuInlineTree(ciMethod* root_method)
    : _root(new YuhuInlineNode(root_method, 0, 0)) {}

 private:
  YuhuInlineNode* _root;

 public:
  YuhuInlineNode* root() const { return _root; }

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

#endif // SHARE_VM_YUHU_YUHUINLINETREE_HPP

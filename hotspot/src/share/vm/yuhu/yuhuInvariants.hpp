/*
 * Copyright (c) 1999, 2013, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_YUHU_YUHUINVARIANTS_HPP
#define SHARE_VM_YUHU_YUHUINVARIANTS_HPP

#include "ci/ciEnv.hpp"
#include "ci/ciInstanceKlass.hpp"
#include "ci/ciMethod.hpp"
#include "ci/ciTypeFlow.hpp"
#include "code/debugInfoRec.hpp"
#include "code/dependencies.hpp"
#include "memory/allocation.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/yuhuCodeBuffer.hpp"
#include "yuhu/yuhuInlineTree.hpp"

class YuhuBuilder;

// The narrowOop encode invariant, and the reason it needs a machine check
// rather than a reviewer: see doc/yuhu/activities/082_*.
//
// A putfield of a reference under compressed oops is built as
//
//   store i32 <encode of the oop>, ptr addrspace(1) <field address>
//
// and is only correct if the encode is still there in the IR the backend
// consumes.  The middle-end can cancel an encode/decode pair into an identity
// (trunc(lshr(shl(zext(n),s),s)) -> n), which leaves the *pre-collection* narrow
// read feeding the store.  That value is an i32, so it is invisible to RS4GC
// liveness, is spilled to a slot that is in no OopMap, and is never relocated:
// the field ends up holding a stale address and the next getfield of it crashes
// in whatever method reads it.  Nothing about the builder's own IR shows this;
// it only shows on the post-optimization artifact, which is what this class
// looks at.
//
// The two marks are attached by the builder and are what makes the check
// precise without having to guess which i32 stores are references (an int field
// copy has an identical-looking IR chain):
//
//   store_md_name()  on the StoreInst of every reference field write, and
//   encode_md_name() on the inline-asm call that produces the narrowOop.
//
// Both marks are emitted whether or not the check is enabled, so the dumped IR
// looks the same with the safety net on and off; only the checking is gated.
// The marks carry the field type, which the IR itself does not: without them an
// int field copy and a folded reference store look exactly alike.
//
// Where the marks are readable matters.  Measured on dumped IR: InstCombine and
// SimplifyCFG leave both marks on their instructions, but the artifact
// YuhuRewriteStatepointsForGC produces has lost every store mark while keeping
// the encode marks (which of its rewrites does it is not pinned down).  So the
// scan runs on the pre-RS4GC IR at both entry points below; a scan of the
// post-RS4GC artifact would find no marked store at all and report that as clean.
//
// The check is then: for every marked store, the def chain of the stored value
// must not reach a narrow (i32) load out of the heap without crossing a marked
// encode.  Reach one and the encode is gone -- report it, and fail the
// compilation so the method runs under C1/C2 instead of installing code that
// corrupts the heap.
class YuhuNarrowOopInvariant : public AllStatic {
 public:
  // Metadata names, shared with the builder sites that set them.
  static const char* store_md_name();
  static const char* encode_md_name();

  // Mark the inline-asm call that encodes `oop` into a narrowOop.
  static void mark_encode(llvm::Value* encoded);

  // Mark a store of an oop into a reference-typed field.
  static void mark_store(llvm::Value* store);

  // Scan already-optimized IR for violations.  Reports each violation, and
  // returns the number found.  This is the deciding entry point when it is
  // called on the module that has been through the real pass pipeline, and a
  // pure observer when it is called from YuhuIRTransformer::runGCPasses().
  static unsigned scan(llvm::Module& module, bool report);

  // How many stores the builder marked in this module.  Used for the trace line
  // that distinguishes "checked and clean" from "nothing to check".
  static unsigned count_marked_stores(llvm::Module& module);

  // How many encodes the builder marked in this module.  A decrease across the
  // middle-end is reported (see check_after_middle_end) but does not by itself
  // fail a compilation, because removing a block the branch is not reachable
  // from also removes the encode inside it, and that is not the bug.
  static unsigned count_marked_encodes(llvm::Module& module);

  // Run the same middle-end prefix the JIT will run (InstCombine +
  // SimplifyCFG, no RS4GC) over a throwaway clone of the module that owns
  // `function`, and scan the result.  Returns the number of violations, and
  // leaves the original IR untouched.  This exists so that a violation can
  // turn into a clean ciEnv failure: the real pipeline runs later, inside the
  // ORC IR-transform callback, where there is no ciEnv left to record against.
  static unsigned check_after_middle_end(llvm::Function* function);
};

// Base classes used to track various values through the compilation.
// YuhuCompileInvariants is used to track values which remain the
// same for the top-level method and any inlined methods it may have
// (ie for the whole compilation).  YuhuTargetInvariants is used to
// track values which differ between methods.

class YuhuCompileInvariants : public ResourceObj {
 protected:
  YuhuCompileInvariants(ciEnv* env, YuhuBuilder* builder)
    : _env(env),
      _builder(builder),
      _thread(NULL) {}

  YuhuCompileInvariants(const YuhuCompileInvariants* parent)
    : _env(parent->_env),
      _builder(parent->_builder),
      _thread(parent->_thread) {}

 private:
  ciEnv*        _env;
  YuhuBuilder* _builder;
  llvm::Value*  _thread;

  // Top-level broker for HotSpot's Compiler Interface.
  //
  // Its main purpose is to allow the various CI classes to access
  // oops in the VM without having to worry about safepointing.  In
  // addition to this it acts as a holder for various recorders and
  // memory allocators.
  //
  // Accessing this directly is kind of ugly, so it's private.  Add
  // new accessors below if you need something from it.
 protected:
  ciEnv* env() const {
    assert(_env != NULL, "env not available");
    return _env;
  }

  // The YuhuBuilder that is used to build LLVM IR.
 protected:
  YuhuBuilder* builder() const {
    return _builder;
  }

  // Pointer to this thread's JavaThread object.  This is not
  // available until a short way into YuhuFunction creation
  // so a setter is required.  Assertions are used to enforce
  // invariance.
 protected:
  llvm::Value* thread() const {
    assert(_thread != NULL, "thread not available");
    return _thread;
  }
  void set_thread(llvm::Value* thread) {
    assert(_thread == NULL, "thread already set");
    _thread = thread;
  }

  // Objects that handle various aspects of the compilation.
 protected:
  DebugInformationRecorder* debug_info() const {
    return env()->debug_info();
  }
  YuhuCodeBuffer* code_buffer() const;

 public:
  Dependencies* dependencies() const {
    return env()->dependencies();
  }

  // Commonly used classes
 protected:
  ciInstanceKlass* java_lang_Object_klass() const {
    return env()->Object_klass();
  }
  ciInstanceKlass* java_lang_Throwable_klass() const {
    return env()->Throwable_klass();
  }
};

class YuhuTargetInvariants : public YuhuCompileInvariants {
 protected:
  YuhuTargetInvariants(ciEnv* env, YuhuBuilder* builder, YuhuInlineNodeOrMethod* inline_node_or_method, ciTypeFlow* flow)
    : YuhuCompileInvariants(env, builder),
      _inline_node_or_method(inline_node_or_method),
      _flow(flow),
      _max_monitors(count_monitors()) {}

  YuhuTargetInvariants(const YuhuCompileInvariants* parent, YuhuInlineNodeOrMethod* inline_node_or_method)
    : YuhuCompileInvariants(parent),
      _inline_node_or_method(inline_node_or_method),
      _flow(NULL),
      _max_monitors(count_monitors()) {}

  YuhuTargetInvariants(const YuhuTargetInvariants* parent)
    : YuhuCompileInvariants(parent),
      _inline_node_or_method(parent->_inline_node_or_method),
      _flow(parent->_flow),
      _max_monitors(parent->_max_monitors) {}

 private:
  int count_monitors();

 private:
  YuhuInlineNodeOrMethod* _inline_node_or_method;
  ciTypeFlow* _flow;
  int         _max_monitors;

  // The method being compiled.
 public:
  ciMethod* target() const {
    return _inline_node_or_method->is_inline_node() ? _inline_node_or_method->as_inline_node()->method() : _inline_node_or_method->as_method();
  }
  YuhuInlineNodeOrMethod* inline_node_or_method() const {
      return _inline_node_or_method;
  }

  // Typeflow analysis of the method being compiled.
 protected:
  ciTypeFlow* flow() const {
    assert(_flow != NULL, "typeflow not available");
    return _flow;
  }

  // Properties of the method.
 protected:
  int max_locals() const {
    return target()->max_locals();
  }
  int max_stack() const {
    return target()->max_stack();
  }
  int max_monitors() const {
    return _max_monitors;
  }
  int arg_size() const {
    return target()->arg_size();
  }
  bool is_static() const {
    return target()->is_static();
  }
  bool is_synchronized() const {
    return target()->is_synchronized();
  }
};

#endif // SHARE_VM_YUHU_YUHUINVARIANTS_HPP

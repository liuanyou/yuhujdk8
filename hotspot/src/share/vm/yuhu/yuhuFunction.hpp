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

class YuhuTopLevelBlock;
class DeferredZeroCheck;

class YuhuFunction : public YuhuTargetInvariants {
 friend class YuhuStackWithNormalFrame;

 public:
  static llvm::Function* build(ciEnv*        env,
                               YuhuBuilder* builder,
                               ciTypeFlow*   flow,
                               const char*   name) {
    YuhuFunction function(env, builder, flow, name);
    return function.function();
  }

 private:
  YuhuFunction(ciEnv*        env,
                YuhuBuilder* builder,
                ciTypeFlow*   flow,
                const char*   name)
    : YuhuTargetInvariants(env, builder, flow) { initialize(name); }

 private:
  void initialize(const char* name);

 private:
  llvm::Function*                   _function;
  YuhuTopLevelBlock**              _blocks;
  GrowableArray<DeferredZeroCheck*> _deferred_zero_checks;
  YuhuStack*                       _stack;

 public:
  llvm::Function* function() const {
    return _function;
  }
  int block_count() const {
    return flow()->block_count();
  }
  YuhuTopLevelBlock* block(int i) const {
    assert(i < block_count(), "should be");
    return _blocks[i];
  }
  GrowableArray<DeferredZeroCheck*>* deferred_zero_checks() {
    return &_deferred_zero_checks;
  }
  YuhuStack* stack() const {
    return _stack;
  }

  // On-stack replacement
 private:
  bool is_osr() const {
    return flow()->is_osr_flow();
  }
  llvm::FunctionType* entry_point_type() const {
    if (is_osr())
      return YuhuType::osr_entry_point_type();
    else
      return YuhuType::entry_point_type();
  }

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

 private:
  void do_deferred_zero_checks();
};

#endif // SHARE_VM_YUHU_YUHUFUNCTION_HPP

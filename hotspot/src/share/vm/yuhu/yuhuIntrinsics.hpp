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

#ifndef SHARE_VM_YUHU_YUHUINTRINSICS_HPP
#define SHARE_VM_YUHU_YUHUINTRINSICS_HPP

#include "ci/ciMethod.hpp"
#include "memory/allocation.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/yuhuState.hpp"

class YuhuIntrinsics : public YuhuTargetInvariants {
 public:
  static bool is_intrinsic(ciMethod* target);
  static void inline_intrinsic(ciMethod* target, YuhuState* state);

 private:
  YuhuIntrinsics(YuhuState* state, ciMethod* target)
    : YuhuTargetInvariants(state, target), _state(state) {}

 private:
  YuhuState* _state;

 private:
  YuhuState* state() const {
    return _state;
  }

 private:
  void do_intrinsic();

 private:
  void do_Math_minmax(llvm::ICmpInst::Predicate p);
  void do_Math_1to1(llvm::Value* function);
  void do_Math_2to1(llvm::Value* function);
  void do_Object_getClass();
  void do_System_currentTimeMillis();
  void do_Thread_currentThread();
  void do_Unsafe_compareAndSwapInt();
};

#endif // SHARE_VM_YUHU_YUHUINTRINSICS_HPP

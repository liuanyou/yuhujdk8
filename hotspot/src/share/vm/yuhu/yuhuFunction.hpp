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

class YuhuTopLevelBlock;
class DeferredZeroCheck;

class YuhuFunction : public YuhuTargetInvariants {
 friend class YuhuStackWithNormalFrame;

 public:
  static llvm::Function* build(ciEnv*        env,
                               YuhuBuilder* builder,
                               ciTypeFlow*   flow,
                               const char*   name,
                               YuhuDebugInformationRecorder** debug_info_recorder = NULL) {
    YuhuFunction function(env, builder, flow, name);
    // Process any deferred OopMaps before function goes out of scope
    function.process_deferred_oopmaps();
    // Optionally return the debug information recorder
    if (debug_info_recorder != NULL) {
      *debug_info_recorder = function.debug_info_recorder();
    }
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
  llvm::Value*                     _arg_base;
  llvm::Value*                     _arg_count;
  llvm::BasicBlock*                _unified_exit_block;  // Unified exit block for all returns

  // Prologue analysis data
  int _fp_offset_from_sp;  // Offset from SP to FP (imm in "add x29, sp, #imm")

  // YuhuDebugInformationRecorder to collect virtual OopMap information
  YuhuDebugInformationRecorder*    _debug_info_recorder;
  // Collection for deferred OopMaps to implement delayed safepoint addition
  GrowableArray<OopMap*>*           _deferred_oopmaps;
  GrowableArray<int>*               _deferred_offsets;

  // Per-function deoptimization stub
  address                           _deoptimization_stub;
  
  // Collections for deferred frame information (for decaching)
  GrowableArray<int>*               _deferred_frame_offsets;
  GrowableArray<ciMethod*>*         _deferred_frame_targets;
  GrowableArray<int>*               _deferred_frame_bcis;
  GrowableArray<GrowableArray<ScopeValue*>*>* _deferred_frame_locals;
  GrowableArray<GrowableArray<ScopeValue*>*>* _deferred_frame_expressions;
  GrowableArray<GrowableArray<MonitorValue*>*>* _deferred_frame_monitors;

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
  llvm::Value* arg_base() const { return _arg_base; }
  llvm::Value* arg_count() const { return _arg_count; }

  // Prologue analysis: get/set FP offset from SP
  int fp_offset_from_sp() const { return _fp_offset_from_sp; }
  void set_fp_offset_from_sp(int offset) { _fp_offset_from_sp = offset; }
  
  // Methods for deferred OopMap handling
  void add_deferred_oopmap(int pc_offset, OopMap* oopmap) {
    if (_deferred_oopmaps == NULL) {
      _deferred_oopmaps = new GrowableArray<OopMap*>();
      _deferred_offsets = new GrowableArray<int>();
    }
    _deferred_oopmaps->append(oopmap);
    _deferred_offsets->append(pc_offset);
  }
  
  GrowableArray<OopMap*>* deferred_oopmaps() const { return _deferred_oopmaps; }
  GrowableArray<int>* deferred_offsets() const { return _deferred_offsets; }
  
  // Methods for deferred frame handling
  void add_deferred_frame(int pc_offset, ciMethod* target, int bci,
                        GrowableArray<ScopeValue*>* locals,
                        GrowableArray<ScopeValue*>* expressions,
                        GrowableArray<MonitorValue*>* monitors) {
    if (_deferred_frame_offsets == NULL) {
      _deferred_frame_offsets = new GrowableArray<int>();
      _deferred_frame_targets = new GrowableArray<ciMethod*>();
      _deferred_frame_bcis = new GrowableArray<int>();
      _deferred_frame_locals = new GrowableArray<GrowableArray<ScopeValue*>*>();
      _deferred_frame_expressions = new GrowableArray<GrowableArray<ScopeValue*>*>();
      _deferred_frame_monitors = new GrowableArray<GrowableArray<MonitorValue*>*>();
    }
    _deferred_frame_offsets->append(pc_offset);
    _deferred_frame_targets->append(target);
    _deferred_frame_bcis->append(bci);
    _deferred_frame_locals->append(locals);
    _deferred_frame_expressions->append(expressions);
    _deferred_frame_monitors->append(monitors);
  }
  
  GrowableArray<int>* deferred_frame_offsets() const { return _deferred_frame_offsets; }
  GrowableArray<ciMethod*>* deferred_frame_targets() const { return _deferred_frame_targets; }
  GrowableArray<int>* deferred_frame_bcis() const { return _deferred_frame_bcis; }
  GrowableArray<GrowableArray<ScopeValue*>*>* deferred_frame_locals() const { return _deferred_frame_locals; }
  GrowableArray<GrowableArray<ScopeValue*>*>* deferred_frame_expressions() const { return _deferred_frame_expressions; }
  GrowableArray<GrowableArray<MonitorValue*>*>* deferred_frame_monitors() const { return _deferred_frame_monitors; }

  // Access to the debug information recorder
  YuhuDebugInformationRecorder* debug_info_recorder() const { return _debug_info_recorder; }

  // Per-function deoptimization stub support
  address deoptimization_stub() const { return _deoptimization_stub; }
  void generate_deoptimization_stub();

  // Process deferred OopMaps before destruction
  void process_deferred_oopmaps();
  
  // On-stack replacement
 private:
  bool is_osr() const {
    return flow()->is_osr_flow();
  }
  // Generate function signature dynamically based on Java method parameters
  // For normal entry: (Java method parameters...) -> int
  // For static methods: (void* null, Java method parameters...) -> int
  // For OSR entry: keep old signature for now
  llvm::FunctionType* entry_point_type() const {
    if (is_osr()) {
      // OSR entry: keep old signature for now (will be handled in phase 6)
      return YuhuType::osr_entry_point_type();
    } else {
      // Normal entry: generate signature based on Java method parameters
      return generate_normal_entry_point_type();
    }
  }
  
 private:
  // Generate function signature for normal entry based on Java method parameters
  llvm::FunctionType* generate_normal_entry_point_type() const;

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

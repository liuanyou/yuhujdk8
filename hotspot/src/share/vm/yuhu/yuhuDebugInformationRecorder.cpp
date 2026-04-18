/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "yuhu/yuhuDebugInformationRecorder.hpp"
#include "runtime/os.hpp"
#include "yuhu/llvmHeaders.hpp"

// Initialize static TLS index
int YuhuDebugInformationRecorder::_tls_index = -1;

// Constructor
YuhuDebugInformationRecorder::YuhuDebugInformationRecorder()
  : _module(NULL) {
  _virtual_offsets = new GrowableArray<int>();
  _oopmaps = new GrowableArray<OopMap*>();
  
  _virtual_frame_offsets = new GrowableArray<int>();
  _frame_targets = new GrowableArray<ciMethod*>();
  _frame_bcis = new GrowableArray<int>();
  _frame_locals = new GrowableArray<GrowableArray<ScopeValue*>*>();
  _frame_expressions = new GrowableArray<GrowableArray<ScopeValue*>*>();
  _frame_monitors = new GrowableArray<GrowableArray<MonitorValue*>*>();
  
  _call_site_virtual_offsets = new GrowableArray<int>();
  _call_site_virtual_addresses = new GrowableArray<uint64_t>();
  _call_site_helper_addresses = new GrowableArray<uint64_t>();
}

// Destructor
YuhuDebugInformationRecorder::~YuhuDebugInformationRecorder() {
  // GrowableArrays will be cleaned up by ResourceArea or C heap
  // depending on how this object was allocated
}

// Initialize thread-local storage index
void YuhuDebugInformationRecorder::initialize_tls() {
  if (_tls_index == -1) {
    _tls_index = os::allocate_thread_local_storage();
  }
}

// Get thread-local instance (creates if not exists)
YuhuDebugInformationRecorder* YuhuDebugInformationRecorder::get() {
  YuhuDebugInformationRecorder* recorder = 
    (YuhuDebugInformationRecorder*) os::thread_local_storage_at(_tls_index);
  
  if (recorder == NULL) {
    // Allocate on C heap to survive ResourceMark scope changes
    recorder = new (ResourceObj::C_HEAP, mtCompiler) YuhuDebugInformationRecorder();
    os::thread_local_storage_at_put(_tls_index, recorder);
  }
  
  return recorder;
}

// Release thread-local instance
void YuhuDebugInformationRecorder::release() {
  YuhuDebugInformationRecorder* recorder = 
    (YuhuDebugInformationRecorder*) os::thread_local_storage_at(_tls_index);
  
  if (recorder != NULL) {
    delete recorder;
    os::thread_local_storage_at_put(_tls_index, NULL);
  }
}

// Add safepoint information
void YuhuDebugInformationRecorder::add_safepoint(int virtual_pc_offset, OopMap* oopmap) {
  _virtual_offsets->append(virtual_pc_offset);
  _oopmaps->append(oopmap);
}

// Describe scope information
void YuhuDebugInformationRecorder::describe_scope(int virtual_pc_offset,
                                                  ciMethod* method,
                                                  int bci,
                                                  bool reexecute,
                                                  bool rethrow_exception,
                                                  bool is_method_handle_invoke,
                                                  GrowableArray<ScopeValue*>* locals,
                                                  GrowableArray<ScopeValue*>* expressions,
                                                  GrowableArray<MonitorValue*>* monitors) {
  _virtual_frame_offsets->append(virtual_pc_offset);
  _frame_targets->append(method);
  _frame_bcis->append(bci);
  _frame_locals->append(locals);
  _frame_expressions->append(expressions);
  _frame_monitors->append(monitors);
}

// End safepoint recording
void YuhuDebugInformationRecorder::end_safepoint(int virtual_pc_offset) {
  // No action needed - all information has been recorded
}

// Register call site for JITLink correlation
void YuhuDebugInformationRecorder::register_call_site(int virtual_offset, 
                                                       uint64_t virtual_address, 
                                                       uint64_t helper_address) {
  _call_site_virtual_offsets->append(virtual_offset);
  _call_site_virtual_addresses->append(virtual_address);
  _call_site_helper_addresses->append(helper_address);
}

// Embed call site mappings as LLVM named metadata
void YuhuDebugInformationRecorder::embed_call_site_metadata() {
  if (get_call_site_count() == 0) {
    return;  // No call sites to embed
  }
  
  if (_module == NULL) {
    return;  // Module not set
  }
  
  llvm::LLVMContext& ctx = _module->getContext();
  
  // Create metadata for each call site: {virtual_offset, virtual_address, helper_address}
  std::vector<llvm::MDNode*> call_site_mds;
  
  for (int i = 0; i < get_call_site_count(); i++) {
    int virtual_offset = get_call_site_virtual_offset(i);
    uint64_t virtual_address = get_call_site_virtual_address(i);
    uint64_t helper_address = get_call_site_helper_address(i);
    
    // Create a tuple: !{i32 virtual_offset, i64 virtual_address, i64 helper_address}
    llvm::Metadata* operands[] = {
      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), virtual_offset)),
      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), virtual_address)),
      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), helper_address))
    };
    
    call_site_mds.push_back(llvm::MDNode::get(ctx, operands));
  }
  
  // Create named metadata node: !yuhu.call_sites = {!{i32, i64, i64}, ...}
  llvm::NamedMDNode* named_md = _module->getOrInsertNamedMetadata("yuhu.call_sites");
  
  for (auto* md : call_site_mds) {
    named_md->addOperand(md);
  }
  
  if (YuhuTraceIRCompilation) {
    llvm::errs() << "[YuhuDebugInformationRecorder] Embedded " 
                 << get_call_site_count() << " call site mappings\n";
  }
}

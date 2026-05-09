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
#include "runtime/os.hpp"

#pragma push_macro("assert")
#ifdef assert
#undef assert
#endif

#include "yuhu/llvmHeaders.hpp"
#include "llvm/Object/StackMapParser.h"

#pragma pop_macro("assert")

#include "yuhu/yuhuDebugInformationRecorder.hpp"

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

  _call_site_entries = new GrowableArray<CallSiteEntry*>();

  _stack_map_instruction_offsets = new GrowableArray<uint32_t>();
  _stack_map_location_kinds = new GrowableArray<GrowableArray<uint8_t>*>();
  _stack_map_location_reg_nums = new GrowableArray<GrowableArray<uint32_t>*>();
  _stack_map_location_offsets = new GrowableArray<GrowableArray<int32_t>*>();
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
    CallSiteEntry* call_site_entry = new CallSiteEntry();
    call_site_entry->virtual_offset = virtual_offset;
    call_site_entry->virtual_address = virtual_address;
    call_site_entry->helper_address = helper_address;
    call_site_entry->return_pc_offset = 0;
    _call_site_entries->append(call_site_entry);
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

void YuhuDebugInformationRecorder::register_stack_map(uint32_t instruction_offset,
                                                      uint8_t location_kind,
                                                      uint32_t location_reg_num,
                                                      int32_t location_offset) {
    if (!_stack_map_instruction_offsets->contains(instruction_offset)) {
        _stack_map_instruction_offsets->append(instruction_offset);
        _stack_map_location_kinds->append(new GrowableArray<uint8_t>());
        _stack_map_location_reg_nums->append(new GrowableArray<uint32_t>());
        _stack_map_location_offsets->append(new GrowableArray<int32_t>());
    }
    int index = _stack_map_instruction_offsets->find(instruction_offset);
    _stack_map_location_kinds->at(index)->append(location_kind);
    _stack_map_location_reg_nums->at(index)->append(location_reg_num);
    _stack_map_location_offsets->at(index)->append(location_offset);
}

void YuhuDebugInformationRecorder::convert_and_add_to_real_recorder(DebugInformationRecorder* real_recorder,
                                      ciMethod* method,
                                      int plus_offset,
                                      int frame_size) {
    using StackMapParser = llvm::StackMapParser<llvm::endianness::little>;
    GrowableArray<uint32_t> processed_instruction_offsets;

    // sort by return_pc_offset, oopmap should be registered in ascending order
    _call_site_entries->sort([](CallSiteEntry** a, CallSiteEntry** b) -> int {
        if ((*a)->return_pc_offset < (*b)->return_pc_offset) return -1;
        if ((*a)->return_pc_offset > (*b)->return_pc_offset) return  1;
        return 0;
    });;

    for (int i = 0; i < _call_site_entries->length(); ++i) {
        CallSiteEntry* call_site_entry = _call_site_entries->at(i);
        uint64_t return_pc_offset = call_site_entry->return_pc_offset;

        if (processed_instruction_offsets.contains(return_pc_offset)) {
            continue;
        }

        // since safepoint poll call is after stack frame is setup and arguments are copied,
        // the 9th, 10th arguments before current stack frame need no GC support
        int arg_count = 0;
        auto *oopmap = new OopMap(YuhuStack::oopmap_slot_munge(frame_size),
                                  YuhuStack::oopmap_slot_munge(arg_count));

        assert(_stack_map_instruction_offsets->contains(return_pc_offset), "Call site should contain stack map");

        if (YuhuTraceOffset) {
            tty->print_cr("Yuhu: Found stack map site by return pc offset=%d", return_pc_offset);
        }

        // add plus_offset to get offset in code cache
        int pc_offset = return_pc_offset + plus_offset;
        uint64_t helper_address = call_site_entry->helper_address;
        if (helper_address == (uint64_t)&gc_safepoint_poll) {
            if (YuhuTraceOffset) {
                tty->print_cr("Yuhu: Call site is safepoint poll call");
            }
            pc_offset -= 8; // pc_offset should be ldr instruction, not adrp instruction
        }

        GrowableArray<int32_t> processed_stack_offsets;
        GrowableArray<uint32_t> processed_register_nums;

        int smio_index = _stack_map_instruction_offsets->find(return_pc_offset);

        for (int j = 0; j < _stack_map_location_kinds->at(smio_index)->length(); ++j) {
            uint8_t kind = _stack_map_location_kinds->at(smio_index)->at(j);
            if (kind == static_cast<uint8_t>(StackMapParser::LocationKind::Direct)) {
                uint32_t reg_num = _stack_map_location_reg_nums->at(smio_index)->at(j);
                // Usually it should be sp register, sometimes it uses fp register,
                // but don't know when, assume it is always sp register
                assert(reg_num == 31, "Should be sp register");
                // offset in bytes
                int32_t offset_in_bytes = _stack_map_location_offsets->at(smio_index)->at(j);
                if (processed_stack_offsets.contains(offset_in_bytes)) {
                    continue;
                }
                processed_stack_offsets.append(offset_in_bytes);
                oopmap->set_oop(YuhuStack::slot2reg(offset_in_bytes >> LogBytesPerWord));
            } else if (kind == static_cast<uint8_t>(StackMapParser::LocationKind::Register)) {
                uint32_t reg_num = _stack_map_location_reg_nums->at(smio_index)->at(j);
                if (processed_register_nums.contains(reg_num)) {
                    continue;
                }
                processed_register_nums.append(reg_num);
                oopmap->set_oop(VMRegImpl::as_VMReg(reg_num << 1));
            } else if (kind == static_cast<uint8_t>(StackMapParser::LocationKind::Indirect)) {
                uint32_t reg_num = _stack_map_location_reg_nums->at(smio_index)->at(j);
                assert(reg_num == 31, "Should be sp register");
                // offset in bytes
                int32_t offset_in_bytes = _stack_map_location_offsets->at(smio_index)->at(j);
                if (processed_stack_offsets.contains(offset_in_bytes)) {
                    continue;
                }
                processed_stack_offsets.append(offset_in_bytes);
                oopmap->set_oop(YuhuStack::slot2reg(offset_in_bytes >> LogBytesPerWord));
            }
        }
        // call sites need an oopmap even there is no live oop
        real_recorder->add_safepoint(pc_offset, oopmap);
        real_recorder->end_safepoint(pc_offset);
        // record processed instruction offset
        processed_instruction_offsets.append(return_pc_offset);
    }

    for (int i = 0; i < _stack_map_instruction_offsets->length(); ++i) {
        if (processed_instruction_offsets.contains(_stack_map_instruction_offsets->at(i))) {
            continue;
        }

        // If instruction offset is not processed, either it is not call site or it has no live oops
        for (int j = 0; j < _stack_map_location_kinds->at(i)->length(); ++j) {
            uint8_t kind = _stack_map_location_kinds->at(i)->at(j);
            assert(kind != static_cast<uint8_t>(StackMapParser::LocationKind::Direct)
                   && kind != static_cast<uint8_t>(StackMapParser::LocationKind::Register)
                   && kind != static_cast<uint8_t>(StackMapParser::LocationKind::Indirect), "Should contain no live oops");
        }
    }
}
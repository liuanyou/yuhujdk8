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
  _call_site_entries = new GrowableArray<CallSiteEntry*>();

  _stack_map_entries = new GrowableArray<StackMapEntry*>();

  _deopt_bundles = new GrowableArray<DeoptBundle*>();

  _frame_layout_info = new FrameLayoutInfo();

  _exception_table_info_records = new GrowableArray<ExceptionTableInfoRecord*>();

  _handler_block_info_records = new GrowableArray<HandlerBlockInfoRecord*>();

  _func_size = 0;
  _unified_exit_block_start_pco = 0;
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

// Register call site for JITLink correlation
void YuhuDebugInformationRecorder::register_call_site(uint64_t virtual_offset,
                                                       uint64_t virtual_address, 
                                                       uint64_t helper_address,
                                                       CallSiteType call_site_type,
                                                       int bci,
                                                       int num_monitors) {
    int index = _call_site_entries->find(&virtual_offset, [](void* token, CallSiteEntry* entry) -> bool {
        return *((uint64_t*)token) == entry->virtual_offset;
    });
    // skip registration if already exists
    if (index != -1) {
        return;
    }
    auto call_site_entry = new CallSiteEntry();
    call_site_entry->virtual_offset = virtual_offset;
    call_site_entry->virtual_address = virtual_address;
    call_site_entry->helper_address = helper_address;
    call_site_entry->call_site_type = call_site_type;
    call_site_entry->bci = bci;
    call_site_entry->num_monitors = num_monitors;
    call_site_entry->machine_code_offsets = new GrowableArray<CallSiteMachineCodeOffsets*>();
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
                                                      int32_t location_offset,
                                                      uint64_t constant) {
    int index = _stack_map_entries->find(&instruction_offset, [](void* token, StackMapEntry* entry) -> bool {
        return *((uint32_t*)token) == entry->instruction_offset;
    });
    if (index == -1) {
        auto stack_map_entry = new StackMapEntry();
        stack_map_entry->instruction_offset = instruction_offset;
        stack_map_entry->locations = new GrowableArray<StackMapLocation*>();
        _stack_map_entries->append(stack_map_entry);
        index = _stack_map_entries->length() - 1;
    }

    StackMapEntry* entry = _stack_map_entries->at(index);
    auto location = new StackMapLocation();
    location->kind = location_kind;
    location->reg_num = location_reg_num;
    location->offset = location_offset;
    location->constant = constant;
    entry->locations->append(location);
}

void YuhuDebugInformationRecorder::register_deopt_bundle(uint32_t instruction_offset, uint64_t bci) {
    int index = _deopt_bundles->find(&instruction_offset, [](void* token, DeoptBundle* bundle) -> bool {
        return *((uint32_t*)token) == bundle->instruction_offset;
    });
    if (index != -1) {
        assert(_deopt_bundles->at(index)->bci == 0 || _deopt_bundles->at(index)->bci == bci, "either bci is not initialized or bci matches");
        // update bci
        _deopt_bundles->at(index)->bci = bci;
    } else {
        auto deopt_bundle = new DeoptBundle();
        deopt_bundle->instruction_offset = instruction_offset;
        deopt_bundle->bci = bci;
        deopt_bundle->locals = new GrowableArray<uint8_t>();
        deopt_bundle->expression_stacks = new GrowableArray<uint8_t>();
        _deopt_bundles->append(deopt_bundle);
    }
}

void YuhuDebugInformationRecorder::register_deopt_bundle_local_data(uint32_t instruction_offset, uint8_t basic_type) {
    int index = _deopt_bundles->find(&instruction_offset, [](void* token, DeoptBundle* bundle) -> bool {
        return *((uint32_t*)token) == bundle->instruction_offset;
    });
    if (index == -1) {
        auto deopt_bundle = new DeoptBundle();
        deopt_bundle->instruction_offset = instruction_offset;
        deopt_bundle->bci = 0;
        deopt_bundle->locals = new GrowableArray<uint8_t>();
        deopt_bundle->expression_stacks = new GrowableArray<uint8_t>();
        _deopt_bundles->append(deopt_bundle);
        index = _deopt_bundles->length() - 1;
    }

    DeoptBundle* bundle = _deopt_bundles->at(index);
    bundle->locals->append(basic_type);
}

void YuhuDebugInformationRecorder::register_deopt_bundle_expression_stack_data(uint32_t instruction_offset, uint8_t basic_type) {
    int index = _deopt_bundles->find(&instruction_offset, [](void* token, DeoptBundle* bundle) -> bool {
        return *((uint32_t*)token) == bundle->instruction_offset;
    });
    if (index == -1) {
        auto deopt_bundle = new DeoptBundle();
        deopt_bundle->instruction_offset = instruction_offset;
        deopt_bundle->bci = 0;
        deopt_bundle->locals = new GrowableArray<uint8_t>();
        deopt_bundle->expression_stacks = new GrowableArray<uint8_t>();
        _deopt_bundles->append(deopt_bundle);
        index = _deopt_bundles->length() - 1;
    }

    DeoptBundle* bundle = _deopt_bundles->at(index);
    bundle->expression_stacks->append(basic_type);
}

void YuhuDebugInformationRecorder::register_deopt_bundle_monitor_data(uint32_t instruction_offset, uint32_t num_monitors) {
    int index = _deopt_bundles->find(&instruction_offset, [](void* token, DeoptBundle* bundle) -> bool {
        return *((uint32_t*)token) == bundle->instruction_offset;
    });
    if (index == -1) {
        auto deopt_bundle = new DeoptBundle();
        deopt_bundle->instruction_offset = instruction_offset;
        deopt_bundle->bci = 0;
        deopt_bundle->locals = new GrowableArray<uint8_t>();
        deopt_bundle->expression_stacks = new GrowableArray<uint8_t>();
        _deopt_bundles->append(deopt_bundle);
        index = _deopt_bundles->length() - 1;
    }

    DeoptBundle* bundle = _deopt_bundles->at(index);
    bundle->num_monitors = num_monitors;
}

void YuhuDebugInformationRecorder::register_frame_layout_info_with_frame_fields(int header_words, int monitor_words, int stack_words, int locals_words, int extended_frame_words) {
    _frame_layout_info->header_words = header_words;
    _frame_layout_info->monitor_words = monitor_words;
    _frame_layout_info->stack_words = stack_words;
    _frame_layout_info->locals_words = locals_words;
    _frame_layout_info->extended_frame_words = extended_frame_words;
}

void YuhuDebugInformationRecorder::register_frame_layout_info_with_prologue_fields(int total_frame_size_in_bytes, int num_of_prologue_registers) {
    _frame_layout_info->total_frame_size_in_bytes = total_frame_size_in_bytes;
    _frame_layout_info->num_of_prologue_registers = num_of_prologue_registers;
}

void YuhuDebugInformationRecorder::register_frame_layout_info_with_stack_map_fields(int extended_frame_reg_num, int extended_frame_kind, int extended_frame_offset) {
    _frame_layout_info->extended_frame_reg_num = extended_frame_reg_num;
    _frame_layout_info->extended_frame_kind = extended_frame_kind;
    _frame_layout_info->extended_frame_offset = extended_frame_offset;
}

void YuhuDebugInformationRecorder::register_exception_handler_info(int start_bci, int limit_bci, int handler_bci, bool is_catch_all) {
    int index = -1;
    for (int i = 0; i < _exception_table_info_records->length(); ++i) {
        ExceptionTableInfoRecord* rec = _exception_table_info_records->at(i);
        if (rec->start_bci == start_bci) {
            continue;
        }
        if (rec->limit_bci != limit_bci) {
            continue;
        }
        if (rec->handler_bci != handler_bci) {
            continue;
        }
        if (rec->is_catch_all != is_catch_all) {
            continue;
        }
        index = i;
        break;
    }
    if (index != -1) {
        return;
    }
    auto exception_table_info_record = new ExceptionTableInfoRecord();
    exception_table_info_record->start_bci = start_bci;
    exception_table_info_record->limit_bci = limit_bci;
    exception_table_info_record->handler_bci = handler_bci;
    exception_table_info_record->is_catch_all = is_catch_all;
    _exception_table_info_records->append(exception_table_info_record);
}

void YuhuDebugInformationRecorder::register_handler_block_info(uint32_t instruction_offset, uint32_t start_bci, uint32_t limit_bci, uint32_t num_exceptions, uint32_t num_successors) {
    int index = _handler_block_info_records->find(&instruction_offset, [](void* token, HandlerBlockInfoRecord* entry) -> bool {
        return *((uint32_t*)token) == entry->instruction_offset;
    });
    if (index != -1) {
        return;
    }
    auto handler_block_info_record = new HandlerBlockInfoRecord();
    handler_block_info_record->instruction_offset = instruction_offset;
    handler_block_info_record->start_bci = start_bci;
    handler_block_info_record->limit_bci = limit_bci;
    handler_block_info_record->num_exceptions = num_exceptions;
    handler_block_info_record->num_successors = num_successors;
    _handler_block_info_records->append(handler_block_info_record);
}

void YuhuDebugInformationRecorder::generate_safepoint_and_describe_scope(DebugInformationRecorder* real_recorder,
                                                                         ciMethod* method,
                                                                         int plus_offset,
                                                                         int frame_size) {
    using StackMapParser = llvm::StackMapParser<llvm::endianness::little>;
    GrowableArray<uint32_t> processed_instruction_offsets;
    GrowableArray<CallSiteEntryOffsetsPair> entry_offsets_flatten_list;
    for (int i = 0; i < _call_site_entries->length(); ++i) {
        assert(_call_site_entries->at(i)->machine_code_offsets->length() > 0, "offsets should not be empty");
        for (int j = 0; j < _call_site_entries->at(i)->machine_code_offsets->length(); ++j) {
            entry_offsets_flatten_list.append(CallSiteEntryOffsetsPair(_call_site_entries->at(i), _call_site_entries->at(i)->machine_code_offsets->at(j)));
        }
    }

    // sort by return_pc_offset, oopmap should be registered in ascending order
    entry_offsets_flatten_list.sort([](CallSiteEntryOffsetsPair* a, CallSiteEntryOffsetsPair* b) -> int {
        if ((*a).offsets->return_pc_offset < (*b).offsets->return_pc_offset) return -1;
        if ((*a).offsets->return_pc_offset > (*b).offsets->return_pc_offset) return 1;
        return 0;
    });

    // RS4GC doesn't include monitor objects in stack map, need to handle it manually
    // a normal frame layout should be like:
    /*
        0x16f7ea3f0: 0x0000000000000003 0x00000000d8001c5a - spill area
        0x16f7ea400: 0x00000006c000e280 0x0000000100000001 - spill area
        0x16f7ea410: 0x0000000102b50b40 0x000000012680c800 - spill area
        0x16f7ea420: 0x000000076ad73c30 0x000000076ad73c20 - spill area
        0x16f7ea430: 0x000000076ad744d8 0x000000076ad73c40 - spill area
        0x16f7ea440: 0x00000006c000e280 0x000000076ad73c30 - spill area
        0x16f7ea450: 0x000000076ad73c20 0x0000000000000001 - spill area
        0x16f7ea460: 0x00000006c000e308 0x0000000000000031 - expression stack (extra for method handle) / expression stack [0] (top)
        0x16f7ea470: 0x00000001bd5b7dde 0x000000076ad744d8 - expression stack [1] / expression stack [2] (physically 1 has T_LONG and 2 has T_LONG2, virtually it is opposite)
        0x16f7ea480: 0x00000006c000f660 0x00000006c000e2d0 - expression stack [3] / expression stack [4] (bottom)
        0x16f7ea490: 0x0000000000000005 0x000000076ad73c30 - monitor [1] header / monitor [1] object (newest at lower address with large index)
        0x16f7ea4a0: 0x0000000000000005 0x000000076ad73c20 - monitor [0] header / monitor [0] object
        0x16f7ea4b0: 0x0000000000000000 0x0000000102b50b40 - oop tmp / method slot
        0x16f7ea4c0: 0x000000016f7ea3f0 0x00000000dead00c0 - final sp / return slot
        0x16f7ea4d0: 0x00000000deadbeef 0x000000016f7ea5a0 - frame marker / fp
        0x16f7ea4e0: 0x0000000000000000 0x00000000dead00c0 - local [18] / local [17]
        0x16f7ea4f0: 0x000000016f7ea5c0 0x000000076ad744d8 - local [16] / local [15]
        0x16f7ea500: 0x0000000000016006 0x000000076ad73c30 - local [14] / local [13]
        0x16f7ea510: 0x00000001bd5b7dde 0x0000000130804f6c - local [12] / local [11]
        0x16f7ea520: 0x000000060000012c 0x000000076ad73c20 - local [10] / local [9]
        0x16f7ea530: 0x000000076ad73c40 0x00000006c000e280 - local [8] / local [7]
        0x16f7ea540: 0x00000000deadbeef 0x0000000000000000 - local [6] / local [5] (physically 6 has T_LONG and 5 has T_LONG2, virtually it is opposite)
        0x16f7ea550: 0x00000001000000c8 0x0000000100000064 - local [4] / local [3]
        0x16f7ea560: 0x000000076ad73c30 0x000000076ad73c20 - local [2] / local [1]
        0x16f7ea570: 0x0000000100000001 0x000000076ad73c30 - local [0] / padding
        0x16f7ea580: 0x0000000000000000 0x000000010c345060 - x0 slot / padding (x0 saves 8th int-like argument)
        0x16f7ea590: 0x0000000000000003 0x00000000dead0048 - prologue x20 / x19
        0x16f7ea5a0: 0x000000016f7ea5c0 0x00000001308185ec - prologue x29 / x30
     */
    assert(_frame_layout_info->total_frame_size_in_bytes != -1 &&
           _frame_layout_info->num_of_prologue_registers != -1 &&
           _frame_layout_info->header_words != -1 &&
           _frame_layout_info->monitor_words != -1 &&
           _frame_layout_info->stack_words != -1 &&
           _frame_layout_info->locals_words != -1 &&
           _frame_layout_info->extended_frame_words != -1 &&
           _frame_layout_info->extended_frame_reg_num != -1 &&
           _frame_layout_info->extended_frame_kind != -1 &&
           _frame_layout_info->extended_frame_offset != -1, "frame layout data is not initialized");
    // Usually it should be fp register, sometimes it uses sp register,
    // but don't know when, assume it is always fp register
    assert(_frame_layout_info->extended_frame_reg_num == 29 &&
           _frame_layout_info->extended_frame_offset < 0 &&
           _frame_layout_info->extended_frame_offset % 8 == 0, "Should be valid fp offset");

    // 2 words is for x29,x30 in prologue
    int spill_words = _frame_layout_info->total_frame_size_in_bytes / wordSize - 2
                      - (-_frame_layout_info->extended_frame_offset / wordSize);

    assert(spill_words >= 0, "spill_words has invalid value");

    int max_monitors = _frame_layout_info->monitor_words / 2;

    for (int i = 0; i < entry_offsets_flatten_list.length(); ++i) {
        CallSiteEntry* call_site_entry = entry_offsets_flatten_list.at(i).entry;
        CallSiteMachineCodeOffsets* machine_code_offsets = entry_offsets_flatten_list.at(i).offsets;

        uint64_t return_pc_offset = machine_code_offsets->return_pc_offset;

        // multiple call targets may use same blr, so skip processed return pc offset
        if (processed_instruction_offsets.contains(return_pc_offset)) {
            continue;
        }

        // since safepoint poll call is after stack frame is setup and arguments are copied,
        // the 9th, 10th arguments before current stack frame need no GC support
        int arg_count = 0;
        auto *oopmap = new OopMap(YuhuStack::oopmap_slot_munge(frame_size),
                                  YuhuStack::oopmap_slot_munge(arg_count));

        if (call_site_entry->call_site_type != CallSiteType::deopt_call && call_site_entry->call_site_type != CallSiteType::unwind_call) {
            assert(contains_stack_map_instruction_offset(return_pc_offset), "Call site should contain stack map");

            if (YuhuTraceOffset) {
                tty->print_cr("Yuhu: Found stack map site by return pc offset=%d", return_pc_offset);
            }

            // add plus_offset to get offset in code cache
            int pc_offset = return_pc_offset + plus_offset;
            uint64_t helper_address = call_site_entry->helper_address;
            if (helper_address == (uint64_t) &gc_safepoint_poll) {
                if (YuhuTraceOffset) {
                    tty->print_cr("Yuhu: Call site is safepoint poll call");
                }
                // safepoint poll makes no differences here, coz it is also a runtime call
//                pc_offset = machine_code_offsets->blr_offset + plus_offset;
            }

            GrowableArray<int32_t> processed_stack_offsets;
            GrowableArray<uint32_t> processed_register_nums;

            StackMapEntry *stack_map_entry = get_stack_map_by_instruction_offset(return_pc_offset);

            for (int j = 0; j < stack_map_entry->locations->length(); ++j) {
                uint8_t kind = stack_map_entry->locations->at(j)->kind;
                if (kind == static_cast<uint8_t>(StackMapParser::LocationKind::Direct)) {
                    uint32_t reg_num = stack_map_entry->locations->at(j)->reg_num;
                    // Usually it should be sp register, sometimes it uses fp register,
                    // but don't know when, assume it is always sp register
                    assert(reg_num == 31, "Should be sp register");
                    // offset in bytes
                    int32_t offset_in_bytes = stack_map_entry->locations->at(j)->offset;
                    if (processed_stack_offsets.contains(offset_in_bytes)) {
                        continue;
                    }
                    processed_stack_offsets.append(offset_in_bytes);
                    oopmap->set_oop(YuhuStack::slot2reg(offset_in_bytes >> LogBytesPerWord));
                } else if (kind == static_cast<uint8_t>(StackMapParser::LocationKind::Register)) {
                    uint32_t reg_num = stack_map_entry->locations->at(j)->reg_num;
                    if (processed_register_nums.contains(reg_num)) {
                        continue;
                    }
                    processed_register_nums.append(reg_num);
                    oopmap->set_oop(VMRegImpl::as_VMReg(reg_num << 1));
                } else if (kind == static_cast<uint8_t>(StackMapParser::LocationKind::Indirect)) {
                    uint32_t reg_num = stack_map_entry->locations->at(j)->reg_num;
                    assert(reg_num == 31, "Should be sp register");
                    // offset in bytes
                    int32_t offset_in_bytes = stack_map_entry->locations->at(j)->offset;
                    if (processed_stack_offsets.contains(offset_in_bytes)) {
                        continue;
                    }
                    processed_stack_offsets.append(offset_in_bytes);
                    oopmap->set_oop(YuhuStack::slot2reg(offset_in_bytes >> LogBytesPerWord));
                }
            }

            if (call_site_entry->num_monitors > 0) {
                // from oldest to newest
                for (int j = 0; j < call_site_entry->num_monitors; ++j) {
                    int monitor_object_offset_in_bytes =
                            (spill_words + _frame_layout_info->stack_words + (max_monitors - j - 1) * 2 + 1) * wordSize;
                    if (YuhuTraceOffset && YuhuStackMapFile != NULL) {
                        FILE *f = fopen(YuhuStackMapFile, "a");
                        fileStream fs(f, true);
                        fs.print_cr("[StackMap] monitor_object_offset_in_bytes: %d", monitor_object_offset_in_bytes);
                        fs.flush();
                    }
                    oopmap->set_oop(YuhuStack::slot2reg(monitor_object_offset_in_bytes >> LogBytesPerWord));
                }
            }

            // call sites need an oopmap even there is no live oop
            real_recorder->add_safepoint(pc_offset, oopmap);
            real_recorder->describe_scope(pc_offset, // PC offset in code (same as passed to add_safepoint)
                                          method, // the method being compiled (the caller)
                                          call_site_entry->bci, // the BCI of the invoke bytecode in the caller
                                          false, // Whether to re-execute the bytecode after deoptimization
                                          false, // Whether this is a MethodHandle invoke
                                          method->signature()->return_type()->is_object(), // Whether the return value is an oop
                                          NULL, // DebugToken* for local variables (can be NULL/empty)
                                          NULL, // DebugToken* for expression stack (can be NULL/empty)
                                          NULL); // DebugToken* for synchronized monitors (can be NULL/empty)
            real_recorder->end_safepoint(pc_offset);
        } else if (call_site_entry->call_site_type == CallSiteType::deopt_call) {
            // add plus_offset to get offset in code cache
            int pc_offset = return_pc_offset + plus_offset;

            DeoptBundle* bundle = get_deopt_bundle_by_instruction_offset(return_pc_offset);
            assert(bundle != NULL, "deopt bundle shouldn't be NULL");
            assert((int) bundle->bci == call_site_entry->bci, "bci should be the same");

            // Convert StackMapLocation arrays to ScopeValue arrays for DebugToken
            GrowableArray<ScopeValue*>* locals = NULL;
            GrowableArray<ScopeValue*>* expressions = NULL;
            GrowableArray<MonitorValue*>* monitors = NULL;

            // Convert locals
            if (bundle->locals && bundle->locals->length() > 0) {
                locals = new GrowableArray<ScopeValue*>();
                for (int j = 0; j < bundle->locals->length(); j++) {
                    uint8_t basic_type = bundle->locals->at(j);
                    int local_offset_in_bytes = (spill_words + _frame_layout_info->stack_words + _frame_layout_info->monitor_words +
                            _frame_layout_info->header_words + _frame_layout_info->locals_words - 1 - j) * wordSize;
                    
                    // Determine Location::Type based on BasicType
                    switch (basic_type) {
                        case T_OBJECT:
                        case T_ARRAY: {
                            ScopeValue *scopeValue = new LocationValue(Location::new_stk_loc(Location::oop, local_offset_in_bytes));
                            locals->append(scopeValue);
                        }
                            break;
                        case T_LONG: {
                            // in deopt bundle, first slot is T_LONG with actual value, second slot is T_LONG2 with padding
                            // but in physical stack frame, first slot has padding, second slot has actual value, and this is
                            // the desired layout for interpreter
                            // construct second slot
                            ScopeValue *scopeValue = new LocationValue(Location::new_stk_loc(Location::lng, local_offset_in_bytes - wordSize));
                            // construct first slot
                            assert(bundle->locals->at(++j) == ciTypeFlow::StateVector::T_LONG2, "should be T_LONG2 type");
                            Location invalid_location;

                            locals->append(new LocationValue(invalid_location));
                            locals->append(scopeValue);
                        }
                            break;
                        case T_DOUBLE: {
                            // in deopt bundle, first slot is T_DOUBLE with actual value, second slot is T_LONG2 with padding
                            // but in physical stack frame, first slot has padding, second slot has actual value, and this is
                            // the desired layout for interpreter
                            // construct second slot
                            ScopeValue *scopeValue = new LocationValue(Location::new_stk_loc(Location::dbl, local_offset_in_bytes - wordSize));
                            // construct first slot
                            assert(bundle->locals->at(++j) == ciTypeFlow::StateVector::T_DOUBLE2, "should be T_DOUBLE2 type");
                            Location invalid_location;

                            locals->append(new LocationValue(invalid_location));
                            locals->append(scopeValue);
                        }
                            break;
                        case ciTypeFlow::StateVector::T_BOTTOM: {
                            Location invalid_location; // use Location::invalid for default constructor
                            locals->append(new LocationValue(invalid_location));
                        }
                            break;
                        default: {
                            // T_INT, T_FLOAT, T_BYTE, T_SHORT, T_CHAR, T_BOOLEAN
                            ScopeValue *scopeValue = new LocationValue(Location::new_stk_loc(Location::normal, local_offset_in_bytes));
                            locals->append(scopeValue);
                        }
                            break;
                    }
                }
            }

            // Convert expression stacks
            if (bundle->expression_stacks && bundle->expression_stacks->length() > 0) {
                expressions = new GrowableArray<ScopeValue*>();
                for (int j = 0; j < bundle->expression_stacks->length(); j++) {
                    uint8_t basic_type = bundle->expression_stacks->at(j);
                    int express_stack_offset_in_bytes = (spill_words + _frame_layout_info->stack_words - bundle->expression_stacks->length() + j) * wordSize;

                    // Determine Location::Type based on BasicType
                    switch (basic_type) {
                        case T_OBJECT:
                        case T_ARRAY: {
                            ScopeValue *scopeValue = new LocationValue(Location::new_stk_loc(Location::oop, express_stack_offset_in_bytes));
                            expressions->append(scopeValue);
                        }
                            break;
                        case ciTypeFlow::StateVector::T_LONG2: {
                            // in deopt bundle, first slot is T_LONG2 with padding, second slot is T_LONG with actual value
                            // but in physical stack frame, first slot has actual value, second slot has padding, and this is
                            // not the desired layout for interpreter
                            // construct first slot
                            Location invalid_location;
                            // construct second slot
                            assert(bundle->expression_stacks->at(++j) == T_LONG, "should be T_LONG type");
                            ScopeValue *scopeValue = new LocationValue(Location::new_stk_loc(Location::lng, express_stack_offset_in_bytes - wordSize));

                            expressions->append(new LocationValue(invalid_location));
                            expressions->append(scopeValue);
                        }
                            break;
                        case ciTypeFlow::StateVector::T_DOUBLE2: {
                            // in deopt bundle, first slot is T_DOUBLE2 with padding, second slot is T_DOUBLE with actual value
                            // but in physical stack frame, first slot has actual value, second slot has padding, and this is
                            // not the desired layout for interpreter
                            // construct first slot
                            Location invalid_location;
                            // construct first slot
                            assert(bundle->expression_stacks->at(++j) == T_DOUBLE, "should be T_DOUBLE type");
                            ScopeValue *scopeValue = new LocationValue(Location::new_stk_loc(Location::dbl, express_stack_offset_in_bytes - wordSize));

                            expressions->append(new LocationValue(invalid_location));
                            expressions->append(scopeValue);
                        }
                            break;
                        case ciTypeFlow::StateVector::T_BOTTOM: {
                            Location invalid_location; // use Location::invalid for default constructor
                            expressions->append(new LocationValue(invalid_location));
                        }
                            break;
                        default: {
                            // T_INT, T_FLOAT, T_BYTE, T_SHORT, T_CHAR, T_BOOLEAN
                            ScopeValue *scopeValue = new LocationValue(Location::new_stk_loc(Location::normal, express_stack_offset_in_bytes));
                            expressions->append(scopeValue);
                        }
                            break;
                    }
                }
            }

            // Convert monitors
            if (bundle->num_monitors > 0) {
                monitors = new GrowableArray<MonitorValue*>();
                // from oldest to newest
                for (uint32_t j = 0; j < bundle->num_monitors; ++j) {
                    int monitor_object_offset_in_bytes =
                            (spill_words + _frame_layout_info->stack_words + (max_monitors - j - 1) * 2 + 1) * wordSize;

                    ScopeValue *scopeValue = new LocationValue(Location::new_stk_loc(Location::oop, monitor_object_offset_in_bytes));
                    Location basicLockLoc = Location::new_stk_loc(Location::normal, monitor_object_offset_in_bytes - wordSize);

                    monitors->append(new MonitorValue(scopeValue, basicLockLoc));
                }
            }

            real_recorder->add_safepoint(pc_offset, oopmap);
            // Create DebugTokens from the ScopeValue arrays
            DebugToken* locals_token = (locals != NULL) ? real_recorder->create_scope_values(locals) : NULL;
            DebugToken* expressions_token = (expressions != NULL) ? real_recorder->create_scope_values(expressions) : NULL;
            DebugToken* monitors_token = (monitors != NULL) ? real_recorder->create_monitor_values(monitors) : NULL;
            real_recorder->describe_scope(pc_offset, // PC offset in code (same as passed to add_safepoint)
                                          method, // the method being compiled (the caller)
                                          call_site_entry->bci, // the BCI of the invoke bytecode in the caller
                                          true, // Whether to re-execute the bytecode after deoptimization
                                          false, // Whether this is a MethodHandle invoke
                                          method->signature()->return_type()->is_object(), // Whether the return value is an oop
                                          locals_token, // DebugToken* for local variables
                                          expressions_token, // DebugToken* for expression stack
                                          monitors_token); // DebugToken* for synchronized monitors
            real_recorder->end_safepoint(pc_offset);
        }

        // record processed instruction offset
        processed_instruction_offsets.append(return_pc_offset);
    }

    // all stack maps containing live oops should be processed, unless they have no live oops
    for (int i = 0; i < _stack_map_entries->length(); ++i) {
        if (processed_instruction_offsets.contains(_stack_map_entries->at(i)->instruction_offset)) {
            continue;
        }

        // If instruction offset is not processed, either it is not call site or it has no live oops
        for (int j = 0; j < _stack_map_entries->at(i)->locations->length(); ++j) {
            uint8_t kind = _stack_map_entries->at(i)->locations->at(j)->kind;
            assert(kind != static_cast<uint8_t>(StackMapParser::LocationKind::Direct)
                   && kind != static_cast<uint8_t>(StackMapParser::LocationKind::Register)
                   && kind != static_cast<uint8_t>(StackMapParser::LocationKind::Indirect), "Should contain no live oops");
        }
    }
}

void YuhuDebugInformationRecorder::generate_exception_handler_table(ciMethod* method, ExceptionHandlerTable* exception_handler_table, int plus_offset) {
    if (!_exception_table_info_records->length())
        return;

    GrowableArray<std::pair<ExceptionTableInfoRecord*, HandlerBlockInfoRecord*>> exception_handler_pair_list;

    for (int i = 0; i < _exception_table_info_records->length(); ++i) {
        ExceptionTableInfoRecord* handler = _exception_table_info_records->at(i);
        assert(_handler_block_info_records->length() > 0, "should contain handler block info records");

        HandlerBlockInfoRecord* found_info_record = NULL;
        for (int j = 0; j < _handler_block_info_records->length(); ++j) {
            if (_handler_block_info_records->at(j)->start_bci == (uint32_t)handler->handler_bci) {
                assert(found_info_record == NULL, "should be only one matched info record");
                found_info_record = _handler_block_info_records->at(j);
            }
        }
        assert(found_info_record != NULL, "there should be matched info record");

        std::pair<ExceptionTableInfoRecord*, HandlerBlockInfoRecord*> exception_handler_pair(handler, found_info_record);
        exception_handler_pair_list.append(exception_handler_pair);
    }

    // ideally only java call and vm call throws exceptions
    // observe some vm call is to throw NullPointerException, bci may be within handler's from/to range or not.
    // for those not within, they are filtered by start/limit check, for those within, they are kept and have
    // exception table entry, which may be redundant but there is no harm.
    GrowableArray<CallSiteEntryOffsetsPair> entry_offsets_flatten_list;
    for (int i = 0; i < _call_site_entries->length(); ++i) {
        assert(_call_site_entries->at(i)->machine_code_offsets->length() > 0, "offsets should not be empty");
        if (_call_site_entries->at(i)->call_site_type != CallSiteType::vm_call &&
            _call_site_entries->at(i)->call_site_type != CallSiteType::java_call &&
            _call_site_entries->at(i)->bci != -1) { // filter entry with bci equals -1
            continue;
        }

        for (int j = 0; j < _call_site_entries->at(i)->machine_code_offsets->length(); ++j) {
            entry_offsets_flatten_list.append(CallSiteEntryOffsetsPair(_call_site_entries->at(i), _call_site_entries->at(i)->machine_code_offsets->at(j)));
        }
    }

    // sort by return_pc_offset and bci, return_pc_offset is also catch_pco
    entry_offsets_flatten_list.sort([](CallSiteEntryOffsetsPair* a, CallSiteEntryOffsetsPair* b) -> int {
        if ((*a).offsets->return_pc_offset < (*b).offsets->return_pc_offset) return -1;
        if ((*a).offsets->return_pc_offset > (*b).offsets->return_pc_offset) return 1;
        if ((*a).entry->bci < (*b).entry->bci) return -1;
        if ((*a).entry->bci > (*b).entry->bci) return 1;
        return 0;
    });
    // group entry_offsets_flatten_list group by return_pc_offset
    GrowableArray<std::pair<uint64_t, GrowableArray<CallSiteEntryOffsetsPair>>> entry_offsets_group_list;

    uint64_t last_return_pc_offset = 0; // return_pc_offset must never be 0
    for (int i = 0; i < entry_offsets_flatten_list.length(); ++i) {
        CallSiteEntryOffsetsPair pair = entry_offsets_flatten_list.at(i);

        if (last_return_pc_offset == pair.offsets->return_pc_offset) {
            entry_offsets_group_list.at(entry_offsets_group_list.length() - 1).second.append(pair);
        } else {
            GrowableArray<CallSiteEntryOffsetsPair> grouped_pairs;
            grouped_pairs.append(pair);
            std::pair<uint64_t, GrowableArray<CallSiteEntryOffsetsPair>> group(pair.offsets->return_pc_offset, grouped_pairs);
            entry_offsets_group_list.append(group);
            last_return_pc_offset = pair.offsets->return_pc_offset;
        }
    }

    // allocate some arrays for use by the collection code.
    const int num_handlers = 5;
    GrowableArray<intptr_t>* bcis = new GrowableArray<intptr_t>(num_handlers);
    GrowableArray<intptr_t>* scope_depths = NULL; // Yuhu doesn't support inline method
    GrowableArray<intptr_t>* pcos = new GrowableArray<intptr_t>(num_handlers);
    GrowableArray<bool>* is_catch_alls = new GrowableArray<bool>(num_handlers);

    for (int i = 0; i < entry_offsets_group_list.length(); ++i) {
        uint64_t catch_pco = entry_offsets_group_list.at(i).first + plus_offset;

        // empty the arrays
        bcis->trunc_to(0);
        pcos->trunc_to(0);
        is_catch_alls->trunc_to(0);

        GrowableArray<CallSiteEntryOffsetsPair> grouped_pairs = entry_offsets_group_list.at(i).second;

        for (int k = 0; k < grouped_pairs.length(); ++k) {
            int catch_bci = grouped_pairs.at(k).entry->bci;

            for (int j = 0; j < exception_handler_pair_list.length(); ++j) {
                int start_bci = exception_handler_pair_list.at(j).first->start_bci;
                int limit_bci = exception_handler_pair_list.at(j).first->limit_bci;
                int handler_bci = exception_handler_pair_list.at(j).first->handler_bci;
                uint64_t handler_pco = exception_handler_pair_list.at(j).second->instruction_offset + plus_offset;
                if (catch_bci >= start_bci && catch_bci <= limit_bci && !bcis->contains(handler_bci)) {
                    bcis->append(handler_bci);
                    assert(!pcos->contains(handler_pco), "same handler_bci should be always matched to same handler_pco");
                    pcos->append(handler_pco);
                    is_catch_alls->append(exception_handler_pair_list.at(j).first->is_catch_all);
                }
            }
        }

        if (bcis->length()) {
            // One bci may throw multiple different exceptions, so multiple handlers may be matched.
            // And there may be multiple catch_all handlers, but we need to make sure these catch_all handlers
            // are at last
            for (int k = 0; k < is_catch_alls->length(); ++k) {
                if (is_catch_alls->at(k)) {
                    for (int j = k; j < is_catch_alls->length(); ++j) {
                        assert(is_catch_alls->at(j), "catch all must be last handler");
                    }
                    break;
                }
            }
            exception_handler_table->add_subtable(catch_pco, bcis, scope_depths, pcos);
        }
    }
}
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
                                                       int bci) {
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
    call_site_entry->return_pc_offset = 0;
    call_site_entry->blr_offset = 0;
    call_site_entry->call_target_offset = 0;
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
                                                      uint32_t constant) {
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
        deopt_bundle->locals = new GrowableArray<StackMapLocation*>();
        deopt_bundle->expression_stacks = new GrowableArray<StackMapLocation*>();
        deopt_bundle->monitors = new GrowableArray<StackMapLocation*>();
        _deopt_bundles->append(deopt_bundle);
    }
}

void YuhuDebugInformationRecorder::register_deopt_bundle_local_data(uint32_t instruction_offset,
                                                                    uint8_t location_kind,
                                                                    uint32_t location_reg_num,
                                                                    int32_t location_offset,
                                                                    uint32_t constant,
                                                                    uint8_t basic_type) {
    int index = _deopt_bundles->find(&instruction_offset, [](void* token, DeoptBundle* bundle) -> bool {
        return *((uint32_t*)token) == bundle->instruction_offset;
    });
    if (index == -1) {
        auto deopt_bundle = new DeoptBundle();
        deopt_bundle->instruction_offset = instruction_offset;
        deopt_bundle->bci = 0;
        deopt_bundle->locals = new GrowableArray<StackMapLocation*>();
        deopt_bundle->expression_stacks = new GrowableArray<StackMapLocation*>();
        deopt_bundle->monitors = new GrowableArray<StackMapLocation*>();
        _deopt_bundles->append(deopt_bundle);
        index = _deopt_bundles->length() - 1;
    }

    DeoptBundle* bundle = _deopt_bundles->at(index);
    auto location = new StackMapLocation();
    location->kind = location_kind;
    location->reg_num = location_reg_num;
    location->offset = location_offset;
    location->constant = constant;
    location->basic_type = basic_type;
    bundle->locals->append(location);
}

void YuhuDebugInformationRecorder::register_deopt_bundle_expression_stack_data(uint32_t instruction_offset,
                                                                               uint8_t location_kind,
                                                                               uint32_t location_reg_num,
                                                                               int32_t location_offset,
                                                                               uint32_t constant,
                                                                               uint8_t basic_type) {
    int index = _deopt_bundles->find(&instruction_offset, [](void* token, DeoptBundle* bundle) -> bool {
        return *((uint32_t*)token) == bundle->instruction_offset;
    });
    if (index == -1) {
        auto deopt_bundle = new DeoptBundle();
        deopt_bundle->instruction_offset = instruction_offset;
        deopt_bundle->bci = 0;
        deopt_bundle->locals = new GrowableArray<StackMapLocation*>();
        deopt_bundle->expression_stacks = new GrowableArray<StackMapLocation*>();
        deopt_bundle->monitors = new GrowableArray<StackMapLocation*>();
        _deopt_bundles->append(deopt_bundle);
        index = _deopt_bundles->length() - 1;
    }

    DeoptBundle* bundle = _deopt_bundles->at(index);
    auto location = new StackMapLocation();
    location->kind = location_kind;
    location->reg_num = location_reg_num;
    location->offset = location_offset;
    location->constant = constant;
    location->basic_type = basic_type;
    bundle->expression_stacks->append(location);
}

void YuhuDebugInformationRecorder::register_deopt_bundle_monitor_data(uint32_t instruction_offset,
                                                                       uint8_t location_kind,
                                                                       uint32_t location_reg_num,
                                                                       int32_t location_offset,
                                                                       uint32_t constant,
                                                                       uint8_t basic_type) {
    int index = _deopt_bundles->find(&instruction_offset, [](void* token, DeoptBundle* bundle) -> bool {
        return *((uint32_t*)token) == bundle->instruction_offset;
    });
    if (index == -1) {
        auto deopt_bundle = new DeoptBundle();
        deopt_bundle->instruction_offset = instruction_offset;
        deopt_bundle->bci = 0;
        deopt_bundle->locals = new GrowableArray<StackMapLocation*>();
        deopt_bundle->expression_stacks = new GrowableArray<StackMapLocation*>();
        deopt_bundle->monitors = new GrowableArray<StackMapLocation*>();
        _deopt_bundles->append(deopt_bundle);
        index = _deopt_bundles->length() - 1;
    }

    DeoptBundle* bundle = _deopt_bundles->at(index);
    auto location = new StackMapLocation();
    location->kind = location_kind;
    location->reg_num = location_reg_num;
    location->offset = location_offset;
    location->constant = constant;
    location->basic_type = basic_type;
    bundle->monitors->append(location);
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

        // multiple call targets may use same blr, so skip processed return pc offset
        if (processed_instruction_offsets.contains(return_pc_offset)) {
            continue;
        }

        // since safepoint poll call is after stack frame is setup and arguments are copied,
        // the 9th, 10th arguments before current stack frame need no GC support
        int arg_count = 0;
        auto *oopmap = new OopMap(YuhuStack::oopmap_slot_munge(frame_size),
                                  YuhuStack::oopmap_slot_munge(arg_count));

        if (call_site_entry->call_site_type != CallSiteType::deopt_call) {

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
                if (call_site_entry->call_target_offset) {
                    pc_offset = call_site_entry->call_target_offset + 4 +
                                plus_offset; // pc_offset should be ldr instruction, not adrp instruction
                } else {
                    pc_offset = call_site_entry->blr_offset + plus_offset;
                }
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

        } else {
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
                for (int i = 0; i < bundle->locals->length(); i++) {
                    StackMapLocation* loc = bundle->locals->at(i);
                    Location location;
                    
                    // Determine Location::Type based on BasicType
                    Location::Type loc_type = Location::normal; // Default for integers
                    switch (loc->basic_type) {
                        case T_OBJECT:
                        case T_ARRAY:
                            loc_type = Location::oop;
                            break;
                        case T_LONG:
                            loc_type = Location::lng;
                            break;
                        case T_FLOAT:
                            loc_type = Location::float_in_dbl;
                            break;
                        case T_DOUBLE:
                            loc_type = Location::dbl;
                            break;
                        default:
                            loc_type = Location::normal; // T_INT, T_BOOLEAN, T_CHAR, T_BYTE, T_SHORT
                            break;
                    }
                    
                    if (loc->kind == static_cast<uint8_t>(StackMapParser::LocationKind::Direct) ||
                        loc->kind == static_cast<uint8_t>(StackMapParser::LocationKind::Indirect)) {
                        // Stack location
                        location = Location::new_stk_loc(loc_type, loc->offset);
                    } else if (loc->kind == static_cast<uint8_t>(StackMapParser::LocationKind::Register)) {
                        // Register location
                        VMReg reg = VMRegImpl::as_VMReg(loc->reg_num << 1);
                        location = Location::new_reg_loc(loc_type, reg);
                    } else if (loc->kind == static_cast<uint8_t>(StackMapParser::LocationKind::Constant)) {
                        // Constant value (likely null or primitive constant)
                        locals->append(new ConstantIntValue(loc->constant));
                        continue;
                    } else {
                        continue; // Skip unsupported location kinds
                    }
                    
                    locals->append(new LocationValue(location));
                }
            }

            // Convert expression stacks
            if (bundle->expression_stacks && bundle->expression_stacks->length() > 0) {
                expressions = new GrowableArray<ScopeValue*>();
                for (int i = 0; i < bundle->expression_stacks->length(); i++) {
                    StackMapLocation* loc = bundle->expression_stacks->at(i);
                    Location location;
                    
                    // Determine Location::Type based on BasicType
                    Location::Type loc_type = Location::normal; // Default for integers
                    switch (loc->basic_type) {
                        case T_OBJECT:
                        case T_ARRAY:
                            loc_type = Location::oop;
                            break;
                        case T_LONG:
                            loc_type = Location::lng;
                            break;
                        case T_FLOAT:
                            loc_type = Location::float_in_dbl;
                            break;
                        case T_DOUBLE:
                            loc_type = Location::dbl;
                            break;
                        default:
                            loc_type = Location::normal; // T_INT, T_BOOLEAN, T_CHAR, T_BYTE, T_SHORT
                            break;
                    }
                    
                    if (loc->kind == static_cast<uint8_t>(StackMapParser::LocationKind::Direct) ||
                        loc->kind == static_cast<uint8_t>(StackMapParser::LocationKind::Indirect)) {
                        // Stack location
                        location = Location::new_stk_loc(loc_type, loc->offset);
                    } else if (loc->kind == static_cast<uint8_t>(StackMapParser::LocationKind::Register)) {
                        // Register location
                        VMReg reg = VMRegImpl::as_VMReg(loc->reg_num << 1);
                        location = Location::new_reg_loc(loc_type, reg);
                    } else if (loc->kind == static_cast<uint8_t>(StackMapParser::LocationKind::Constant)) {
                        // Constant value
                        expressions->append(new ConstantIntValue(loc->constant));
                        continue;
                    } else {
                        continue; // Skip unsupported location kinds
                    }
                    
                    expressions->append(new LocationValue(location));
                }
            }

            // Convert monitors
            if (bundle->monitors && bundle->monitors->length() > 0) {
                monitors = new GrowableArray<MonitorValue*>();
                for (int i = 0; i < bundle->monitors->length(); i++) {
                    StackMapLocation* loc = bundle->monitors->at(i);
                    Location owner_loc;
                    
                    // Monitors are always T_OBJECT
                    Location::Type loc_type = Location::oop;
                    
                    if (loc->kind == static_cast<uint8_t>(StackMapParser::LocationKind::Direct) ||
                        loc->kind == static_cast<uint8_t>(StackMapParser::LocationKind::Indirect)) {
                        // Stack location for monitor owner (oop)
                        owner_loc = Location::new_stk_loc(loc_type, loc->offset);
                    } else if (loc->kind == static_cast<uint8_t>(StackMapParser::LocationKind::Register)) {
                        // Register location for monitor owner (oop)
                        VMReg reg = VMRegImpl::as_VMReg(loc->reg_num << 1);
                        owner_loc = Location::new_reg_loc(loc_type, reg);
                    } else if (loc->kind == static_cast<uint8_t>(StackMapParser::LocationKind::Constant)) {
                        // Constant (likely null) - skip this monitor
                        continue;
                    } else {
                        continue; // Skip unsupported location kinds
                    }
                    
                    // For the basic_lock location, we use a stack location
                    // The actual BasicLock is in the interpreter frame
                    Location basic_lock_loc = Location::new_stk_loc(Location::normal, loc->offset + wordSize);
                    
                    ScopeValue* owner = new LocationValue(owner_loc);
                    monitors->append(new MonitorValue(owner, basic_lock_loc));
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
                                          false, // Whether to re-execute the bytecode after deoptimization
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
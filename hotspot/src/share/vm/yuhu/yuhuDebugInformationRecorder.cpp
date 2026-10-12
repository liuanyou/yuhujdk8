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

// File-based logging for debugging
static FILE* yuhu_stack_map_log = NULL;
static void yuhu_stack_map_log_init() {
    if (yuhu_stack_map_log == NULL) {
        yuhu_stack_map_log = fopen(YuhuStackMapFile, "a");
    }
}
#define YUHU_STACK_MAP_LOG(fmt, ...) \
    do { \
        yuhu_stack_map_log_init(); \
        if (yuhu_stack_map_log) { \
            fprintf(yuhu_stack_map_log, fmt "\n", ##__VA_ARGS__); \
            fflush(yuhu_stack_map_log); \
        } \
    } while(0)

// Initialize static TLS index
int YuhuDebugInformationRecorder::_tls_index = -1;

// Constructor
YuhuDebugInformationRecorder::YuhuDebugInformationRecorder()
  : _module(NULL) {
  // All memory (GrowableArrays, elements, nested objects) is allocated from this Arena.
  // Deleting the Arena frees everything in one shot — zero manual cleanup.
  _arena = new (mtCompiler) Arena();
  init_collections();
  _func_size = 0;
  _total_frame_size_in_bytes = -1;
  _num_of_prologue_registers = -1;
}

// (Re)initialize all collections from current _arena.
void YuhuDebugInformationRecorder::init_collections() {
  _call_site_entries = new (_arena) GrowableArray<CallSiteEntry*>(_arena, 10, 0, (CallSiteEntry*)NULL);
  _stack_map_entries = new (_arena) GrowableArray<StackMapEntry*>(_arena, 10, 0, (StackMapEntry*)NULL);
  _deopt_bundles = new (_arena) GrowableArray<DeoptBundle*>(_arena, 10, 0, (DeoptBundle*)NULL);
  _patchpoint_entries = new (_arena) GrowableArray<PatchPointEntry*>(_arena, 10, 0, (PatchPointEntry*)NULL);
  _const_symbol_entries = new (_arena) GrowableArray<SymbolEntry*>(_arena, 10, 0, (SymbolEntry*)NULL);
  _edge_entries = new (_arena) GrowableArray<EdgeEntry*>(_arena, 10, 0, (EdgeEntry*)NULL);
  _frame_layout_entries = new (_arena) GrowableArray<FrameLayoutEntry*>(_arena, 10, 0, (FrameLayoutEntry*)NULL);
}

// Reset all data for reuse in next compilation.
// Delete the Arena (frees ALL memory), create a fresh one, reinitialize collections.
void YuhuDebugInformationRecorder::reset() {
  delete _arena;
  _arena = new (mtCompiler) Arena();
  init_collections();
  // Reset scalar fields
  _mangled_func_name.clear();
  _func_size = 0;
  _total_frame_size_in_bytes = -1;
  _num_of_prologue_registers = -1;
  _module = NULL;
}

// Destructor
YuhuDebugInformationRecorder::~YuhuDebugInformationRecorder() {
  // Deleting the Arena frees all memory allocated from it
  delete _arena;
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
void YuhuDebugInformationRecorder::register_call_site(uint64_t statepoint_id,
                                                       YuhuInlineNodeOrMethod* node_method,
                                                       uint64_t call_target,
                                                       CallSiteType call_site_type,
                                                       int bci) {
    int index = _call_site_entries->find(&statepoint_id, [](void* token, CallSiteEntry* entry) -> bool {
        return *((uint64_t*)token) == entry->statepoint_id;
    });
    // skip registration if already exists
    if (index != -1) {
        return;
    }
    auto call_site_entry = new (_arena) CallSiteEntry();
    call_site_entry->statepoint_id = statepoint_id;
    call_site_entry->inline_node_or_method = node_method;
    call_site_entry->call_target = call_target;
    call_site_entry->call_site_type = call_site_type;
    call_site_entry->bci = bci;
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
  
  // Create metadata for each call site: {statepoint_id, call_target}
  std::vector<llvm::MDNode*> call_site_mds;
  
  for (int i = 0; i < get_call_site_count(); i++) {
    uint64_t statepoint_id = get_call_site_statepoint_id(i);
    uint64_t call_target = get_call_site_call_target(i);
    
    // Create a tuple: !{i64 statepoint_id, i64 call_target}
    llvm::Metadata* operands[] = {
      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), statepoint_id)),
      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), call_target))
    };
    
    call_site_mds.push_back(llvm::MDNode::get(ctx, operands));
  }
  
  // Create named metadata node: !yuhu.call_sites = {!{i64, i64}, ...}
  llvm::NamedMDNode* named_md = _module->getOrInsertNamedMetadata("yuhu.call_sites");
  
  for (auto* md : call_site_mds) {
    named_md->addOperand(md);
  }
  
  if (YuhuTraceIRCompilation) {
    llvm::errs() << "[YuhuDebugInformationRecorder] Embedded " 
                 << get_call_site_count() << " call site mappings\n";
  }
}

void YuhuDebugInformationRecorder::register_stack_map(uint64_t statepoint_id, uint32_t instruction_offset) {
    int index = -1;
    for (int i = 0; i < _stack_map_entries->length(); ++i) {
        StackMapEntry* stack_map = _stack_map_entries->at(i);
        if (stack_map->statepoint_id == statepoint_id && stack_map->instruction_offset == instruction_offset) {
            index = i;
            break;
        }
    }

    if (index != -1) {
        return;
    }

    auto stack_map_entry = new (_arena) StackMapEntry();
    stack_map_entry->statepoint_id = statepoint_id;
    stack_map_entry->instruction_offset = instruction_offset;
    stack_map_entry->locations = new (_arena) GrowableArray<StackMapLocation*>(_arena, 10, 0, (StackMapLocation*)NULL);
    _stack_map_entries->append(stack_map_entry);
}

void YuhuDebugInformationRecorder::register_stack_map_location_data(uint64_t statepoint_id,
                                                                    uint32_t instruction_offset,
                                                                    uint8_t location_kind,
                                                                    uint32_t location_reg_num,
                                                                    int32_t location_offset,
                                                                    uint64_t constant) {
    int index = -1;
    for (int i = 0; i < _stack_map_entries->length(); ++i) {
        StackMapEntry* stack_map = _stack_map_entries->at(i);
        if (stack_map->statepoint_id == statepoint_id && stack_map->instruction_offset == instruction_offset) {
            index = i;
            break;
        }
    }
    if (index == -1) {
        auto stack_map_entry = new (_arena) StackMapEntry();
        stack_map_entry->statepoint_id = statepoint_id;
        stack_map_entry->instruction_offset = instruction_offset;
        stack_map_entry->locations = new (_arena) GrowableArray<StackMapLocation*>(_arena, 10, 0, (StackMapLocation*)NULL);
        _stack_map_entries->append(stack_map_entry);
        index = _stack_map_entries->length() - 1;
    }

    StackMapEntry* entry = _stack_map_entries->at(index);
    auto location = new (_arena) StackMapLocation();
    location->kind = location_kind;
    location->reg_num = location_reg_num;
    location->offset = location_offset;
    location->constant = constant;
    entry->locations->append(location);
}

void YuhuDebugInformationRecorder::register_deopt_bundle(uint64_t statepoint_id, GrowableArray<uint8_t>* locals_basic_type_list, GrowableArray<uint8_t>* expressions_basic_type_list,
                                                         int num_monitors) {
    int index = _deopt_bundles->find(&statepoint_id, [](void* token, DeoptBundle* entry) -> bool {
        return *((uint64_t*)token) == entry->statepoint_id;
    });
    // skip registration if already exists
    if (index != -1) {
        return;
    }
    auto deopt_bundle = new (_arena) DeoptBundle();
    deopt_bundle->statepoint_id = statepoint_id;
    deopt_bundle->locals = new (_arena) GrowableArray<uint8_t>(_arena, 10, 0, (uint8_t)0);
    deopt_bundle->expression_stacks = new (_arena) GrowableArray<uint8_t>(_arena, 10, 0, (uint8_t)0);
    deopt_bundle->num_monitors = num_monitors;
    _deopt_bundles->append(deopt_bundle);
    for (int i = 0; i < locals_basic_type_list->length(); ++i) {
        deopt_bundle->locals->append(locals_basic_type_list->at(i));
    }
    for (int i = 0; i < expressions_basic_type_list->length(); ++i) {
        deopt_bundle->expression_stacks->append(expressions_basic_type_list->at(i));
    }
}

void YuhuDebugInformationRecorder::register_patch_point(uint64_t statepoint_id, uint32_t reserved_bytes, uint64_t call_site_statepoint_id, jobject jobj) {
    int index = _patchpoint_entries->find(&statepoint_id, [](void* token, PatchPointEntry* entry) -> bool {
        return *((uint64_t*)token) == entry->statepoint_id;
    });
    // skip registration if already exists
    if (index != -1) {
        return;
    }
    auto patch_point_entry = new (_arena) PatchPointEntry();
    patch_point_entry->statepoint_id = statepoint_id;
    patch_point_entry->reserved_bytes = reserved_bytes;
    patch_point_entry->call_site_statepoint_id = call_site_statepoint_id;
    patch_point_entry->oop = jobj;
    _patchpoint_entries->append(patch_point_entry);
}

void YuhuDebugInformationRecorder::register_const_symbol(uint64_t addr, uint64_t start, uint64_t end) {
    int index = _const_symbol_entries->find(&addr, [](void* token, SymbolEntry* entry) -> bool {
        return *((uint32_t*)token) == entry->addr;
    });
    if (index == -1) {
        auto const_symbol_entry = new (_arena) SymbolEntry();
        const_symbol_entry->addr = addr;
        const_symbol_entry->start = start;
        const_symbol_entry->end = end;
        _const_symbol_entries->append(const_symbol_entry);
    }
}

void YuhuDebugInformationRecorder::register_edge(uint32_t offset, EdgeTargetType edge_target_type, uint64_t target_address) {
    int index = _edge_entries->find(&offset, [](void* token, EdgeEntry* entry) -> bool {
        return *((uint64_t*)token) == entry->offset;
    });
    // skip registration if already exists
    if (index != -1) {
        return;
    }
    auto edge_entry = new (_arena) EdgeEntry();
    edge_entry->offset = offset;
    edge_entry->edge_target_type = edge_target_type;
    edge_entry->target_address = target_address;
    _edge_entries->append(edge_entry);
}

void YuhuDebugInformationRecorder::register_frame_layout_frame_fields(uint64_t statepoint_id, YuhuInlineNodeOrMethod* node_method,
                                                                      int header_words, int monitor_words, int stack_words, int locals_words, int extended_frame_words) {
    int index = _frame_layout_entries->find(&statepoint_id, [](void* token, FrameLayoutEntry* entry) -> bool {
        return *((uint64_t*)token) == entry->statepoint_id;
    });
    // skip registration if already exists
    if (index != -1) {
        return;
    }
    auto frame_layout_entry = new (_arena) FrameLayoutEntry();
    frame_layout_entry->statepoint_id = statepoint_id;
    frame_layout_entry->inline_node_or_method = node_method;
    frame_layout_entry->header_words = header_words;
    frame_layout_entry->monitor_words = monitor_words;
    frame_layout_entry->stack_words = stack_words;
    frame_layout_entry->locals_words = locals_words;
    frame_layout_entry->extended_frame_words = extended_frame_words;
    _frame_layout_entries->append(frame_layout_entry);
}

void YuhuDebugInformationRecorder::register_prologue_fields(int total_frame_size_in_bytes, int num_of_prologue_registers) {
    _total_frame_size_in_bytes = total_frame_size_in_bytes;
    _num_of_prologue_registers = num_of_prologue_registers;
}

void YuhuDebugInformationRecorder::register_frame_layout_stack_map_fields(uint64_t statepoint_id, int extended_frame_reg_num, int extended_frame_kind, int extended_frame_offset) {
    int index = _frame_layout_entries->find(&statepoint_id, [](void* token, FrameLayoutEntry* entry) -> bool {
        return *((uint64_t*)token) == entry->statepoint_id;
    });
    // skip registration if doesn't exist
    if (index == -1) {
        return;
    }
    auto frame_layout_entry = _frame_layout_entries->at(index);
    frame_layout_entry->extended_frame_reg_num = extended_frame_reg_num;
    frame_layout_entry->extended_frame_kind = extended_frame_kind;
    frame_layout_entry->extended_frame_offset = extended_frame_offset;
}

// RS4GC doesn't include monitor objects in stack map, need to handle it manually
// a normal yuhu frame layout should be like:
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
// for caller inlines callees, eg:
//
//     static long run(int lo, int hi) {
//        long acc = 0;
//        for (int x = lo; x <= hi; x++) {
//            acc += signAdd(x);
//            acc += choose(x, hi - x);
//            acc += clamp(x);
//        }
//        return acc;
//    }
// an inlined yuhu frame layout should be like:
/*
    0x16b59e540: 0x0000000500000000 0x0000000100000032 - spill area
    0x16b59e550: 0x000000056b59e5e0 0x000000000000001c - spill area
    0x16b59e560: 0x0000005600000005 0x000000016b59e680 - spill area
    0x16b59e570: 0x0000000106d0c718 0x0000000138816800 - spill area
    ------ method run [topmost frame layout] ------
    0x16b59e580: 0x000000016b59e650 0x000000013c899da0 - expression stack [0] (top) / expression stack [1]
    0x16b59e590: 0x000000013c87a1d0 0x0000000100000000 - expression stack [2] / expression stack [3]
    0x16b59e5a0: 0x000000016b59e690 0x000000016b59e650 - expression stack [4] / expression stack [5] (bottom)
    0x16b59e5b0: 0x000000011047ef58 0x0000000106d0c718 - oop tmp / method slot
    0x16b59e5c0: 0x000000016b59e540 0x0000000138816800 - final sp / return slot
    0x16b59e5d0: 0x00000000deadbabe 0x000000016b59e680 - frame marker / fp
    0x16b59e5e0: 0x00000001ffffffdc 0x000000000000001c - local [4] / local [3]
    0x16b59e5f0: 0x000000013c899da0 0x0000000100000032 - local [2] / local [1]
    0x16b59e600: 0x00000001ffffffce 0x0000600003134ab8 - local [0] / x0 slot
    ------ method signAdd [reused frame layout] ------
    0x16b59e610: 0x0000000000000000 0x000000016b59e600 - expression stack [0] (top) / expression stack [1]
    0x16b59e620: 0x000000016b59e690 0x000000013c87c688 - expression stack [2] (bottom) / oop tmp
    0x16b59e630: 0x0000000100000057 0x00000001ffffffdb - method slot / final sp
    0x16b59e640: 0x000000000000001b 0x00000000ffffffdb - return slot / frame marker
    0x16b59e650: 0x000000016b59e650 0x0000000106d0c6dd - fp / local [0]
    0x16b59e660: 0x000000016b59e6c8 0x0000000106d0ca40 - padding
    0x16b59e670: 0x000000016b59e690 0x000000013c817590 - prologue x20 / x19
    0x16b59e680: 0x000000016b59e710 0x000000013c87c7a0 - prologue x29 / x30
    ------ method choose [reused frame layout] ------
    0x16b59e610: 0x0000000000000000 0x000000016b59e600 - expression stack [0] (top) / expression stack [1]
    0x16b59e620: 0x000000016b59e690 0x000000013c87c688 - expression stack [2] (bottom) / oop tmp
    0x16b59e630: 0x0000000100000057 0x00000001ffffffdb - method slot / final sp
    0x16b59e640: 0x000000000000001b 0x00000000ffffffdb - return slot / frame marker
    0x16b59e650: 0x000000016b59e650 0x0000000106d0c6dd - fp / local [1]
    0x16b59e660: 0x000000016b59e6c8 0x0000000106d0ca40 - local [0] / padding
    0x16b59e670: 0x000000016b59e690 0x000000013c817590 - prologue x20 / x19
    0x16b59e680: 0x000000016b59e710 0x000000013c87c7a0 - prologue x29 / x30
    ------ method clamp [reused frame layout] ------
    0x16b59e610: 0x0000000000000000 0x000000016b59e600 - expression stack [0] (top) / expression stack [1]
    0x16b59e620: 0x000000016b59e690 0x000000013c87c688 - expression stack [2] (bottom) / oop tmp
    0x16b59e630: 0x0000000100000057 0x00000001ffffffdb - method slot / final sp
    0x16b59e640: 0x000000000000001b 0x00000000ffffffdb - return slot / frame marker
    0x16b59e650: 0x000000016b59e650 0x0000000106d0c6dd - fp / local [0]
    0x16b59e660: 0x000000016b59e6c8 0x0000000106d0ca40 - padding
    0x16b59e670: 0x000000016b59e690 0x000000013c817590 - prologue x20 / x19
    0x16b59e680: 0x000000016b59e710 0x000000013c87c7a0 - prologue x29 / x30
 */
// btw,
// an interpreter layout should be like:
/*
    x22 = 0x0000000107736348
    x24 = 0x000000016b580c18
    x26 = 0x0000000107005c88
    x12 = 0x000000010707f9c0
    sp = 0x000000016b580b20
    x20 = 0x000000016b580b40

    0x16b580b20: 0x000000016b580b30 0x0000000148072ba8
    0x16b580b30: 0x000000016b580bb0 0x000000014807fa90
    0x16b580b40: 0x00000006c02f72e8 0xdeaddeaf00000009 - / expression stack [4] (top)
    0x16b580b50: 0x000000076aceb8b8 0xdeaddeaf32641199 - expression stack [3] / expression stack [2]
    0x16b580b60: 0x000000076b4e1688 0x00000006c0344020 - expression stack [1] / expression stack [0] (bottom)
    0x16b580b70: 0x000000016b580b70 0x0000000107004de0 - initial sp / byte code pointer
    0x16b580b80: 0x000000016b580c18 0x0000000107005c88 - locals pointer / constant pool cache
    0x16b580b90: 0x0000000107736348 0x0000000107004ef0 - method data / method
    0x16b580ba0: 0x0000000000000000 0x000000016b580c00 - last_esp / sender_sp
    0x16b580bb0: 0x000000016b580c80 0x000000014807cca8 - prologue x29 / x30
    0x16b580bc0: 0x0000000000000000 0x0000000000000000 - local [11] / local [10]
    0x16b580bd0: 0x0000000000000000 0x0000000000000000 - local [9] / local [8]
    0x16b580be0: 0x0000000000000000 0x000000076aceb8b8 - local [7] / local [6]
    0x16b580bf0: 0xdeaddeaf00000009 0xdeaddeaf00000001 - local [5] / local [4]

    0x16b580c00: 0x000000076b4e16e8 0x000000076b4e1688 - arg [3] / arg [2]
    0x16b580c10: 0xdeaddeaf32641199 0x00000006c0344020 - arg [1] / arg [0]
 */
void YuhuDebugInformationRecorder::generate_safepoint_and_describe_scope(DebugInformationRecorder* real_recorder,
                                                                         int plus_offset,
                                                                         int frame_size) {
    using StackMapParser = llvm::StackMapParser<llvm::endianness::little>;
    GrowableArray<uint32_t> processed_instruction_offsets;
    GrowableArray<CallSiteInstructionHolder> call_site_instruction_holder_list;
    for (int i = 0; i < _call_site_entries->length(); ++i) {
        CallSiteEntry* call_site = _call_site_entries->at(i);

        for (int j = 0; j < _stack_map_entries->length(); ++j) {
            StackMapEntry* stack_map = _stack_map_entries->at(j);
            assert(stack_map->statepoint_id != 0, "statepoint id should exist");
            if (call_site->statepoint_id == stack_map->statepoint_id) {
                // 1. patch point statepoint id won't match here, hence calls like unwind/oop/metadata call are skipped
                // 2. inlinee call is skipped too
                call_site_instruction_holder_list.append(CallSiteInstructionHolder(call_site, stack_map)); // multiple stack map may match here
            }
        }
        // some call site may be eliminated from final machine code, so those call site won't be processed
    }

    // sort by instruction offset, oopmap should be registered in ascending order
    call_site_instruction_holder_list.sort([](CallSiteInstructionHolder* a, CallSiteInstructionHolder* b) -> int {
        if ((*a).instruction_offset() < (*b).instruction_offset()) return -1;
        if ((*a).instruction_offset() > (*b).instruction_offset()) return 1;
        return 0;
    });

    for (int i = 0; i < call_site_instruction_holder_list.length(); ++i) {
        CallSiteInstructionHolder holder = call_site_instruction_holder_list.at(i);
        CallSiteEntry* call_site_entry = holder.call_site_entry;
        YuhuInlineNodeOrMethod* inline_node_or_method = call_site_entry->inline_node_or_method;

        uint64_t return_pc_offset = holder.instruction_offset();

        // multiple call targets may use same blr, so skip processed return pc offset
        if (processed_instruction_offsets.contains(return_pc_offset)) {
            if (inline_node_or_method->is_inline_node()) {
                // same pc offset should belong to same method, usually this should not happen, just in case
                YuhuInlineNodeOrMethod* prev_inline_node_or_method = call_site_instruction_holder_list.at(i-1).call_site_entry->inline_node_or_method;
                assert(inline_node_or_method->target() == prev_inline_node_or_method->target(), "usually methods should be the same");
            }
            continue;
        }

        // since safepoint poll call is after stack frame is setup and arguments are copied,
        // the 9th, 10th arguments before current stack frame need no GC support
        int arg_count = 0;
        auto *oopmap = new OopMap(YuhuStack::oopmap_slot_munge(frame_size),
                                  YuhuStack::oopmap_slot_munge(arg_count));

        if (call_site_entry->call_site_type == CallSiteType::safepoint_poll ||
            call_site_entry->call_site_type == CallSiteType::vm_call ||
            call_site_entry->call_site_type == CallSiteType::java_call ||
            call_site_entry->call_site_type == CallSiteType::deopt_call) {
            assert(contains_stack_map_instruction_offset(return_pc_offset), "Call site should contain stack map");

            if (YuhuTraceOffset) {
                tty->print_cr("Yuhu: Found stack map site by return pc offset=%d", return_pc_offset);
            }

            // add plus_offset to get offset in code cache
            int pc_offset = return_pc_offset + plus_offset;

            // first, process gc-live oops
            GrowableArray<int32_t> processed_stack_offsets;
            GrowableArray<uint32_t> processed_register_nums;

            StackMapEntry* stack_map_entry = holder.stack_map_entry;
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

            GrowableArray<CallSiteDeoptBundleFrameHolder*> frame_holder_chain_list;
            if (inline_node_or_method->is_inline_node()) {
                // from child to parent
                GrowableArray<YuhuInlineNode*> inline_node_chain_list;
                YuhuInlineNode* current_node = inline_node_or_method->as_inline_node();
                while (current_node) {
                    inline_node_chain_list.append(current_node);
                    current_node = current_node->parent();
                }
                for (int m = 0; m < inline_node_chain_list.length(); ++m) {
                    CallSiteEntry* node_call_site = NULL;
                    if (m == 0) {
                        // if it is first child, then current call site entry is the one we need
                        node_call_site = call_site_entry;
                    } else {
                        for (int n = 0; n < _call_site_entries->length(); ++n) {
                            CallSiteEntry* site = _call_site_entries->at(n);
                            YuhuInlineNode* prev_node = inline_node_chain_list.at(m-1);
                            // multiple call sites may match parent inline node, but bci makes unique match
                            // put call site type check in first condition, coz as_inline_node may fail for leaf call
                            if (site->call_site_type == CallSiteType::inlinee_call && site->bci == prev_node->bci() && site->inline_node_or_method->as_inline_node() == inline_node_chain_list.at(m)) {
                                node_call_site = site;
                                break;
                            }
                        }
                    }
                    assert(node_call_site != NULL, "call site entry should exist");

                    DeoptBundle* node_deopt_bundle = NULL;
                    for (int n = 0; n < _deopt_bundles->length(); ++n) {
                        if (node_call_site->statepoint_id == _deopt_bundles->at(n)->statepoint_id) {
                            node_deopt_bundle = _deopt_bundles->at(n);
                            break;
                        }
                    }
                    assert(node_deopt_bundle != NULL, "deopt bundle should exist");

                    FrameLayoutEntry* node_frame_layout = NULL;
                    for (int n = 0; n < _frame_layout_entries->length(); ++n) {
                        if (inline_node_chain_list.at(m) == _frame_layout_entries->at(n)->inline_node_or_method->as_inline_node()) {
                            node_frame_layout = _frame_layout_entries->at(n);
                            break;
                        }
                    }
                    assert(node_frame_layout != NULL, "frame layout entry should exist");
                    frame_holder_chain_list.append(new CallSiteDeoptBundleFrameHolder(node_call_site, node_deopt_bundle, node_frame_layout));
                }
            } else {
                // non-inlined method, it is just topmost method
                DeoptBundle* node_deopt_bundle = NULL;
                for (int n = 0; n < _deopt_bundles->length(); ++n) {
                    if (call_site_entry->statepoint_id == _deopt_bundles->at(n)->statepoint_id) {
                        node_deopt_bundle = _deopt_bundles->at(n);
                        break;
                    }
                }
                assert(node_deopt_bundle != NULL, "deopt bundle should exist");
                frame_holder_chain_list.append(new CallSiteDeoptBundleFrameHolder(call_site_entry, node_deopt_bundle, get_topmost_frame_entry()));
            }

            // second, process deopt bundle
            // Iterate from parent to child to collect oop map
            for (int m = frame_holder_chain_list.length() - 1; m >= 0; --m) {
                DeoptBundle* bundle = frame_holder_chain_list.at(m)->deopt_bundle;
                FrameLayoutEntry* frame_entry = frame_holder_chain_list.at(m)->frame_layout_entry;
                // 2 words is for x29,x30 in prologue
                int adjust_words = _total_frame_size_in_bytes / wordSize - 2
                                  - (-frame_entry->extended_frame_offset / wordSize);
                assert(adjust_words >= 0, "adjust_words has invalid value");
                // Convert locals
                if (bundle->locals && bundle->locals->length() > 0) {
                    for (int j = 0; j < bundle->locals->length(); j++) {
                        uint8_t basic_type = bundle->locals->at(j);
                        int local_offset_in_bytes = (adjust_words + frame_entry->stack_words + frame_entry->monitor_words +
                                frame_entry->header_words + frame_entry->locals_words - 1 - j) * wordSize;

                        // Determine Location::Type based on BasicType
                        switch (basic_type) {
                            case T_OBJECT:
                            case T_ARRAY:
                                oopmap->set_oop(YuhuStack::slot2reg(local_offset_in_bytes >> LogBytesPerWord));
                                break;
                            default:
                                break;
                        }
                    }
                }

                // Convert expression stacks
                if (bundle->expression_stacks && bundle->expression_stacks->length() > 0) {
                    // deopt bundle is from top to bottom, but we need to iterate it from bottom to top, which is desired by deoptimization blob
                    for (int j = bundle->expression_stacks->length() - 1; j >= 0; j--) {
                        uint8_t basic_type = bundle->expression_stacks->at(j);
                        int express_stack_offset_in_bytes = (adjust_words + frame_entry->stack_words - bundle->expression_stacks->length() + j) * wordSize;

                        // Determine Location::Type based on BasicType
                        switch (basic_type) {
                            case T_OBJECT:
                            case T_ARRAY:
                                oopmap->set_oop(YuhuStack::slot2reg(express_stack_offset_in_bytes >> LogBytesPerWord));
                                break;
                            default:
                                break;
                        }
                    }
                }

                // Convert monitors
                if (bundle->num_monitors > 0) {
                    int max_monitors = frame_entry->monitor_words / 2;
                    // from oldest to newest
                    for (int j = 0; j < bundle->num_monitors; ++j) {
                        int monitor_object_offset_in_bytes = (adjust_words + frame_entry->stack_words + (max_monitors - j - 1) * 2 + 1) * wordSize;
                        if (YuhuTraceOffset && YuhuStackMapFile != NULL) {
                            YUHU_STACK_MAP_LOG("[StackMap] monitor_object_offset_in_bytes: %d", monitor_object_offset_in_bytes);
                        }
                        oopmap->set_oop(YuhuStack::slot2reg(monitor_object_offset_in_bytes >> LogBytesPerWord));
                    }
                }
            }

            // call sites need an oopmap even there is no live oop
            real_recorder->add_safepoint(pc_offset, oopmap);
            // Iterate from parent to child to describe scope
            for (int m = frame_holder_chain_list.length() - 1; m >= 0; --m) {
                DeoptBundle* bundle = frame_holder_chain_list.at(m)->deopt_bundle;
                FrameLayoutEntry* frame_entry = frame_holder_chain_list.at(m)->frame_layout_entry;
                CallSiteEntry* call_site = frame_holder_chain_list.at(m)->call_site_entry;

                // 2 words is for x29,x30 in prologue
                int adjust_words = _total_frame_size_in_bytes / wordSize - 2
                                   - (-frame_entry->extended_frame_offset / wordSize);
                assert(adjust_words >= 0, "adjust_words has invalid value");

                GrowableArray<ScopeValue*>* locals = NULL;
                GrowableArray<ScopeValue*>* expressions = NULL;
                GrowableArray<MonitorValue*>* monitors = NULL;
                // Convert locals
                if (bundle->locals && bundle->locals->length() > 0) {
                    locals = new GrowableArray<ScopeValue*>();
                    for (int j = 0; j < bundle->locals->length(); j++) {
                        uint8_t basic_type = bundle->locals->at(j);
                        int local_offset_in_bytes = (adjust_words + frame_entry->stack_words + frame_entry->monitor_words +
                                frame_entry->header_words + frame_entry->locals_words - 1 - j) * wordSize;

                        // Determine Location::Type based on BasicType
                        switch (basic_type) {
                            case T_OBJECT:
                            case T_ARRAY: {
                                ScopeValue *scopeValue = new LocationValue(Location::new_stk_loc(Location::oop, local_offset_in_bytes));
                                locals->append(scopeValue);
                            }
                                break;
                            case T_LONG: {
                                // in deopt bundle, j slot is T_LONG with actual value, j+1 slot is T_LONG2 with padding
                                // and in physical stack frame, local[j] slot has padding, local[j+1] slot has actual value, and this is
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
                                // in deopt bundle, j slot is T_DOUBLE with actual value, j+1 slot is T_LONG2 with padding
                                // and in physical stack frame, local[j] slot has padding, local[j+1] slot has actual value, and this is
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
                    // deopt bundle is from top to bottom, but we need to iterate it from bottom to top, which is desired by deoptimization blob
                    for (int j = bundle->expression_stacks->length() - 1; j >= 0; j--) {
                        uint8_t basic_type = bundle->expression_stacks->at(j);
                        int express_stack_offset_in_bytes = (adjust_words + frame_entry->stack_words - bundle->expression_stacks->length() + j) * wordSize;

                        // Determine Location::Type based on BasicType
                        switch (basic_type) {
                            case T_OBJECT:
                            case T_ARRAY: {
                                ScopeValue *scopeValue = new LocationValue(Location::new_stk_loc(Location::oop, express_stack_offset_in_bytes));
                                expressions->append(scopeValue);
                            }
                                break;
                            case T_LONG: {
                                // in deopt bundle, j slot is T_LONG with actual value, j-1 slot is T_LONG2 with padding
                                // but in physical stack frame, expression[j] slot has padding, expression[j-1] slot has actual value, and this is
                                // not the desired layout for interpreter, we need to reverse
                                // construct second slot
                                ScopeValue *scopeValue = new LocationValue(Location::new_stk_loc(Location::lng, express_stack_offset_in_bytes - wordSize));
                                // construct first slot
                                assert(bundle->expression_stacks->at(--j) == ciTypeFlow::StateVector::T_LONG2, "should be T_LONG2 type");
                                Location invalid_location;

                                expressions->append(new LocationValue(invalid_location));
                                expressions->append(scopeValue);
                            }
                                break;
                            case T_DOUBLE: {
                                // in deopt bundle, j slot is T_DOUBLE with actual value, j-1 slot is T_LONG2 with padding
                                // but in physical stack frame, expression[j] slot has padding, expression[j-1] slot has actual value, and this is
                                // not the desired layout for interpreter, we need to reverse
                                // construct second slot
                                ScopeValue *scopeValue = new LocationValue(Location::new_stk_loc(Location::dbl, express_stack_offset_in_bytes - wordSize));
                                // construct first slot
                                assert(bundle->expression_stacks->at(--j) == ciTypeFlow::StateVector::T_DOUBLE2, "should be T_DOUBLE2 type");
                                Location invalid_location;

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
                    int max_monitors = frame_entry->monitor_words / 2;
                    // from oldest to newest
                    for (int j = 0; j < bundle->num_monitors; ++j) {
                        int monitor_object_offset_in_bytes = (adjust_words + frame_entry->stack_words + (max_monitors - j - 1) * 2 + 1) * wordSize;

                        ScopeValue *scopeValue = new LocationValue(Location::new_stk_loc(Location::oop, monitor_object_offset_in_bytes));
                        Location basicLockLoc = Location::new_stk_loc(Location::normal, monitor_object_offset_in_bytes - wordSize);

                        monitors->append(new MonitorValue(scopeValue, basicLockLoc));
                    }
                }
                // Create DebugTokens from the ScopeValue arrays
                DebugToken* locals_token = (locals != NULL) ? real_recorder->create_scope_values(locals) : NULL;
                DebugToken* expressions_token = (expressions != NULL) ? real_recorder->create_scope_values(expressions) : NULL;
                DebugToken* monitors_token = (monitors != NULL) ? real_recorder->create_monitor_values(monitors) : NULL;
                real_recorder->describe_scope(pc_offset, // PC offset in code (same as passed to add_safepoint)
                                              call_site->inline_node_or_method->target(), // the method being compiled (the caller)
                                              call_site->bci, // the BCI of the invoke bytecode in the caller
                                              call_site->call_site_type == CallSiteType::deopt_call, // Whether to re-execute the bytecode after deoptimization
                                              false, // Whether this is a MethodHandle invoke
                                              call_site->inline_node_or_method->target()->signature()->return_type()->is_object(), // Whether the return value is an oop
                                              locals_token, // DebugToken* for local variables
                                              expressions_token, // DebugToken* for expression stack
                                              monitors_token); // DebugToken* for synchronized monitors
            }
            real_recorder->end_safepoint(pc_offset);
        } else if (call_site_entry->call_site_type != CallSiteType::leaf_call) {
            ShouldNotReachHere();
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
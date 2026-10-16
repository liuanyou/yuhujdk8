#ifndef SHARE_VM_YUHU_YUHUDEBUGINFORMATIONRECORDER_HPP
#define SHARE_VM_YUHU_YUHUDEBUGINFORMATIONRECORDER_HPP

#include "code/debugInfoRec.hpp"
#include "compiler/oopMap.hpp"
#include "ci/ciMethod.hpp"
#include "code/scopeDesc.hpp"
#include "yuhu/yuhu_globals.hpp"
#include "runtime/threadLocalStorage.hpp"
#include "yuhu/yuhuStack.hpp"
#include "utilities/debug.hpp"

namespace llvm {
    class Module;
}

enum class CallSiteType : uint8_t {
    none = 0,
    safepoint_poll = 1,
    vm_call = 2,
    java_call = 3,
    deopt_call = 4,
    unwind_call = 5,
    leaf_call = 6,
    inlinee_call = 7
};

enum class EdgeTargetType : uint8_t {
    none = 0,
    got_symbol = 1,
    const_symbol = 2
};

enum class GlobalVarType : uint8_t {
    none = 0,
    metadata = 1,
    oop = 2
};

enum class SectionType : uint8_t {
    none = 0,
    const_section = 1,
    data_section = 2
};

class CallSiteEntry : public ResourceObj {
public:
    uint64_t statepoint_id; // generated at IR phase - assigned
    YuhuInlineNodeOrMethod* inline_node_or_method;
    uint64_t call_target; // generated at IR phase - stub or target address or 0
    CallSiteType call_site_type; // generated at IR phase
    int bci; // generated at IR phase
};

class StackMapLocation : public ResourceObj {
public:
    uint8_t kind;
    uint32_t reg_num;
    int32_t offset;
    uint64_t constant;
};

// statepoint_id & instruction_offset makes unique
class StackMapEntry : public ResourceObj {
public:
    uint64_t statepoint_id;
    uint32_t instruction_offset;
    GrowableArray<StackMapLocation*>* locations;
};

class DeoptBundle : public ResourceObj {
public:
    uint64_t statepoint_id;
    GrowableArray<uint8_t>* locals; // list of locals with basic type only...from 0 to max_locals-1
    GrowableArray<uint8_t>* expression_stacks; // list of expression stacks with basic type only...from 0 to stack_depth-1
    int num_monitors;
};

class PatchPointEntry : public ResourceObj {
public:
    uint64_t statepoint_id;
    uint32_t reserved_bytes = 0;
    uint64_t call_site_statepoint_id = 0;
    jobject oop = NULL;
};

class GlobalVarEntry : public ResourceObj {
public:
    uint64_t unique_id;
    GlobalVarType global_var_type;
    uint64_t target_value;
    jobject oop = NULL;
};

class SymbolEntry : public ResourceObj {
public:
    uint64_t addr;
    uint64_t start;
    uint64_t end;
    SectionType section_type;
};

// adrp + ldr generates 2 edge entries, but points to the same target address with different offsets
class EdgeEntry : public ResourceObj {
public:
    uint32_t offset; // this is offset in func
    EdgeTargetType edge_target_type;
    uint64_t target_address; // this is the address of got/const entry, not function address
};

class FrameLayoutEntry : public ResourceObj {
public:
    uint64_t statepoint_id;
    YuhuInlineNodeOrMethod* inline_node_or_method;
    int header_words = -1; // initialized in YuhuStack::initialize method
    int monitor_words = -1; // initialized in YuhuStack::initialize method
    int stack_words = - 1; // initialized in YuhuStack::initialize method
    int locals_words = -1; // initialized in YuhuStack::initialize method
    int extended_frame_words = -1; // initialized in YuhuStack::initialize method
    int extended_frame_reg_num = -1; // extracted from stack map
    int extended_frame_kind = -1; // extracted from stack map
    int extended_frame_offset = -1; // extracted from stack map
};

class PatchPointStackMapPair : public ResourceObj {
public:
    PatchPointEntry* patchpoint_entry;
    StackMapEntry* stack_map_entry;

    PatchPointStackMapPair() : patchpoint_entry(nullptr), stack_map_entry(nullptr) {}

    PatchPointStackMapPair(PatchPointEntry* patchpoint, StackMapEntry* stack_map) : patchpoint_entry(patchpoint), stack_map_entry(stack_map) {}
};

class CallSiteInstructionHolder : public ResourceObj {
public:
    CallSiteEntry* call_site_entry; // same IR may be duplicated in machine code, it is 1:M relations, same call site may have multiple stack map
    StackMapEntry* stack_map_entry;

    CallSiteInstructionHolder() : call_site_entry(nullptr), stack_map_entry(nullptr) {}

    // explicitly set to nullptr, otherwise, it points to undefined area
    CallSiteInstructionHolder(CallSiteEntry* call_site, StackMapEntry* stack_map) : call_site_entry(call_site), stack_map_entry(stack_map) {}

    uint64_t instruction_offset() const {
        assert(stack_map_entry != NULL, "stack map should exist");
        return stack_map_entry->instruction_offset;
    }
};

class CallSiteDeoptBundleFrameHolder : public ResourceObj {
public:
    CallSiteEntry* call_site_entry;
    DeoptBundle* deopt_bundle;
    FrameLayoutEntry* frame_layout_entry;

    CallSiteDeoptBundleFrameHolder(): call_site_entry(nullptr), deopt_bundle(nullptr), frame_layout_entry(nullptr) {}

    CallSiteDeoptBundleFrameHolder(CallSiteEntry* call_site, DeoptBundle* bundle, FrameLayoutEntry* frame_layout):
        call_site_entry(call_site), deopt_bundle(bundle), frame_layout_entry(frame_layout) {}
};

// YuhuDebugInformationRecorder: Thread-local recorder for collecting debug information
// and call site metadata during LLVM IR generation
// Lives for the duration of a single compilation, managed via thread-local storage
class YuhuDebugInformationRecorder : public ResourceObj {
private:

  // C_HEAP Arena for all allocations. Deleting this Arena frees all memory
  // allocated from it (GrowableArray headers, internal data, and elements).
  // This avoids manual delete calls for each element.
  Arena* _arena;

  // Call site metadata for JITLink correlation
  GrowableArray<CallSiteEntry*>* _call_site_entries;

  // Stack map statepoint locations for call sites
  GrowableArray<StackMapEntry*>* _stack_map_entries;

  // deopt bundles for call sites
  GrowableArray<DeoptBundle*>* _deopt_bundles;

  GrowableArray<PatchPointEntry*>* _patchpoint_entries;

  GrowableArray<GlobalVarEntry*>* _global_var_entries;

  GrowableArray<SymbolEntry*>* _symbol_entries;

  GrowableArray<EdgeEntry*>* _edge_entries;

  // frame layout information
  GrowableArray<FrameLayoutEntry*>* _frame_layout_entries;

  int _total_frame_size_in_bytes = -1; // extracted from machine code's prologue
  int _num_of_prologue_registers = -1; // extracted from machine code's prologue

  // Mangled function name
  std::string _mangled_func_name;
  size_t _func_size;

  // LLVM Module reference for embedding metadata
  llvm::Module* _module;

  // Thread-local storage index
  static int _tls_index;

  void check_frame_layout_info() const {
      assert(_total_frame_size_in_bytes != -1 && _num_of_prologue_registers != -1, "prologue data is not initialized");
      for (int i = 0; i < _frame_layout_entries->length(); ++i) {
          auto frame_layout_entry = _frame_layout_entries->at(i);
          assert(frame_layout_entry->header_words != -1 &&
                 frame_layout_entry->monitor_words != -1 &&
                 frame_layout_entry->stack_words != -1 &&
                 frame_layout_entry->locals_words != -1 &&
                 frame_layout_entry->extended_frame_words != -1 &&
                 frame_layout_entry->extended_frame_reg_num != -1 &&
                 frame_layout_entry->extended_frame_kind != -1 &&
                 frame_layout_entry->extended_frame_offset != -1, "frame layout data is not initialized");
          // Usually it should be fp register, sometimes it uses sp register,
          // but don't know when, assume it is always fp register
          assert(frame_layout_entry->extended_frame_reg_num == 29 &&
                 frame_layout_entry->extended_frame_offset < 0 &&
                 frame_layout_entry->extended_frame_offset % 8 == 0, "Should be valid fp offset");
      }
  }

  /*
   * Return topmost frame layout entry. If method is not inlined, just return the only frame, otherwise, return the frame of root method.
   */
  FrameLayoutEntry* get_topmost_frame_entry() const {
      check_frame_layout_info();
      if (_frame_layout_entries->length() == 1) {
          return _frame_layout_entries->at(0);
      }
      for (int i = 0; i < _frame_layout_entries->length(); ++i) {
          FrameLayoutEntry* entry = _frame_layout_entries->at(i);
          YuhuInlineNode* inline_node = entry->inline_node_or_method->as_inline_node();
          // Check root node and return its frame layout entry
          if (inline_node->bci() == 0 && inline_node->inline_depth() == 0 && !inline_node->parent()) {
              return entry;
          }
      }
      ShouldNotReachHere();
      return NULL;
  }

public:
  YuhuDebugInformationRecorder();
  ~YuhuDebugInformationRecorder();
  
  // Thread-local storage management
  static void initialize_tls();
  static YuhuDebugInformationRecorder* get();
  static void release();
  void set_module(llvm::Module* mod) { _module = mod; }

  // Reset all data for reuse in next compilation
  void reset();

  // (Re)initialize all collections from current _arena
  void init_collections();

  // Call site related functions
  void register_call_site(uint64_t statepoint_id,
                          YuhuInlineNodeOrMethod* inline_node_or_method,
                          uint64_t call_target,
                          CallSiteType call_site_type,
                          int bci = -2);

  bool has_java_call_sites() const {
      CallSiteType java_call = CallSiteType::java_call;
      int index = _call_site_entries->find(&java_call, [](void* token, CallSiteEntry* entry) -> bool {
          return *((CallSiteType*)token) == entry->call_site_type;
      });
      return index != -1;
  }

  bool contains_call_target(uint64_t call_target) const {
      int index = _call_site_entries->find(&call_target, [](void* token, CallSiteEntry* entry) -> bool {
          return *((uint64_t*)token) == entry->call_target;
      });
      return index != -1;
  }

  bool contains_stack_map_instruction_offset(uint32_t instruction_offset) const {
      int index = _stack_map_entries->find(&instruction_offset, [](void* token, StackMapEntry* entry) -> bool {
          return *((uint32_t*)token) == entry->instruction_offset;
      });
      return index != -1;
  }

  GrowableArray<PatchPointStackMapPair*>* get_patchpoint_stack_maps() const {
      auto result = new GrowableArray<PatchPointStackMapPair*>();

      for (int i = 0; i < _patchpoint_entries->length(); ++i) {
          PatchPointEntry* patchpoint_entry = _patchpoint_entries->at(i);

          for (int j = 0; j < _stack_map_entries->length(); ++j) {
              StackMapEntry* stack_map_entry = _stack_map_entries->at(j);
              if (patchpoint_entry->statepoint_id != stack_map_entry->statepoint_id) {
                  continue;
              }
              result->append(new PatchPointStackMapPair(patchpoint_entry, stack_map_entry));
          }
      }
      return result;
  }

  void embed_call_site_metadata();

  // during IR transform phase, some inlinees may not survive after running IR passes,
  // hence eliminate those inlinees' frames
  void eliminate_not_survived_frames() {
      for (int i = 0; i < _frame_layout_entries->length(); ) {
          auto frame_layout_entry = _frame_layout_entries->at(i);
          if (frame_layout_entry->header_words != -1 &&
                 frame_layout_entry->monitor_words != -1 &&
                 frame_layout_entry->stack_words != -1 &&
                 frame_layout_entry->locals_words != -1 &&
                 frame_layout_entry->extended_frame_words != -1 &&
                 frame_layout_entry->extended_frame_reg_num == -1 &&
                 frame_layout_entry->extended_frame_kind == -1 &&
                 frame_layout_entry->extended_frame_offset == -1) {
              _frame_layout_entries->remove_at(i);
              continue;
          }
          ++i;
      }
  }
  
  int get_call_site_count() const {
    return _call_site_entries ? _call_site_entries->length() : 0;
  }
  
  uint64_t get_call_site_statepoint_id(int index) const {
    assert(index < get_call_site_count(), "index out of bounds");
    return _call_site_entries->at(index)->statepoint_id;
  }
  
  uint64_t get_call_site_call_target(int index) const {
    assert(index < get_call_site_count(), "index out of bounds");
    return _call_site_entries->at(index)->call_target;
  }

  CallSiteType get_call_site_type_by_statepoint_id(uint64_t statepoint_id) const {
      if (!_call_site_entries) return CallSiteType::none;
      int index = _call_site_entries->find(&statepoint_id, [](void* token, CallSiteEntry* entry) -> bool {
          return *((uint64_t*)token) == entry->statepoint_id;
      });
      if (index == -1) return CallSiteType::none;
      return _call_site_entries->at(index)->call_site_type;
  }

  CallSiteType get_call_site_type_by_patchpoint_statepoint_id(uint64_t patchpoint_statepoint_id) const {
      if (!_patchpoint_entries) return CallSiteType::none;
      if (!_call_site_entries) return CallSiteType::none;
      int patchpoint_index = _patchpoint_entries->find(&patchpoint_statepoint_id, [](void* token, PatchPointEntry* entry) -> bool {
          return *((uint64_t*)token) == entry->statepoint_id;
      });
      if (patchpoint_index == -1) return CallSiteType::none;
      int index = _call_site_entries->find(&(_patchpoint_entries->at(patchpoint_index)->call_site_statepoint_id), [](void* token, CallSiteEntry* entry) -> bool {
          return *((uint64_t*)token) == entry->statepoint_id;
      });
      if (index == -1) return CallSiteType::none;
      return _call_site_entries->at(index)->call_site_type;
    }

  GlobalVarEntry* get_global_var_by_target_value(uint64_t target_value) const {
      if (!_global_var_entries) return NULL;
      int index = _global_var_entries->find(&target_value, [](void* token, GlobalVarEntry* entry) -> bool {
          return *((uint64_t*)token) == entry->target_value;
      });
      if (index == -1) return NULL;
      return _global_var_entries->at(index);
  }

   SymbolEntry* get_symbol_by_section_type_and_range_addr(SectionType section_type, uint64_t addr) const {
       if (!addr) return NULL;
       int index = -1;
       for (int i = 0; i < _symbol_entries->length(); ++i) {
           SymbolEntry* entry = _symbol_entries->at(i);
           // data is at least 1 byte, so addr should never be entry->end
           if (entry->section_type == section_type && addr >= entry->start && addr < entry->end) {
               index = i;
               break;
           }
       }
       if (index == -1) return NULL;
       return _symbol_entries->at(index);
  }

  SymbolEntry* get_symbol_by_range_addr(uint64_t addr) const {
      if (!addr) return NULL;
      int index = _symbol_entries->find(&addr, [](void* token, SymbolEntry* entry) -> bool {
          uint64_t range_addr = *((uint64_t*)token);
          // data is at least 1 byte, so addr should never be entry->end
          return  range_addr >= entry->start && range_addr < entry->end;
      });
      if (index == -1) return NULL;
      return _symbol_entries->at(index);
  }

  size_t total_symbol_size() const {
      size_t total = 0;
      for (int i = 0; i < _symbol_entries->length(); ++i) {
          total += _symbol_entries->at(i)->end - _symbol_entries->at(i)->start;
      }
      return total;
  }

  GrowableArray<EdgeEntry*>* edge_entries() const {
      return _edge_entries;
  }

  bool is_frame_layout_statepoint_id(uint64_t statepoint_id) const {
      int index = _frame_layout_entries->find(&statepoint_id, [](void* token, FrameLayoutEntry* entry) -> bool {
          return *((uint64_t*)token) == entry->statepoint_id;
      });
      return index != -1;
  }

  // stack map related functions
  void register_stack_map(uint64_t statepoint_id, uint32_t instruction_offset);

  void register_stack_map_location_data(uint64_t statepoint_id, uint32_t instruction_offset, uint8_t location_kind, uint32_t location_reg_num, int32_t location_offset, uint64_t constant = 0);

  void register_deopt_bundle(uint64_t statepoint_id, GrowableArray<uint8_t>* locals_basic_type_list, GrowableArray<uint8_t>* expressions_basic_type_list, int num_monitors);

  void register_patch_point(uint64_t statepoint_id, uint32_t reserved_bytes, uint64_t call_site_statepoint_id, jobject jobj = NULL);

  void register_global_var(uint64_t unique_id, GlobalVarType global_var_type, uint64_t target_value, jobject jobj = NULL);

  void register_symbol(uint64_t address, uint64_t start, uint64_t end, SectionType section_type);

  void register_edge(uint32_t offset, EdgeTargetType edge_target_type, uint64_t target_address);

  void register_frame_layout_frame_fields(uint64_t statepoint_id, YuhuInlineNodeOrMethod* inline_node_or_method,
                                          int header_words, int monitor_words, int stack_words, int locals_words, int extended_frame_words);

  void register_prologue_fields(int total_frame_size_in_bytes, int num_of_prologue_registers);

  void register_frame_layout_stack_map_fields(uint64_t statepoint_id, int extended_frame_reg_num, int extended_frame_kind, int extended_frame_offset);

  /**
   * Return topmost frame entry's max monitors.
   *
   * @return
   */
  int max_monitors() const {
      FrameLayoutEntry* topmost_frame_entry = get_topmost_frame_entry();

      int max_monitors = topmost_frame_entry->monitor_words / 2;

      return max_monitors;
  }

  /**
   * Use topmost frame entry's data.
   *
   * for T_LONG/T_DOUBLE type, local_index+1 has actual value and local_index has padding value.
   *
   * @param local_index
   * @return
   */
    int local_offset_in_bytes(int local_index) const {
        FrameLayoutEntry* topmost_frame_entry = get_topmost_frame_entry();

        // 2 words is for x29,x30 in prologue
        int adjust_words = _total_frame_size_in_bytes / wordSize - 2 - (-topmost_frame_entry->extended_frame_offset / wordSize);

        assert(adjust_words >= 0, "adjust_words has invalid value");

        assert(local_index < topmost_frame_entry->locals_words, "local index is invalid value");

        int offset_in_bytes = (adjust_words + topmost_frame_entry->stack_words + topmost_frame_entry->monitor_words +
                topmost_frame_entry->header_words + topmost_frame_entry->locals_words - 1 - local_index) * wordSize;
        return offset_in_bytes;
    }

    /**
     * Use topmost frame entry's data.
     *
     * @param monitor_index
     * @return
     */
    int monitor_header_offset_in_bytes(int monitor_index) const {
        FrameLayoutEntry* topmost_frame_entry = get_topmost_frame_entry();

        // 2 words is for x29,x30 in prologue
        int adjust_words = _total_frame_size_in_bytes / wordSize - 2 - (-topmost_frame_entry->extended_frame_offset / wordSize);

        assert(adjust_words >= 0, "adjust_words has invalid value");

        int max_monitors = topmost_frame_entry->monitor_words / 2;

        assert(monitor_index < max_monitors, "monitor index is invalid value");

        int monitor_object_offset_in_bytes =
                (adjust_words + topmost_frame_entry->stack_words + (max_monitors - monitor_index - 1) * 2 + 1) * wordSize;

        return monitor_object_offset_in_bytes - wordSize;
    }

    /**
     * Use topmost frame entry's data.
     *
     * @param monitor_index
     * @return
     */
    int monitor_object_offset_in_bytes(int monitor_index) const {
        FrameLayoutEntry* topmost_frame_entry = get_topmost_frame_entry();

        // 2 words is for x29,x30 in prologue
        int adjust_words = _total_frame_size_in_bytes / wordSize - 2 - (-topmost_frame_entry->extended_frame_offset / wordSize);

        assert(adjust_words >= 0, "adjust_words has invalid value");

        int max_monitors = topmost_frame_entry->monitor_words / 2;

        assert(monitor_index < max_monitors, "monitor index is invalid value");

        int monitor_object_offset_in_bytes =
                (adjust_words + topmost_frame_entry->stack_words + (max_monitors - monitor_index - 1) * 2 + 1) * wordSize;

        return monitor_object_offset_in_bytes;
    }

  void set_mangled_func_name(std::string mangled_func_name) {
      _mangled_func_name = mangled_func_name;
  }

  std::string get_mangled_func_name() const {
      return _mangled_func_name;
  }

  void set_func_size(size_t func_size) {
      _func_size = func_size;
  }

  size_t get_func_size() const {
      return _func_size;
  }

  void generate_safepoint_and_describe_scope(DebugInformationRecorder* real_recorder,
                                             int plus_offset,
                                             int frame_size);
};

#endif // SHARE_VM_YUHU_YUHUDEBUGINFORMATIONRECORDER_HPP
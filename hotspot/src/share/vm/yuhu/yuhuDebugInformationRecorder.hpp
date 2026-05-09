#ifndef SHARE_VM_YUHU_YUHUDEBUGINFORMATIONRECORDER_HPP
#define SHARE_VM_YUHU_YUHUDEBUGINFORMATIONRECORDER_HPP

#include "code/debugInfoRec.hpp"
#include "compiler/oopMap.hpp"
#include "ci/ciMethod.hpp"
#include "code/scopeDesc.hpp"
#include "yuhu/yuhuOffsetMapper.hpp"
#include "yuhu/yuhu_globals.hpp"
#include "runtime/threadLocalStorage.hpp"
#include "yuhu/yuhuStack.hpp"
#include "utilities/debug.hpp"

namespace llvm {
    class Module;
}

struct CallSiteEntry {
    uint64_t virtual_offset;
    uint64_t virtual_address;
    uint64_t helper_address;
    uint64_t return_pc_offset;
};

// Forward declaration of gc_safepoint_poll from yuhuRuntime.cpp
extern "C" void gc_safepoint_poll();

// YuhuDebugInformationRecorder: Thread-local recorder for collecting debug information
// and call site metadata during LLVM IR generation
// Lives for the duration of a single compilation, managed via thread-local storage
class YuhuDebugInformationRecorder : public ResourceObj {
private:
  // Storage for virtual PC offset OopMap information
  GrowableArray<int>* _virtual_offsets;
  GrowableArray<OopMap*>* _oopmaps;
  
  // Storage for virtual PC offset frame information (for deoptimization)
  GrowableArray<int>* _virtual_frame_offsets;
  GrowableArray<ciMethod*>* _frame_targets;
  GrowableArray<int>* _frame_bcis;
  GrowableArray<GrowableArray<ScopeValue*>*>* _frame_locals;
  GrowableArray<GrowableArray<ScopeValue*>*>* _frame_expressions;
  GrowableArray<GrowableArray<MonitorValue*>*>* _frame_monitors;

  // Call site metadata for JITLink correlation
  // Maps virtual_offset → virtual_address → helper_address
  GrowableArray<CallSiteEntry*>* _call_site_entries;

  // Stack map statepoint locations
  GrowableArray<uint32_t>* _stack_map_instruction_offsets;
  GrowableArray<GrowableArray<uint8_t>*>* _stack_map_location_kinds;
  GrowableArray<GrowableArray<uint32_t>*>* _stack_map_location_reg_nums; // for Constant and ConstantIndex kind, it is 0
  GrowableArray<GrowableArray<int32_t>*>* _stack_map_location_offsets; // for Register, Constant and ConstantIndex kind, it is 0

  // Mangled function name
  std::string _mangled_func_name;
  size_t _func_size;

  // LLVM Module reference for embedding metadata
  llvm::Module* _module;

  // Thread-local storage index
  static int _tls_index;

public:
  YuhuDebugInformationRecorder();
  ~YuhuDebugInformationRecorder();
  
  // Thread-local storage management
  static void initialize_tls();
  static YuhuDebugInformationRecorder* get();
  static void release();
  void set_module(llvm::Module* mod) { _module = mod; }

  // Add virtual PC offset safepoint information
  void add_safepoint(int virtual_pc_offset, OopMap* oopmap);

  // Describe scope information at virtual PC offset
  void describe_scope(int virtual_pc_offset,
                     ciMethod* method,
                     int bci,
                     bool reexecute,
                     bool rethrow_exception,
                     bool is_method_handle_invoke,
                     GrowableArray<ScopeValue*>* locals,
                     GrowableArray<ScopeValue*>* expressions,
                     GrowableArray<MonitorValue*>* monitors);

  // End safepoint recording at virtual PC offset
  void end_safepoint(int virtual_pc_offset);

  // Getter methods
  GrowableArray<int>* virtual_offsets() const { return _virtual_offsets; }
  GrowableArray<OopMap*>* oopmaps() const { return _oopmaps; }
  GrowableArray<int>* virtual_frame_offsets() const { return _virtual_frame_offsets; }
  GrowableArray<ciMethod*>* frame_targets() const { return _frame_targets; }
  GrowableArray<int>* frame_bcis() const { return _frame_bcis; }
  GrowableArray<GrowableArray<ScopeValue*>*>* frame_locals() const { return _frame_locals; }
  GrowableArray<GrowableArray<ScopeValue*>*>* frame_expressions() const { return _frame_expressions; }
  GrowableArray<GrowableArray<MonitorValue*>*>* frame_monitors() const { return _frame_monitors; }

  // Call site related functions
  void register_call_site(int virtual_offset, uint64_t virtual_address, uint64_t helper_address);
  void embed_call_site_metadata();
  
  int get_call_site_count() const {
    return _call_site_entries ? _call_site_entries->length() : 0;
  }
  
  int get_call_site_virtual_offset(int index) const {
    assert(index < get_call_site_count(), "index out of bounds");
    return _call_site_entries->at(index)->virtual_offset;
  }
  
  uint64_t get_call_site_virtual_address(int index) const {
    assert(index < get_call_site_count(), "index out of bounds");
    return _call_site_entries->at(index)->virtual_address;
  }
  
  uint64_t get_call_site_helper_address(int index) const {
    assert(index < get_call_site_count(), "index out of bounds");
    return _call_site_entries->at(index)->helper_address;
  }
  
  // NEW: Look up virtual address by virtual_offset (not array index)
  uint64_t get_call_site_virtual_address_by_offset(uint64_t virtual_offset) const {
    if (!_call_site_entries) return 0;
      int index = _call_site_entries->find(&virtual_offset, [](void* token, CallSiteEntry* entry) -> bool {
          return *((uint64_t*)token) == entry->virtual_offset;
      });
    if (index == -1) return 0;
    return _call_site_entries->at(index)->virtual_address;
  }
  
  // NEW: Look up helper address by virtual_offset (not array index)
  uint64_t get_call_site_helper_address_by_offset(uint64_t virtual_offset) const {
    if (!_call_site_entries) return 0;
      int index = _call_site_entries->find(&virtual_offset, [](void* token, CallSiteEntry* entry) -> bool {
          return *((uint64_t*)token) == entry->virtual_offset;
      });
    if (index == -1) return 0;
    return _call_site_entries->at(index)->helper_address;
  }

  void update_call_site_return_pc_offset(uint64_t virtual_offset, uint64_t return_pc_offset) const {
      if (!_call_site_entries) return;
      int index = _call_site_entries->find(&virtual_offset, [](void* token, CallSiteEntry* entry) -> bool {
          return *((uint64_t*)token) == entry->virtual_offset;
      });
      if (index == -1) return;
      _call_site_entries->at(index)->return_pc_offset = return_pc_offset;
  }

  // stack map related functions
  void register_stack_map(uint32_t instruction_offset, uint8_t location_kind, uint32_t location_reg_num, int32_t location_offset);

  void set_mangled_func_name(std::string mangled_func_name) {
      _mangled_func_name = mangled_func_name;
  }

  std::string get_mangled_func_name() {
      return _mangled_func_name;
  }

  void set_func_size(size_t func_size) {
      _func_size = func_size;
  }

  size_t get_func_size() {
      return _func_size;
  }

  void quick_sort_by_actual_offset(int left, int right, YuhuOffsetMapper* offset_mapper) {
      if (left >= right) return;

      int pivot_index = (left + right) / 2;
      int pivot_virtual_offset = _virtual_offsets->at(pivot_index);
      int pivot_actual_offset = offset_mapper->get_actual_offset(pivot_virtual_offset);

      swap_entries(pivot_index, right);

      int store_index = left;

      for (int i = left; i < right; i++) {
          if (offset_mapper->get_actual_offset(_virtual_offsets->at(i)) < pivot_actual_offset) {
              swap_entries(i, store_index);
              store_index++;
          }
      }

      swap_entries(store_index, right);

      quick_sort_by_actual_offset(left, store_index - 1, offset_mapper);
      quick_sort_by_actual_offset(store_index + 1, right, offset_mapper);
  }
  void swap_entries(int i, int j) {
      if (i == j) return;
      int temp =  _virtual_offsets->at(i);
      _virtual_offsets->at_put(i, _virtual_offsets->at(j));
      _virtual_offsets->at_put(j, temp);
  }

  void convert_and_add_to_real_recorder(DebugInformationRecorder* real_recorder,
                                       ciMethod* method,
                                       int plus_offset,
                                       int frame_size);
};

#endif // SHARE_VM_YUHU_YUHUDEBUGINFORMATIONRECORDER_HPP
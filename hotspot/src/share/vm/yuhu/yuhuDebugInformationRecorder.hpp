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
    java_call = 3
};

struct CallSiteEntry {
    uint64_t virtual_offset;
    uint64_t virtual_address;
    uint64_t helper_address;
    uint64_t return_pc_offset;
    CallSiteType call_site_type;
    int bci;
};

// Forward declaration of gc_safepoint_poll from yuhuRuntime.cpp
extern "C" void gc_safepoint_poll();

// YuhuDebugInformationRecorder: Thread-local recorder for collecting debug information
// and call site metadata during LLVM IR generation
// Lives for the duration of a single compilation, managed via thread-local storage
class YuhuDebugInformationRecorder : public ResourceObj {
private:

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

  // Call site related functions
  void register_call_site(uint64_t virtual_offset,
                          uint64_t virtual_address,
                          uint64_t helper_address,
                          CallSiteType call_site_type,
                          int bci);
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

  CallSiteType get_call_site_type_by_offset(uint64_t virtual_offset) const {
      if (!_call_site_entries) return CallSiteType::none;
      int index = _call_site_entries->find(&virtual_offset, [](void* token, CallSiteEntry* entry) -> bool {
          return *((uint64_t*)token) == entry->virtual_offset;
      });
      if (index == -1) return CallSiteType::none;
      return _call_site_entries->at(index)->call_site_type;
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

  void convert_and_add_to_real_recorder(DebugInformationRecorder* real_recorder,
                                       ciMethod* method,
                                       int plus_offset,
                                       int frame_size);
};

#endif // SHARE_VM_YUHU_YUHUDEBUGINFORMATIONRECORDER_HPP
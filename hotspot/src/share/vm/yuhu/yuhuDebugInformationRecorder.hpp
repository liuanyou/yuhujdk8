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
    deopt_call = 4
};

struct CallSiteEntry {
    uint64_t virtual_offset; // generated at IR phase
    uint64_t virtual_address; // generated at IR phase
    uint64_t helper_address; // generated at IR phase
    CallSiteType call_site_type; // generated at IR phase
    int bci; // generated at IR phase
    uint64_t call_target_offset; // extracted by ORC plugin
    uint64_t blr_offset; // extracted by ORC plugin
    uint64_t return_pc_offset; // extracted by ORC plugin
};

struct StackMapLocation {
    uint8_t kind;
    uint32_t reg_num;
    int32_t offset;
    uint32_t constant;
    uint8_t basic_type; // BasicType from ciType.hpp (T_INT, T_OBJECT, etc.)
};

struct StackMapEntry {
    uint32_t instruction_offset;
    GrowableArray<StackMapLocation*>* locations;
};

struct DeoptBundle {
    uint32_t instruction_offset;
    uint64_t bci;
    GrowableArray<StackMapLocation*>* locals;
    GrowableArray<StackMapLocation*>* expression_stacks;
    GrowableArray<StackMapLocation*>* monitors;
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
  GrowableArray<StackMapEntry*>* _stack_map_entries;

  // deopt statepoint locations
  GrowableArray<DeoptBundle*>* _deopt_bundles;

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

  void clean_eliminated_call_sites() {
      // remove call sites which don't appear in llvm machine code
      // this can happen because llvm passes may eliminate some blocks
      for (int i = 0; i < _call_site_entries->length(); ) {
          if (_call_site_entries->at(i)->return_pc_offset) {
              i++;
              continue;
          }
          if (YuhuTraceOffset) {
              tty->print_cr("Yuhu: remove call site entry: index=%d, virtual_offset=%d, call_site_type=%d",
                            i, _call_site_entries->at(i)->virtual_offset, static_cast<uint8_t>(_call_site_entries->at(i)->call_site_type));
          }
          _call_site_entries->remove_at(i);
          // scan the same position again
      }
  }

  bool has_java_call_sites() const {
      CallSiteType java_call = CallSiteType::java_call;
      int index = _call_site_entries->find(&java_call, [](void* token, CallSiteEntry* entry) -> bool {
          return *((CallSiteType*)token) == entry->call_site_type;
      });
      return index != -1;
  }

  bool contains_stack_map_instruction_offset(uint32_t instruction_offset) const {
      int index = _stack_map_entries->find(&instruction_offset, [](void* token, StackMapEntry* entry) -> bool {
          return *((uint32_t*)token) == entry->instruction_offset;
      });
      return index != -1;
  }

  StackMapEntry* get_stack_map_by_instruction_offset(uint32_t instruction_offset) const {
      int index = _stack_map_entries->find(&instruction_offset, [](void* token, StackMapEntry* entry) -> bool {
          return *((uint32_t*)token) == entry->instruction_offset;
      });
      if (index == -1) return NULL;
      return _stack_map_entries->at(index);
  }

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

  CallSiteEntry* get_call_site_by_helper_address_and_call_target_offset(uint64_t helper_address, uint64_t call_target_offset) const {
      if (!_call_site_entries) return NULL;
      std::pair<uint64_t, uint64_t> pair(helper_address, call_target_offset);
      int index = _call_site_entries->find(&pair, [](void* token, CallSiteEntry* entry) -> bool {
          auto [ha, cto] = *((std::pair<uint64_t, uint64_t>*)token);
          return ha == entry->helper_address && cto == entry->call_target_offset;
      });
      if (index == -1) return NULL;
      return _call_site_entries->at(index);
  }

    CallSiteEntry* get_call_site_by_helper_address_and_blr_offset(uint64_t helper_address, uint64_t blr_offset) const {
        if (!_call_site_entries) return NULL;
        std::pair<uint64_t, uint64_t> pair(helper_address, blr_offset);
        int index = _call_site_entries->find(&pair, [](void* token, CallSiteEntry* entry) -> bool {
            auto [ha, cto] = *((std::pair<uint64_t, uint64_t>*)token);
            return ha == entry->helper_address && cto == entry->blr_offset;
        });
        if (index == -1) return NULL;
        return _call_site_entries->at(index);
    }

    uint64_t get_call_site_blr_offset_by_helper_address_and_call_target_offset(uint64_t helper_address, uint64_t call_target_offset) const {
        if (!_call_site_entries) return 0;
        std::pair<uint64_t, uint64_t> pair(helper_address, call_target_offset);
        int index = _call_site_entries->find(&pair, [](void* token, CallSiteEntry* entry) -> bool {
            auto [ha, cto] = *((std::pair<uint64_t, uint64_t>*)token);
            return ha == entry->helper_address && cto == entry->call_target_offset;
        });
        if (index == -1) return 0;
        return _call_site_entries->at(index)->blr_offset;
    }

    DeoptBundle* get_deopt_bundle_by_instruction_offset(uint64_t instruction_offset) const {
      if (!_deopt_bundles) return NULL;
        int index = _deopt_bundles->find(&instruction_offset, [](void* token, DeoptBundle* entry) -> bool {
            return *((uint64_t*)token) == entry->instruction_offset;
        });
        if (index == -1) return NULL;
        return _deopt_bundles->at(index);
    }

  void update_call_site_machine_code_offsets(uint64_t virtual_offset,
                                             uint64_t return_pc_offset,
                                             uint64_t blr_offset,
                                             uint64_t call_target_offset = 0) const {
      if (!_call_site_entries) return;
      int index = _call_site_entries->find(&virtual_offset, [](void* token, CallSiteEntry* entry) -> bool {
          return *((uint64_t*)token) == entry->virtual_offset;
      });
      if (index == -1) return;
      _call_site_entries->at(index)->return_pc_offset = return_pc_offset;
      _call_site_entries->at(index)->blr_offset = blr_offset;
      if (call_target_offset) {
          _call_site_entries->at(index)->call_target_offset = call_target_offset;
      }
  }

  // stack map related functions
  void register_stack_map(uint32_t instruction_offset, uint8_t location_kind, uint32_t location_reg_num, int32_t location_offset, uint32_t constant = 0);

  void register_deopt_bundle(uint32_t instruction_offset, uint64_t bci);

  void register_deopt_bundle_local_data(uint32_t instruction_offset, uint8_t location_kind, uint32_t location_reg_num, int32_t location_offset, uint32_t constant = 0, uint8_t basic_type = T_VOID);

  void register_deopt_bundle_expression_stack_data(uint32_t instruction_offset, uint8_t location_kind, uint32_t location_reg_num, int32_t location_offset, uint32_t constant = 0, uint8_t basic_type = T_VOID);

  void register_deopt_bundle_monitor_data(uint32_t instruction_offset, uint8_t location_kind, uint32_t location_reg_num, int32_t location_offset, uint32_t constant = 0, uint8_t basic_type = T_OBJECT);

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

  void convert_and_add_to_real_recorder(DebugInformationRecorder* real_recorder,
                                       ciMethod* method,
                                       int plus_offset,
                                       int frame_size);
};

#endif // SHARE_VM_YUHU_YUHUDEBUGINFORMATIONRECORDER_HPP
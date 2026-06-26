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
    unwind_call = 5
};

struct CallSiteMachineCodeOffsets {
    uint64_t call_target_offset; // extracted by ORC plugin
    uint64_t blr_offset; // extracted by ORC plugin
    uint64_t return_pc_offset; // extracted by ORC plugin
};

struct CallSiteEntry {
    uint64_t virtual_offset; // generated at IR phase
    uint64_t virtual_address; // generated at IR phase
    uint64_t helper_address; // generated at IR phase
    CallSiteType call_site_type; // generated at IR phase
    int bci; // generated at IR phase
    int num_monitors; // generated at IR phase
    GrowableArray<CallSiteMachineCodeOffsets*>* machine_code_offsets; // same IR may be duplicated in machine code, it is 1:M relations.
};

struct StackMapLocation {
    uint8_t kind;
    uint32_t reg_num;
    int32_t offset;
    uint64_t constant;
};

struct StackMapEntry {
    uint32_t instruction_offset;
    GrowableArray<StackMapLocation*>* locations;
};

struct DeoptBundle {
    uint32_t instruction_offset;
    uint64_t bci;
    GrowableArray<uint8_t>* locals; // list of locals with basic type only...from 0 to max_locals-1
    GrowableArray<uint8_t>* expression_stacks; // list of expression stacks with basic type only...from 0 to stack_depth-1
    uint32_t num_monitors = 0; // num of monitors, 0 by default
};

struct FrameLayoutInfo {
    int total_frame_size_in_bytes = -1; // extracted from machine code's prologue
    int num_of_prologue_registers = -1; // extracted from machine code's prologue
    int header_words = -1; // initialized in YuhuStack::initialize method
    int monitor_words = -1; // initialized in YuhuStack::initialize method
    int stack_words = - 1; // initialized in YuhuStack::initialize method
    int locals_words = -1; // initialized in YuhuStack::initialize method
    int extended_frame_words = -1; // initialized in YuhuStack::initialize method
    int extended_frame_reg_num = -1; // extracted from stack map
    int extended_frame_kind = -1; // extracted from stack map
    int extended_frame_offset = -1; // extracted from stack map
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

  FrameLayoutInfo* _frame_layout_info;

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
                          int bci,
                          int num_monitors);

  void clean_eliminated_call_sites() {
      // remove call sites which don't appear in llvm machine code
      // this can happen because llvm passes may eliminate some blocks
      for (int i = 0; i < _call_site_entries->length(); ) {
          if (_call_site_entries->at(i)->machine_code_offsets->length()) {
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
          // search machine code offsets
          int cto_index = entry->machine_code_offsets->find(&cto, [](void* cto_token, CallSiteMachineCodeOffsets* cto_entry) -> bool {
              return *((uint64_t*)cto_token) == cto_entry->call_target_offset;
          });
          return ha == entry->helper_address && cto_index != -1;
      });
      if (index == -1) return NULL;
      return _call_site_entries->at(index);
  }

    CallSiteEntry* get_call_site_by_helper_address_and_blr_offset(uint64_t helper_address, uint64_t blr_offset) const {
        if (!_call_site_entries) return NULL;
        std::pair<uint64_t, uint64_t> pair(helper_address, blr_offset);
        int index = _call_site_entries->find(&pair, [](void* token, CallSiteEntry* entry) -> bool {
            auto [ha, bo] = *((std::pair<uint64_t, uint64_t>*)token);
            // search machine code offsets
            int bo_index = entry->machine_code_offsets->find(&bo, [](void* bo_token, CallSiteMachineCodeOffsets* bo_entry) -> bool {
                return *((uint64_t*)bo_token) == bo_entry->blr_offset;
            });

            return ha == entry->helper_address && bo_index != -1;
        });
        if (index == -1) return NULL;
        return _call_site_entries->at(index);
    }

    uint64_t get_call_site_blr_offset_by_helper_address_and_call_target_offset(uint64_t helper_address, uint64_t call_target_offset) const {
        if (!_call_site_entries) return 0;
        std::pair<uint64_t, uint64_t> pair(helper_address, call_target_offset);
        int index = _call_site_entries->find(&pair, [](void* token, CallSiteEntry* entry) -> bool {
            auto [ha, cto] = *((std::pair<uint64_t, uint64_t>*)token);
            // search machine code offsets
            int cto_index = entry->machine_code_offsets->find(&cto, [](void* cto_token, CallSiteMachineCodeOffsets* cto_entry) -> bool {
                return *((uint64_t*)cto_token) == cto_entry->call_target_offset;
            });
            return ha == entry->helper_address && cto_index != -1;
        });
        if (index == -1) return 0;
        int cto_index = _call_site_entries->at(index)->machine_code_offsets->find(&call_target_offset, [](void* cto_token, CallSiteMachineCodeOffsets* cto_entry) -> bool {
            return *((uint64_t*)cto_token) == cto_entry->call_target_offset;
        });
        assert(cto_index != -1, "should be valid index");
        return _call_site_entries->at(index)->machine_code_offsets->at(cto_index)->blr_offset;
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
      std::pair<uint64_t, uint64_t> pair(return_pc_offset, blr_offset);
      int mco_index = _call_site_entries->at(index)->machine_code_offsets->find(&pair, [](void* token, CallSiteMachineCodeOffsets* entry) -> bool {
          auto [rpo, bo] = *((std::pair<uint64_t, uint64_t> *) token);
          return rpo == entry->return_pc_offset && bo == entry->blr_offset;
      });
      if (mco_index != -1) {
          uint64_t old_cto = _call_site_entries->at(index)->machine_code_offsets->at(mco_index)->call_target_offset;
          if (call_target_offset == old_cto) {
              // if same call_target_offset, do nothing
              return;
          } else if (call_target_offset && old_cto == 0) {
              // if call_target_offset exists and old call_target_offset is 0, then update
              _call_site_entries->at(index)->machine_code_offsets->at(mco_index)->call_target_offset = call_target_offset;
              return;
          }
      }
      auto call_site_mco = new CallSiteMachineCodeOffsets();
      call_site_mco->return_pc_offset = return_pc_offset;
      call_site_mco->blr_offset = blr_offset;
      if (call_target_offset) {
          call_site_mco->call_target_offset = call_target_offset;
      }
      _call_site_entries->at(index)->machine_code_offsets->append(call_site_mco);
  }

  // stack map related functions
  void register_stack_map(uint32_t instruction_offset, uint8_t location_kind, uint32_t location_reg_num, int32_t location_offset, uint64_t constant = 0);

  void register_deopt_bundle(uint32_t instruction_offset, uint64_t bci);

  void register_deopt_bundle_local_data(uint32_t instruction_offset, uint8_t basic_type);

  void register_deopt_bundle_expression_stack_data(uint32_t instruction_offset, uint8_t basic_type);

  void register_deopt_bundle_monitor_data(uint32_t instruction_offset, uint32_t num_monitors);

  void register_frame_layout_info_with_frame_fields(int header_words, int monitor_words, int stack_words, int locals_words, int extended_frame_words);

  void register_frame_layout_info_with_prologue_fields(int total_frame_size_in_bytes, int num_of_prologue_registers);

  void register_frame_layout_info_with_stack_map_fields(int extended_frame_reg_num, int extended_frame_kind, int extended_frame_offset);

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
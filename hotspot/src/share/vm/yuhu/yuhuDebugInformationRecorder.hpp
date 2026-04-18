#ifndef SHARE_VM_YUHU_YUHUDEBUGINFORMATIONRECORDER_HPP
#define SHARE_VM_YUHU_YUHUDEBUGINFORMATIONRECORDER_HPP

#include "code/debugInfoRec.hpp"
#include "compiler/oopMap.hpp"
#include "ci/ciMethod.hpp"
#include "code/scopeDesc.hpp"
#include "yuhu/yuhuOffsetMapper.hpp"
#include "yuhu/yuhu_globals.hpp"
#include "runtime/threadLocalStorage.hpp"

namespace llvm {
    class Module;
}

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
  GrowableArray<int>* _call_site_virtual_offsets;
  GrowableArray<uint64_t>* _call_site_virtual_addresses;
  GrowableArray<uint64_t>* _call_site_helper_addresses;

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

  // Call site metadata methods (moved from YuhuFunction)
  void register_call_site(int virtual_offset, uint64_t virtual_address, uint64_t helper_address);
  void embed_call_site_metadata();
  
  int get_call_site_count() const {
    return _call_site_virtual_offsets ? _call_site_virtual_offsets->length() : 0;
  }
  
  int get_call_site_virtual_offset(int index) const {
    assert(index < get_call_site_count(), "index out of bounds");
    return _call_site_virtual_offsets->at(index);
  }
  
  uint64_t get_call_site_virtual_address(int index) const {
    assert(index < get_call_site_count(), "index out of bounds");
    return _call_site_virtual_addresses->at(index);
  }
  
  uint64_t get_call_site_helper_address(int index) const {
    assert(index < get_call_site_count(), "index out of bounds");
    return _call_site_helper_addresses->at(index);
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

  // 将虚拟 PC offset 转换为真实 PC offset，并将结果添加到真实的 DebugInformationRecorder
  void convert_and_add_to_real_recorder(DebugInformationRecorder* real_recorder,
                                       ciMethod* method,
                                       YuhuOffsetMapper* offset_mapper,
                                       int plus_offset) {
    // 按照真实的pc offset排序virtual offset
    if (_virtual_offsets->length() > 1) {
        quick_sort_by_actual_offset(0, _virtual_offsets->length() - 1, offset_mapper);
    }

    int last_real_offset = -1;
    
    // 处理 OopMap 信息
    for (int i = 0; i < _virtual_offsets->length(); i++) {
      int virtual_offset = _virtual_offsets->at(i);
      OopMap* oopmap = _oopmaps->at(i);
      
      // 使用 offset_mapper 将虚拟 offset 转换为真实 offset
      int real_offset = offset_mapper->get_actual_offset(virtual_offset) + plus_offset;
      
      if (real_offset != -1) {  // -1 表示未找到映射
          if (YuhuTraceOffset) {
              tty->print_cr("Yuhu: Converted virtual offset=%d to real offset=%d", virtual_offset, real_offset);
          }
        if (last_real_offset == real_offset) {
            continue;
        }
        
        // 添加到真实的 recorder
        real_recorder->add_safepoint(real_offset, oopmap);
        last_real_offset = real_offset;
        
        // 查找对应的帧信息
        for (int j = 0; j < _virtual_frame_offsets->length(); j++) {
          if (_virtual_frame_offsets->at(j) == virtual_offset) {
            ciMethod* target = _frame_targets->at(j);
            int bci = _frame_bcis->at(j);
            GrowableArray<ScopeValue*>* locals = _frame_locals->at(j);
            GrowableArray<ScopeValue*>* expressions = _frame_expressions->at(j);
            GrowableArray<MonitorValue*>* monitors = _frame_monitors->at(j);
            
            real_recorder->describe_scope(
              real_offset,
              target != NULL ? target : method,  // 如果没有特定目标，则使用主方法
              bci,
              false,  // reexecute
              false,  // rethrow_exception
              false,  // is_method_handle_invoke
              real_recorder->create_scope_values(locals),
              real_recorder->create_scope_values(expressions),
              real_recorder->create_monitor_values(monitors));
              
            real_recorder->end_safepoint(real_offset);
            break;
          }
        }
      } else {
      }
    }
  }
};

#endif // SHARE_VM_YUHU_YUHUDEBUGINFORMATIONRECORDER_HPP
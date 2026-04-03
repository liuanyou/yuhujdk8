#ifndef SHARE_VM_YUHU_YUHUDEBUGINFORMATIONRECORDER_HPP
#define SHARE_VM_YUHU_YUHUDEBUGINFORMATIONRECORDER_HPP

#include "code/debugInfoRec.hpp"
#include "compiler/oopMap.hpp"
#include "ci/ciMethod.hpp"
#include "code/scopeDesc.hpp"
#include "yuhu/yuhuOffsetMapper.hpp"
#include "yuhu/yuhu_globals.hpp"

// YuhuDebugInformationRecorder: 用于收集虚拟 PC offset 的 OopMap 信息
// 在 LLVM IR 生成期间收集信息，在机器码生成后转换为真实 PC offset
class YuhuDebugInformationRecorder : public ResourceObj {
private:
  // 存储虚拟 PC offset 的 OopMap 信息
  GrowableArray<int>* _virtual_offsets;
  GrowableArray<OopMap*>* _oopmaps;
  
  // 存储虚拟 PC offset 的帧信息（用于解缓存）
  GrowableArray<int>* _virtual_frame_offsets;
  GrowableArray<ciMethod*>* _frame_targets;
  GrowableArray<int>* _frame_bcis;
  GrowableArray<GrowableArray<ScopeValue*>*>* _frame_locals;
  GrowableArray<GrowableArray<ScopeValue*>*>* _frame_expressions;
  GrowableArray<GrowableArray<MonitorValue*>*>* _frame_monitors;

public:
  YuhuDebugInformationRecorder() {
    _virtual_offsets = new GrowableArray<int>();
    _oopmaps = new GrowableArray<OopMap*>();
    
    _virtual_frame_offsets = new GrowableArray<int>();
    _frame_targets = new GrowableArray<ciMethod*>();
    _frame_bcis = new GrowableArray<int>();
    _frame_locals = new GrowableArray<GrowableArray<ScopeValue*>*>();
    _frame_expressions = new GrowableArray<GrowableArray<ScopeValue*>*>();
    _frame_monitors = new GrowableArray<GrowableArray<MonitorValue*>*>();
  }

  // 添加虚拟 PC offset 的 safepoint 信息
  void add_safepoint(int virtual_pc_offset, OopMap* oopmap) {
    _virtual_offsets->append(virtual_pc_offset);
    _oopmaps->append(oopmap);
  }

  // 描述虚拟 PC offset 的作用域信息
  void describe_scope(int virtual_pc_offset,
                     ciMethod* method,
                     int bci,
                     bool reexecute,
                     bool rethrow_exception,
                     bool is_method_handle_invoke,
                     GrowableArray<ScopeValue*>* locals,
                     GrowableArray<ScopeValue*>* expressions,
                     GrowableArray<MonitorValue*>* monitors) {
    // 记录帧信息，等待后续转换
    _virtual_frame_offsets->append(virtual_pc_offset);
    _frame_targets->append(method);
    _frame_bcis->append(bci);
    _frame_locals->append(locals);
    _frame_expressions->append(expressions);
    _frame_monitors->append(monitors);
  }

  // 结束虚拟 PC offset 的 safepoint 记录
  void end_safepoint(int virtual_pc_offset) {
    // 实际上不需要做任何事情，因为我们已经记录了所需的所有信息
  }

  // 获取虚拟 PC offset 数组
  GrowableArray<int>* virtual_offsets() const { return _virtual_offsets; }
  GrowableArray<OopMap*>* oopmaps() const { return _oopmaps; }
  GrowableArray<int>* virtual_frame_offsets() const { return _virtual_frame_offsets; }
  GrowableArray<ciMethod*>* frame_targets() const { return _frame_targets; }
  GrowableArray<int>* frame_bcis() const { return _frame_bcis; }
  GrowableArray<GrowableArray<ScopeValue*>*>* frame_locals() const { return _frame_locals; }
  GrowableArray<GrowableArray<ScopeValue*>*>* frame_expressions() const { return _frame_expressions; }
  GrowableArray<GrowableArray<MonitorValue*>*>* frame_monitors() const { return _frame_monitors; }

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
#include "precompiled.hpp"
#include "yuhu/yuhuDebugInfo.hpp"
#include "yuhu/yuhuFunction.hpp"
#include "code/debugInfo.hpp"
#include "code/pcDesc.hpp"
#include "compiler/oopMap.hpp"

// 为方法生成最小的 scope descriptor
// 使用已经记录的 OopMapSet
void YuhuDebugInfo::generate_minimal_debug_info(DebugInformationRecorder* recorder,
                                                 ciMethod* method,
                                                 int frame_size,
                                                 YuhuFunction* function) {
  assert(recorder != NULL, "recorder must not be null");
  assert(method != NULL, "method must not be null");
  
  tty->print_cr("Yuhu: generate_minimal_debug_info called for method %s", method->name()->as_utf8());
  
  // If function is provided, process deferred OopMaps and frames
  // These were collected during IR generation with virtual PC offsets
  // After machine code generation, we now have the actual PC offsets from offset markers
  if (function != NULL) {
    GrowableArray<int>* deferred_offsets = function->deferred_offsets();
    GrowableArray<OopMap*>* deferred_oopmaps = function->deferred_oopmaps();
    GrowableArray<int>* deferred_frame_offsets = function->deferred_frame_offsets();
    GrowableArray<ciMethod*>* deferred_frame_targets = function->deferred_frame_targets();
    GrowableArray<int>* deferred_frame_bcis = function->deferred_frame_bcis();
    GrowableArray<GrowableArray<ScopeValue*>*>* deferred_frame_locals = function->deferred_frame_locals();
    GrowableArray<GrowableArray<ScopeValue*>*>* deferred_frame_expressions = function->deferred_frame_expressions();
    GrowableArray<GrowableArray<MonitorValue*>*>* deferred_frame_monitors = function->deferred_frame_monitors();
    
    if (deferred_offsets != NULL && deferred_oopmaps != NULL) {
      int num_deferred = deferred_offsets->length();
      tty->print_cr("Yuhu: Processing %d deferred OopMaps with actual PC offsets", num_deferred);
      
      // Create pairs of (offset, index) for sorting
      // We need to sort by offset to ensure proper ordering
      GrowableArray<int>* sorted_indices = new GrowableArray<int>(num_deferred);
      for (int i = 0; i < num_deferred; i++) {
        sorted_indices->append(i);
      }
      
      // Bubble sort indices by offset
      for (int i = 0; i < num_deferred; i++) {
        for (int j = i + 1; j < num_deferred; j++) {
          int idx_i = sorted_indices->at(i);
          int idx_j = sorted_indices->at(j);
          if (deferred_offsets->at(idx_i) > deferred_offsets->at(idx_j)) {
            sorted_indices->at_put(i, idx_j);
            sorted_indices->at_put(j, idx_i);
          }
        }
      }
      
      // Process OopMaps and frames in sorted order
      int last_recorded_offset = -1;
      for (int i = 0; i < num_deferred; i++) {
        int idx = sorted_indices->at(i);
        int pc_offset = deferred_offsets->at(idx);
        OopMap* oopmap = deferred_oopmaps->at(idx);
        
        // Skip duplicate offsets
        if (pc_offset <= last_recorded_offset) {
          tty->print_cr("Yuhu: Skipping duplicate pc_offset=%d (was %d)", pc_offset, last_recorded_offset);
          continue;
        }
        
        tty->print_cr("Yuhu: Recording safepoint at actual pc_offset=%d", pc_offset);
        
        // Now call add_safepoint with the actual PC offset
        recorder->add_safepoint(pc_offset, oopmap);
        
        // Find matching frame information if available
        int frame_idx = -1;
        if (deferred_frame_offsets != NULL) {
          for (int j = 0; j < deferred_frame_offsets->length(); j++) {
            if (deferred_frame_offsets->at(j) == pc_offset) {
              frame_idx = j;
              break;
            }
          }
        }
        
        if (frame_idx >= 0 && deferred_frame_targets != NULL) {
          // We have frame information for this offset
          ciMethod* target = deferred_frame_targets->at(frame_idx);
          int bci = deferred_frame_bcis->at(frame_idx);
          GrowableArray<ScopeValue*>* locals = deferred_frame_locals->at(frame_idx);
          GrowableArray<ScopeValue*>* expressions = deferred_frame_expressions->at(frame_idx);
          GrowableArray<MonitorValue*>* monitors = deferred_frame_monitors->at(frame_idx);
          
          tty->print_cr("Yuhu: Describing scope for target=%s, bci=%d", target->name()->as_utf8(), bci);
          
          recorder->describe_scope(
            pc_offset,
            target,
            bci,
            true,   // reexecute
            false,  // rethrow_exception
            false,  // is_method_handle_invoke
            recorder->create_scope_values(locals),
            recorder->create_scope_values(expressions),
            recorder->create_monitor_values(monitors));
        } else {
          // No frame information, use minimal scope
          tty->print_cr("Yuhu: No frame info for offset=%d, using minimal scope", pc_offset);
          
          recorder->describe_scope(
            pc_offset,
            method,
            0,      // bci = 0
            false,  // reexecute
            false,  // rethrow_exception
            false,  // is_method_handle_invoke
            recorder->create_scope_values(NULL),
            recorder->create_scope_values(NULL),
            recorder->create_monitor_values(NULL));
        }
        
        // End the safepoint
        recorder->end_safepoint(pc_offset);
        
        last_recorded_offset = pc_offset;
      }
      
      delete sorted_indices;
      tty->print_cr("Yuhu: Completed processing deferred OopMaps and frames");
      return;
    }
  }
  
  // Fallback: process existing OopMaps from recorder (old behavior)
  tty->print_cr("Yuhu: No deferred OopMaps provided, using fallback method");
  
  // 获取已经记录的 OopMapSet
  OopMapSet* oopmaps = recorder->_oopmaps;
  if (oopmaps == NULL || oopmaps->size() == 0) {
    tty->print_cr("Yuhu: Warning - No OopMaps found, cannot generate debug info");
    return;
  }
  
  tty->print_cr("Yuhu: Generating scope descriptors for %d OopMaps", oopmaps->size());
  
  // 问题修复：由于虚拟offset和物理PC offset可能存在顺序不一致的问题，
  // 我们需要先收集所有OopMap信息，然后按照offset排序后再添加到recorder中
  // 这样确保所有PC offset严格递增，满足DebugInformationRecorder的要求
  
  // 创建临时数组来存储OopMap及其offset，然后进行排序
  GrowableArray<OopMap*>* sorted_maps = new GrowableArray<OopMap*>(oopmaps->size());
  
  // 复制所有OopMap到临时数组
  for (int i = 0; i < oopmaps->size(); i++) {
    OopMap* oopmap = oopmaps->at(i);
    if (oopmap != NULL) {
      sorted_maps->append(oopmap);
    }
  }
  
  // 按照offset对OopMap进行排序 - 实现一个简单的冒泡排序
  for (int i = 0; i < sorted_maps->length(); i++) {
    for (int j = i + 1; j < sorted_maps->length(); j++) {
      OopMap* oopmap_i = sorted_maps->at(i);
      OopMap* oopmap_j = sorted_maps->at(j);
      if (oopmap_i->offset() > oopmap_j->offset()) {
        // 交换元素
        sorted_maps->at_put(i, oopmap_j);
        sorted_maps->at_put(j, oopmap_i);
      }
    }
  }
  
  // 按排序后的顺序添加到recorder中
  int last_recorded_offset = -1;  // 初始化为无效值
  for (int i = 0; i < sorted_maps->length(); i++) {
    OopMap* oopmap = sorted_maps->at(i);
    int pc_offset = oopmap->offset();
    
    // 跳过重复的offset
    if (pc_offset <= last_recorded_offset) {
      tty->print_cr("Yuhu: Skipping duplicate or non-increasing pc_offset=%d (was %d)", 
                   pc_offset, last_recorded_offset);
      continue;
    }
    
    tty->print_cr("Yuhu: Recording scope for OopMap %d at pc_offset=%d", i, pc_offset);
    record_scope_for_oopmap(recorder, method, pc_offset, oopmap);
    last_recorded_offset = pc_offset;
  }
  
  // 清理临时数组
  delete sorted_maps;
}

// 为特定的 OopMap 记录 scope descriptor
void YuhuDebugInfo::record_scope_for_oopmap(DebugInformationRecorder* recorder,
                                              ciMethod* method,
                                              int pc_offset,
                                              OopMap* oopmap) {
  // NOTE: OopMap 已经由 LLVM IR 生成阶段添加到 recorder->_oopmaps
  // 并在 relocate_oopmaps 中重定位和排序完成
  // 这里只需为已存在的 OopMap 创建对应的 scope descriptor
  // 使用 add_non_safepoint 避免重复添加 OopMap
  
  tty->print_cr("Yuhu: Adding non-safepoint for pc_offset=%d", pc_offset);
  
  // 添加非 safepoint PC 描述符（不会重复添加 OopMap，仅添加 PC 信息）
  recorder->add_non_safepoint(pc_offset);
  
  tty->print_cr("Yuhu: Describing scope for pc_offset=%d", pc_offset);
  
  // 描述 scope（为 deoptimization 提供必要的帧信息）
  int bci = 0;  // BCI=0 表示方法入口点
  bool reexecute = false;  // 不需要重新执行
  bool is_method_handle_invoke = false;
  bool return_oop = false;  // 简化处理
  
  // 创建空的 locals/expressions/monitors
  // NOTE: 这是最小实现，只支持帧遍历和 GC（通过 OopMap）
  // 如果将来需要逆优化 Yuhu 方法本身，必须记录完整的局部变量和表达式栈信息
  // 当前场景：Yuhu 方法调用其他方法，被调用方法逆优化时，只需要遍历 Yuhu 帧
  DebugToken* locals = recorder->create_scope_values(NULL);
  DebugToken* expressions = recorder->create_scope_values(NULL);
  DebugToken* monitors = recorder->create_monitor_values(NULL);
  
  recorder->describe_scope(pc_offset,
                           method,
                           bci,
                           reexecute,
                           is_method_handle_invoke,
                           return_oop,
                           locals,
                           expressions,
                           monitors);
  
  tty->print_cr("Yuhu: Completed describing scope for pc_offset=%d", pc_offset);
  
  // 结束非 safepoint 记录
  recorder->end_non_safepoint(pc_offset);
}

// 不再需要这个方法，但保留空实现以避免编译错误
void YuhuDebugInfo::record_entry_scope(DebugInformationRecorder* recorder,
                                        ciMethod* method,
                                        int frame_size) {
  // 已废弃 - 现在使用 record_scope_for_oopmap
}

// 不再需要这个方法，但保留空实现以避免编译错误
OopMap* YuhuDebugInfo::create_simple_oop_map(int frame_size) {
  // 已废弃 - 现在使用已经记录的 OopMap
  return NULL;
}

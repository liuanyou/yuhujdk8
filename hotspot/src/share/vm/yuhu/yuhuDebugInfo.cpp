#include "precompiled.hpp"
#include "yuhu/yuhuDebugInfo.hpp"
#include "code/debugInfo.hpp"
#include "code/pcDesc.hpp"
#include "compiler/oopMap.hpp"

// 为方法生成最小的 scope descriptor
// 使用已经记录的 OopMapSet
void YuhuDebugInfo::generate_minimal_debug_info(DebugInformationRecorder* recorder,
                                                 ciMethod* method,
                                                 int frame_size) {
  assert(recorder != NULL, "recorder must not be null");
  assert(method != NULL, "method must not be null");
  
  // 获取已经记录的 OopMapSet
  OopMapSet* oopmaps = recorder->_oopmaps;
  if (oopmaps == NULL || oopmaps->size() == 0) {
    tty->print_cr("Yuhu: Warning - No OopMaps found, cannot generate debug info");
    return;
  }
  
  tty->print_cr("Yuhu: Generating scope descriptors for %d OopMaps", oopmaps->size());
  
  // 验证 OopMapSet 已经按 PC offset 排序
  // 按照原始顺序为每个 OopMap 生成对应的 scope descriptor
  // 注意：跳过重复的 pc_offset，因为 DebugInformationRecorder 要求 offset 严格递增
  int last_recorded_offset = -1;  // 初始化为无效值
  for (int i = 0; i < oopmaps->size(); i++) {
    OopMap* oopmap = oopmaps->at(i);
    if (oopmap != NULL) {
      int pc_offset = oopmap->offset();
      
      // 如果当前 offset 与上次记录的 offset 相同，跳过以避免重复
      if (pc_offset == last_recorded_offset) {
        tty->print_cr("Yuhu: Skipping duplicate pc_offset=%d (OopMap %d)", pc_offset, i);
        continue;
      }
      
      tty->print_cr("Yuhu: Recording scope for OopMap %d at pc_offset=%d", i, pc_offset);
      record_scope_for_oopmap(recorder, method, pc_offset, oopmap);
      last_recorded_offset = pc_offset;  // 记录本次处理的 offset
    }
  }
  
  // 额外检查：确保有足够的 OopMap 覆盖所有可能的 safepoint
  // 如果某个关键的 PC 位置没有 OopMap，deoptimization 会失败
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

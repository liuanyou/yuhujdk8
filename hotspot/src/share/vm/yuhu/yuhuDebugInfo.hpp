#ifndef SHARE_VM_YUHU_YUHUDEBUGINFO_HPP
#define SHARE_VM_YUHU_YUHUDEBUGINFO_HPP

#include "code/debugInfoRec.hpp"
#include "ci/ciMethod.hpp"

class YuhuFunction;

// YuhuDebugInfo: 为 Yuhu 编译的方法生成最小的 scope descriptor
// 用于支持逆优化（deoptimization）
class YuhuDebugInfo : public AllStatic {
public:
  // 为方法生成最小的 scope descriptor
  // 使用已经记录的 OopMapSet，为每个 OopMap 生成对应的 scope
  static void generate_minimal_debug_info(DebugInformationRecorder* recorder,
                                           ciMethod* method,
                                           int frame_size,
                                           YuhuFunction* function = NULL);

private:
  // 为特定的 OopMap 记录 scope descriptor
  static void record_scope_for_oopmap(DebugInformationRecorder* recorder,
                                       ciMethod* method,
                                       int pc_offset,
                                       OopMap* oopmap);
  
  // 已废弃的方法，保留以避免编译错误
  static void record_entry_scope(DebugInformationRecorder* recorder,
                                  ciMethod* method,
                                  int frame_size);
  
  static OopMap* create_simple_oop_map(int frame_size);
};

#endif // SHARE_VM_YUHU_YUHUDEBUGINFO_HPP

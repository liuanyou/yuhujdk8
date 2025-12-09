# 活动 011: 编译失败触发 Method::set_not_compilable 断言

## 时间
2025-12-09

## 现象
- 崩溃位置：`method.cpp:785` 断言 `!CompilationPolicy::can_be_compiled(this, comp_level)` 失败。
- 发生在 Yuhu 编译任务（comp_level=6，自定义层级）被标记为“不可编译”时。
- 调用栈：`CompileBroker::invoke_compiler_on_method` → `Method::set_not_compilable_quietly` → `Method::set_not_compilable`。

## 原因分析（当前结论）
- `set_not_compilable` 仅针对 C1/C2 层级（1~4）或 `CompLevel_all` 设置“不可编译”标志。对自定义 comp_level=6 不会设置标志，最后断言仍为 “可编译” 而触发。
- 触发前提：编译任务被判定“失败/放弃”（如 `env->failing()` 为 true 或编译流程返回错误），Broker 尝试标记不可编译，进而落入该断言。

## 待确认的关键问题
1. **为什么本次编译被判定失败？**  
   - 需要确定 `env->failing()` 的来源（IR 构建/LLVM 编译/安装时）。
   - 是否有 `record_method_not_compilable` 或其他异常路径导致标记失败。

## 已添加的诊断日志
- 在 `yuhuCompiler.cpp::compile_method` 中，当 IR 构建后 `env->failing()` 为 true 时打印：
  ```
  Yuhu: compile failing during IR build for <name> entry_bci=<bci> comp_level=<level>
  ```
  便于识别构建阶段是否直接失败。

## 下一步建议（分析层面）
1. 重跑用例，观察是否出现新增的诊断日志，确认失败发生阶段。
2. 如果日志未触发，说明失败发生在 IR 之后，需在 generate_native_code/注册环节继续加点轻量日志（仅在 env->failing() 为 true 时打印）。
3. 如有新的 hs_err/replay，请关注编译任务的 “failed” 记录或 `env->failing` 触发点。

## 暂不修改逻辑
- 当前仅增加诊断打印，未改动行为。待定位到具体失败点后再决定修复策略：
  - 若是 comp_level=6 不被策略支持：需要策略层或 set_not_compilable 对自定义层级的处理。
  - 若是编译流程自身失败：修正具体报错路径，避免触发 not_compilable。


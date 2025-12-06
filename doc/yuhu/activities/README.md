# Yuhu 编译器开发活动记录

本目录记录了 Yuhu 编译器开发过程中的重要活动和问题解决过程。

## 活动列表

### [活动 001: LLVM 目标初始化错误修复](001_llvm_target_initialization_error.md)

**日期**: 2024-12-XX  
**问题**: `Error while creating Yuhu JIT: Unable to find target for this triple (no targets are registered)`

**摘要**: 解决了 LLVM ExecutionEngine 创建时无法找到目标架构的问题。通过完整的 AArch64 目标初始化、强制链接 MCJIT 和 Interpreter、以及正确设置 Target Triple 来解决。

**关键修改**:
- 添加完整的 AArch64 目标初始化（6 个初始化函数）
- 添加强制链接调用 `LLVMLinkInMCJIT()` 和 `LLVMLinkInInterpreter()`
- 在 `yuhuContext.cpp` 中设置正确的 Target Triple
- 更新 Makefile 链接选项

---

### [活动 002: 使用 LLVM MC 框架替代 Keystone](002_replace_keystone_with_llvm_mc.md)

**日期**: 2024-12-XX  
**问题**: Keystone 库与 LLVM 库的符号冲突导致链接错误

**摘要**: 使用 LLVM 的 MC (Machine Code) 框架完全替代 Keystone 库，避免了符号冲突，减少了依赖，并实现了统一的工具链。

**关键修改**:
- 移除 Keystone 相关代码和依赖
- 实现 LLVM MC 版本的 `machine_code` 方法
- 区分可复用组件和状态组件的生命周期管理
- 处理 C++11/C++17 兼容性问题
- 处理 HotSpot 宏冲突
- 更新构建系统（移除 Keystone 链接）

**技术要点**:
- 可复用组件：在构造函数中初始化一次
- 状态组件：每次 `machine_code` 调用时创建新的
- 使用前向声明避免在头文件中包含 LLVM 头文件
- 使用 C++17 编译实现文件，C++11 编译头文件

---

### [活动 003: Yuhu 触发机制实现（阶段 1）](003_yuhu_trigger_implementation.md)

**日期**: 2024-12-XX  
**功能**: 实现基于调用次数 + 回边次数 + 复杂度评估的 Yuhu 编译器触发机制

**摘要**: 实现了阶段 1 的 Yuhu 编译器触发机制，当方法满足热点条件（调用次数 >= 200 或回边次数 >= 60000）且复杂度超过阈值（默认 5000）时，自动触发 Yuhu 编译器。

**关键修改**:
- 在 `globals.hpp` 中添加三个命令行选项：
  - `UseYuhuCompiler`: 启用 Yuhu 编译器
  - `YuhuComplexityThreshold`: 复杂度阈值（默认 5000）
  - `YuhuUseComplexityBased`: 启用基于复杂度的选择
- 在 `simpleThresholdPolicy.cpp` 中实现：
  - `calculate_complexity_score()`: 计算方法的复杂度分数
  - `should_compile_with_yuhu()`: 判断是否应该使用 Yuhu 编译器
- 修改 `call_event()` 和 `loop_event()` 集成 Yuhu 选择逻辑

**复杂度计算公式**:
```
complexity = code_size * (num_blocks + 1) * (has_loops ? 2 : 1)
```

**触发条件**:
1. Yuhu 编译器已启用（`-XX:+UseYuhuCompiler`）
2. 方法为热点（调用次数 >= 200 或回边次数 >= 60000）
3. 复杂度 >= 5000（可通过 `-XX:YuhuComplexityThreshold` 调整）

---

## 活动记录格式

每个活动文档包含以下部分：

1. **问题描述** - 详细描述遇到的问题和错误信息
2. **错误原因分析** - 深入分析问题的根本原因
3. **解决方案** - 详细的解决步骤和代码修改
4. **关键修改点** - 列出所有修改的文件和关键代码
5. **验证方法** - 如何验证修复是否成功
6. **经验总结** - 从问题中学到的经验和教训
7. **相关文件** - 涉及的所有文件列表
8. **参考文档** - 相关的技术文档和资源

## 如何添加新活动

1. 创建新的活动文档，命名格式：`NNN_description.md`（NNN 为三位数字序号）
2. 按照上述格式编写活动记录
3. 在本 README 中添加活动摘要
4. 更新主 README.md 中的链接（如需要）

## 相关文档

- [开发日志](../DEVELOPMENT_LOG.md) - 完整的开发过程记录
- [问题和解决方案](../ISSUES_AND_SOLUTIONS.md) - 问题跟踪表
- [构建说明](../BUILD_INSTRUCTIONS.md) - 构建相关的问题和解决方案


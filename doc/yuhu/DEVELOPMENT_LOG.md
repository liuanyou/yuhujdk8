# Yuhu 编译器开发日志

## 项目概述

**目标**：基于 Shark 编译器实现，创建一个使用 LLVM 的 Yuhu JIT 编译器，适配 AArch64 平台。

**策略**：直接迁移 Shark 的完整实现，然后进行平台和命名适配。

**开始日期**：2024-12-07

---

## 开发阶段

### Phase 1: 文件迁移和重命名 ✅

**日期**：2024-12-07

**完成的工作**：

1. **复制 Shark 文件**
   ```bash
   cp -r hotspot/src/share/vm/shark hotspot/src/share/vm/yuhu
   ```

2. **批量重命名文件**
   - 所有 `shark*` 文件重命名为 `yuhu*`
   - 约 45 个文件被重命名

3. **批量替换内容**
   - `Shark` → `Yuhu`
   - `shark` → `yuhu`
   - `SHARK` → `YUHU`
   - 使用 `sed` 命令批量替换所有 `.cpp` 和 `.hpp` 文件

4. **更新 Include Guards**
   - 所有头文件的 include guards 更新为 `SHARE_VM_YUHU_*`
   - 例如：`SHARE_VM_YUHU_YUHU_COMPILER_HPP`

**文件列表**：
- `yuhuCompiler.cpp/hpp` - 主编译器
- `yuhuContext.cpp/hpp` - LLVM 上下文管理
- `yuhuFunction.cpp/hpp` - 函数 IR 生成
- `yuhuBuilder.cpp/hpp` - IR 构建器
- `yuhuStack.cpp/hpp` - 栈管理
- `yuhuMemoryManager.cpp/hpp` - 内存管理
- `yuhuRuntime.cpp/hpp` - 运行时调用
- 以及其他约 30+ 个文件

---

### Phase 2: 平台适配 ⚠️ 进行中

**日期**：2024-12-07

**完成的工作**：

1. **LLVM 目标初始化**
   - 修改 `yuhuCompiler.cpp` 中的目标初始化
   - 从 `InitializeNativeTarget()` 改为 `InitializeAArch64Target()`
   - 从 `InitializeNativeTargetAsmPrinter()` 改为 `InitializeAArch64TargetAsmPrinter()`

2. **条件编译处理**
   - `yuhu_globals.hpp` - 添加了 Zero 架构的条件编译注释
   - `yuhuRuntime.cpp` - 添加了 Zero 架构的条件编译注释

**待处理的问题**：

1. **ZeroStack 适配** ⚠️
   - ZeroStack 是 Zero 架构特有的栈管理机制
   - AArch64 使用标准栈管理，不需要 ZeroStack
   - 需要处理的文件：
     - `yuhuStack.hpp` - 使用 ZeroStack
     - `yuhuContext.cpp` - 定义 `zeroStack_type`
     - `yuhuBuilder.cpp` - 引用 `ZeroStack::handle_overflow`
   - **解决方案选项**：
     - Option A: 条件编译（仅在 `TARGET_ARCH_zero` 时编译）
     - Option B: 实现 AArch64 等效的栈管理
     - Option C: 暂时禁用，后续添加

---

### Phase 3: CompileBroker 集成 ✅

**日期**：2024-12-07

**完成的工作**：

1. **添加头文件包含**
   ```cpp
   #ifdef YUHU
   #include "yuhu/yuhuCompiler.hpp"
   #endif
   ```

2. **扩展编译器数组**
   - 从 `_compilers[2]` 扩展到 `_compilers[3]`
   - `[0]` = C1, `[1]` = C2, `[2]` = YUHU

3. **实现 YUHU 与 C1/C2 并存**
   - 修改 `compilation_init()` 让 YUHU 与 C1/C2 同时初始化
   - YUHU 不再替代 C2，而是作为第三个编译器
   - 添加独立的 YUHU 编译队列 `_yuhu_method_queue`

4. **更新编译器选择逻辑**
   - `compiler()` 函数支持 YUHU 编译级别
   - `compile_queue()` 函数支持 YUHU 队列
   - 支持 `CompLevel_yuhu_fast` (5) 和 `CompLevel_yuhu_optimized` (6)

5. **更新编译器线程初始化**
   - `init_compiler_threads()` 支持 YUHU 线程参数
   - 创建独立的 YUHU 编译器线程

6. **更新编译器检查**
   - 在 `compile_method()` 中添加 `is_yuhu()` 检查
   - 在 `init_compiler_runtime()` 中排除 Yuhu 的 `initialize()` 调用

7. **更新断言和其他函数**
   - 在 `init_compiler_threads()` 的断言中排除 YUHU
   - `mark_on_stack()` 支持 YUHU 队列

8. **添加编译器识别方法**
   - 在 `yuhuCompiler.hpp` 中添加：
     ```cpp
     bool is_yuhu() { return true; }
     bool is_c1() { return false; }
     bool is_c2() { return false; }
     bool is_shark() { return false; }
     ```

**集成模式**：
- 使用 `_compilers[2]`（独立的 YUHU 槽位）
- YUHU 与 C1、C2 可以同时存在
- 独立的编译队列和线程
- 在构造函数中完成初始化（类似 Shark）
- 不需要调用 `initialize()`

---

## 关键发现

### 1. aarch64.ad 不需要

**发现**：使用 LLVM 实现 Yuhu 编译器时，`aarch64.ad` 文件完全不需要。

**原因**：
- C2 编译器流程：字节码 → Ideal Graph → Matcher (使用 aarch64.ad) → 机器码
- Yuhu 编译器流程：字节码 → LLVM IR → LLVM ExecutionEngine → LLVM AArch64 后端 → 机器码
- LLVM 内置了完整的 AArch64 后端，包括指令选择、寄存器分配、指令调度等
- 所有架构相关的代码生成都由 LLVM 处理

**优势**：
- 简化实现：不需要维护架构描述文件
- 跨平台：LLVM 支持多个平台
- 优化质量：LLVM 后端经过充分优化和测试
- 维护成本低：架构相关的优化由 LLVM 团队维护

**文档**：详见 `doc/YUHU_VS_C2_ARCHITECTURE.md`

---

## 当前状态

### ✅ 已完成
- [x] 文件复制和重命名
- [x] 内容替换（Shark → Yuhu）
- [x] Include guards 更新
- [x] 平台初始化（AArch64）
- [x] CompileBroker 集成
- [x] 编译器识别方法

### ✅ 已完成（新增）
- [x] ZeroStack 适配（采用 Option B：实现 AArch64 等效）
- [x] YUHU 与 C1、C2 并存实现

### 📋 待处理
- [ ] 修复编译错误（LLVM 头文件路径）
- [ ] 更新构建系统（Makefiles）
- [ ] 安装和配置 LLVM
- [ ] 测试基本编译功能
- [ ] 处理 LLVM 版本兼容性
- [ ] 编译策略支持 YUHU 编译级别
- [ ] 测试 OSR 编译
- [ ] 测试异常处理
- [ ] 性能测试和优化

---

## 下一步计划

1. **处理 ZeroStack 问题**
   - 决定采用哪种方案（条件编译/实现等效/暂时禁用）
   - 实现选定的方案

2. **修复编译错误**
   - 编译项目，识别所有编译错误
   - 逐个修复

3. **更新构建系统**
   - 更新 Makefiles 以包含 Yuhu 文件
   - 添加 YUHU 宏定义

4. **测试**
   - 测试基本编译功能
   - 测试简单方法（getter/setter）
   - 测试 OSR
   - 测试异常处理

---

## 相关文档

- `MIGRATION_STEPS.md` - 详细的迁移步骤
- `INTEGRATION_DETAILS.md` - CompileBroker 集成详情
- `ISSUES_AND_SOLUTIONS.md` - 遇到的问题和解决方案
- `ARCHITECTURE_COMPARISON.md` - Yuhu vs C2 架构对比

---

## 更新日志

- **2024-12-07**: 初始开发日志创建
  - 记录 Phase 1-3 的完成情况
  - 记录关键发现（aarch64.ad 不需要）
  - 记录当前状态和下一步计划

- **2024-12-07**: 更新 Phase 3 和 Phase 2
  - Phase 2: ZeroStack 适配完成（采用 Option B）
  - Phase 3: 实现 YUHU 与 C1、C2 并存
  - 扩展编译器数组到 3 个槽位
  - 添加独立的 YUHU 编译队列和线程


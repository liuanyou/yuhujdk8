# 问题和解决方案

## 概述

本文档记录 Yuhu 编译器开发过程中遇到的问题及其解决方案。

---

## 问题 1: ZeroStack 适配

### 问题描述

ZeroStack 是 Zero 架构特有的栈管理机制，但 AArch64 使用标准栈管理，不需要 ZeroStack。

### 影响范围

以下文件使用了 ZeroStack：
- `yuhuStack.hpp` - 使用 `ZeroStack::base_offset()`, `ZeroStack::sp_offset()`
- `yuhuContext.cpp` - 定义 `zeroStack_type`
- `yuhuBuilder.cpp` - 引用 `ZeroStack::handle_overflow`
- `yuhuRuntime.cpp` - 条件包含 `stack_zero.inline.hpp`

### 解决方案选项

#### Option A: 条件编译（推荐）

**优点**:
- 保持代码完整性
- 支持未来可能的 Zero 架构支持
- 最小化代码修改

**实现**:
```cpp
#ifdef TARGET_ARCH_zero
  // ZeroStack 相关代码
  llvm::Value* zero_stack() const {
    return builder()->CreateAddressOfStructEntry(
      thread(),
      JavaThread::zero_stack_offset(),
      YuhuType::zeroStack_type(),
      "zero_stack");
  }
#else
  // AArch64 使用标准栈管理
  // 可能需要实现等效的栈访问方法
#endif
```

#### Option B: 实现 AArch64 等效

**优点**:
- 完全适配 AArch64
- 不依赖条件编译

**缺点**:
- 需要深入了解 AArch64 栈布局
- 工作量较大

**实现**:
- 研究 AArch64 的栈布局（`frame_aarch64.hpp`）
- 实现等效的栈访问方法
- 替换所有 ZeroStack 引用

#### Option C: 暂时禁用

**优点**:
- 快速推进
- 最小化修改

**缺点**:
- 功能不完整
- 可能需要后续重构

**实现**:
- 注释掉 ZeroStack 相关代码
- 添加 TODO 标记
- 后续实现

### 当前状态

**状态**: ✅ 已完成 - 采用 Option B（实现 AArch64 等效）

**决策**: 采用 Option B，因为：
1. 完全适配 AArch64，不依赖条件编译
2. 代码更清晰，不需要维护 Zero 架构的代码
3. 性能更好，直接使用 AArch64 的栈管理

**实施完成**:
1. ✅ 移除 `_zeroStack_type` 定义（`yuhuContext.cpp/hpp`）
2. ✅ 移除 `zeroStack_type()` 方法（`yuhuType.hpp`）
3. ✅ 实现 AArch64 栈溢出检查（`yuhuStack.cpp` - `CreateStackOverflowCheck`）
4. ✅ 修改 `tos_at()` 使用标准 frame API（`yuhuRuntime.hpp`）
5. ✅ 修改 `last_frame()` 使用标准 frame API（`yuhuRuntime.hpp`）
6. ✅ 重新实现 `uncommon_trap()` 使用标准 frame API（`yuhuRuntime.cpp`）
7. ✅ 移除 `FakeStubFrame`（Zero 架构特有，AArch64 不需要）

**关键修改**:
- `yuhuStack.cpp`: 实现 AArch64 栈溢出检查，只检查 ABI 栈
- `yuhuRuntime.hpp`: `tos_at()` 使用 `frame::interpreter_frame_tos_address()`
- `yuhuRuntime.cpp`: `uncommon_trap()` 使用标准 deoptimization API
3. 替换 `ZeroStack::handle_overflow` 为 `SharedRuntime::throw_StackOverflowError`
4. 移除或保留（但不使用）`zeroStack_type`
5. 使用标准的栈指针和帧指针访问方式

---

## 问题 2: aarch64.ad 的使用

### 问题描述

**问题**: 如果使用 LLVM 来实现 Yuhu 编译器，那么 `aarch64.ad` 是不是就用不到了？

### 答案

**是的，`aarch64.ad` 对 Yuhu 编译器完全不需要。**

### 原因分析

#### C2 编译器的流程
```
字节码 → Ideal Graph → Matcher (使用 aarch64.ad) → 机器码
```

#### Yuhu 编译器的流程
```
字节码 → LLVM IR → LLVM ExecutionEngine → LLVM AArch64 后端 → 机器码
```

### 关键代码

**yuhuCompiler.cpp:304**:
```cpp
code = (address) execution_engine()->getPointerToFunction(function);
```

这里直接调用 LLVM ExecutionEngine，LLVM 内部会：
1. 使用 LLVM 的 AArch64 后端进行指令选择
2. 使用 LLVM 的寄存器分配器
3. 使用 LLVM 的指令调度器
4. 生成 AArch64 机器码

**完全不经过 `aarch64.ad`！**

### 优势

1. **简化实现**: 不需要维护架构描述文件
2. **跨平台**: LLVM 支持多个平台，只需调用对应的后端
3. **优化质量**: LLVM 的后端经过充分优化和测试
4. **维护成本低**: 架构相关的优化由 LLVM 团队维护

### 文档

详见 `doc/YUHU_VS_C2_ARCHITECTURE.md`

---

## 问题 3: Include Guards 格式

### 问题描述

在批量替换后，某些 include guards 的格式可能不正确。

### 示例

**错误格式**:
```cpp
#ifndef SHARE_VM_YUHU_YUHUCOMPILER_HPP
#define SHARE_VM_YUHU_YUHUCOMPILER_HPP
```

**正确格式**:
```cpp
#ifndef SHARE_VM_YUHU_YUHU_COMPILER_HPP
#define SHARE_VM_YUHU_YUHU_COMPILER_HPP
```

### 解决方案

手动检查并修复所有 include guards，确保：
1. 使用下划线分隔单词
2. 格式一致
3. 与文件名匹配

### 当前状态

**状态**: ✅ 已修复

**修复文件**:
- `yuhuCompiler.hpp` - 已更新为 `SHARE_VM_YUHU_YUHU_COMPILER_HPP`

---

## 问题 4: 编译器识别方法缺失

### 问题描述

`YuhuCompiler` 类缺少 `is_yuhu()` 等编译器识别方法。

### 错误信息

编译时可能出现：
```
error: 'class YuhuCompiler' has no member named 'is_yuhu'
```

### 解决方案

在 `yuhuCompiler.hpp` 中添加：

```cpp
class YuhuCompiler : public AbstractCompiler {
public:
  // Compiler identification
  bool is_yuhu()         { return true; }
  bool is_c1()           { return false; }
  bool is_c2()           { return false; }
  bool is_shark()        { return false; }
  // ...
};
```

### 当前状态

**状态**: ✅ 已修复

---

## 问题 5: 平台初始化

### 问题描述

Shark 使用 `InitializeNativeTarget()`，但 Yuhu 需要针对 AArch64 的初始化。

### 解决方案

修改 `yuhuCompiler.cpp`:

```cpp
// Initialize the native target (AArch64)
#ifdef TARGET_ARCH_aarch64
  InitializeAArch64Target();
  InitializeAArch64TargetAsmPrinter();
#else
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
#endif
```

### 当前状态

**状态**: ✅ 已修复

---

## 问题 6: CompileBroker 集成遗漏

### 问题描述

在 `compileBroker.cpp` 中需要添加多个集成点，可能遗漏某些检查。

### 需要更新的位置

1. ✅ 头文件包含
2. ✅ 编译器初始化
3. ✅ 编译器检查（`is_c2() || is_shark()`）
4. ✅ 初始化检查（`!comp->is_shark()`）
5. ✅ 断言更新

### 当前状态

**状态**: ✅ 已完成

---

## 问题 7: LLVM 头文件路径

### 问题描述

编译时找不到 LLVM 头文件，特别是 `llvm/IR/Verifier.h` 或 `llvm/Analysis/Verifier.h`。

### 错误信息

```
fatal error: 'llvm/IR/Verifier.h' file not found
```

### 原因

- LLVM 未安装
- LLVM 路径未配置
- LLVM 版本不兼容（Verifier.h 位置在不同版本中不同）

### 解决方案

1. **安装 LLVM**（见 `BUILD_INSTRUCTIONS.md`）
2. **配置构建系统**（见 `BUILD_INSTRUCTIONS.md`）
3. **暂时不编译 YUHU**：如果不定义 `YUHU` 宏，所有 YUHU 代码都会被条件编译排除

### 当前状态

**状态**: ⚠️ 待处理

**说明**: 需要安装和配置 LLVM 才能编译 YUHU 编译器。

---

## 问题 8: YUHU 与 C1/C2 并存

### 问题描述

用户希望 YUHU 与 C1、C2 编译器并存，而不是替代它们。

### 解决方案

**状态**: ✅ 已完成

**实现**:
1. 扩展编译器数组到 3 个槽位
2. YUHU 使用 `_compilers[2]` 独立槽位
3. 添加独立的 YUHU 编译队列
4. 支持 YUHU 编译级别（`CompLevel_yuhu_fast` 和 `CompLevel_yuhu_optimized`）

**详情**: 见 `COEXISTENCE_IMPLEMENTATION.md`

---

## 待解决问题

### 1. 编译策略支持

**优先级**: 中

**状态**: ⚠️ 待处理

**问题**: 需要修改 `CompilationPolicy` 来支持 YUHU 编译级别的选择

**说明**: 目前 YUHU 编译器会在定义 `YUHU` 宏时自动创建，但需要策略层支持才能实际使用 YUHU 编译级别进行编译。

---

## 问题跟踪

| 问题 | 状态 | 优先级 | 解决方案 |
|------|------|--------|----------|
| ZeroStack 适配 | ✅ 已完成 | 高 | Option B（AArch64 等效） |
| aarch64.ad 使用 | ✅ 已解决 | 中 | 不需要 |
| Include Guards | ✅ 已修复 | 中 | 手动修复 |
| 编译器识别方法 | ✅ 已修复 | 高 | 添加方法 |
| 平台初始化 | ✅ 已修复 | 高 | AArch64 初始化 |
| CompileBroker 集成 | ✅ 已完成 | 高 | 完整集成（并存模式） |
| LLVM 头文件路径 | ⚠️ 待处理 | 高 | 安装和配置 LLVM |
| YUHU 与 C1/C2 并存 | ✅ 已完成 | 高 | 扩展数组和队列 |
| 编译策略支持 | ⚠️ 待处理 | 中 | 修改 CompilationPolicy |

---

## 经验总结

1. **批量替换后需要仔细检查**: 自动化工具可能无法处理所有情况
2. **参考现有实现**: Shark 的实现是很好的参考
3. **条件编译很重要**: 不同架构需要不同的处理
4. **文档记录**: 及时记录问题和解决方案，避免重复工作


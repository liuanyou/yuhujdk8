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

### [活动 004: Function 没有 parent Module 导致 DataLayout 无法访问](004_function_no_parent_module.md)

**日期**: 2025-12-06  
**问题**: `Function has no parent Module` - Function 创建时没有指定 Module，导致 IRBuilder 无法访问 DataLayout

**摘要**: 发现 `Function::Create` 使用了不包含 Module 参数的重载版本，导致 Function 没有 parent Module。当 IRBuilder 尝试通过 `func->getParent()->getDataLayout()` 访问 DataLayout 时失败。

**根本原因**:
- `Function::Create` 有两个重载版本：一个会自动添加到 Module，另一个不会
- 当前代码使用了不会自动添加的版本
- Function 要等到编译完成后才通过 `add_function` 添加到 Module，但此时已经在生成 IR 了

**解决方案**:
- 方案 1（推荐）：在创建 Function 时传入 Module 参数
- 方案 2：创建 Function 后立即调用 `add_function` 添加到 Module

**关键发现**:
- DataLayout 已正确设置到 Module（已验证）
- 问题在于 Function 没有 parent Module，无法通过 `func->getParent()` 访问 Module
- IRBuilder 在 LLVM 20 中需要 Module 的 DataLayout 来计算对齐

---

### [活动 005: 帧大小不匹配错误](005_frame_size_mismatch.md)

**日期**: 2025-12-06  
**问题**: `assert(offset == extended_frame_size()) failed: should do` - 帧大小计算与实际分配不匹配

**摘要**: 发现 `header_words = 2` 的定义不正确，导致 `extended_frame_size` 计算错误。实际分配的帧头包含 6 个 slot（oop_tmp, method, unextended_sp, pc, frame_marker, frame_pointer_addr），但 `header_words` 只计算了 2 个（frame_marker + frame_pointer_addr）。

**根本原因**:
- `header_words = 2` 只包含了 Frame marker 和 Frame pointer address
- 但实际分配的帧头包含 6 个 slot
- `extended_frame_size = 2 + monitor_words + stack_words + locals_words`
- 实际 `offset = stack_words + monitor_words + 6 + locals_words`
- 差异：多 4 个 slot

**解决方案**:
- 将 `header_words` 从 2 改为 6，以匹配实际分配的 slot 数量
- 参考 Shark 的实现，`SharkFrame::header_words = 6`

**关键发现**:
- Shark 的 `header_words = 6` 包含了所有帧头元数据
- Yuhu 的注释错误地认为 `header_words = 2` 是 "frame pointer + return address"
- 实际上，AArch64 的 frame pointer 和 return address 由 ABI 管理，不在 Java 帧布局中
- Java 帧布局中的 "header" 是指 Java 运行时需要的元数据

---

### [活动 006: getPointerToFunction 返回 NULL](006_getpointertofunction_returns_null.md)

**日期**: 2025-12-06  
**问题**: `assert(code != NULL) failed: code must be != NULL` - `ExecutionEngine::getPointerToFunction()` 返回 NULL

**摘要**: 发现 `execution_engine()->getPointerToFunction(function)` 返回 NULL，导致代码生成失败。可能的原因包括：Function 不在 ExecutionEngine 的 Module 中、Module 所有权转移问题、IR 验证失败、代码生成错误、或 MemoryManager 实现问题。

**根本原因分析**:
- **Module 所有权问题**：在 LLVM 20 中，`ExecutionEngine` 通过 `EngineBuilder` 取得 Module 的所有权，但 `YuhuContext` 仍持有指向 Module 的指针，可能失效
- **Function 添加时机**：Function 在创建时添加到 Module，但 Module 的所有权已转移给 `ExecutionEngine`，可能导致指针失效
- **IR 验证或代码生成失败**：LLVM IR 可能存在错误，导致代码生成失败
- **MemoryManager 实现**：`YuhuMemoryManager` 的 `allocateCodeSection` 和 `finalizeMemory` 可能未正确实现

**调试建议**:
- 验证 Function 是否在 Module 中
- 验证 Module 指针的有效性
- 检查 ExecutionEngine 的错误信息
- 验证 IR 的正确性
- 检查 MemoryManager 实现

**关键发现**:
- `getPointerToFunction` 返回 NULL 通常表示代码生成失败
- 需要检查 Module 所有权、Function 添加时机、IR 验证、和 MemoryManager 实现
- 与 Shark 的差异：Shark 显式调用 `add_function()`，Yuhu 移除了这个调用

---

### [活动 007: execution_engine_lock 断言失败](007_execution_engine_lock_assertion_failure.md)

**日期**: 2025-12-06  
**问题**: `assert(execution_engine_lock()->owned_by_self()) failed: should be` - 调试代码在锁外调用了 `execution_engine()`

**摘要**: 发现调试代码在 `MutexLocker` 块外调用了 `execution_engine()` 方法，但该方法要求调用者必须持有 `execution_engine_lock()`。调试代码被放在了锁外（第 455-513 行），但锁的获取在第 515 行，导致断言失败。

**根本原因**:
- 调试代码在锁外执行（第 455-513 行）
- 第 484 和 487 行调用了 `execution_engine()`
- 但 `execution_engine()` 方法要求持有锁（`yuhuCompiler.hpp:121`）
- 锁在第 515 行才被获取

**解决方案**:
- 方案 1（推荐）：将需要访问 `execution_engine()` 的调试代码移到 `MutexLocker` 块内
- 方案 2：直接访问 `_execution_engine` 成员变量（不推荐，违反封装）
- 方案 3：创建无锁检查方法（不推荐，增加复杂度）

**关键发现**:
- `execution_engine()` 和 `memory_manager()` 方法都要求持有锁，这是为了避免 LLVM 锁和 HotSpot 锁交错导致死锁
- 调试代码应该遵循与正常代码相同的锁规则
- 需要将需要锁的调试代码和不需要锁的调试代码分开

---

### [活动 008: LLVM frameaddress intrinsic 名称 mangling 错误](008_llvm_frameaddress_intrinsic_name_mangling.md)

**日期**: 2025-12-06  
**问题**: `Intrinsic name not mangled correctly for type arguments! Should be: llvm.frameaddress.p0` - IR 验证失败

**摘要**: 发现 `llvm.frameaddress` intrinsic 在 LLVM 20 中需要使用新的命名规则，必须包含类型信息（`.p0` 表示指针类型）。当前代码使用旧的名称 `"llvm.frameaddress"`，导致 IR 验证失败。

**根本原因**:
- LLVM 20 引入了 opaque pointer types，intrinsic 的类型信息不再可以从指针类型推断
- Intrinsic 名称必须显式包含类型信息，如 `llvm.frameaddress.p0`
- 当前代码使用旧的名称 `"llvm.frameaddress"`，缺少类型后缀

**解决方案**:
- 方案 1（推荐）：修改 `frame_address()` 方法，使用正确的名称 `"llvm.frameaddress.p0"`
- 方案 2：使用 `Intrinsic::getDeclaration` API（更标准但需要更多代码）

**关键发现**:
- LLVM 20 的 intrinsic 命名规则发生了变化，必须包含类型信息
- `llvm.memset` 已经使用了正确的名称（`llvm.memset.p0i8.i32`）
- 其他数学 intrinsic（`llvm.sin.f64` 等）也使用了正确的命名

---

### [活动 009: SIGILL 非法指令错误](009_sigill_illegal_instruction.md)

**日期**: 2025-12-06  
**问题**: `SIGILL (0x4) at pc=0x000000010de5da00` - 执行生成的代码时发生非法指令错误

**摘要**: 在成功通过 IR 验证、代码生成和安装后，执行 Yuhu 编译器生成的代码时发生 SIGILL 错误。生成的代码只有 52 字节，对于矩阵乘法方法来说太小，可能表明代码生成不完整或入口点设置有问题。

**关键观察**:
- ✅ IR 验证通过
- ✅ `getPointerToFunction` 返回非 NULL（`0x14c01ca18`）
- ✅ 代码已安装（可以看到编译方法信息）
- ❌ 执行时发生 SIGILL
- ⚠️ 代码只有 52 字节（对于矩阵乘法来说太小）

**可能的原因**:
1. **代码生成不完整**：只生成了入口代码，没有生成实际的方法体
2. **栈对齐问题**：AArch64 要求栈指针 16 字节对齐
3. **ABI 约定不匹配**：参数传递、寄存器使用、栈帧布局不符合 AArch64 ABI
4. **入口点设置错误**：`getPointerToFunction` 返回的地址可能不是正确的入口点
5. **内存权限问题**：代码段没有执行权限

**调试建议**:
- 使用调试器检查生成的代码（反汇编）
- 检查代码大小（52 字节太小）
- 检查栈对齐（16 字节对齐）
- 检查入口点设置
- 对比 Shark 的实现
- 启用 LLVM 调试输出查看生成的汇编代码

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


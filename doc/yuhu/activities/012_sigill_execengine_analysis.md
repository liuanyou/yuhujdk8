# 活动 012: SIGILL（零代码尺寸）与 ExecutionEngine 重编译失败

## 日期
2025-12-09

## 问题描述

在运行 Yuhu 编译器编译的普通入口方法时，遇到以下运行时错误：

```
SIGILL (0x4) at pc=0x000000010dfc9844, pid=87605, tid=5379

Problematic frame:
J 10 yuhu com.example.Matrix.multiply([[I[[I[[II)V (79 bytes) @ 0x000000010dfc9844 [0x000000010dfc9840+0x4]
```

**关键观察**：
- 在 `compile_method` 中调用 `register_method` 之前：
  - `entry->code_start()` = `0x10dfc9100`
  - `llvm_code_size` = 0（`entry->code_limit() - entry->code_start()` = 0）
- 反汇编 `0x10dfc9100` 地址的内容：
  ```
  0x10dfc9100: .long  0xdddddddd                ; unknown opcode
  0x10dfc9104: .long  0xdddddddd                ; unknown opcode
  ...
  ```
  全是 `0xdddddddd`（未初始化内存模式）

## Module 概念说明

### 什么是 Module？

**LLVM Module** 是 LLVM IR 的容器，类似于一个"编译单元"：
- 包含多个 **Function**（函数）
- 包含 **GlobalVariable**（全局变量）
- 包含 **Type** 定义
- 类似于 C++ 的编译单元（.cpp 文件）

### Yuhu 中 Module 的使用方式

**当前实现**：
1. **YuhuCompiler 初始化时创建 2 个 Module**（只创建一次）：
   ```cpp
   _normal_context = new YuhuContext("normal");  // 创建 "normal" Module
   _native_context = new YuhuContext("native");  // 创建 "native" Module
   ```

2. **这两个 Module 在整个 YuhuCompiler 生命周期中复用**：
   - 所有的 Function 都添加到这两个 Module 中（主要是 `normal` Module）
   - 不是每次编译都创建新的 Module
   - Module 可以包含多个 Function

3. **ExecutionEngine 拥有 Module 的所有权**：
   ```cpp
   EngineBuilder builder(std::move(module_ptr));  // ExecutionEngine 拥有 Module
   ```

## 问题分析

### 1. 代码大小计算

在 `compile_method` 中：
```cpp
size_t llvm_code_size = (size_t)(entry->code_limit() - entry->code_start());
```

如果 `llvm_code_size` 为 0，说明 `entry->code_limit() == entry->code_start()`。

### 2. entry 的 code_start 和 code_limit 设置

在 `generate_native_code` 中：
```cpp
if (mm_base != NULL && mm_size > 0) {
  entry->set_entry_point((address)mm_base);
  entry->set_code_limit((address)(mm_base + mm_size));
} else {
  entry->set_entry_point(code);  // code 是 getPointerToFunction 返回的地址
  entry->set_code_limit(code);   // ⚠️ 这里 code_limit == code_start，导致 size = 0
}
```

### 3. 根本原因

**`mm_base` 为 NULL 或 `mm_size` 为 0**，导致进入了 `else` 分支，将 `code_limit` 设置为与 `code_start` 相同的值。

**为什么 `mm_base` 或 `mm_size` 会是 0？**

1. **`YuhuMemoryManager::allocateCodeSection` 没有被调用**：
   - LLVM 20 的 ExecutionEngine 在调用 `getPointerToFunction` 时，可能不会触发 `allocateCodeSection`
   - 或者 LLVM 使用了不同的内存分配路径（例如，代码已经在之前的编译中生成并缓存）

2. **`_last_code` 被清空**：
   - 如果之前编译过其他方法，`release_last_code_blob()` 会调用 `clear_last_code_allocation()`
   - 这会清空 `_last_code.base` 和 `_last_code.size`
   - 如果两次编译使用同一个 ExecutionEngine，第二次编译时 `_last_code` 可能已经被清空

3. **LLVM 20 的内存管理机制变化**：
   - LLVM 20 可能使用不同的内存管理策略
   - `getPointerToFunction` 可能返回一个已经存在的代码地址（来自之前的编译），而不是新分配的地址
   - 这种情况下，`allocateCodeSection` 可能不会被调用

### 4. 为什么会出现 `0xdddddddd`？

- `0xdddddddd` 是典型的未初始化内存填充模式（在某些调试构建中）
- 或者这是 CodeCache 中未使用区域的填充值
- 说明 `0x10dfc9100` 这个地址指向的内存区域没有被 LLVM 实际写入代码

### 5. 为什么 `getPointerToFunction` 返回了非 NULL 地址？

- LLVM 可能返回了一个之前编译的函数的地址（如果同一个 Module 中有同名函数）
- 或者返回了一个占位符地址，但实际代码还没有生成
- 或者 LLVM 使用了代码缓存，返回了缓存的地址，但缓存的内容已经被释放或无效

## 当前情况

**当前情况**：
- 同一个 Module 被多次使用（添加多个 Function）
- 第一次编译（OSR）：添加 `com.example.Matrix::multiply.osr.29` → 成功
- 第二次编译（普通入口）：添加 `com.example.Matrix::multiply` → 失败

**问题根源**：
- LLVM 20 的 ExecutionEngine 在第一次编译后，可能将 Module 标记为"已编译"或"已提交"
- 导致新添加的 Function 无法被 ExecutionEngine 处理
- `getPointerToFunction` 返回 NULL，`allocateCodeSection` 没有被调用

## 详细日志分析

### 第一次编译（OSR，entry_bci=29）

**Module 状态**：
- Module 中有 2 个函数：
  - `com.example.Matrix::multiply.osr.29` (ptr=0x600002e34488)
  - `llvm.frameaddress.p0` (ptr=0x600002e34518)

**执行流程**：
1. MemoryManager 状态：`base=0x0, size=0`（初始状态）
2. `getPointerToFunction` 调用
3. `allocateCodeSection` **被调用**：Size=2403, base=0x10e28d3c0, size=2408
4. `getPointerToFunction` 返回：`0x10e28d3c0` ✅
5. `release_last_code_blob`：清空 `_last_code`（base=0x0, size=0）

### 第二次编译（普通入口，entry_bci=-1）

**Module 状态**：
- Module 中有 3 个函数：
  - `com.example.Matrix::multiply.osr.29` (ptr=0x600002e34488) ← 第一次编译的函数还在
  - `llvm.frameaddress.p0` (ptr=0x600002e34518)
  - `com.example.Matrix::multiply` (ptr=0x600002e395f8) ← 新添加的函数

**执行流程**：
1. MemoryManager 状态：`base=0x0, size=0`（第一次编译后已清空）
2. Function 在 Module 中：✅ `Function found in Module by name (same pointer)`
3. IR 验证通过：✅
4. `getPointerToFunction` 调用
5. **`allocateCodeSection` 没有被调用** ❌
6. `getPointerToFunction` 返回：`0x0` ❌

### 关键发现

1. **Module 状态正常**：
   - Module 中确实有新的 Function（`com.example.Matrix::multiply`）
   - Function 对象有效，IR 验证通过
   - Module 指针相同（0x15a609350）

2. **ExecutionEngine 状态**：
   - ExecutionEngine 指针相同（0x15c814800）
   - 但 `getPointerToFunction` 返回 NULL
   - **`allocateCodeSection` 没有被调用**，说明 ExecutionEngine 根本没有尝试生成代码

3. **MemoryManager 状态**：
   - 两次编译前都是 `base=0x0, size=0`（正常，因为第一次编译后清空了）
   - 但这不是问题，因为 `allocateCodeSection` 应该在 `getPointerToFunction` 内部被调用

### 问题根源推测

**LLVM 20 的 ExecutionEngine 可能在第一次编译后，将 Module 标记为"已编译"或"已提交"状态，导致：**

1. ExecutionEngine 认为 Module 已经"完成"，不再处理新添加的 Function
2. `getPointerToFunction` 在查找 Function 时，发现 Module 处于"已完成"状态，直接返回 NULL
3. 因此 `allocateCodeSection` 根本没有被调用

**这解释了为什么**：
- Function 在 Module 中，但 ExecutionEngine 无法处理它
- `getPointerToFunction` 在 `allocateCodeSection` 之前就返回了 NULL
- 第一次编译成功，但第二次编译失败

## LLVM ExecutionEngine API 分析

### 当前使用的 API

#### 1. `addModule`
```cpp
// LLVM 20+
execution_engine()->addModule(std::move(module_ptr));
```

**作用**：将 Module 添加到 ExecutionEngine，ExecutionEngine 取得 Module 的所有权。

**使用场景**：
- 在 `YuhuCompiler::initialize()` 中，将 `_normal_context->module()` 和 `_native_context->module()` 添加到 ExecutionEngine
- 只在初始化时调用一次

**问题**：
- Module 的所有权已转移给 ExecutionEngine
- 无法再次调用 `addModule` 添加同一个 Module

#### 2. `getPointerToFunction`
```cpp
code = (address) execution_engine()->getPointerToFunction(function);
```

**作用**：获取已编译函数的地址。如果函数尚未编译，会触发编译。

**行为**：
- 如果 Function 在 ExecutionEngine 的 Module 中，且尚未编译，会触发编译
- 如果 Function 不在 ExecutionEngine 的 Module 中，返回 NULL
- 如果编译失败，返回 NULL

**问题**：
- 对于已编译的 Module 中新添加的 Function，可能返回 NULL
- 不提供错误信息，静默失败

### LLVM 20 ExecutionEngine 的行为特性

#### Module 生命周期管理
- ExecutionEngine 拥有 Module 的所有权（通过 `addModule` 转移）
- Module 一旦被添加到 ExecutionEngine，其生命周期由 ExecutionEngine 管理
- 无法直接修改已添加到 ExecutionEngine 的 Module

#### 编译时机
- `getPointerToFunction` 是**延迟编译**（lazy compilation）
- 只有在调用 `getPointerToFunction` 时，才会编译对应的 Function
- 编译后的代码存储在 MemoryManager 分配的内存中

#### Module 状态
- **假设**：ExecutionEngine 在第一次编译后，可能将 Module 标记为"已完成"
- 新添加的 Function 可能不会被 ExecutionEngine 识别
- 即使 Function 在 Module 中，ExecutionEngine 也可能无法找到它

## Shark vs Yuhu 的 Function 创建方式对比

### Shark 的 Function 创建方式

**Shark 使用版本 2**（不自动添加到 Module）：

```cpp
// sharkFunction.cpp:43-46
_function = Function::Create(
  entry_point_type(),
  GlobalVariable::InternalLinkage,
  name);  // 没有传入 Module 参数！
```

**特点**：
- Function 创建时**没有** parent Module
- `getParent()` 返回 `NULL`
- 需要后续显式调用 `add_function` 来添加到 Module

**Shark 的 `add_function` 调用**：
```cpp
// sharkCompiler.cpp:277
context()->add_function(function);  // 显式添加到 Module
code = (address) execution_engine()->getPointerToFunction(function);
```

`add_function` 的实现：
```cpp
// sharkContext.hpp:58
void add_function(llvm::Function* function) const {
  module()->getFunctionList().push_back(function);
}
```

### Yuhu 的 Function 创建方式

**Yuhu 使用版本 1**（自动添加到 Module）：

```cpp
// yuhuFunction.cpp:45-49
_function = Function::Create(
  entry_point_type(),
  GlobalVariable::InternalLinkage,
  name,
  YuhuContext::current().module());  // 传入 Module 参数！
```

**特点**：
- Function 创建时**已经**在 Module 中
- `getParent()` 返回有效的 Module
- **不需要**再调用 `add_function`

**Yuhu 的当前实现**：
```cpp
// yuhuCompiler.cpp:663
// Note: Function is already added to Module in YuhuFunction::initialize()
// via Function::Create() with Module parameter, so add_function() is no longer needed
// context()->add_function(function);  // Removed: Function already in Module
```

### 关键差异

| 特性 | Shark | Yuhu |
|------|-------|------|
| Function 创建方式 | `Function::Create(..., name)` | `Function::Create(..., name, module)` |
| 创建时是否在 Module 中 | ❌ 否 | ✅ 是 |
| 是否需要 `add_function` | ✅ 是 | ❌ 否（理论上） |

### 问题分析

**虽然 Yuhu 的 Function 已经在 Module 中，但可能存在以下问题**：

1. **ExecutionEngine 的"通知"机制**：
   - Shark 的 `add_function` 可能不仅仅是添加到函数列表
   - 它可能触发了 ExecutionEngine 的某些内部机制，通知 ExecutionEngine 有新函数需要处理
   - 即使 Function 已经在 Module 中，ExecutionEngine 可能也需要这个"通知"

2. **Module 状态管理**：
   - ExecutionEngine 可能在第一次编译后，将 Module 标记为"已完成"
   - 新添加的 Function（即使已经在 Module 中）可能不会被 ExecutionEngine 识别
   - 显式调用 `add_function` 可能可以"刷新" ExecutionEngine 的状态

3. **LLVM 版本差异**：
   - Shark 使用的是较旧的 LLVM 版本
   - Yuhu 使用的是 LLVM 20
   - 不同版本的 ExecutionEngine 行为可能不同

## 已尝试的解决方案

### 方案 1：区分 OSR 和普通入口的函数名

**实现**：
- 为 OSR 方法使用不同的函数名：`com.example.Matrix::multiply.osr.29`
- 为普通入口使用原始函数名：`com.example.Matrix::multiply`

**结果**：
- ✅ 第一次编译（OSR）成功
- ❌ 第二次编译（普通入口）仍然失败，`getPointerToFunction` 返回 NULL

**结论**：函数名不同并不能解决 ExecutionEngine 的状态问题。

### 方案 2：显式调用 `add_function`（即使 Function 已在 Module 中）

**实现**：
```cpp
// 在 generate_native_code 中，显式调用 add_function
// 即使 Function 已经在 Module 中，这也可能触发 ExecutionEngine 的"通知"机制
context()->add_function(function);
code = (address) execution_engine()->getPointerToFunction(function);
```

**改进的 `add_function` 实现**：
```cpp
void add_function(llvm::Function* function) const {
  // Check if function is already in the module
  // If it is, don't add it again (to avoid issues with push_back)
  llvm::Module* mod = module();
  if (function->getParent() == mod) {
    // Function is already in the module, check if it's in the function list
    bool found = false;
    for (llvm::Module::iterator I = mod->begin(), E = mod->end(); I != E; ++I) {
      if (&*I == function) {
        found = true;
        break;
      }
    }
    if (!found) {
      // Function is in module but not in list, add it
      mod->getFunctionList().push_back(function);
    }
    // If found, do nothing - function is already in the list
  } else {
    // Function is not in this module, add it
    mod->getFunctionList().push_back(function);
  }
}
```

**测试结果**：
- Module 函数数量在 `add_function` 前后不变（LLVM 忽略重复添加）
- `getPointerToFunction` 仍然返回 NULL
- `allocateCodeSection` 仍然没有被调用

**结论**：❌ **显式调用 `add_function` 无法解决问题**。

### 方案 3：检查是否有其他 ExecutionEngine API

**尝试的方法**：
- 检查是否有 `removeModule`、`finalizeObject`、`updateModule` 等 API
- 检查 LLVM 20 的 ExecutionEngine 头文件

**结果**：
- LLVM 20 的 ExecutionEngine API 非常有限
- 没有找到可以"刷新"或"更新" Module 状态的 API
- `removeModule` 可能不存在或不可用

## 最终结论

### 核心问题

**同一 ExecutionEngine + 同一 Module 在 LLVM 20 MCJIT 下，首次编译后对后续新函数的编译会被拒绝**：

1. **现象**：
   - `getPointerToFunction` 返回 NULL
   - `allocateCodeSection` 没有被调用
   - ExecutionEngine 根本没有尝试生成代码

2. **根本原因**：
   - LLVM 20 的 MCJIT（通过 ExecutionEngine）在第一次编译后，将 Module 标记为"已完成"
   - 后续新添加的 Function 不会被 ExecutionEngine 处理
   - 即使 Function 已经在 Module 中，IR 验证通过，ExecutionEngine 仍然拒绝编译

3. **尝试的解决方案都无效**：
   - ❌ 区分函数名：无法解决 ExecutionEngine 状态问题
   - ❌ 显式调用 `add_function`：LLVM 忽略重复添加，无法触发 ExecutionEngine 的"通知"机制
   - ❌ 检查其他 API：没有找到可以刷新 Module 状态的 API

### 关键发现

1. **重新添加 function 无法解决问题**：
   - 即使显式调用 `add_function`，LLVM 会忽略重复添加
   - Module 中的函数数量不变
   - ExecutionEngine 仍然拒绝编译新函数

2. **Module 状态正常，但 ExecutionEngine 状态异常**：
   - Module 中确实有新函数（函数计数从 2 增加到 3）
   - Function 对象有效，IR 验证通过
   - 但 ExecutionEngine 认为 Module 已完成，不再处理新函数

3. **这是 MCJIT 的设计限制**：
   - MCJIT 使用全局符号表，所有 Module 共享
   - 一旦 Module 被编译，可能无法再添加新函数
   - MCJIT 已进入维护模式，不再积极开发

## 建议的后续方向

### 方案 1：每次编译使用新的 Module

**优点**：
- 避免 Module 状态问题
- 每个编译任务独立，互不干扰

**缺点**：
- 性能开销（需要重新初始化类型系统等）
- 需要管理多个 Module 的生命周期

**实现**：
```cpp
// 在 compile_method 中
// 创建临时 Module
std::unique_ptr<llvm::Module> temp_module = ...;
// 创建 Function 并添加到 temp_module
// 调用 addModule(temp_module)
// 调用 getPointerToFunction
// 编译完成后，可以移除 Module（如果需要）
```

### 方案 2：迁移到 ORC JIT（推荐）

**优点**：
- **完全隔离**：ORC JIT 的 JITDylib 天然支持隔离编译
- **更好的符号管理**：不会出现 MCJIT 的全局符号冲突
- **aarch64 优化**：LLVM 对 ORC 在 aarch64 上投入更多优化
- **未来证明**：MCJIT 已进入维护模式，ORC 是未来方向

**缺点**：
- 需要较大的代码改动
- 需要学习新的 API

**实现**：
- 参见 [活动 014: 迁移到 ORC JIT](014_migrate_to_orc_jit.md)

## 相关文件

- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - `generate_native_code` 和 `compile_method`
- `hotspot/src/share/vm/yuhu/yuhuMemoryManager.cpp` - `allocateCodeSection` 和 `clear_last_code_allocation`
- `hotspot/src/share/vm/yuhu/yuhuMemoryManager.hpp` - `_last_code` 的定义
- `hotspot/src/share/vm/yuhu/yuhuContext.hpp` - `add_function` 方法定义
- `hotspot/src/share/vm/yuhu/yuhuFunction.cpp` - Function 创建位置

## 状态

- ✅ 问题已定位：`llvm_code_size` 为 0，导致注册了空的代码段
- ✅ 根本原因确认：LLVM 20 MCJIT 在第一次编译后，将 Module 标记为"已完成"
- ✅ 日志分析完成：确认了 Module 状态正常，但 ExecutionEngine 拒绝编译新函数
- ✅ 解决方案测试完成：重新添加 function 无法解决问题
- ⏳ **待实施**：迁移到 ORC JIT 或每次编译创建新 Module

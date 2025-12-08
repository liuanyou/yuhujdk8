# 活动 006: getPointerToFunction 返回 NULL 导致代码生成失败

## 日期
2025-12-06

## 问题描述

在运行 Yuhu 编译器时，遇到以下运行时错误：

```
Internal Error (/Users/liuanyou/CLionProjects/jdk8/hotspot/src/share/vm/yuhu/yuhuCompiler.cpp:474), pid=25935, tid=18691
assert(code != NULL) failed: code must be != NULL
```

错误发生在 `YuhuCompiler::generate_native_code()` 方法中，当调用 `execution_engine()->getPointerToFunction(function)` 时返回了 `NULL`。

## 错误调用栈

```
YuhuCompiler::compile_method
  → YuhuFunction::build
    → YuhuFunction::initialize
      → 创建 Function 并添加到 Module
  → generate_native_code
    → execution_engine()->getPointerToFunction(function)
      → 返回 NULL ❌
    → assert(code != NULL) 失败
```

## 问题根源分析

### 1. `getPointerToFunction` 返回 NULL 的可能原因

`ExecutionEngine::getPointerToFunction()` 返回 NULL 通常表示代码生成失败。可能的原因包括：

#### 原因 1：Function 不在 ExecutionEngine 的 Module 中

在 LLVM 20 中，`ExecutionEngine` 通过 `EngineBuilder` 创建时，会取得 Module 的所有权：

```cpp
// yuhuCompiler.cpp:235
std::unique_ptr<llvm::Module> module_ptr(_normal_context->module());
EngineBuilder builder(std::move(module_ptr));
// ExecutionEngine 现在拥有 _normal_context->module() 的所有权
```

**问题**：Function 是在 `YuhuFunction::initialize()` 中创建的，此时它被添加到 `YuhuContext::current().module()`。但是：

1. `YuhuContext::current()` 返回的是哪个 Context？
2. Function 是否被添加到了正确的 Module（即 ExecutionEngine 拥有的 Module）？

#### 原因 2：Module 所有权转移问题

在 LLVM 20 中，`EngineBuilder` 会取得 Module 的所有权：

```cpp
// yuhuCompiler.cpp:235
std::unique_ptr<llvm::Module> module_ptr(_normal_context->module());
EngineBuilder builder(std::move(module_ptr));
_execution_engine = builder.create();
```

**问题**：
- `_normal_context->module()` 的所有权被转移给了 `ExecutionEngine`
- 但 `YuhuContext` 仍然持有指向该 Module 的指针
- 如果 `ExecutionEngine` 内部重新创建或替换了 Module，`YuhuContext` 的指针可能指向无效的 Module

#### 原因 3：Function 没有被正确添加到 Module

虽然我们在 `YuhuFunction::initialize()` 中通过 `Function::Create(..., module)` 将 Function 添加到 Module，但可能存在以下问题：

1. **Module 指针错误**：`YuhuContext::current().module()` 可能返回了错误的 Module
2. **时机问题**：Function 被添加到 Module 的时机可能不对
3. **Module 状态问题**：Module 可能处于无效状态（例如，所有权已转移但指针未更新）

#### 原因 4：IR 验证失败

如果 LLVM IR 验证失败，`getPointerToFunction` 可能返回 NULL。但通常验证失败会抛出异常或返回错误，而不是静默返回 NULL。

#### 原因 5：代码生成错误

LLVM 代码生成过程中可能出现错误（例如，目标架构不支持某些指令、内存分配失败等），导致 `getPointerToFunction` 返回 NULL。

### 2. 与 Shark 的对比

Shark 的实现（`sharkCompiler.cpp:277`）：

```cpp
// Shark 在调用 getPointerToFunction 之前显式添加 Function
context()->add_function(function);
code = (address) execution_engine()->getPointerToFunction(function);
```

**关键差异**：
- Shark 使用 `add_function()` 显式将 Function 添加到 Module
- Yuhu 移除了 `add_function()` 调用，因为 Function 在创建时就添加到 Module 了
- 但可能存在 Module 所有权或指针的问题

### 3. Module 所有权分析

在 `yuhuCompiler.cpp` 构造函数中：

```cpp
// 创建 Context 和 Module
_normal_context = new YuhuContext("normal");
_native_context = new YuhuContext("native");

// 创建 ExecutionEngine，转移 _normal_context->module() 的所有权
std::unique_ptr<llvm::Module> module_ptr(_normal_context->module());
EngineBuilder builder(std::move(module_ptr));
_execution_engine = builder.create();

// 添加 _native_context->module()
std::unique_ptr<llvm::Module> native_module_ptr(_native_context->module());
execution_engine()->addModule(std::move(native_module_ptr));
```

**问题**：
- `_normal_context->module()` 的所有权被转移给 `ExecutionEngine`
- `_native_context->module()` 的所有权也被转移给 `ExecutionEngine`
- 但 `YuhuContext` 仍然持有指向这些 Module 的指针
- 如果 `ExecutionEngine` 内部重新创建了 Module，这些指针可能无效

### 4. YuhuContext::current() 的实现

```cpp
// yuhuContext.hpp:52
static YuhuContext& current() {
  return *YuhuCompiler::compiler()->context();
}
```

**问题**：
- `YuhuCompiler::compiler()->context()` 返回的是哪个 Context？
- 是 `_normal_context` 还是 `_native_context`？
- Function 应该添加到哪个 Module？

### 5. Function 创建和 Module 添加

在 `yuhuFunction.cpp:45`：

```cpp
_function = Function::Create(
    entry_point_type(),
    GlobalVariable::InternalLinkage,
    name,
    YuhuContext::current().module());  // 添加到当前 Context 的 Module
```

**问题**：
- `YuhuContext::current().module()` 返回的 Module 是否就是 `ExecutionEngine` 拥有的 Module？
- 如果 Module 的所有权已转移，这个指针是否仍然有效？

## 可能的问题场景

### 场景 1：Module 指针失效

1. `ExecutionEngine` 取得 Module 的所有权
2. `ExecutionEngine` 内部可能重新创建或替换 Module
3. `YuhuContext` 持有的 Module 指针失效
4. Function 被添加到无效的 Module
5. `getPointerToFunction` 找不到 Function，返回 NULL

### 场景 2：Function 添加到错误的 Module

1. `YuhuContext::current()` 返回了错误的 Context
2. Function 被添加到了 `_native_context->module()` 而不是 `_normal_context->module()`
3. `ExecutionEngine` 查找 Function 时找不到（因为它在错误的 Module 中）
4. `getPointerToFunction` 返回 NULL

### 场景 3：Module 未正确添加到 ExecutionEngine

1. Function 被添加到 Module
2. 但 Module 没有被正确添加到 `ExecutionEngine`
3. `ExecutionEngine` 无法找到 Function
4. `getPointerToFunction` 返回 NULL

### 场景 4：IR 验证或代码生成失败

1. Function 的 IR 存在错误（例如，使用了不支持的指令）
2. LLVM 验证或代码生成失败
3. `getPointerToFunction` 返回 NULL（静默失败）

## 调试建议

### 1. 验证 Function 是否在 Module 中

在 `generate_native_code` 中添加调试代码：

```cpp
// 检查 Function 是否在 Module 中
llvm::Module* mod = function->getParent();
if (mod == NULL) {
  fatal(err_msg("Function %s has no parent Module!", name));
}

// 检查 Module 是否在 ExecutionEngine 中
// 注意：LLVM 20 的 ExecutionEngine 可能不直接暴露 Module 列表
```

### 2. 验证 Module 指针的有效性

```cpp
// 检查 _normal_context->module() 是否仍然有效
llvm::Module* normal_mod = _normal_context->module();
llvm::Module* func_mod = function->getParent();
if (normal_mod != func_mod) {
  tty->print_cr("WARNING: Function's Module != _normal_context->module()");
  tty->print_cr("  Function Module: %p", func_mod);
  tty->print_cr("  Normal Context Module: %p", normal_mod);
}
```

### 3. 检查 ExecutionEngine 的错误信息

```cpp
// LLVM 20 的 ExecutionEngine 可能提供错误信息
std::string ErrorMsg;
// 检查是否有错误信息可用
```

### 4. 验证 IR 的正确性

```cpp
// 在调用 getPointerToFunction 之前验证 IR
if (llvm::verifyFunction(*function, &llvm::errs())) {
  fatal(err_msg("Function %s failed IR verification!", name));
}
```

### 5. 检查 MemoryManager

```cpp
// 检查 MemoryManager 是否正确实现
// 特别是 allocateCodeSection 和 finalizeMemory
```

## 可能的解决方案

### 方案 1：显式添加 Function 到 ExecutionEngine（推荐）

即使 Function 在创建时已添加到 Module，也可能需要在调用 `getPointerToFunction` 之前显式通知 `ExecutionEngine`：

```cpp
// 在 generate_native_code 中，调用 getPointerToFunction 之前
// 确保 Function 在正确的 Module 中，并且 Module 在 ExecutionEngine 中
// 可能需要调用 ExecutionEngine 的方法来刷新或重新编译 Module
```

### 方案 2：修复 Module 所有权问题

确保 `YuhuContext` 的 Module 指针始终有效，或者在使用前验证指针的有效性。

### 方案 3：检查 IR 验证

在调用 `getPointerToFunction` 之前验证 IR，如果验证失败，提供更详细的错误信息。

### 方案 4：检查 MemoryManager 实现

确保 `YuhuMemoryManager` 的 `allocateCodeSection` 和 `finalizeMemory` 正确实现，特别是内存权限设置（可执行权限）。

## 相关文件

- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - `generate_native_code` 方法（第 427-491 行）
- `hotspot/src/share/vm/yuhu/yuhuFunction.cpp` - Function 创建（第 41-46 行）
- `hotspot/src/share/vm/yuhu/yuhuContext.hpp` - `YuhuContext::current()` 实现
- `hotspot/src/share/vm/yuhu/yuhuMemoryManager.cpp` - MemoryManager 实现
- `hotspot/src/share/vm/shark/sharkCompiler.cpp` - Shark 的参考实现

## 经验教训

1. **Module 所有权管理**：在 LLVM 20 中，`ExecutionEngine` 会取得 Module 的所有权，需要确保指针的有效性。

2. **Function 添加到 Module 的时机**：虽然 Function 在创建时可以添加到 Module，但可能需要确保 Module 已正确添加到 `ExecutionEngine`。

3. **错误处理**：`getPointerToFunction` 返回 NULL 时，应该检查 LLVM 的错误信息，而不是直接断言失败。

4. **IR 验证**：在代码生成之前验证 IR 可以帮助提前发现问题。

5. **MemoryManager 实现**：确保 `allocateCodeSection` 和 `finalizeMemory` 正确实现，特别是内存权限设置。

## 调试代码实现

根据文档中的调试建议，已在 `generate_native_code` 方法中添加了详细的调试代码：

### 添加的调试检查

1. **验证 Function 是否在 Module 中**
   - 检查 `function->getParent()` 是否为 NULL
   - 打印 Function 和 Module 的详细信息

2. **验证 Module 指针的有效性**
   - 比较 Function 的 Module 与 `_normal_context->module()` 和 `_native_context->module()`
   - 如果 Module 不匹配，打印警告

3. **检查 ExecutionEngine 状态**
   - 验证 `execution_engine()` 不为 NULL

4. **验证 IR 的正确性**
   - 在调用 `getPointerToFunction` 之前验证 Function IR
   - 如果验证失败，立即 fatal

5. **检查 Function 是否在 Module 的函数列表中**
   - 遍历 Module 的函数列表，确认 Function 存在

6. **打印 Function 详细信息**
   - Function linkage、isDeclaration、hasBody、basic blocks 数量

7. **调用前后的日志**
   - 在调用 `getPointerToFunction` 前后打印日志
   - 如果返回 NULL，打印详细的错误信息

### 调试代码位置

`hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` 第 455-553 行

### 使用方法

运行程序时，调试信息会自动输出到控制台，包括：
- Function 和 Module 的状态
- Module 指针比较结果
- IR 验证结果
- `getPointerToFunction` 的返回值

如果 `getPointerToFunction` 返回 NULL，会输出详细的错误信息，帮助定位问题。

## 状态

- ✅ 问题已定位
- ✅ 根本原因已分析（多个可能原因）
- ✅ 调试代码已添加
- ⏳ 等待运行测试以收集调试信息
- ⏳ 等待根据调试信息确定具体原因并修复


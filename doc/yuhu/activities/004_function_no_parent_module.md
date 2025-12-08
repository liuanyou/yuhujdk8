# 活动 004: Function 没有 parent Module 导致 DataLayout 无法访问

## 日期
2025-12-06

## 问题描述

在运行 Yuhu 编译器时，遇到以下运行时错误：

```
Yuhu: ERROR - Function has no parent Module!
fatal error: Function has no parent Module
```

错误发生在 `YuhuBuilder::CreateValueOfStructEntry` 中，当尝试访问 `Function` 的 `parent Module` 以获取 `DataLayout` 时。

## 错误调用栈

```
YuhuFunction::initialize
  → Function::Create (创建 Function，但没有 Module)
  → YuhuStack::CreateBuildAndPushFrame
    → YuhuStack::initialize
      → CreateStackOverflowCheck
        → CreateValueOfStructEntry
          → CreateLoad
            → 需要访问 Module 的 DataLayout
              → func->getParent() 返回 NULL！❌
```

## 问题根源分析

### 1. Function 创建时没有指定 Module

在 `yuhuFunction.cpp` 第 43 行：

```cpp
_function = Function::Create(
    entry_point_type(),
    GlobalVariable::InternalLinkage,
    name);  // 没有传入 Module 参数！
```

**关键问题**：`Function::Create` 有两个重载版本：

1. **版本 1**（自动添加到 Module）：
   ```cpp
   Function::Create(FunctionType*, LinkageTypes, const Twine&, Module*)
   ```
   - 会自动将 Function 添加到指定的 Module
   - Function 的 `getParent()` 会返回该 Module

2. **版本 2**（不添加到 Module）：
   ```cpp
   Function::Create(FunctionType*, LinkageTypes, const Twine&)
   ```
   - **不会**将 Function 添加到任何 Module
   - Function 的 `getParent()` 返回 `NULL`

**当前代码使用的是版本 2**，所以 Function 没有 parent Module。

### 2. Function 被添加到 Module 的时机太晚

在 `yuhuCompiler.cpp` 第 451 行：

```cpp
context()->add_function(function);  // 在 generate_native_code 之后才调用
```

**问题**：Function 要等到编译完成后才通过 `add_function` 添加到 Module，但此时已经在使用 IRBuilder 生成 IR 了。

### 3. IRBuilder 需要 Module 的 DataLayout

在 LLVM 20 中，`IRBuilder::CreateLoad` → `CreateAlignedLoad` → `getABITypeAlign` 需要访问 Module 的 DataLayout：

```cpp
// yuhuBuilder.cpp
LoadInst* YuhuBuilder::CreateValueOfStructEntry(...) {
  // 尝试从 Function 获取 Module
  llvm::Function* func = bb->getParent();
  llvm::Module* mod = func->getParent();  // ← 这里返回 NULL！
  const llvm::DataLayout& dl = mod->getDataLayout();  // ← 崩溃！
}
```

## 调试过程

### 1. 验证 DataLayout 设置

添加调试代码验证 DataLayout 是否被正确设置：

```cpp
// yuhuCompiler.cpp
_normal_context->module()->setDataLayout(DLStr);
_native_context->module()->setDataLayout(DLStr);

// 验证
std::string verify1 = _normal_context->module()->getDataLayout().getStringRepresentation();
std::string verify2 = _native_context->module()->getDataLayout().getStringRepresentation();
```

**结果**：✅ DataLayout 被正确设置（"e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"）

### 2. 检查 Function 的 parent Module

添加调试代码检查 Function 是否有 parent Module：

```cpp
// yuhuBuilder.cpp
llvm::BasicBlock* bb = GetInsertBlock();
llvm::Function* func = bb->getParent();
llvm::Module* mod = func->getParent();  // ← 返回 NULL！
```

**结果**：❌ `func->getParent()` 返回 `NULL`，说明 Function 没有被添加到 Module。

## 解决方案

### 方案 1：在创建 Function 时指定 Module（推荐）

修改 `yuhuFunction.cpp`，在创建 Function 时传入 Module：

```cpp
_function = Function::Create(
    entry_point_type(),
    GlobalVariable::InternalLinkage,
    name,
    YuhuContext::current().module());  // 传入 Module
```

**优点**：
- Function 立即有 parent Module
- IRBuilder 可以立即访问 DataLayout
- 符合 LLVM 的最佳实践

### 方案 2：创建 Function 后立即添加到 Module

在 `YuhuFunction::initialize` 中，创建 Function 后立即添加到 Module：

```cpp
_function = Function::Create(...);
YuhuContext::current().add_function(_function);  // 立即添加
```

**优点**：
- 不需要修改 Function::Create 的调用
- Function 立即有 parent Module

### 方案 3：使用 IRBuilder 时传入 Module

修改 `YuhuBuilder` 构造函数，传入 Module 或 DataLayout：

```cpp
YuhuBuilder::YuhuBuilder(YuhuCodeBuffer* code_buffer)
  : IRBuilder<>(YuhuContext::current().module()),  // 传入 Module
    _code_buffer(code_buffer) {
}
```

**注意**：需要检查 LLVM 20 的 IRBuilder 构造函数是否支持传入 Module。

## 推荐方案

**推荐使用方案 1**：在创建 Function 时指定 Module。这是最直接、最符合 LLVM 设计的方式。

## 相关文件

- `hotspot/src/share/vm/yuhu/yuhuFunction.cpp` - Function 创建位置
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` - IRBuilder 使用 DataLayout 的位置
- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - Function 添加到 Module 的位置（太晚）
- `hotspot/src/share/vm/yuhu/yuhuContext.hpp` - `add_function` 方法定义

## 经验教训

1. **LLVM Function 必须属于某个 Module**：Function 的 `getParent()` 必须返回有效的 Module，否则无法访问 Module 的资源（如 DataLayout）。

2. **Function::Create 的重载版本**：需要明确使用哪个版本，如果需要在创建时自动添加到 Module，必须传入 Module 参数。

3. **IRBuilder 需要 Module 的 DataLayout**：在 LLVM 20 中，IRBuilder 的某些操作（如 CreateLoad）需要访问 Module 的 DataLayout，因此 Function 必须已经属于某个 Module。

4. **调试策略**：通过添加调试代码逐步检查调用链，最终定位到 Function 没有 parent Module 的问题。

## 修复实现

### 修改文件

1. **`hotspot/src/share/vm/yuhu/yuhuFunction.cpp`** (第 41-46 行)
   - 修改 `Function::Create` 调用，添加 Module 参数
   - Function 在创建时自动添加到 Module，立即拥有 parent Module

   ```cpp
   // 修改前
   _function = Function::Create(
     entry_point_type(),
     GlobalVariable::InternalLinkage,
     name);
   
   // 修改后
   _function = Function::Create(
     entry_point_type(),
     GlobalVariable::InternalLinkage,
     name,
     YuhuContext::current().module());  // 传入 Module
   ```

2. **`hotspot/src/share/vm/yuhu/yuhuCompiler.cpp`** (第 451 行)
   - 移除 `add_function` 调用，因为 Function 已在创建时添加到 Module

   ```cpp
   // 修改前
   context()->add_function(function);
   
   // 修改后
   // Note: Function is already added to Module in YuhuFunction::initialize()
   // via Function::Create() with Module parameter, so add_function() is no longer needed
   // context()->add_function(function);  // Removed: Function already in Module
   ```

### 修复效果

- ✅ Function 在创建时立即拥有 parent Module
- ✅ IRBuilder 可以通过 `func->getParent()->getDataLayout()` 访问 DataLayout
- ✅ `CreateLoad` 等操作可以正常工作
- ✅ 避免了重复添加 Function 到 Module

## 状态

- ✅ 问题已定位
- ✅ 修复已实现
- ⏳ 等待测试验证


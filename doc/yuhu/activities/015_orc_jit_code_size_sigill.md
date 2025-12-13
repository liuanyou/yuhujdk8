# 活动 015: ORC JIT 代码大小问题和 SIGILL 错误

## 日期
2025-12-09

## 问题描述

在解决符号查找问题后（活动 014），出现了新的问题：

```
ORC JIT: lookup returned: 0x100418000
Yuhu: compile_method - entry_bci=29, is_osr=1, code_start=0x100418000, code_limit=0x100418000, llvm_code_size=0
SIGILL (0x4) at pc=0x000000010a5bda2c
```

### 关键观察

1. **符号查找成功**：`ORC JIT: lookup returned: 0x100418000`
   - 函数已经被找到并编译
   - 代码地址：`0x100418000`

2. **代码大小为 0**：`llvm_code_size=0`
   - `code_start=0x100418000`
   - `code_limit=0x100418000`
   - `code_limit - code_start = 0`

3. **SIGILL 错误**：非法指令错误
   - 可能是因为代码大小不正确
   - 或者代码地址有问题

4. **链接属性问题**：`Function linkage: 0`
   - 虽然代码中使用了 `ExternalLinkage`，但日志显示 `linkage=0`（`InternalLinkage`）
   - 但符号查找成功了，说明链接属性可能不是问题

## 问题分析

### 1. 代码大小问题

**当前实现**（`yuhuCompiler.cpp` 第 885-893 行）：
```cpp
if (mm_base != NULL && mm_size > 0) {
  entry->set_entry_point((address)mm_base);
  entry->set_code_limit((address)(mm_base + mm_size));
} else {
  // For ORC JIT stage 1, we don't have CodeCache integration yet
  entry->set_entry_point(code);
  entry->set_code_limit(code);  // ⚠️ 问题：code_limit = code_start，导致 size = 0
}
```

**问题**：
- ORC JIT 使用默认内存管理，`mm_base` 和 `mm_size` 都是 `NULL/0`
- 代码设置了 `code_limit = code`，导致 `code_limit == code_start`
- 结果：`llvm_code_size = code_limit - code_start = 0`

**影响**：
- 代码大小为 0 可能导致 HotSpot 无法正确管理代码
- 可能导致 `SIGILL` 错误

### 2. ORC JIT 代码大小获取

**问题**：ORC JIT 没有直接提供获取函数代码大小的 API。

**可能的解决方案**：

#### 方案 A：使用符号大小（如果可用）

ORC JIT 的 `lookup` 可能返回符号大小信息，但 `ExecutorAddr` 只包含地址。

#### 方案 B：通过反汇编或调试信息获取

需要额外的工具或 API，可能不可行。

#### 方案 C：使用估算值或固定值（临时方案）

对于 Stage 1，可以使用一个合理的估算值或固定值。

#### 方案 D：集成 CodeCache（Stage 2）

在 Stage 2 中，通过自定义 `MemoryMapper` 从 CodeCache 分配内存，可以知道代码大小。

### 3. 链接属性问题

**观察**：
- 代码中使用 `ExternalLinkage`
- 但日志显示 `linkage=0`（`InternalLinkage`）

**可能原因**：
1. **代码没有重新编译**：修改后没有重新编译
2. **LLVM 自动修改**：LLVM 可能在某些情况下自动修改链接属性
3. **日志显示错误**：`getLinkage()` 返回的值可能不是我们期望的

**验证**：
- 检查 `yuhuFunction.cpp` 中的实际代码
- 确认 `ExternalLinkage` 的值（应该是 6）

### 4. SIGILL 错误分析

**可能原因**：

1. **代码大小不正确**：
   - 代码大小为 0，HotSpot 可能无法正确管理代码
   - 可能导致访问了错误的内存区域

2. **代码地址问题**：
   - ORC JIT 返回的地址可能不是可执行代码的起始地址
   - 或者地址不正确

3. **代码对齐问题**：
   - 代码可能没有正确对齐
   - 导致执行时出现非法指令

4. **OSR 适配器问题**：
   - 这是 OSR 方法（`entry_bci=29`）
   - 可能需要 OSR 适配器代码（见活动 010）
   - 但当前实现可能没有生成适配器

## 解决方案

### 方案 1：使用估算的代码大小（临时方案）

**思路**：
- 对于 Stage 1，使用一个合理的估算值
- 例如：使用函数的基本块数量或指令数量估算

**实现**：
```cpp
// 估算代码大小（临时方案）
// 可以根据函数的基本块数量或指令数量估算
size_t estimated_size = function->getBasicBlockList().size() * 64;  // 每个基本块约 64 字节
if (estimated_size < 256) estimated_size = 256;  // 最小 256 字节
entry->set_code_limit((address)(code + estimated_size));
```

**优点**：
- 简单快速
- 可以解决代码大小为 0 的问题

**缺点**：
- 不准确
- 可能导致内存访问问题

### 方案 2：通过函数信息估算代码大小

**思路**：
- 使用函数的 IR 信息（基本块数量、指令数量）估算代码大小
- 或者使用函数的字节码大小作为参考

**实现**：
```cpp
// 使用函数的基本块数量估算
size_t bb_count = function->getBasicBlockList().size();
size_t estimated_size = bb_count * 128;  // 每个基本块约 128 字节
if (estimated_size < 512) estimated_size = 512;  // 最小 512 字节
entry->set_code_limit((address)(code + estimated_size));
```

**优点**：
- 相对准确
- 基于函数的实际结构

**缺点**：
- 仍然不准确
- 不同架构的指令大小不同

### 方案 3：集成 CodeCache（推荐，但需要 Stage 2）

**思路**：
- 实现自定义 `MemoryMapper`，从 CodeCache 分配内存
- 这样可以知道准确的代码大小

**实现**：
- 需要实现 `llvm::orc::MemoryMapper` 接口
- 从 CodeCache 分配内存
- 记录分配的大小

**优点**：
- 准确
- 符合 HotSpot 的设计

**缺点**：
- 需要更多工作
- 属于 Stage 2 的任务

### 方案 4：检查链接属性

**思路**：
- 确认 `ExternalLinkage` 是否正确设置
- 检查是否有其他地方修改了链接属性

**实现**：
- 在 `YuhuFunction::initialize` 中打印链接属性
- 确认 `ExternalLinkage` 的值

## 推荐方案

**建议采用方案 2（临时）+ 方案 4（检查）**：

1. **立即修复代码大小问题**（方案 2）：
   - 使用函数的基本块数量估算代码大小
   - 设置一个合理的最小值（例如 512 字节）

2. **检查链接属性**（方案 4）：
   - 确认 `ExternalLinkage` 是否正确设置
   - 如果还是 `InternalLinkage`，需要找出原因

3. **长期方案**（方案 3）：
   - 在 Stage 2 中集成 CodeCache
   - 获取准确的代码大小

## 实施步骤

### 步骤 1：修复代码大小问题

```cpp
// 在 generate_native_code 中，ORC JIT 分支
if (mm_base != NULL && mm_size > 0) {
  entry->set_entry_point((address)mm_base);
  entry->set_code_limit((address)(mm_base + mm_size));
} else {
  entry->set_entry_point(code);
  // 估算代码大小（临时方案）
  size_t bb_count = function->getBasicBlockList().size();
  size_t estimated_size = bb_count * 128;  // 每个基本块约 128 字节
  if (estimated_size < 512) estimated_size = 512;  // 最小 512 字节
  entry->set_code_limit((address)(code + estimated_size));
  tty->print_cr("ORC JIT: Estimated code size: %zu bytes (%zu basic blocks)", 
                estimated_size, bb_count);
}
```

### 步骤 2：检查链接属性

```cpp
// 在 YuhuFunction::initialize 中
_function = Function::Create(
  entry_point_type(),
  GlobalVariable::ExternalLinkage,  // 应该是 6
  name,
  YuhuContext::current().module());

// 验证链接属性
tty->print_cr("YuhuFunction: Created function %s with linkage %d (ExternalLinkage=6)", 
               name, (int)_function->getLinkage());
```

## 状态

- 🔍 问题已分析
- ⏳ 待实施解决方案
- 📋 推荐方案：使用估算的代码大小（临时）+ 检查链接属性

## 相关活动

- 活动 010: OSR 参数传递问题
- 活动 012: SIGILL 零代码尺寸问题
- 活动 013: 迁移到 ORC JIT
- 活动 014: ORC JIT 符号名称修饰问题


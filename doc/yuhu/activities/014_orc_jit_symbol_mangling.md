# 活动 014: ORC JIT 符号名称修饰（Symbol Mangling）问题

## 日期
2025-12-09

## 问题描述

在实施 ORC JIT 迁移（活动 013）后，首次运行时遇到符号查找失败的错误：

```
ORC JIT: Function not found, adding module to JITDylib...
fatal error: Failed to lookup function com.example.Matrix::multiply.osr.29 after adding module: 
Symbols not found: [ _com.example.Matrix::multiply.osr.29 ]
```

### 重要发现

**`.osr.29` 后缀问题**：
- 这个后缀是在活动 012 中添加的，用于解决 MCJIT 的缓存问题
- **但现在使用 ORC JIT，这个后缀可能不再需要，甚至可能导致符号查找问题**
- 函数名包含特殊字符（`::` 和 `.osr.`），可能导致 C++ 链接器的名称修饰无法正确处理
- **建议优先尝试移除这个后缀**

## 符号注册流程分析

### 1. 函数名生成（`compile_method` 中）

```363:375:hotspot/src/share/vm/yuhu/yuhuCompiler.cpp
  // Generate function name: for OSR entries, add ".osr.<entry_bci>" suffix
  // to avoid LLVM ExecutionEngine caching issues (it caches by function name only,
  // ignoring FunctionType differences between OSR and normal entries).
  char func_name_buf[512];
  const char *func_name;
  if (entry_bci != InvocationEntryBci) {
    // OSR entry: add suffix to distinguish from normal entry
    snprintf(func_name_buf, sizeof(func_name_buf), "%s.osr.%d", base_name, entry_bci);
    func_name = func_name_buf;
  } else {
    // Normal entry: use base name
    func_name = base_name;
  }
```

**结果**：
- OSR 方法：`com.example.Matrix::multiply.osr.29`（**带后缀**）
- 普通入口：`com.example.Matrix::multiply`（不带后缀）

### 2. 函数创建（`YuhuFunction::build` -> `YuhuFunction::initialize`）

```427:427:hotspot/src/share/vm/yuhu/yuhuCompiler.cpp
  Function *function = YuhuFunction::build(env, &builder, flow, func_name);
```

```45:49:hotspot/src/share/vm/yuhu/yuhuFunction.cpp
  _function = Function::Create(
    entry_point_type(),
    GlobalVariable::InternalLinkage,
    name,
    YuhuContext::current().module());  // Pass Module so Function is added automatically
```

**关键点**：
- `Function::Create` 的第三个参数 `name` 就是 `func_name`（**带后缀的**）
- 函数创建时使用的名称就是带后缀的（如果是 OSR）

### 3. 符号注册（ORC JIT 添加 Module 时）

当 Module 被添加到 JITDylib 时，ORC JIT 会：
1. 遍历 Module 中的所有函数
2. 对每个函数进行名称修饰（mangling）
3. 将修饰后的符号名注册到符号表中

**符号注册使用的名称**：
- 从 `Function::getName()` 获取，也就是创建时使用的 `func_name`
- **所以符号注册时使用的名称也是带后缀的**（如果是 OSR）

### 4. 符号查找（`generate_native_code` 中）

```811:815:hotspot/src/share/vm/yuhu/yuhuCompiler.cpp
    std::string func_name = function->getName().str();
    tty->print_cr("ORC JIT: Looking up function: %s", func_name.c_str());
    
    // Try to lookup the function
    auto Sym = jit()->lookup(func_name);
```

**关键点**：
- `function->getName()` 返回的是函数创建时使用的名称（**带后缀的**）
- `jit()->lookup(func_name)` 会对这个名称进行名称修饰，然后查找

### 5. 名称修饰过程

**ORC JIT 的 `lookup()` 方法**：
1. 接收原始函数名：`com.example.Matrix::multiply.osr.29`
2. 调用 `mangle()` 进行名称修饰：`_com.example.Matrix::multiply.osr.29`
3. 在符号表中查找修饰后的名称

**问题**：
- 函数名包含特殊字符（`::` 和 `.osr.`）
- C++ 链接器的名称修饰可能无法正确处理这些特殊字符
- 导致符号查找失败

## 结论

**符号注册和查找使用的都是同一个名称**：
- **函数创建时**：使用 `func_name`（带后缀，如果是 OSR）
- **符号注册时**：从 `Function::getName()` 获取（带后缀）
- **符号查找时**：从 `Function::getName()` 获取（带后缀）

**所以问题可能在于**：
1. **特殊字符导致名称修饰失败**：`::` 和 `.osr.` 可能导致 C++ 链接器无法正确处理
2. **`.osr.29` 后缀不再需要**：ORC JIT 使用 JITDylib 隔离，不需要通过函数名区分

## 关键观察

1. **函数名**：`com.example.Matrix::multiply.osr.29`
   - **重要发现**：`.osr.29` 后缀是在活动 012 中添加的，用于区分 OSR 和普通入口点
   - 这个后缀是为了解决 MCJIT 的缓存问题（MCJIT 按函数名缓存，忽略 FunctionType 差异）
   - **但现在使用 ORC JIT，这个后缀可能不再需要，甚至可能导致问题**

2. **查找失败**：第一次 `lookup()` 返回失败
3. **添加 Module 后仍然失败**：重新添加 Module 到 JITDylib 后，第二次 `lookup()` 仍然失败
4. **错误信息**：`Symbols not found: [ _com.example.Matrix::multiply.osr.29 ]`
   - 注意：符号名前面有一个下划线 `_`
   - 这表明 ORC JIT 对符号名进行了**名称修饰（Name Mangling）**
   - **问题**：函数名包含特殊字符（`::` 和 `.osr.`），可能导致名称修饰或符号查找问题

## 问题分析

### 1. 符号名称修饰（Symbol Mangling）

在 LLVM ORC JIT 中，函数符号会被自动进行名称修饰：
- **原始函数名**：`com.example.Matrix::multiply.osr.29`
- **修饰后的符号名**：`_com.example.Matrix::multiply.osr.29`（前面有下划线）

这是 C++ 链接器的标准行为，用于支持函数重载和链接。

### 2. ORC JIT 的 lookup 方法

ORC JIT 提供了两种查找方式：

#### 方式 A：`lookup(StringRef)` - 自动处理名称修饰
```cpp
auto Sym = jit()->lookup(func_name);
```
- 这个方法会**自动进行名称修饰**
- 内部会调用 `mangle()` 方法
- 然后使用修饰后的名称查找

#### 方式 B：`lookupLinkerMangled(StringRef)` - 直接查找修饰后的名称
```cpp
auto Sym = jit()->lookupLinkerMangled(func_name);
```
- 这个方法期望传入**已经修饰过的名称**
- 不会再次进行名称修饰

### 3. 当前实现的问题

**当前代码**：
```cpp
std::string func_name = function->getName().str();
auto Sym = jit()->lookup(func_name);
```

**问题**：
- 使用 `lookup()` 应该会自动处理名称修饰
- 但是错误信息显示查找的是 `_com.example.Matrix::multiply.osr.29`（已修饰）
- 说明 `lookup()` 确实进行了名称修饰，但为什么还是找不到？

### 4. 可能的原因

#### 原因 1：Module 添加时机问题

**当前流程**：
1. 初始化时添加了 normal 和 native Module 到 JITDylib
2. 编译新函数时，函数被添加到现有的 Module 中
3. 但是**现有的 Module 已经在 JITDylib 中**，新添加的函数不会自动可见

**问题**：
- ORC JIT 在添加 Module 时，会编译 Module 中的所有函数
- 如果之后向 Module 中添加新函数，这些新函数**不会自动编译**
- 需要重新添加 Module 到 JITDylib，或者使用增量编译

#### 原因 2：Module 克隆问题

**当前实现**：
```cpp
// 克隆 Module（ORC JIT 需要拥有所有权）
std::unique_ptr<llvm::Module> module_clone(
  llvm::CloneModule(*func_mod).release());
auto TSM = llvm::orc::ThreadSafeModule(
  std::move(module_clone), *TSCtx);
```

**问题**：
- 克隆的 Module 可能不包含最新添加的函数
- 或者克隆时函数的某些属性丢失

#### 原因 3：符号名称不匹配

**观察**：
- 错误信息：`Symbols not found: [ _com.example.Matrix::multiply.osr.29 ]`
- 函数名：`com.example.Matrix::multiply.osr.29`

**可能的问题**：
- ORC JIT 的 `mangle()` 方法可能使用了不同的修饰规则
- 或者函数在 Module 中的实际符号名与预期不符

### 5. 日志分析

从日志中可以看到：
```
ORC JIT: Looking up function: com.example.Matrix::multiply.osr.29
ORC JIT: Function not found, adding module to JITDylib...
fatal error: Failed to lookup function com.example.Matrix::multiply.osr.29 after adding module: 
Symbols not found: [ _com.example.Matrix::multiply.osr.29 ]
```

**分析**：
1. 第一次 `lookup()` 失败（函数未找到）
2. 尝试添加 Module 到 JITDylib
3. 第二次 `lookup()` 仍然失败
4. 错误信息显示查找的是 `_com.example.Matrix::multiply.osr.29`（已修饰）

**结论**：
- `lookup()` 确实进行了名称修饰（添加了下划线）
- 但是即使添加了 Module，仍然找不到符号
- 说明问题不在名称修饰，而在于**符号本身不存在于 JITDylib 中**

## 根本原因分析

### 关键发现

从日志和代码分析，发现以下关键信息：

1. **初始化时添加了 Module**：
   ```cpp
   // 在 YuhuCompiler 构造函数中
   // Add normal module to JITDylib
   // Add native module to JITDylib
   ```
   - 在初始化时，normal 和 native Module 已经被添加到 JITDylib
   - 但此时这些 Module 中**还没有编译的函数**

2. **函数在编译时才添加到 Module**：
   ```cpp
   // 在 compile_method 中
   YuhuFunction::build(...)  // 创建 Function 并添加到 Module
   generate_native_code(...) // 编译函数
   ```
   - 函数是在 `YuhuFunction::build` 时创建并添加到 Module 的
   - 这发生在初始化**之后**

3. **问题流程**：
   - 初始化：添加空的 normal/native Module 到 JITDylib
   - 编译时：函数被添加到 Module（但 Module 已经在 JITDylib 中）
   - 查找失败：ORC JIT 找不到新添加的函数
   - 尝试重新添加：克隆 Module 并重新添加到 JITDylib
   - 仍然失败：即使重新添加，仍然找不到符号

### 假设 1：Module 添加后函数未编译（最可能）

**可能原因**：
- ORC JIT 在添加 Module 时，只编译 Module 中**已经存在的函数**
- 如果函数是在添加 Module **之后**才添加到 Module 中的，它不会被编译
- 需要重新添加 Module 才能编译新函数

**当前实现的问题**：
- 初始化时添加了 Module（此时函数还不存在）
- 编译时函数被添加到 Module（但 Module 已经在 JITDylib 中）
- 尝试重新添加 Module，但可能有时机或方式问题

**验证方法**：
- 检查函数添加到 Module 的时机（`YuhuFunction::build`）
- 检查 Module 添加到 JITDylib 的时机（初始化时）
- 确认函数是否在 Module 添加到 JITDylib **之前**就已经在 Module 中

### 假设 2：ThreadSafeModule 的问题

**可能原因**：
- `ThreadSafeModule` 在创建时，可能捕获了 Module 的某个快照
- 如果之后向原始 Module 添加函数，`ThreadSafeModule` 中的 Module 可能不包含新函数
- 需要确保在创建 `ThreadSafeModule` **之前**，所有函数都已经添加到 Module 中

**当前实现的问题**：
- 在 `generate_native_code` 中，使用 `CloneModule(*func_mod)` 克隆 Module
- 此时函数应该已经在 Module 中了（因为 `YuhuFunction::build` 已经完成）
- 但克隆的 Module 可能有问题，或者添加后仍然找不到符号

**验证方法**：
- 检查函数添加到 Module 的时机（`YuhuFunction::build` 中的 `Function::Create`）
- 检查 `ThreadSafeModule` 创建的时机（`generate_native_code` 中）
- 确认函数是否在创建 `ThreadSafeModule` **之前**就已经在 Module 中
- 检查克隆的 Module 是否包含函数

### 假设 3：符号可见性问题

**可能原因**：
- 函数的链接属性（linkage）可能不正确
- 某些链接属性（如 `internal`）可能导致函数不可见
- ORC JIT 可能只查找特定链接属性的函数

**当前实现**：
```cpp
// 在 YuhuFunction::initialize 中
_function = Function::Create(
  entry_point_type(),
  GlobalVariable::InternalLinkage,  // 使用 InternalLinkage
  name,
  YuhuContext::current().module());
```

**问题**：
- 函数使用 `InternalLinkage`（内部链接）
- 内部链接的函数可能不会被导出到符号表
- ORC JIT 可能无法查找内部链接的函数

**验证方法**：
- 检查函数的链接属性：`function->getLinkage()`
- 日志显示：`Function linkage: 7`（应该是 `WeakAnyLinkage` 或 `ExternalLinkage`）
- 但代码中使用的是 `InternalLinkage`（值为 0），为什么日志显示是 7？
- 需要确认函数的实际链接属性

### 假设 4：初始化时添加空 Module 的问题

**可能原因**：
- 初始化时添加了空的 normal/native Module 到 JITDylib
- 这些 Module 在 JITDylib 中已经存在
- 重新添加相同的 Module（即使包含新函数）可能被忽略或冲突

**当前实现流程**：
1. **初始化**：添加空的 normal/native Module 到 JITDylib
2. **编译时**：函数被添加到 Module（但 Module 已在 JITDylib 中）
3. **查找失败**：ORC JIT 找不到新函数
4. **尝试重新添加**：克隆 Module 并重新添加到同一个 JITDylib
5. **仍然失败**：可能因为 JITDylib 中已存在该 Module，或者符号冲突

**验证方法**：
- 检查初始化时添加的 Module 是否为空
- 检查重新添加 Module 时是否有错误或警告
- 考虑使用不同的 JITDylib 或移除初始化时的 Module 添加

### 假设 5：`.osr.29` 后缀导致的问题（新发现）

**背景**：
- `.osr.29` 后缀是在活动 012 中添加的，用于解决 MCJIT 的缓存问题
- MCJIT 按函数名缓存，忽略 FunctionType 差异，导致 OSR 和普通入口点冲突
- 添加后缀可以区分它们

**问题**：
- **ORC JIT 不需要这个后缀**：
  - ORC JIT 使用 JITDylib 隔离，不会出现 MCJIT 的缓存问题
  - 每个编译会话可以使用独立的 JITDylib
  - 不需要通过函数名区分 OSR 和普通入口点

- **后缀可能导致符号查找问题**：
  - 函数名包含特殊字符：`com.example.Matrix::multiply.osr.29`
  - 包含 `::`（C++ 命名空间分隔符）和 `.osr.`（点号）
  - C++ 链接器的名称修饰可能无法正确处理这些特殊字符
  - ORC JIT 的 `mangle()` 方法可能产生不符合链接器规范的符号名

**验证方法**：
- 检查使用原始函数名（不带 `.osr.29` 后缀）是否能成功查找
- 检查 `mangle()` 方法对包含特殊字符的函数名的处理
- 对比 MCJIT 和 ORC JIT 对函数名的处理差异

**可能的解决方案**：
- **移除 `.osr.29` 后缀**：ORC JIT 不需要这个后缀
- 如果需要区分 OSR 和普通入口点，可以使用其他方式（例如不同的 JITDylib）

## 对比：MCJIT vs ORC JIT

### MCJIT 的行为

**MCJIT**：
- 使用 `getPointerToFunction(function)` 直接查找函数
- 不需要名称修饰
- 函数添加到 Module 后，如果 Module 已经在 ExecutionEngine 中，可能需要重新编译

### ORC JIT 的行为

**ORC JIT**：
- 使用 `lookup(function_name)` 通过符号名查找
- 需要名称修饰（自动处理）
- 函数添加到 Module 后，如果 Module 已经在 JITDylib 中，**不会自动编译新函数**
- 需要重新添加 Module 或使用增量编译

## 解决方案

### 方案 1：移除初始化时的 Module 添加，改为在编译时添加（推荐）

**思路**：
- **移除初始化时的 Module 添加**：不在构造函数中添加 normal/native Module
- 在每次编译函数时，确保函数已经在 Module 中
- 然后添加包含该函数的 Module 到 JITDylib
- 如果 Module 已经添加过，可以跳过或使用增量编译

**实现**：
```cpp
// 在 YuhuCompiler 构造函数中
// 移除添加 Module 的代码，只创建 LLJIT

// 在 generate_native_code 中
// 1. 确保函数已经在 Module 中（YuhuFunction::build 已完成）
// 2. 检查函数是否已经在 JITDylib 中（lookup）
// 3. 如果不存在，创建 ThreadSafeModule（包含该函数）
// 4. 添加到 JITDylib
// 5. 查找函数
```

**优点**：
- 简单直接
- 确保函数在 Module 添加到 JITDylib 时已经存在
- 避免空 Module 的问题

**缺点**：
- 每次编译都要重新添加 Module（可能包含多个函数）
- 性能可能不是最优（但可以优化，例如缓存已添加的 Module）

**关键点**：
- 初始化时只创建 LLJIT，不添加 Module
- 编译时按需添加包含函数的 Module

### 方案 2：使用增量编译

**思路**：
- 使用 ORC JIT 的增量编译功能
- 在函数添加到 Module 后，通知 ORC JIT 重新编译

**实现**：
- 需要研究 ORC JIT 的增量编译 API
- 可能需要使用 `LLJIT::define` 或其他方法

**优点**：
- 性能更好
- 不需要重新添加整个 Module

**缺点**：
- API 可能更复杂
- 需要更多研究

### 方案 3：使用独立的 JITDylib 每个函数

**思路**：
- 为每个函数创建独立的 JITDylib
- 每个 JITDylib 只包含一个函数

**实现**：
```cpp
// 为每个函数创建新的 JITDylib
auto JD = jit()->createJITDylib(func_name);
// 添加包含该函数的 Module
jit()->addIRModule(JD, TSM);
// 从该 JITDylib 查找函数
auto Sym = jit()->lookup(JD, func_name);
```

**优点**：
- 完全隔离
- 避免符号冲突

**缺点**：
- 可能创建太多 JITDylib
- 管理复杂

### 方案 4：检查并使用正确的符号名

**思路**：
- 使用 `mangle()` 方法获取正确的符号名
- 使用 `lookupLinkerMangled()` 直接查找修饰后的名称
- 或者检查函数的链接属性，确保使用正确的链接属性

**实现**：
```cpp
// 获取修饰后的符号名
auto MangledName = jit()->mangle(func_name);
tty->print_cr("Mangled name: %s", MangledName.c_str());
// 使用修饰后的名称查找
auto Sym = jit()->lookupLinkerMangled(MangledName);
```

**优点**：
- 明确控制符号名
- 可以调试符号名问题
- 可以验证名称修饰是否正确

**缺点**：
- 可能不是根本解决方案
- 如果符号不存在，仍然会失败

### 方案 5：修改函数的链接属性

**思路**：
- 将函数的链接属性从 `InternalLinkage` 改为 `ExternalLinkage`
- 确保函数可以被导出到符号表

**实现**：
```cpp
// 在 YuhuFunction::initialize 中
_function = Function::Create(
  entry_point_type(),
  GlobalVariable::ExternalLinkage,  // 改为 ExternalLinkage
  name,
  YuhuContext::current().module());
```

**优点**：
- 简单直接
- 确保函数可以被查找

**缺点**：
- 可能影响其他功能
- 需要验证是否会导致符号冲突

### 方案 6：移除 `.osr.29` 后缀（新发现，可能最有效）

**背景**：
- `.osr.29` 后缀是在活动 012 中添加的，用于解决 MCJIT 的缓存问题
- 但现在使用 ORC JIT，不需要这个后缀

**思路**：
- **移除 `.osr.29` 后缀**，使用原始函数名
- ORC JIT 的 JITDylib 隔离机制可以解决 MCJIT 的缓存问题
- 避免特殊字符导致的符号查找问题

**实现**：
```cpp
// 在 compile_method 中
// 移除后缀生成逻辑，直接使用 base_name
const char *func_name = base_name;  // 不再添加 .osr.29 后缀
```

**优点**：
- **简单直接**：只需要移除几行代码
- **符合 ORC JIT 设计**：ORC JIT 不需要通过函数名区分
- **避免符号问题**：不使用特殊字符，符号名更规范
- **易于调试**：函数名更简洁

**缺点**：
- 如果 OSR 和普通入口点使用相同的函数名，可能会有冲突
- 但 ORC JIT 的 JITDylib 隔离应该可以解决这个问题

**验证方法**：
- 移除后缀后，检查 OSR 和普通入口点是否能正确编译
- 检查是否会出现符号冲突
- 如果出现冲突，可以使用不同的 JITDylib 来隔离

## 推荐方案

**建议优先尝试方案 6，如果不行再采用方案 1 + 方案 5 的组合**：

### 优先方案：方案 6 - 移除 `.osr.29` 后缀

**理由**：
1. **最简单**：只需要移除几行代码
2. **最可能解决问题**：特殊字符可能导致符号查找失败
3. **符合 ORC JIT 设计**：ORC JIT 不需要通过函数名区分 OSR 和普通入口点
4. **避免名称修饰问题**：不使用特殊字符，符号名更规范

**实施步骤**：
1. **修改 compile_method**：
   - 移除 `.osr.29` 后缀生成逻辑
   - 直接使用 `base_name` 作为函数名

2. **验证**：
   - 检查 OSR 和普通入口点是否能正确编译
   - 如果出现符号冲突，再考虑其他方案

### 备选方案：方案 1 + 方案 5 的组合

如果方案 6 不够，可以结合使用：

#### 主要方案：方案 1 - 移除初始化时的 Module 添加

1. **简单直接**：确保函数在 Module 添加到 JITDylib 之前已经存在
2. **易于实现**：只需要调整 Module 添加的时机
3. **可靠性高**：不依赖复杂的增量编译 API
4. **避免空 Module 问题**：不在初始化时添加空的 Module

#### 辅助方案：方案 5 - 修改函数链接属性

1. **确保符号可见**：使用 `ExternalLinkage` 确保函数可以被查找
2. **简单修改**：只需要修改 `YuhuFunction::initialize` 中的链接属性

**实施步骤**：

1. **修改 YuhuCompiler 构造函数**：
   - 移除添加 normal/native Module 到 JITDylib 的代码
   - 只保留创建 LLJIT 的代码

2. **修改 generate_native_code**：
   - 确保函数已经在 Module 中（`YuhuFunction::build` 已完成）
   - 检查函数是否已经在 JITDylib 中（`lookup`）
   - 如果不存在，创建 `ThreadSafeModule`（包含该函数）
   - 添加到 JITDylib
   - 查找函数

3. **修改 YuhuFunction::initialize**（可选）：
   - 将 `InternalLinkage` 改为 `ExternalLinkage`
   - 确保函数可以被导出到符号表

**注意**：
- **优先尝试方案 6**：最简单，最可能解决问题
- 如果方案 6 不够，再尝试方案 1 + 方案 5
- 也可以组合使用所有方案

## 调试建议

### 1. 检查函数链接属性

```cpp
tty->print_cr("Function linkage: %d", (int)function->getLinkage());
// 应该是 ExternalLinkage (6) 或 WeakAnyLinkage (7)
```

### 2. 检查 Module 中的函数列表

```cpp
for (llvm::Module::iterator I = mod->begin(), E = mod->end(); I != E; ++I) {
  tty->print_cr("Function in Module: %s", I->getName().str().c_str());
}
```

### 3. 检查 JITDylib 中的符号

```cpp
// 可能需要使用 ORC JIT 的调试 API
// 或者检查 JITDylib 的内容
```

### 4. 使用 mangle() 获取正确的符号名

```cpp
auto MangledName = jit()->mangle(func_name);
tty->print_cr("Mangled name: %s", MangledName.c_str());
// 应该输出: _com.example.Matrix::multiply.osr.29
```

### 5. 检查 Module 克隆是否包含函数

```cpp
// 在克隆 Module 后，检查是否包含函数
std::unique_ptr<llvm::Module> module_clone(
  llvm::CloneModule(*func_mod).release());
llvm::Function* cloned_func = module_clone->getFunction(func_name);
if (cloned_func == NULL) {
  tty->print_cr("ERROR: Cloned module does not contain function %s!", func_name.c_str());
} else {
  tty->print_cr("Cloned module contains function: %s", func_name.c_str());
}
```

### 6. 检查 JITDylib 中的符号

```cpp
// 尝试列出 JITDylib 中的所有符号（如果 API 支持）
// 或者使用调试工具检查
```

## 相关资源

- [LLVM ORC JIT Symbol Resolution](https://llvm.org/docs/ORCv2.html#symbol-resolution)
- [LLVM ORC JIT Mangling](https://llvm.org/doxygen/classllvm_1_1orc_1_1LLJIT.html#a8f8c8c8c8c8c8c8c8c8c8c8c8c8c8c8)
- [LLVM ORC JIT Incremental Compilation](https://llvm.org/docs/ORCv2.html#incremental-compilation)

## 状态

- 🔍 问题已分析
- ⏳ 待实施解决方案
- 📋 **优先推荐方案 6**：移除 `.osr.29` 后缀（最简单，最可能解决问题）
- 📋 备选方案 1 + 方案 5：移除初始化时的 Module 添加 + 修改函数链接属性

## 关键发现总结

1. **符号注册流程**：
   - 函数创建时使用的名称：`func_name`（带后缀，如果是 OSR）
   - 符号注册时使用的名称：从 `Function::getName()` 获取（带后缀）
   - 符号查找时使用的名称：从 `Function::getName()` 获取（带后缀）
   - **结论**：符号注册和查找使用的都是同一个名称（带后缀）

2. **`.osr.29` 后缀可能不再需要**：
   - 这个后缀是为了解决 MCJIT 的缓存问题而添加的
   - ORC JIT 使用 JITDylib 隔离，不需要通过函数名区分
   - 特殊字符可能导致符号查找问题

3. **初始化时添加空 Module 的问题**：
   - 初始化时添加了空的 Module，函数在编译时才添加
   - ORC JIT 无法看到新添加的函数

4. **函数链接属性问题**：
   - 使用 `InternalLinkage` 可能导致函数不可见
   - 可能需要改为 `ExternalLinkage`

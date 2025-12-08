# 活动 007: execution_engine_lock 断言失败

## 日期
2025-12-06

## 问题描述

在运行 Yuhu 编译器时，遇到以下运行时错误：

```
Internal Error (/Users/liuanyou/CLionProjects/jdk8/hotspot/src/share/vm/yuhu/yuhuCompiler.hpp:121), pid=11866, tid=18179
assert(execution_engine_lock()->owned_by_self()) failed: should be
```

错误发生在调试代码中，当调用 `execution_engine()` 方法时。

## 为什么"只是加了调试代码，错误就变了"？

### 执行流程对比

#### 之前的代码流程（没有调试代码）：
```
generate_native_code()
  → 第 515 行：获取锁 MutexLocker locker(execution_engine_lock())
  → 第 537 行：调用 execution_engine()->getPointerToFunction(function)
  → 第 540 行：检查 code 是否为 NULL
  → 第 551 行：如果 code == NULL，触发断言 ❌
     "assert(code != NULL) failed: code must be != NULL"
```

#### 现在的代码流程（有调试代码）：
```
generate_native_code()
  → 第 455-513 行：执行调试代码（锁外）
  → 第 484 行：调用 execution_engine() ❌ 触发断言失败
     "assert(execution_engine_lock()->owned_by_self()) failed: should be"
  → 程序在这里就崩溃了，永远到不了第 537 行的 getPointerToFunction
```

### 关键点

1. **错误并没有"变"**，而是：
   - **之前的错误**：程序执行到了 `getPointerToFunction`，但返回了 NULL
   - **现在的错误**：程序在到达 `getPointerToFunction` **之前**就失败了

2. **这是一个"提前失败"（fail-fast）的情况**：
   - 调试代码在锁外调用了 `execution_engine()`
   - `execution_engine()` 方法要求调用者必须持有锁
   - 因此程序在更早的位置就失败了

3. **如果修复了锁的问题**：
   - 程序应该能够继续执行到 `getPointerToFunction`
   - 然后可能会再次遇到原来的 `getPointerToFunction` 返回 NULL 的问题
   - 但至少现在有了调试输出，可以帮助诊断问题

## 错误调用栈

```
YuhuCompiler::generate_native_code
  → 调试代码（第 455-513 行，锁外）
    → execution_engine() 调用（第 484 行）
      → assert(execution_engine_lock()->owned_by_self()) 失败 ❌
```

## 问题根源分析

### 1. 锁的获取时机

查看 `generate_native_code` 方法的代码结构：

```cpp
void YuhuCompiler::generate_native_code(...) {
  // ... 前面的代码 ...
  
  // ========== Debug 代码（第 455-513 行）==========
  // 调试代码在锁外执行
  tty->print_cr("=== Yuhu: generate_native_code for %s ===", name);
  // ...
  if (!execution_engine()) {  // ← 第 484 行：调用 execution_engine()
    fatal(...);
  }
  tty->print_cr("ExecutionEngine: %p", execution_engine());  // ← 第 487 行：再次调用
  // ...
  // ========== End of Debug code ==========
  
  {
    MutexLocker locker(execution_engine_lock());  // ← 第 515 行：这里才获取锁！
    // ... 锁内的代码 ...
  }
}
```

### 2. execution_engine() 方法的锁要求

在 `yuhuCompiler.hpp:120-122` 中：

```cpp
llvm::ExecutionEngine* execution_engine() const {
  assert(execution_engine_lock()->owned_by_self(), "should be");  // ← 要求持有锁
  return _execution_engine;
}
```

### 3. 问题原因

**根本原因**：调试代码在**锁外**调用了 `execution_engine()` 方法，但该方法要求调用者必须持有 `execution_engine_lock()`。

**执行顺序**：
1. 第 455-513 行：调试代码执行（**锁外**）
2. 第 484 行：`if (!execution_engine())` - 调用 `execution_engine()`，触发断言失败
3. 第 487 行：`execution_engine()` - 如果第 484 行没有 fatal，这里也会失败
4. 第 515 行：`MutexLocker locker(execution_engine_lock())` - **这里才获取锁**

### 4. 为什么需要锁？

从 `yuhuCompiler.hpp` 的注释（第 102-106 行）：

```cpp
// The LLVM execution engine is the JIT we use to generate native
// code.  It is thread safe, but we need to protect it with a lock
// of our own because otherwise LLVM's lock and HotSpot's locks
// interleave and deadlock.  The YuhuMemoryManager is not thread
// safe, and is protected by the same lock as the execution engine.
```

**原因**：
- LLVM ExecutionEngine 本身是线程安全的
- 但为了避免 LLVM 的锁和 HotSpot 的锁交错导致死锁，需要统一管理
- `YuhuMemoryManager` 不是线程安全的，需要锁保护
- 因此，访问 `execution_engine()` 和 `memory_manager()` 必须持有锁

### 5. 调试代码的位置问题

调试代码被放在了锁外，但需要访问 `execution_engine()`，这违反了锁的要求。

## 解决方案

### 方案 1：将调试代码移到锁内（推荐）

将需要访问 `execution_engine()` 的调试代码移到 `MutexLocker` 块内：

```cpp
void YuhuCompiler::generate_native_code(...) {
  // ... 前面的代码 ...
  
  // ========== Debug: 锁外的检查 ==========
  // 这些检查不需要锁
  llvm::Module* func_mod = function->getParent();
  // ... 其他不需要 execution_engine() 的检查 ...
  // ========== End of 锁外调试代码 ==========
  
  {
    MutexLocker locker(execution_engine_lock());
    
    // ========== Debug: 锁内的检查 ==========
    // 这些检查需要访问 execution_engine() 或 memory_manager()
    if (!execution_engine()) {
      fatal(...);
    }
    tty->print_cr("ExecutionEngine: %p", execution_engine());
    // ========== End of 锁内调试代码 ==========
    
    // ... 原有代码 ...
  }
}
```

### 方案 2：直接访问 _execution_engine（不推荐）

在调试代码中直接访问 `_execution_engine` 成员变量，绕过锁检查：

```cpp
// 在调试代码中
if (!_execution_engine) {  // 直接访问，不通过方法
  fatal(...);
}
tty->print_cr("ExecutionEngine: %p", _execution_engine);
```

**缺点**：
- 违反了封装原则
- 可能在其他线程访问时出现竞态条件
- 不推荐使用

### 方案 3：创建无锁检查方法（可选）

创建一个专门用于调试的无锁访问方法：

```cpp
// 在 yuhuCompiler.hpp 中添加
private:
  llvm::ExecutionEngine* execution_engine_unsafe() const {
    return _execution_engine;  // 无锁检查，仅用于调试
  }
```

**缺点**：
- 增加了 API 复杂度
- 可能被误用
- 不推荐使用

## 推荐的修复方案

**使用方案 1**：将需要访问 `execution_engine()` 的调试代码移到锁内。

具体修改：
1. 将第 483-487 行的 ExecutionEngine 检查移到 `MutexLocker` 块内
2. 其他不需要 `execution_engine()` 的调试代码可以保留在锁外

## 相关文件

- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - `generate_native_code` 方法（第 427-553 行）
- `hotspot/src/share/vm/yuhu/yuhuCompiler.hpp` - `execution_engine()` 方法（第 120-122 行）

## 经验教训

1. **锁的作用域**：访问需要锁保护的资源时，必须确保在锁的作用域内。

2. **调试代码的位置**：调试代码应该遵循与正常代码相同的锁规则，不能因为调试就绕过锁检查。

3. **方法的设计意图**：`execution_engine()` 方法要求持有锁是有原因的，不应该绕过这个检查。

4. **代码组织**：将需要锁的调试代码和不需要锁的调试代码分开，可以提高代码的可读性和正确性。

5. **"提前失败"的好处**：虽然调试代码导致程序提前失败，但这实际上帮助发现了锁使用的问题，避免了潜在的竞态条件。

## 修复实现

### 修复内容

按照方案 1，将需要访问 `execution_engine()` 的调试代码移到了 `MutexLocker` 块内：

**修改前**（第 483-487 行在锁外）：
```cpp
  // Debug 3: Check ExecutionEngine state
  if (!execution_engine()) {  // ← 锁外调用，触发断言失败
    fatal(err_msg("ExecutionEngine is NULL!"));
  }
  tty->print_cr("ExecutionEngine: %p", execution_engine());
  
  // ... 其他调试代码 ...
  
  {
    MutexLocker locker(execution_engine_lock());  // ← 锁在这里才获取
    // ...
  }
```

**修改后**（第 515-518 行在锁内）：
```cpp
  // Debug 1-2, 4: 锁外的检查（不需要 execution_engine()）
  // ... 其他不需要锁的调试代码 ...
  
  {
    MutexLocker locker(execution_engine_lock());
    
    // ========== Debug 3: 锁内的检查（需要访问 execution_engine()）==========
    // Debug 3: Check ExecutionEngine state
    if (!execution_engine()) {  // ← 锁内调用，安全
      fatal(err_msg("ExecutionEngine is NULL!"));
    }
    tty->print_cr("ExecutionEngine: %p", execution_engine());
    // ========== End of 锁内调试代码 ==========
    
    free_queued_methods();
    // ...
  }
```

### 关键修改点

1. **保留在锁外的调试代码**（第 455-513 行）：
   - Debug 1: Function 和 Module 检查
   - Debug 2: Module 指针有效性检查
   - Debug 4: IR 验证
   - Function 在 Module 中的检查
   - Function 详细信息打印

2. **移到锁内的调试代码**（第 515-518 行）：
   - Debug 3: ExecutionEngine 状态检查
   - 需要访问 `execution_engine()` 的所有代码

### 修改的文件

- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - `generate_native_code` 方法

## 状态

- ✅ 问题已定位
- ✅ 根本原因已分析
- ✅ 修复已实现
- ⏳ 等待测试验证

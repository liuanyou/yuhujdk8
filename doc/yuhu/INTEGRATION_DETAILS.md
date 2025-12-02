# CompileBroker 集成详情

## 概述

本文档详细说明 Yuhu 编译器如何集成到 HotSpot 的 CompileBroker 系统中。

**重要**: YUHU 编译器与 C1、C2 编译器**并存**，而不是替代它们。

---

## 集成点

### 1. 头文件包含

**文件**: `hotspot/src/share/vm/compiler/compileBroker.cpp`

**位置**: 第 55-59 行

```cpp
#ifdef COMPILER2
#include "opto/c2compiler.hpp"
#endif
#ifdef SHARK
#include "shark/sharkCompiler.hpp"
#endif
#ifdef YUHU
#include "yuhu/yuhuCompiler.hpp"
#endif
```

**说明**: 添加 Yuhu 编译器的头文件包含，使用条件编译确保只在定义 YUHU 宏时包含。

---

### 2. 编译器数组

**文件**: `hotspot/src/share/vm/compiler/compileBroker.hpp` 和 `compileBroker.cpp`

**位置**: 第 258 行（hpp）和第 136 行（cpp）

```cpp
// [0] = C1, [1] = C2, [2] = YUHU
AbstractCompiler* CompileBroker::_compilers[3] = {NULL, NULL, NULL};
```

**说明**: 
- 编译器数组扩展到 3 个槽位
- `_compilers[0]` - C1 编译器
- `_compilers[1]` - C2 编译器
- `_compilers[2]` - YUHU 编译器

**关键**: YUHU 使用独立的槽位，可以与 C1、C2 同时存在

---

### 3. 编译器初始化

**文件**: `hotspot/src/share/vm/compiler/compileBroker.cpp`

**位置**: `compilation_init()` 函数（第 789-836 行）

#### 3.1 初始化逻辑

```cpp
void CompileBroker::compilation_init() {
  // ...
  if (!UseCompiler) {
    return;
  }
#ifndef SHARK
  // YUHU can coexist with C1 and C2
  int c1_count = CompilationPolicy::policy()->compiler_count(CompLevel_simple);
  int c2_count = CompilationPolicy::policy()->compiler_count(CompLevel_full_optimization);
  int yuhu_count = 0;

#ifdef COMPILER1
  if (c1_count > 0) {
    _compilers[0] = new Compiler();
  }
#endif

#ifdef COMPILER2
  if (c2_count > 0) {
    _compilers[1] = new C2Compiler();
  }
#endif

#ifdef YUHU
  // YUHU compiler can coexist with C1 and C2
  yuhu_count = 1;  // Default to 1 YUHU compiler thread
  _compilers[2] = new YuhuCompiler();
#endif

  // 初始化编译线程（支持 YUHU）
  init_compiler_threads(c1_count, c2_count, yuhu_count);
  // ...
}
```

#### 3.2 初始化参数

- **c1_count**: C1 编译器线程数（从策略获取）
- **c2_count**: C2 编译器线程数（从策略获取）
- **yuhu_count**: YUHU 编译器线程数（默认 1，可通过策略配置）
- **并存模式**: C1、C2、YUHU 可以同时初始化
- **构造函数初始化**: YuhuCompiler 在构造函数中完成所有初始化（类似 Shark）

---

### 4. 编译器线程初始化

**文件**: `hotspot/src/share/vm/compiler/compileBroker.cpp`

**位置**: `init_compiler_threads()` 函数（第 1020-1077 行）

#### 4.1 函数签名更新

```cpp
void CompileBroker::init_compiler_threads(int c1_compiler_count, int c2_compiler_count, int yuhu_compiler_count) {
  EXCEPTION_MARK;
#if !defined(ZERO) && !defined(SHARK) && !defined(YUHU)
  assert(c2_compiler_count > 0 || c1_compiler_count > 0, "No compilers?");
#endif // !ZERO && !SHARK && !YUHU
  // ...
}
```

**说明**: 
- 添加 `yuhu_compiler_count` 参数
- 排除 YUHU 的断言检查，因为 Yuhu 可以与 C1/C2 并存

#### 4.2 编译队列初始化

```cpp
// Initialize the compilation queues
if (c2_compiler_count > 0) {
  _c2_method_queue = new CompileQueue("C2MethodQueue", MethodCompileQueue_lock);
  _compilers[1]->set_num_compiler_threads(c2_compiler_count);
}
if (c1_compiler_count > 0) {
  _c1_method_queue = new CompileQueue("C1MethodQueue", MethodCompileQueue_lock);
  _compilers[0]->set_num_compiler_threads(c1_compiler_count);
}
#ifdef YUHU
if (yuhu_compiler_count > 0) {
  _yuhu_method_queue = new CompileQueue("YuhuMethodQueue", MethodCompileQueue_lock);
  _compilers[2]->set_num_compiler_threads(yuhu_compiler_count);
}
#endif
```

**说明**: YUHU 有独立的编译队列 `_yuhu_method_queue`。

#### 4.3 线程创建

```cpp
int compiler_count = c1_count + c2_count + yuhu_count;
_compiler_threads = new GrowableArray<CompilerThread*>(compiler_count, true);

// Create C2 compiler threads
for (int i = 0; i < c2_count; i++) {
  // ... create C2 threads
}

#ifdef YUHU
// Create YUHU compiler threads
for (int i = 0; i < yuhu_count; i++) {
  sprintf(name_buffer, "Yuhu CompilerThread%d", i);
  // ... create YUHU threads using _yuhu_method_queue and _compilers[2]
}
#endif

// Create C1 compiler threads
for (int i = 0; i < c1_count; i++) {
  // ... create C1 threads
}
```

**说明**: 
- YUHU 使用独立的编译队列 `_yuhu_method_queue`
- YUHU 使用 `_compilers[2]` 槽位
- 线程创建顺序：C2 → YUHU → C1

---

### 5. 编译方法检查

**文件**: `hotspot/src/share/vm/compiler/compileBroker.cpp`

**位置**: `compile_method()` 函数（第 1276 行）

```cpp
// some prerequisites that are compiler specific
if (comp->is_c2() || comp->is_shark() || comp->is_yuhu()) {
  method->constants()->resolve_string_constants(CHECK_AND_CLEAR_NULL);
  // Resolve all classes seen in the signature of the method
  // we are compiling.
  Method::load_signature_classes(method, CHECK_AND_CLEAR_NULL);
}
```

**说明**: 
- Yuhu 编译器（类似 C2 和 Shark）需要解析字符串常量和签名类
- 这确保在编译时所有必要的类都已加载

---

### 6. 运行时初始化

**文件**: `hotspot/src/share/vm/compiler/compileBroker.cpp`

**位置**: `init_compiler_runtime()` 函数（第 1598 行）

```cpp
if (!comp->is_shark() && !comp->is_yuhu()) {
  // Perform per-thread and global initializations
  comp->initialize();
}
```

**说明**: 
- Yuhu 编译器（类似 Shark）在构造函数中完成所有初始化
- 不需要调用 `initialize()` 方法
- 这避免了重复初始化

---

## 编译器识别

### AbstractCompiler 基类

**文件**: `hotspot/src/share/vm/compiler/abstractCompiler.hpp`

```cpp
#if defined(TIERED) || ( !defined(COMPILER1) && !defined(COMPILER2) && !defined(SHARK))
  virtual bool is_c1()   { return false; }
  virtual bool is_c2()   { return false; }
  virtual bool is_shark() { return false; }
  virtual bool is_yuhu() { return false; }
#endif
```

### YuhuCompiler 实现

**文件**: `hotspot/src/share/vm/yuhu/yuhuCompiler.hpp`

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

---

## 编译级别

### CompLevel 枚举

**文件**: `hotspot/src/share/vm/utilities/globalDefinitions.hpp`

```cpp
enum CompLevel {
  CompLevel_none              = 0,  // Interpreter
  CompLevel_simple            = 1,  // C1
  CompLevel_limited_profile   = 2,  // C1
  CompLevel_full_profile      = 3,  // C1
  CompLevel_full_optimization = 4,  // C2
  CompLevel_yuhu_fast         = 5,  // YUHU fast compilation
  CompLevel_yuhu_optimized    = 6,  // YUHU optimized compilation
  // ...
};
```

### 编译器选择

**文件**: `hotspot/src/share/vm/compiler/compileBroker.hpp`

```cpp
static AbstractCompiler* compiler(int comp_level) {
  if (is_yuhu_compile(comp_level)) return _compilers[2]; // YUHU
  if (is_c2_compile(comp_level)) return _compilers[1];   // C2
  if (is_c1_compile(comp_level)) return _compilers[0];   // C1
  return NULL;
}
```

**编译级别映射**:
- `CompLevel_simple` (1) → C1 (`_compilers[0]`)
- `CompLevel_full_optimization` (4) → C2 (`_compilers[1]`)
- `CompLevel_yuhu_fast` (5) → YUHU (`_compilers[2]`)
- `CompLevel_yuhu_optimized` (6) → YUHU (`_compilers[2]`)

### 编译队列选择

**文件**: `hotspot/src/share/vm/compiler/compileBroker.hpp`

```cpp
static CompileQueue* compile_queue(int comp_level) {
  if (is_yuhu_compile(comp_level)) return _yuhu_method_queue;
  if (is_c2_compile(comp_level)) return _c2_method_queue;
  if (is_c1_compile(comp_level)) return _c1_method_queue;
  return NULL;
}
```

---

## 编译流程

### 1. 编译请求

```cpp
CompileBroker::compile_method(methodHandle method, ...)
```

### 2. 编译器选择

```cpp
AbstractCompiler *comp = CompileBroker::compiler(comp_level);
```

### 3. 编译执行

```cpp
comp->compile_method(&ci_env, target, osr_bci);
```

### 4. 代码安装

```cpp
env->register_method(target, entry_bci, ...);
```

---

## 与 Shark 的对比

| 特性 | Shark | Yuhu (并存模式) |
|------|-------|-----------------|
| **槽位** | `_compilers[1]` | `_compilers[2]` |
| **与 C1/C2 并存** | ❌ | ✅ |
| **编译队列** | 共享 C2 队列 | 独立队列 (`_yuhu_method_queue`) |
| **线程数** | `c2_count = 1` | `yuhu_count = 1` |
| **初始化** | 构造函数中 | 构造函数中 |
| **需要 initialize()** | ❌ | ❌ |
| **编译级别** | `CompLevel_full_optimization` | `CompLevel_yuhu_fast` (5) 或 `CompLevel_yuhu_optimized` (6) |
| **字符串常量解析** | ✅ | ✅ |
| **签名类解析** | ✅ | ✅ |
| **灵活性** | 替代 C2 | 与 C1/C2 并存，可选择使用 |

---

## 测试验证

### 1. 编译器注册

```cpp
// 在 compilation_init() 后检查
// C1 编译器
assert(CompileBroker::compiler(CompLevel_simple) != NULL);
assert(CompileBroker::compiler(CompLevel_simple)->is_c1());

// C2 编译器
assert(CompileBroker::compiler(CompLevel_full_optimization) != NULL);
assert(CompileBroker::compiler(CompLevel_full_optimization)->is_c2());

// YUHU 编译器
assert(CompileBroker::compiler(CompLevel_yuhu_fast) != NULL);
assert(CompileBroker::compiler(CompLevel_yuhu_fast)->is_yuhu());
```

### 2. 编译功能

```java
// 简单的测试方法
public class Test {
    public int add(int a, int b) {
        return a + b;
    }
}
```

### 3. 日志输出

使用 `-XX:+PrintCompilation` 查看编译输出：
```
1234    1       3       Test::add (4 bytes)
```

---

## 注意事项

1. **条件编译**: 所有 Yuhu 相关代码都使用 `#ifdef YUHU` 保护
2. **并存模式**: YUHU 与 C1、C2 可以同时存在，通过编译级别选择
3. **独立队列**: YUHU 有独立的编译队列，不会阻塞 C1/C2
4. **向后兼容**: 不影响现有的 C1/C2 编译器，如果不定义 YUHU 宏，行为不变
5. **线程安全**: Yuhu 编译器使用与 Shark 相同的线程安全机制
6. **内存管理**: 使用 `YuhuMemoryManager` 管理生成的代码
7. **编译策略**: 需要修改 `CompilationPolicy` 来支持 YUHU 编译级别的选择（待实现）

---

## 相关文件

- `compileBroker.cpp` - 主要集成点
- `abstractCompiler.hpp` - 编译器基类
- `yuhuCompiler.hpp` - Yuhu 编译器类
- `yuhuCompiler.cpp` - Yuhu 编译器实现


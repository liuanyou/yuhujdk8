# YUHU 与 C1、C2 并存实现

## 概述

本文档说明如何实现 YUHU 编译器与 C1、C2 编译器并存，而不是替代它们。

## 设计决策

### 编译器数组扩展

**修改前**:
```cpp
AbstractCompiler* _compilers[2];  // [0] = C1, [1] = C2
```

**修改后**:
```cpp
AbstractCompiler* _compilers[3];  // [0] = C1, [1] = C2, [2] = YUHU
```

### 编译队列扩展

**新增**:
```cpp
static CompileQueue* _yuhu_method_queue;
```

## 实现细节

### 1. 编译器初始化 (`compilation_init()`)

**文件**: `hotspot/src/share/vm/compiler/compileBroker.cpp`

```cpp
void CompileBroker::compilation_init() {
  // ...
  
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

  init_compiler_threads(c1_count, c2_count, yuhu_count);
#endif
}
```

**关键点**:
- C1、C2、YUHU 可以同时初始化
- YUHU 使用 `_compilers[2]` 槽位
- 每个编译器独立计数

### 2. 编译器选择 (`compiler()`)

**文件**: `hotspot/src/share/vm/compiler/compileBroker.hpp`

```cpp
static AbstractCompiler* compiler(int comp_level) {
  if (is_yuhu_compile(comp_level)) return _compilers[2]; // YUHU
  if (is_c2_compile(comp_level)) return _compilers[1];     // C2
  if (is_c1_compile(comp_level)) return _compilers[0];   // C1
  return NULL;
}
```

**编译级别映射**:
- `CompLevel_simple` (1) → C1
- `CompLevel_full_optimization` (4) → C2
- `CompLevel_yuhu_fast` (5) → YUHU
- `CompLevel_yuhu_optimized` (6) → YUHU

### 3. 编译队列选择 (`compile_queue()`)

**文件**: `hotspot/src/share/vm/compiler/compileBroker.hpp`

```cpp
static CompileQueue* compile_queue(int comp_level) {
  if (is_yuhu_compile(comp_level)) return _yuhu_method_queue;
  if (is_c2_compile(comp_level)) return _c2_method_queue;
  if (is_c1_compile(comp_level)) return _c1_method_queue;
  return NULL;
}
```

### 4. 编译器线程初始化 (`init_compiler_threads()`)

**文件**: `hotspot/src/share/vm/compiler/compileBroker.cpp`

```cpp
void CompileBroker::init_compiler_threads(int c1_count, int c2_count, int yuhu_count) {
  // Initialize queues
  if (c2_count > 0) {
    _c2_method_queue = new CompileQueue("C2MethodQueue", MethodCompileQueue_lock);
    _compilers[1]->set_num_compiler_threads(c2_count);
  }
  if (c1_count > 0) {
    _c1_method_queue = new CompileQueue("C1MethodQueue", MethodCompileQueue_lock);
    _compilers[0]->set_num_compiler_threads(c1_count);
  }
#ifdef YUHU
  if (yuhu_count > 0) {
    _yuhu_method_queue = new CompileQueue("YuhuMethodQueue", MethodCompileQueue_lock);
    _compilers[2]->set_num_compiler_threads(yuhu_count);
  }
#endif

  int compiler_count = c1_count + c2_count + yuhu_count;
  _compiler_threads = new GrowableArray<CompilerThread*>(compiler_count, true);

  // Create threads in order: C2, YUHU, C1
  // ...
}
```

**线程创建顺序**:
1. C2 编译器线程
2. YUHU 编译器线程
3. C1 编译器线程

### 5. 其他更新

#### `mark_on_stack()`

```cpp
void CompileBroker::mark_on_stack() {
  if (_c2_method_queue != NULL) {
    _c2_method_queue->mark_on_stack();
  }
  if (_c1_method_queue != NULL) {
    _c1_method_queue->mark_on_stack();
  }
#ifdef YUHU
  if (_yuhu_method_queue != NULL) {
    _yuhu_method_queue->mark_on_stack();
  }
#endif
}
```

## 编译级别支持

### 已定义的编译级别

在 `hotspot/src/share/vm/utilities/globalDefinitions.hpp` 中：

```cpp
enum CompLevel {
  CompLevel_none              = 0,  // Interpreter
  CompLevel_simple            = 1,  // C1
  CompLevel_limited_profile   = 2,  // C1
  CompLevel_full_profile      = 3,  // C1
  CompLevel_full_optimization = 4,  // C2
  CompLevel_yuhu_fast         = 5,  // YUHU fast
  CompLevel_yuhu_optimized    = 6,  // YUHU optimized
  // ...
};

inline bool is_yuhu_compile(int comp_level) {
  return comp_level == CompLevel_yuhu_fast || comp_level == CompLevel_yuhu_optimized;
}
```

## 使用方式

### 1. 编译时启用 YUHU

在构建时定义 `YUHU` 宏：
```makefile
CFLAGS += -DYUHU
```

### 2. 运行时选择编译器

通过编译级别来选择使用哪个编译器：
- 使用 C1: `CompLevel_simple` (1)
- 使用 C2: `CompLevel_full_optimization` (4)
- 使用 YUHU: `CompLevel_yuhu_fast` (5) 或 `CompLevel_yuhu_optimized` (6)

### 3. 编译策略

需要修改编译策略（`CompilationPolicy`）来支持 YUHU 编译级别的选择。目前 YUHU 编译器会在定义 `YUHU` 宏时自动创建，但需要策略层支持才能实际使用 YUHU 编译级别。

## 与 Shark 的对比

| 特性 | Shark | YUHU (旧) | YUHU (新) |
|------|-------|-----------|-----------|
| **槽位** | `_compilers[1]` | `_compilers[1]` | `_compilers[2]` |
| **与 C1/C2 并存** | ❌ | ❌ | ✅ |
| **编译队列** | 共享 C2 队列 | 共享 C2 队列 | 独立队列 |
| **线程数** | `c2_count = 1` | `c2_count = 1` | `yuhu_count = 1` |

## 优势

1. **灵活性**: 可以根据需要选择使用 C1、C2 或 YUHU
2. **性能对比**: 可以同时运行多个编译器，对比性能
3. **渐进式迁移**: 可以逐步将方法从 C2 迁移到 YUHU
4. **独立队列**: YUHU 有独立的编译队列，不会阻塞 C1/C2

## 待完成工作

1. **编译策略支持**: 修改 `CompilationPolicy` 支持 YUHU 编译级别选择
2. **配置选项**: 添加 JVM 选项控制 YUHU 编译器的启用/禁用
3. **性能监控**: 添加 YUHU 编译器的性能统计
4. **测试**: 测试多编译器并存场景

## 相关文件

- `compileBroker.hpp` - 编译器数组和队列声明
- `compileBroker.cpp` - 编译器初始化和线程管理
- `globalDefinitions.hpp` - 编译级别定义
- `yuhuCompiler.hpp` - YUHU 编译器实现


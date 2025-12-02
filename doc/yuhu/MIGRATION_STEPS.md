# Shark 到 Yuhu 迁移步骤

## 概述

本文档详细记录了从 Shark 编译器迁移到 Yuhu 编译器的所有步骤。

---

## Step 1: 复制文件

### 命令
```bash
cd /Users/liuanyou/CLionProjects/jdk8
cp -r hotspot/src/share/vm/shark hotspot/src/share/vm/yuhu
```

### 结果
- 约 45 个文件被复制到 `hotspot/src/share/vm/yuhu/` 目录

---

## Step 2: 批量重命名文件

### 命令
```bash
cd hotspot/src/share/vm/yuhu
for file in shark*; do 
  if [ -f "$file" ]; then 
    mv "$file" "${file/shark/yuhu}"
  fi
done
```

### 重命名示例
- `sharkCompiler.cpp` → `yuhuCompiler.cpp`
- `sharkCompiler.hpp` → `yuhuCompiler.hpp`
- `sharkContext.cpp` → `yuhuContext.cpp`
- `sharkContext.hpp` → `yuhuContext.hpp`
- ... 等等

---

## Step 3: 批量替换内容

### 3.1 替换类名和标识符

```bash
find . -type f \( -name "*.cpp" -o -name "*.hpp" \) -exec sed -i '' 's/Shark/Yuhu/g' {} +
```

**替换内容**：
- `SharkCompiler` → `YuhuCompiler`
- `SharkContext` → `YuhuContext`
- `SharkBuilder` → `YuhuBuilder`
- ... 等等

### 3.2 替换命名空间和文件名引用

```bash
find . -type f \( -name "*.cpp" -o -name "*.hpp" \) -exec sed -i '' 's/shark/yuhu/g' {} +
```

**替换内容**：
- `shark/` → `yuhu/`
- `sharkCompiler` → `yuhuCompiler`
- ... 等等

### 3.3 替换宏定义

```bash
find . -type f \( -name "*.cpp" -o -name "*.hpp" \) -exec sed -i '' 's/SHARK/YUHU/g' {} +
```

**替换内容**：
- `SHARK` → `YUHU`
- `SHARE_VM_SHARK_` → `SHARE_VM_YUHU_`
- ... 等等

---

## Step 4: 更新 Include Guards

### 手动更新示例

**yuhuCompiler.hpp**:
```cpp
#ifndef SHARE_VM_YUHU_YUHU_COMPILER_HPP
#define SHARE_VM_YUHU_YUHU_COMPILER_HPP
```

**yuhuContext.hpp**:
```cpp
#ifndef SHARE_VM_YUHU_YUHU_CONTEXT_HPP
#define SHARE_VM_YUHU_YUHU_CONTEXT_HPP
```

### 批量更新（如果需要）
```bash
# 注意：需要仔细检查每个文件，确保格式正确
```

---

## Step 5: 平台适配

### 5.1 LLVM 目标初始化

**文件**: `yuhuCompiler.cpp`

**修改前**:
```cpp
// Initialize the native target
InitializeNativeTarget();

// MCJIT require a native AsmPrinter
InitializeNativeTargetAsmPrinter();
```

**修改后**:
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

### 5.2 条件编译处理

**文件**: `yuhu_globals.hpp`

添加注释：
```cpp
#ifdef TARGET_ARCH_zero
# include "yuhu_globals_zero.hpp"
#endif
// Note: AArch64 does not need platform-specific globals
```

**文件**: `yuhuRuntime.cpp`

添加注释：
```cpp
#ifdef TARGET_ARCH_zero
# include "stack_zero.inline.hpp"
#endif
// Note: AArch64 uses standard stack management, no ZeroStack needed
```

---

## Step 6: CompileBroker 集成

### 6.1 添加头文件包含

**文件**: `compileBroker.cpp` (第 55-59 行)

```cpp
#ifdef SHARK
#include "shark/sharkCompiler.hpp"
#endif
#ifdef YUHU
#include "yuhu/yuhuCompiler.hpp"
#endif
```

### 6.2 扩展编译器数组

**文件**: `compileBroker.hpp` 和 `compileBroker.cpp`

```cpp
// [0] = C1, [1] = C2, [2] = YUHU
AbstractCompiler* CompileBroker::_compilers[3] = {NULL, NULL, NULL};
```

### 6.3 添加 YUHU 编译队列

**文件**: `compileBroker.hpp` 和 `compileBroker.cpp`

```cpp
static CompileQueue* _yuhu_method_queue;
CompileQueue* CompileBroker::_yuhu_method_queue = NULL;
```

### 6.4 实现 YUHU 与 C1/C2 并存

**文件**: `compileBroker.cpp` (第 796-836 行)

```cpp
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
  yuhu_count = 1;
  _compilers[2] = new YuhuCompiler();
#endif

  init_compiler_threads(c1_count, c2_count, yuhu_count);
#endif
```

### 6.5 更新编译器选择函数

**文件**: `compileBroker.hpp`

```cpp
static AbstractCompiler* compiler(int comp_level) {
  if (is_yuhu_compile(comp_level)) return _compilers[2]; // YUHU
  if (is_c2_compile(comp_level)) return _compilers[1];   // C2
  if (is_c1_compile(comp_level)) return _compilers[0];   // C1
  return NULL;
}

static CompileQueue* compile_queue(int comp_level) {
  if (is_yuhu_compile(comp_level)) return _yuhu_method_queue;
  if (is_c2_compile(comp_level)) return _c2_method_queue;
  if (is_c1_compile(comp_level)) return _c1_method_queue;
  return NULL;
}
```

### 6.6 更新编译器线程初始化

**文件**: `compileBroker.cpp`

```cpp
void CompileBroker::init_compiler_threads(int c1_count, int c2_count, int yuhu_count) {
  // Initialize queues
  // ... C1, C2 queues
  
#ifdef YUHU
  if (yuhu_count > 0) {
    _yuhu_method_queue = new CompileQueue("YuhuMethodQueue", MethodCompileQueue_lock);
    _compilers[2]->set_num_compiler_threads(yuhu_count);
  }
#endif

  // Create threads: C2 → YUHU → C1
  // ...
}
```

### 6.7 更新编译器检查

**文件**: `compileBroker.cpp`

```cpp
if (comp->is_c2() || comp->is_shark() || comp->is_yuhu()) {
  method->constants()->resolve_string_constants(CHECK_AND_CLEAR_NULL);
  Method::load_signature_classes(method, CHECK_AND_CLEAR_NULL);
}
```

### 6.8 更新初始化检查

**文件**: `compileBroker.cpp`

```cpp
if (!comp->is_shark() && !comp->is_yuhu()) {
  comp->initialize();
}
```

### 6.9 更新断言和其他函数

**文件**: `compileBroker.cpp`

```cpp
#if !defined(ZERO) && !defined(SHARK) && !defined(YUHU)
  assert(c2_compiler_count > 0 || c1_compiler_count > 0, "No compilers?");
#endif

void CompileBroker::mark_on_stack() {
  // ... C1, C2 queues
#ifdef YUHU
  if (_yuhu_method_queue != NULL) {
    _yuhu_method_queue->mark_on_stack();
  }
#endif
}
```

### 6.10 添加编译器识别方法

**文件**: `yuhuCompiler.hpp`

```cpp
// Compiler identification
bool is_yuhu()         { return true; }
bool is_c1()           { return false; }
bool is_c2()           { return false; }
bool is_shark()        { return false; }
```

---

## 验证步骤

### 1. 检查文件重命名
```bash
cd hotspot/src/share/vm/yuhu
ls -la | grep yuhu
```

### 2. 检查内容替换
```bash
grep -r "Shark" . | head -5  # 应该没有结果
grep -r "shark" . | head -5  # 应该没有结果（除了注释）
grep -r "SHARK" . | head -5  # 应该没有结果
```

### 3. 检查 Include Guards
```bash
grep -r "SHARE_VM_YUHU" . | head -10
```

### 4. 检查平台适配
```bash
grep -r "InitializeAArch64Target" yuhuCompiler.cpp
```

---

## 注意事项

1. **保留原始文件**：在迁移过程中，Shark 的原始文件保持不变，作为参考。

2. **逐步验证**：每个步骤完成后都应该验证，确保没有遗漏。

3. **条件编译**：注意处理不同架构的条件编译代码。

4. **命名一致性**：确保所有命名都一致（Yuhu/YUHU/yuhu）。

5. **文档更新**：更新所有相关的文档和注释。

---

## 迁移统计

- **文件数量**: ~45 个文件
- **代码行数**: ~10,000+ 行
- **类数量**: ~20+ 个类
- **迁移时间**: 约 1 小时（自动化步骤）

---

## 后续工作

1. 处理 ZeroStack 适配问题
2. 修复编译错误
3. 更新构建系统
4. 测试和验证


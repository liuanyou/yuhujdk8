# 活动 008: LLVM frameaddress intrinsic 名称 mangling 错误

## 日期
2025-12-06

## 问题描述

在运行 Yuhu 编译器时，遇到以下 IR 验证错误：

```
Intrinsic name not mangled correctly for type arguments! Should be: llvm.frameaddress.p0
ptr @llvm.frameaddress

fatal error: Function com.example.Matrix::multiply failed IR verification!
```

错误发生在 IR 验证阶段（`llvm::verifyFunction`），表明 `llvm.frameaddress` intrinsic 的名称没有正确 mangled。

## 错误分析

### 1. LLVM 20 的 Intrinsic 命名规则变化

在 LLVM 20 中，intrinsic 函数的命名规则发生了变化：

- **旧版本（LLVM < 20）**：intrinsic 使用简单的名称，如 `llvm.frameaddress`
- **LLVM 20+**：intrinsic 名称必须包含类型信息，如 `llvm.frameaddress.p0`（其中 `.p0` 表示指针类型）

### 2. 当前代码的问题

查看 `yuhuBuilder.cpp:414-416`：

```cpp
Value* YuhuBuilder::frame_address() {
  return make_function("llvm.frameaddress", "i", "C");
}
```

这里使用了简单的名称 `"llvm.frameaddress"`，但在 LLVM 20 中，这个名称需要包含类型信息。

### 3. 错误信息解读

错误信息 `Should be: llvm.frameaddress.p0` 表明：
- LLVM 期望的名称是 `llvm.frameaddress.p0`
- 当前使用的名称是 `llvm.frameaddress`（缺少类型后缀）
- `.p0` 表示返回类型是指针类型（pointer type）

### 4. 为什么需要类型信息？

在 LLVM 20 中，由于引入了 opaque pointer types，intrinsic 的类型信息不再可以从指针类型推断，因此必须显式包含在名称中。

## 根本原因

**问题根源**：`frame_address()` 方法使用旧的 intrinsic 命名方式（`"llvm.frameaddress"`），但 LLVM 20 要求使用新的命名方式（`"llvm.frameaddress.p0"`）。

## 解决方案

### 方案 1：使用正确的 intrinsic 名称（推荐）

修改 `frame_address()` 方法，使用 LLVM 20 的正确名称：

```cpp
Value* YuhuBuilder::frame_address() {
#if LLVM_VERSION_MAJOR >= 20
  // LLVM 20+ requires type information in intrinsic name
  // llvm.frameaddress.p0: returns pointer type (p0)
  return make_function("llvm.frameaddress.p0", "i", "C");
#else
  return make_function("llvm.frameaddress", "i", "C");
#endif
}
```

### 方案 2：使用 Intrinsic::getDeclaration（更推荐）

使用 LLVM 的 `Intrinsic::getDeclaration` API，这是创建 intrinsic 的标准方式：

```cpp
#include <llvm/IR/Intrinsics.h>

Value* YuhuBuilder::frame_address() {
#if LLVM_VERSION_MAJOR >= 20
  // Use Intrinsic::getDeclaration for LLVM 20+
  llvm::Module* mod = YuhuContext::current().module();
  llvm::FunctionType* func_type = llvm::FunctionType::get(
    PointerType::getUnqual(YuhuType::jbyte_type()),
    std::vector<llvm::Type*>{YuhuType::jint_type()},
    false);
  llvm::Function* intrinsic = llvm::Intrinsic::getDeclaration(
    mod, llvm::Intrinsic::frameaddress, 
    {PointerType::getUnqual(YuhuType::jbyte_type())});
  return intrinsic;
#else
  return make_function("llvm.frameaddress", "i", "C");
#endif
}
```

### 方案 3：条件编译处理

根据 LLVM 版本使用不同的名称：

```cpp
Value* YuhuBuilder::frame_address() {
#if LLVM_VERSION_MAJOR >= 20
  // LLVM 20+ uses mangled intrinsic names with type information
  // Format: llvm.frameaddress.<return_type>
  // For pointer return type, use .p0
  return make_function("llvm.frameaddress.p0", "i", "C");
#elif LLVM_VERSION_MAJOR >= 15
  // LLVM 15-19 might use different naming
  return make_function("llvm.frameaddress", "i", "C");
#else
  // Older LLVM versions
  return make_function("llvm.frameaddress", "i", "C");
#endif
}
```

## 推荐的修复方案

**使用方案 1**：最简单直接，只需要修改名称字符串。

**如果方案 1 不工作，使用方案 2**：使用 LLVM 的标准 API，更可靠但需要更多代码。

## 相关文件

- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` - `frame_address()` 方法（第 414-416 行）
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` - `CreateGetFrameAddress()` 方法（第 438-446 行）
- `hotspot/src/share/vm/yuhu/yuhuStack.cpp` - 使用 `CreateGetFrameAddress()`（第 58 行）

## 其他可能受影响的 Intrinsic

检查代码中是否还有其他 intrinsic 需要更新：

1. **llvm.memset** - 已经在使用 `"llvm.memset.p0i8.i32"`，看起来是正确的
2. **llvm.sin, llvm.cos, llvm.sqrt 等** - 这些使用 `.f64` 后缀，看起来也是正确的

## 验证方法

修复后，重新编译并运行测试：

```bash
java -XX:+UseYuhuCompiler -XX:TieredStopAtLevel=6 \
  -XX:CompileCommand=yuhuonly,com/example/Matrix.multiply \
  -XX:+PrintCompilation com.example.Matrix
```

如果修复成功，应该不再出现 IR 验证错误。

## 为什么这个错误现在才被发现？

### 与 006 调试代码的关系

这个错误是由 **006_getpointertofunction_returns_null.md** 中的调试代码暴露的：

1. **之前的 IR 验证**（第 443-447 行）：
   ```cpp
   if (YuhuVerifyFunction != NULL) {
     if (!fnmatch(YuhuVerifyFunction, name, 0)) {
       verifyFunction(*function);
     }
   }
   ```
   - 这是**条件验证**，只有当用户设置了 `-XX:YuhuVerifyFunction=...` 时才会执行
   - 默认情况下不会执行

2. **006 添加的调试代码**（第 484-489 行）：
   ```cpp
   // Debug 4: Verify IR correctness (不需要锁)
   tty->print_cr("Verifying Function IR...");
   if (llvm::verifyFunction(*function, &llvm::errs())) {
     fatal(err_msg("Function %s failed IR verification!", name));
   }
   ```
   - 这是**无条件验证**，总是执行
   - 这导致 IR 验证总是运行，从而发现了 intrinsic 名称的问题

3. **如果没有 006 的调试代码**：
   - IR 验证可能不会执行（除非设置了 `YuhuVerifyFunction`）
   - 即使 IR 有问题，程序可能会继续执行到 `getPointerToFunction`
   - 然后 `getPointerToFunction` 可能会因为 IR 问题而返回 NULL
   - 导致 006 的错误：`assert(code != NULL) failed: code must be != NULL`

### 结论

**是的，这个错误（008）是由 006 的调试代码暴露的。**

006 的调试代码添加了无条件的 IR 验证，从而在更早的阶段发现了 IR 问题（intrinsic 名称不正确）。这是一个**提前失败**（fail-fast）的好处：
- 在代码生成之前就发现了问题
- 错误信息更清晰（IR 验证错误 vs. 代码生成返回 NULL）
- 更容易定位问题根源

## 修复实现

### 修复内容

按照方案 1，修改了 `frame_address()` 方法，使用 LLVM 20 的正确 intrinsic 名称：

**修改前**：
```cpp
Value* YuhuBuilder::frame_address() {
  return make_function("llvm.frameaddress", "i", "C");
}
```

**修改后**：
```cpp
Value* YuhuBuilder::frame_address() {
#if LLVM_VERSION_MAJOR >= 20
  // LLVM 20+ requires type information in intrinsic name
  // llvm.frameaddress.p0: returns pointer type (p0)
  return make_function("llvm.frameaddress.p0", "i", "C");
#else
  return make_function("llvm.frameaddress", "i", "C");
#endif
}
```

### 关键修改点

1. **添加了条件编译**：根据 `LLVM_VERSION_MAJOR >= 20` 判断使用哪个名称
2. **LLVM 20+**：使用 `"llvm.frameaddress.p0"`（包含类型信息）
3. **旧版本**：使用 `"llvm.frameaddress"`（保持兼容性）

### 修改的文件

- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` - `frame_address()` 方法（第 414-422 行）

### 验证方法

修复后，重新编译并运行测试：

```bash
java -XX:+UseYuhuCompiler -XX:TieredStopAtLevel=6 \
  -XX:CompileCommand=yuhuonly,com/example/Matrix.multiply \
  -XX:+PrintCompilation com.example.Matrix
```

如果修复成功，应该不再出现 IR 验证错误，程序应该能够继续执行到 `getPointerToFunction`。

## 状态

- ✅ 问题已定位
- ✅ 根本原因已分析
- ✅ 与 006 调试代码的关系已确认
- ✅ 修复已实现
- ⏳ 等待测试验证


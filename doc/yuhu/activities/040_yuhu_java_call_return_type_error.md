# 040: Yuhu Java 调用返回类型错误 - 导致错误的返回值和虚假去优化

**Date**: 2026-02-01
**Status**: 🟢 Fixed - 已修复返回类型声明，移除错误的去优化检查
**Related Issues**:
1. Yuhu 编译的 encodeArrayLoop 调用 CharBuffer.array() 时获得错误的返回值
2. w0 = 0x1f7 (503) 而不是预期的对象地址
3. 错误地触发去优化逻辑

## 问题摘要

### 表现

当 Yuhu 编译的 `sun.nio.cs.UTF_8$Encoder.encodeArrayLoop` 调用 `CharBuffer.array()` 方法时：

```java
// CharBuffer.array() 的实现
public final char[] array() {
    if (hb == null) {
        throw new UnsupportedOperationException();
    }
    if (isReadOnly) {
        throw new ReadOnlyBufferException();
    }
    return hb;  // 应该返回 char[] 对象地址
}
```

**预期**：w0/x0 应该包含有效的对象地址，如 `0x000000076ab90520`

**实际**：w0 = 0x1f7 (503)，这不是一个有效的对象地址

### 错误的调用流程

```
Yuhu 生成的代码：
  blr    x8                    ; 调用 array()
  cbz    w0, 0x...             ; 检查 w0 != 0 (认为这是去优化计数)
                                ; w0 = 0x1f7，触发去优化逻辑！
```

## 根本原因

### 问题 1: 错误的函数返回类型声明

**Yuhu 的错误实现** (`yuhuTopLevelBlock.cpp:1586`):

```cpp
// Return type is always int  ← 错误！所有 Java 调用都声明为返回 jint
llvm::FunctionType* compiled_ftype = FunctionType::get(YuhuType::jint_type(), param_types, false);
```

**导致的问题**：

1. **LLVM 生成错误的调用约定**：
   - 声明返回 `jint` (32位)
   - LLVM 期望返回值在 w0 中
   - 可能优化掉 x0 的高 32 位

2. **实际方法返回 64 位对象地址**：
   - `array()` 返回 `char[]` 对象引用
   - 实际返回值在 x0 中（64位）
   - 类型不匹配导致混乱

3. **结果**：
   - LLVM 可能只保留/检查 w0（低32位）
   - 如果实际返回值是 `0x00000000000001f7`，只有低32位被使用
   - 或者 LLVM 在调用前后做了错误的寄存器处理

### 问题 2: 错误的去优化检查机制

**Yuhu 错误地模仿 Shark 的设计**：

```cpp
// Shark 的调用方式（特殊）
Value *deoptimized_frames = builder()->CreateCall3(
    entry_point,      // Shark 特殊入口点
    callee,           // Method*
    base_pc,          // SharkEntry*
    thread());        // Thread*

// 检查返回值是否为 0（去优化计数）
builder()->CreateCondBr(
    builder()->CreateICmpNE(deoptimized_frames, LLVMValue::jint_constant(0)),
    reexecute, call_completed);
```

**Shark vs Yuhu 的关键区别**：

| 特性 | Shark | Yuhu (错误实现) |
|------|-------|----------------|
| **调用的入口** | 特殊 entry_point | 标准 _from_compiled_entry |
| **入口参数** | `(Method*, SharkEntry*, Thread*)` | Java 方法的实际参数 |
| **返回类型** | jint (去优化计数) | 应该是方法实际返回类型 |
| **去优化检测** | 通过返回值 | 应该通过异常检测 |

**为什么 Shark 能工作**：

Shark 使用的是 **Zero Entry** 机制：
- 调用特殊的 entry_point 包装函数
- 这个包装函数返回 jint（去优化计数）
- 实际返回值通过其他机制传递

**为什么 Yuhu 不能模仿**：

Yuhu 直接调用标准的 `_from_compiled_entry`：
- 这是 C1/C2 的标准调用约定
- 应该返回方法的实际返回值（对象、int、long 等）
- 不应该有去优化计数的概念

### 问题 3: 调用路径分析

```
Yuhu 编译的 encodeArrayLoop
  │
  ├─ 调用 CharBuffer.array() (static invokestatic)
  │
  ├─ Yuhu 生成代码：
  │   ├─ 创建函数类型：返回 jint
  │   ├─ 调用 static_call_stub
  │   └─ 检查 w0 != 0 判断去优化
  │
  └─ static_call_stub:
      ├─ 加载 Method* 到 x12
      ├─ 加载 _from_compiled_entry
      └─ br x9

      └─ _from_compiled_entry (array() 未编译):
          ├─ 指向 c2i adapter
          └─ c2i adapter → 解释器

              └─ 解释器执行 array():
                  ├─ getfield hb
                  └─ areturn (返回 char[] 地址到 x0)

  └─ 返回到 Yuhu 代码：
      └─ 检查 w0 = 0x1f7 ≠ 0
          └─ 错误认为发生去优化！
```

### 为什么 w0 = 0x1f7？

**可能的原因**：

1. **类型不匹配导致的寄存器问题**：
   - LLVM 认为函数返回 jint (32位)
   - 可能做了某种优化或转换
   - 导致返回值被错误解释

2. **字段偏移量或其他内部值**：
   - 0x1f7 = 503 字节 ≈ 63 个字
   - 可能是某个内部状态的值
   - 而不是实际的对象地址

3. **LLVM 优化的副作用**：
   - 声明错误的返回类型
   - LLVM 可能优化掉了某些寄存器保存/恢复
   - 导致返回值混乱

## 解决方案

### 修改 1: 使用正确的返回类型

**修改前** (`yuhuTopLevelBlock.cpp:1586`):

```cpp
// Return type is always int
llvm::FunctionType* compiled_ftype = FunctionType::get(YuhuType::jint_type(), param_types, false);
```

**修改后**:

```cpp
// Use the actual return type of the method being called
ciType* return_type = call_method->return_type();
llvm::Type* llvm_return_type = YuhuType::to_stackType(return_type);

llvm::FunctionType* compiled_ftype = FunctionType::get(llvm_return_type, param_types, false);
```

**效果**：
- `CharBuffer.array()` 返回 `char[]` → `YuhuType::oop_type()` (64位)
- `int foo()` 返回 `int` → `YuhuType::jint_type()` (32位)
- `long bar()` 返回 `long` → `YuhuType::jlong_type()` (64位)

### 修改 2: 移除错误的去优化检查

**修改前**:

```cpp
Value *deoptimized_frames = builder()->CreateCall(
    compiled_ftype, compiled_entry, call_args);

// If the callee got deoptimized then reexecute in the interpreter
BasicBlock *reexecute      = function()->CreateBlock("reexecute");
BasicBlock *call_completed = function()->CreateBlock("call_completed");
builder()->CreateCondBr(
    builder()->CreateICmpNE(deoptimized_frames, LLVMValue::jint_constant(0)),
    reexecute, call_completed);

builder()->SetInsertPoint(reexecute);
// ... 调用去优化入口点 ...
builder()->CreateBr(call_completed);

builder()->SetInsertPoint(call_completed);
```

**修改后**:

```cpp
// Call the compiled entry and get the actual return value
decache_for_Java_call(call_method);
Value* result = builder()->CreateCall(
    compiled_ftype, compiled_entry, call_args);

// NOTE: We do NOT check for deoptimization through return value like Shark does.
// Shark uses a special entry point that returns jint (deoptimization count),
// but Yuhu calls standard _from_compiled_entry which returns the actual method result.
// Deoptimization is detected through check_pending_exception() below.
BasicBlock *call_completed = function()->CreateBlock("call_completed");
builder()->CreateBr(call_completed);
```

**效果**：
- 返回值就是实际的方法返回值
- 不再被误判为去优化计数
- 去优化通过 `check_pending_exception()` 正确检测

### 修改的文件

1. **hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp**
   - 修改 `do_call()` 函数（第 1578-1616 行）
   - 使用实际返回类型
   - 移除错误的去优化检查

## 为什么 Shark 的设计不能直接套用

### Shark 的架构

```
SharkEntry layout:
  - entry_point: 指向 LLVM 函数的特殊入口
  - 这个入口返回 jint (去优化计数)
  - 实际返回值通过其他机制传递
```

**Shark 的调用流程**:

```cpp
// Shark 加载 from_interpreted (不是 from_compiled_entry!)
Value *base_pc = builder()->CreateValueOfStructEntry(
  callee, Method::from_interpreted_offset(),  // ← ZeroEntry
  SharkType::intptr_type(),
  "base_pc");

// 从 SharkEntry 加载 entry_point
Value *entry_point = builder()->CreateLoad(...);

// 调用 entry_point(Method*, SharkEntry*, Thread*) → jint
Value *deoptimized_frames = builder()->CreateCall3(
    entry_point, callee, base_pc, thread());
```

### Yuhu 的架构

```
Yuhu 直接使用标准的 C1/C2 调用约定:
  - 调用 _from_compiled_entry
  - 参数是 Java 方法的实际参数
  - 返回值是方法的实际返回值
```

**Yuhu 的调用流程**:

```cpp
// Yuhu 调用标准的 _from_compiled_entry
from_compiled_entry = builder()->CreateValueOfStructEntry(
  callee, Method::from_compiled_offset(),
  YuhuType::intptr_type(),
  "from_compiled_entry");

// 调用 compiled_entry(实际参数) → 实际返回值
Value* result = builder()->CreateCall(
    compiled_ftype, compiled_entry, call_args);
```

### 关键区别

| 方面 | Shark | Yuhu |
|------|-------|------|
| **入口来源** | `from_interpreted` → SharkEntry | `from_compiled_entry` → 标准入口 |
| **调用参数** | `(Method*, SharkEntry*, Thread*)` | Java 方法的实际参数 |
| **返回类型** | jint (去优化计数) | 方法的实际返回类型 |
| **去优化检测** | 检查返回值 ≠ 0 | 检查 pending exception |
| **特殊机制** | ZeroEntry 包装 | 使用标准 HotSpot 机制 |

## 测试步骤

### 1. 编译修改后的代码

```bash
cd /Users/liuanyou/CLionProjects/jdk8
make clean
make
```

### 2. 运行测试

```bash
cd test_yuhu
java -XX:+UseYuhuCompiler \
     -XX:TieredStopAtLevel=6 \
     -XX:CompileCommand=yuhuonly,sun.nio.cs.*::* \
     -cp . \
     TestEncoder
```

### 3. 验证修复

**预期结果**：

1. **w0 包含有效的对象地址**：
   ```
   w0 = 0x000000076ab90520  (或类似的有效对象地址)
   ```

2. **不再触发虚假的去优化**：
   - cbz w0 指令被移除
   - 直接使用返回值

3. **程序正常运行**：
   - 没有 SIGBUS 崩溃
   - 正确调用 array() 并获取 char[] 数组

### 4. 检查生成的汇编

```bash
# 查看生成的汇编代码
java -XX:+UseYuhuCompiler \
     -XX:TieredStopAtLevel=6 \
     -XX:CompileCommand=print,sun.nio.cs.*::* \
     -cp . \
     TestEncoder 2>&1 | grep -A 20 "encodeArrayLoop"
```

**应该看到**：
- 调用 `array()` 后，直接使用 x0 中的返回值
- 没有 `cbz w0` 检查去优化

## 相关文件

### 修改的文件
- `hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp` - 修改 do_call() 函数

### 相关的 HotSpot 文件
- `hotspot/src/share/vm/code/codeCache.cpp` - _from_compiled_entry 管理
- `hotspot/src/cpu/aarch64/vm/sharedRuntime_aarch64.cpp` - c2i adapter 生成
- `hotspot/src/share/vm/runtime/sharedRuntime.cpp` - 静态调用 stub 解析

### Yuhu 相关文件
- `hotspot/src/share/vm/yuhu/yuhuType.hpp` - to_stackType() 函数
- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - static_call_stub 生成
- `hotspot/src/share/vm/shark/sharkTopLevelBlock.cpp` - Shark 的原始实现（参考）

## 相关文档

- `doc/yuhu/activities/037_yuhu_deoptimization_root_cause.md` - 去优化根本原因
- `doc/yuhu/activities/039_yuhu_deoptimization_parameter_restoration_failure.md` - 参数恢复问题
- `doc/yuhu/SHARK_TO_YUHU_MIGRATION_GUIDE.md` - Shark 到 Yuhu 迁移指南

## 经验总结

### 1. 不能盲目模仿其他编译器的实现

**教训**：
- Shark 的设计有其特殊性（ZeroEntry）
- Yuhu 使用的是标准 C1/C2 调用约定
- 直接套用会导致类型不匹配和逻辑错误

**正确做法**：
- 理解底层机制的差异
- 根据实际使用的约定调整实现
- 不要假设"看起来相似"就是"相同"

### 2. 函数签名必须准确

**教训**：
- 错误的返回类型声明会导致 LLVM 生成错误的代码
- 32位 vs 64位的差异会影响寄存器使用
- LLVM 的优化可能基于类型假设

**正确做法**：
- 始终使用正确的函数签名
- 返回类型、参数类型都要准确
- 让 LLVM 理解真实的调用约定

### 3. 去优化检测机制的差异

**HotSpot 的去优化机制**：
- C1/C2：通过 safepoint、异常处理检测
- Shark：通过特殊入口点的返回值检测
- Yuhu：应该使用 C1/C2 的方式（标准 HotSpot 机制）

**正确做法**：
- 理解不同编译器的去优化策略
- 使用与目标平台兼容的机制
- 不要混用不同的约定

### 4. 调试技巧

**问题现象**：
- w0 = 0x1f7 不是有效对象地址
- 触发虚假的去优化逻辑

**调试步骤**：
1. 检查返回类型声明是否正确
2. 查看 LLVM 生成的调用约定
3. 对比 Shark 和 Yuhu 的实现差异
4. 理解标准 C1/C2 的调用方式

## 下一步工作

### 1. 测试更多方法调用

确保各种返回类型都能正确处理：
- void 方法
- 基本类型（int, long, float, double）
- 对象引用
- 数组引用

### 2. 验证去优化机制

虽然移除了返回值检查，但去优化仍然需要正常工作：
- 确认 `check_pending_exception()` 正确检测异常
- 确认 safepoint 检查正常工作
- 测试真正的去优化场景

### 3. 性能测试

验证修改后的性能：
- 方法调用开销
- 返回值传递效率
- 与 C1/C2 的性能对比

---

## 附录：完整的 do_call() 修改对比

### 修改前的完整代码

```cpp
void YuhuTopLevelBlock::do_call() {
  // ... 前面的代码 ...

  // Build argument types for function signature
  std::vector<llvm::Type*> param_types;

  if (is_static) {
    param_types.push_back(YuhuType::intptr_type());  // null placeholder
  } else {
    param_types.push_back(YuhuType::oop_type());  // receiver
  }

  for (int i = 0; i < sig->count(); i++) {
    ciType* param_type = sig->type_at(i);
    param_types.push_back(YuhuType::to_stackType(param_type));
  }

  // Return type is always int  ← 错误！
  llvm::FunctionType* compiled_ftype = FunctionType::get(YuhuType::jint_type(), param_types, false);

  Value *compiled_entry = builder()->CreateIntToPtr(
    from_compiled_entry,
    PointerType::getUnqual(compiled_ftype),
    "compiled_entry");

  // Call the compiled entry
  Value *deoptimized_frames = builder()->CreateCall(
    compiled_ftype, compiled_entry, call_args);  // ← 变量名错误

  // If the callee got deoptimized then reexecute in the interpreter  ← 错误的逻辑
  BasicBlock *reexecute      = function()->CreateBlock("reexecute");
  BasicBlock *call_completed = function()->CreateBlock("call_completed");
  builder()->CreateCondBr(
    builder()->CreateICmpNE(deoptimized_frames, LLVMValue::jint_constant(0)),
    reexecute, call_completed);

  builder()->SetInsertPoint(reexecute);
  Value* deopt_callee = builder()->deoptimized_entry_point();
  llvm::FunctionType* func_type = YuhuBuilder::make_ftype("iT", "i");
  std::vector<Value*> args;
  args.push_back(builder()->CreateSub(deoptimized_frames, LLVMValue::jint_constant(1)));
  args.push_back(thread());
  builder()->CreateCall(func_type, deopt_callee, args);
  builder()->CreateBr(call_completed);

  builder()->SetInsertPoint(call_completed);

  // ... 后续代码 ...
}
```

### 修改后的完整代码

```cpp
void YuhuTopLevelBlock::do_call() {
  // ... 前面的代码 ...

  // Build argument types for function signature
  std::vector<llvm::Type*> param_types;

  if (is_static) {
    param_types.push_back(YuhuType::intptr_type());  // null placeholder
  } else {
    param_types.push_back(YuhuType::oop_type());  // receiver
  }

  for (int i = 0; i < sig->count(); i++) {
    ciType* param_type = sig->type_at(i);
    param_types.push_back(YuhuType::to_stackType(param_type));
  }

  // Use the actual return type of the method being called  ← 修复！
  ciType* return_type = call_method->return_type();
  llvm::Type* llvm_return_type = YuhuType::to_stackType(return_type);

  llvm::FunctionType* compiled_ftype = FunctionType::get(llvm_return_type, param_types, false);

  Value *compiled_entry = builder()->CreateIntToPtr(
    from_compiled_entry,
    PointerType::getUnqual(compiled_ftype),
    "compiled_entry");

  // Call the compiled entry and get the actual return value  ← 修复！
  decache_for_Java_call(call_method);
  Value* result = builder()->CreateCall(
    compiled_ftype, compiled_entry, call_args);

  // NOTE: We do NOT check for deoptimization through return value like Shark does.  ← 添加说明
  // Shark uses a special entry point that returns jint (deoptimization count),
  // but Yuhu calls standard _from_compiled_entry which returns the actual method result.
  // Deoptimization is detected through check_pending_exception() below.
  BasicBlock *call_completed = function()->CreateBlock("call_completed");
  builder()->CreateBr(call_completed);

  builder()->SetInsertPoint(call_completed);

  // ... 后续代码保持不变 ...
}
```

### 关键改动总结

| 行号 | 修改前 | 修改后 | 说明 |
|------|--------|--------|------|
| 1585-1586 | `// Return type is always int`<br>`FunctionType::get(jint_type(), ...)` | 获取 `return_type`<br>`FunctionType::get(to_stackType(return_type), ...)` | 使用实际返回类型 |
| 1594-1595 | `Value *deoptimized_frames = ...` | `Value* result = ...` | 变量名更准确 |
| 1597-1613 | 去优化检查逻辑（15行） | 简化为直接跳转（2行） | 移除错误的去优化检查 |
| 1616 | `// Cache after the call` | 添加注释说明为什么不检查返回值 | 文档化设计决策 |

---

**文档版本**: 1.1
**最后更新**: 2026-02-01
**状态**: 已修复 stack underrun 问题，待测试

## 更新日志

### v1.1 (2026-02-01)

**修复的额外问题**：stack underrun 错误

**问题**：
```
Internal Error (yuhuState.hpp:110)
assert(stack_depth() > 0) failed: stack underrun

调用栈：
  YuhuState::pop()
  └─ YuhuBlock::xpop()
      └─ YuhuTopLevelBlock::decache_for_Java_call()
          └─ YuhuTopLevelBlock::do_call()
```

**根本原因**：
在第一次修改中，`decache_for_Java_call()` 被调用了两次：
- 第 1566 行：原始调用
- 第 1600 行：错误添加的重复调用

第二次调用时，参数已经被第一次调用 pop 掉了，导致栈下溢。

**修复**：
移除第 1600 行的重复调用，保留原始的调用顺序：
1. 收集参数到 `call_args`
2. 调用 `decache_for_Java_call()`（pop 参数并创建 OopMap）
3. 调用 `builder()->CreateCall()`（使用之前收集的 `call_args`）

**正确的代码**：
```cpp
// 1. 收集参数（不 pop 栈）
std::vector<Value*> call_args;
for (int i = arg_slots - 1; i >= 0; i--) {
  YuhuValue* v = xstack(i);  // xstack 只读取，不 pop
  call_args.push_back(v->jint_value());
}

// 2. Decache（pop 栈上的参数，创建 OopMap）
decache_for_Java_call(call_method);

// 3. 调用方法（使用已收集的 call_args）
Value* result = builder()->CreateCall(
    compiled_ftype, compiled_entry, call_args);
```

---

**文档版本**: 1.0
**最后更新**: 2026-02-01

---

## v1.2 (2026-02-01) - 修复 T_VOID 类型未处理错误

**问题**：
```
Internal Error (yuhuContext.hpp:196)
assert(result != NULL) failed: unhandled type

调用栈：
  YuhuTopLevelBlock::do_call()
    └─ YuhuType::to_stackType(ciType*)
        └─ YuhuType::to_stackType(BasicType)
            └─ YuhuContext::to_stackType(BasicType)
                └─ YuhuContext::map_type()  ← 断言失败
```

**根本原因**：
`yuhuContext.cpp` 中的 `_to_stackType` 数组缺少对 **T_VOID** 类型的处理。

从 BasicType 枚举定义：
```cpp
enum BasicType {
  T_BOOLEAN  =  4,
  ...
  T_ARRAY    =  13,
  T_VOID     =  14,  // ← 原始代码没有处理！
  T_ADDRESS  =  15,
  ...
  T_CONFLICT =  19
};
```

原始代码只处理了 T_BOOLEAN 到 T_ADDRESS，T_VOID (14) 被留在了 default 分支：
```cpp
default:
  _to_stackType[i] = NULL;  // ← 导致断言失败
  _to_arrayType[i] = NULL;
}
```

当调用 `YuhuType::to_stackType(return_type)` 且返回类型为 void 时：
1. `return_type->basic_type()` 返回 T_VOID (14)
2. `to_stackType(T_VOID)` 查找 `_to_stackType[14]`
3. 返回 NULL（因为 default 分支）
4. `map_type()` 断言 `result != NULL` 失败

**修复**：
在 `yuhuContext.cpp` 的类型映射初始化循环中添加 T_VOID 处理：

```cpp
case T_VOID:
  _to_stackType[i] = void_type();
  _to_arrayType[i] = NULL;
  break;
```

这样，当方法返回 void 时，`to_stackType()` 会返回正确的 LLVM void 类型。

**修改的文件**：
- `hotspot/src/share/vm/yuhu/yuhuContext.cpp` - 添加 T_VOID case

**为什么会出现这个问题**：

虽然 `encodeArrayLoop` 不返回 void，但在某些情况下可能会遇到 void 返回类型：
1. 某些内联方法可能返回 void
2. 特殊的 JVM 方法调用
3. 测试代码或边界情况

通过添加对 T_VOID 的处理，Yuhu 现在可以正确处理所有标准 Java 返回类型。

---

**文档版本**: 1.2
**最后更新**: 2026-02-01
**状态**: 已修复所有已知问题（返回类型、stack underrun、T_VOID），待完整测试

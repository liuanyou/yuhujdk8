# 方法重复编译失败问题分析

## 问题现象

虽然使用了 `-XX:CompileCommand=yuhuonly,com/example/Matrix.multiply` 参数，但仍然触发了其他方法的编译：

1. **第一次编译**：`Matrix.multiply` 方法（符合预期）
2. **第二次编译**：`encodeArrayLoop` 方法（**不符合预期**），编译失败：
   ```
   Internal Error (/Users/liuanyou/CLionProjects/jdk8/hotspot/src/share/vm/yuhu/yuhuCacheDecache.cpp:174)
   Error: ShouldNotReachHere()
   ```

错误发生在 `YuhuNormalEntryCacher::get_function_arg()` 方法中，当尝试访问函数参数时超出了参数范围。

## yuhuonly 参数为什么没有生效？

### yuhuonly 的实现机制

`-XX:CompileCommand=yuhuonly,method` 参数在以下位置被检查：

1. **simpleThresholdPolicy.cpp** (行 432-445):
   ```cpp
   CompLevel SimpleThresholdPolicy::call_event(Method* method, CompLevel cur_level) {
   #ifdef YUHU
     // Check if method is forced to use Yuhu compiler via CompilerOracle
     methodHandle mh(method);
     if (CompilerOracle::should_compile_with_yuhu_only(mh)) {
       // Force use Yuhu optimized compilation level
       CompLevel yuhu_level = CompLevel_yuhu_optimized;
       return MIN2(yuhu_level, (CompLevel)TieredStopAtLevel);
     }
   #endif
     // ... 正常的编译级别决策
   }
   ```

2. **advancedThresholdPolicy.cpp** (行 405-420):
   ```cpp
   CompLevel AdvancedThresholdPolicy::call_event(Method* method, CompLevel cur_level) {
   #ifdef YUHU
     methodHandle mh(method);
     if (CompilerOracle::should_compile_with_yuhu_only(mh)) {
       CompLevel yuhu_level = CompLevel_yuhu_optimized;
       return MIN2(yuhu_level, (CompLevel)TieredStopAtLevel);
     }
   #endif
     // ... 正常的编译级别决策
   }
   ```

### 问题：yuhuonly 只控制编译级别，不控制是否编译

**关键发现：**
- `yuhuonly` 检查只在 `call_event()` 和 `loop_event()` 中执行
- 这些方法决定的是**用哪个编译器编译**（返回 CompLevel）
- 但它们**不决定是否编译该方法**
- 即使没有匹配 `yuhuonly` 规则的方法，仍然会被编译，只是使用 C1/C2 而不是 Yuhu

### 为什么 encodeArrayLoop 会被编译？

可能的触发原因：

1. **内联决策触发编译**：
   - 当编译 `Matrix.multiply` 时，可能调用了其他方法
   - JVM 的内联优化器决定内联某个调用点
   - 这触发了被调用方法的编译（如 `encodeArrayLoop`）
   - 但因为不匹配 `yuhuonly` 规则，所以用 C1/C2 编译

2. **字符串操作触发**：
   - `Matrix.multiply` 可能包含字符串操作（如日志输出）
   - 这些操作最终调用到 `CharBuffer.encodeArrayLoop`
   - JVM 决定编译这个热点方法

3. **分层编译策略**：
   - 即使使用 `yuhuonly`，分层编译策略仍然在运行
   - 它监控所有方法的调用计数
   - 当某个方法达到阈值时，仍然会触发编译

### yuhuonly 的真实语义

```
yuhuonly 的实际含义：
  "如果要编译这个方法，只能用 Yuhu 编译器"
  
而不是：
  "只编译这个方法，其他方法都不编译"
```

要实现"只编译指定方法"的效果，需要结合：
- `yuhuonly,method` (强制该方法用 Yuhu)
- `exclude,*.*` (排除所有其他方法)
- 或者修改 `should_compile_with_yuhu_only` 的检查逻辑

## `func->arg_begin()` 和 `func->arg_end()` 的作用

### LLVM 迭代器基础

`func->arg_begin()` 和 `func->arg_end()` 是 **LLVM 函数参数的迭代器边界**：

```cpp
llvm::Function* func = ...;
llvm::Function::arg_iterator ai = func->arg_begin();  // 指向第一个参数
llvm::Function::arg_iterator end = func->arg_end();   // 指向最后一个参数之后的位置

while (ai != end) {
  llvm::Argument* arg = &*ai;  // 获取当前参数
  ai++;  // 移动到下一个参数
}
```

**关键点：**
- `arg_begin()` - 指向第一个参数
- `arg_end()` - 指向**越界位置**（不是最后一个参数，而是最后一个参数的下一个位置）
- `ai == arg_end()` - 表示**已经遍历完所有参数，没有更多参数了**

### 为什么 `local_index=2` 就失败了

根据您提供的信息：
- `local_index = 2`
- 在 `for (int i = 0; i < local_index; i++)` 循环中
- 当 `i = 1` 时，`ai++` 后匹配到 `ai == func->arg_end()`

**这说明什么？**

```cpp
// 当 local_index = 2 时
for (int i = 0; i < 2; i++) {  // i 会是 0, 1
  ai++;  // 第1次：ai 从参数0移动到参数1
         // 第2次：ai 从参数1移动到 arg_end()
  if (ai == func->arg_end()) {  // 第2次循环时触发
    ShouldNotReachHere();  // ← 在这里失败
  }
}
```

**结论：LLVM 函数只有 2 个参数！**

### 为什么只有 2 个参数？

查看 `yuhuFunction.cpp:45-64` 中的 `generate_normal_entry_point_type()` 方法：

```cpp
llvm::FunctionType* YuhuFunction::generate_normal_entry_point_type() const {
  std::vector<llvm::Type*> params;
  
  // For static methods, first parameter is NULL (x0)
  if (is_static()) {
    params.push_back(YuhuType::intptr_type());  // 参数 0: void* null
  }
  
  // Add Java method parameters
  ciSignature* sig = target()->signature();
  int param_count = sig->count();  // 从方法签名获取参数数量
  for (int i = 0; i < param_count; i++) {
    ciType* param_type = sig->type_at(i);
    llvm::Type* llvm_type = YuhuType::to_stackType(param_type);
    params.push_back(llvm_type);  // 参数 1+: Java 方法参数
  }
  
  return FunctionType::get(YuhuType::jint_type(), params, false);
}
```

**对于 `encodeArrayLoop` 方法：**
```java
CoderResult encodeArrayLoop(CharBuffer cb, ByteBuffer bb)
```

**LLVM 函数签名应该是：**
```cpp
// 非静态方法
jint encodeArrayLoop(oop this, oop cb, oop bb)  // 3 个参数
```

**但实际上只有 2 个参数，说明：**
1. 可能 `is_static()` 误判为 `true`，导致添加了 NULL 参数但少了 `this`
2. 或者 `sig->count()` 返回的值不正确（只返回了 1 个参数而不是 2 个）
3. 或者在函数创建过程中出现了其他错误

### 如何调试

需要打印以下信息来确定根本原因：

```cpp
llvm::Argument* YuhuNormalEntryCacher::get_function_arg(int local_index) {
  llvm::Function* func = function()->function();
  int actual_arg_count = std::distance(func->arg_begin(), func->arg_end());
  
  tty->print_cr("get_function_arg: local_index=%d", local_index);
  tty->print_cr("  LLVM function arg_count=%d", actual_arg_count);
  tty->print_cr("  is_static=%d", is_static());
  tty->print_cr("  Java arg_size=%d", arg_size());
  tty->print_cr("  Java max_locals=%d", max_locals());
  
  // Print all LLVM function arguments
  tty->print_cr("  LLVM function arguments:");
  int idx = 0;
  for (auto& arg : func->args()) {
    tty->print_cr("    [%d] %s", idx++, arg.getName().str().c_str());
  }
  
  // ... 原有代码
}
```

### 预期的正确行为

**对于非静态方法 `encodeArrayLoop(CharBuffer cb, ByteBuffer bb)`：**
- Java `arg_size` = 3 (this + 2个参数)
- LLVM 函数应该有 3 个参数：
  ```cpp
  参数[0]: oop this
  参数[1]: oop cb
  参数[2]: oop bb
  ```

**对于静态方法：**
- Java `arg_size` = n (n个参数)
- LLVM 函数应该有 n+1 个参数：
  ```cpp
  参数[0]: void* null  (占位符，对应 x0 寄存器)
  参数[1..n]: Java 参数
  ```

### 当前的 Bug

**症状：**
- Java 方法有 3 个参数（this + 2个对象）
- 但 LLVM 函数只有 2 个参数
- 当尝试访问 `local[2]`（对应第3个参数）时失败

**可能的原因：**
1. `ciSignature::count()` 没有包含 `this` 指针（这是正常的，因为 `this` 不在签名中）
2. 但代码没有为非静态方法添加隐式的 `this` 参数到 LLVM 函数签名中
3. 或者 `this` 参数被添加了，但有一个参数丢失了

```
Method: encodeArrayLoop
Signature: (Ljava/nio/CharBuffer;Ljava/nio/ByteBuffer;)Ljava/nio/charset/CoderResult;
Max locals: 12, arg_size: 3
```

**关键信息：**
- 方法签名表明有 2 个对象参数（CharBuffer, ByteBuffer）
- `arg_size: 3` 表示包括隐式的 `this` 指针，共 3 个参数
- `max_locals: 12` 表示局部变量表有 12 个槽位

## 问题原因分析

### 1. 参数数量不匹配

查看 `get_function_arg` 的实现：

```cpp
llvm::Argument* YuhuNormalEntryCacher::get_function_arg(int local_index) {
  llvm::Function* func = function()->function();
  llvm::Function::arg_iterator ai = func->arg_begin();
  
  if (is_static()) {
    // Static methods: skip NULL at function_arg[0]
    ai++;  // Skip NULL argument
  }
  
  // Advance to the requested local index
  for (int i = 0; i < local_index; i++) {
    ai++;
    if (ai == func->arg_end()) {
      ShouldNotReachHere();  // <-- 在这里失败
      return NULL;
    }
  }
  
  return &*ai;
}
```

**问题：**
1. 代码尝试访问 `local_index` 对应的函数参数
2. 但函数参数数量（LLVM Function 的参数）可能少于 `max_locals`（12）
3. 当 `local_index >= 实际函数参数数量` 时，迭代器会到达 `arg_end()`，触发断言

### 2. 局部变量 vs 函数参数的区别

在 Java 字节码中：
- **函数参数**（`arg_size`）：方法的实际参数，存储在局部变量表的开头
  - 对于非静态方法：local[0] = this, local[1..n] = 参数
  - 对于静态方法：local[0..n] = 参数
  
- **局部变量表**（`max_locals`）：包含所有局部变量
  - 包括函数参数
  - 包括方法内部定义的局部变量
  - 包括临时变量

**示例：**
```java
void encodeArrayLoop(CharBuffer cb, ByteBuffer bb) {
  // arg_size = 3: this, cb, bb
  // max_locals = 12: this, cb, bb, + 9 个局部变量
  int i = 0;        // local[3]
  int j = 0;        // local[4]
  char c;           // local[5]
  // ... 更多局部变量到 local[11]
}
```

### 3. LLVM 函数签名 vs Java 局部变量表

LLVM 函数的参数列表只对应 Java 方法的 **函数参数**（arg_size），不包括方法内部的局部变量。

**LLVM 函数签名（对于非静态方法）：**
```cpp
// 对于 encodeArrayLoop(CharBuffer cb, ByteBuffer bb)
// LLVM 函数可能是：
i32 encodeArrayLoop(i8* %this, i8* %cb, i8* %bb)
// 只有 3 个参数
```

**但是 `process_local_slot` 被调用时：**
```cpp
void YuhuNormalEntryCacher::process_local_slot(int index, YuhuValue** addr, int offset) {
  // index 可能是 0-11（因为 max_locals = 12）
  // 但 LLVM 函数只有 3 个参数（arg_size = 3）
  
  if (local_slot_needs_read(index, value) && index < arg_size()) {
    // 这个条件检查了 index < arg_size()，应该是安全的
    llvm::Argument* arg = get_function_arg(index);  // 但这里还是失败了
  }
}
```

### 4. 可能的根本原因

**情况 1：`arg_size` 计算错误**
- 如果 `arg_size()` 返回的值大于实际的 LLVM 函数参数数量
- 导致 `index < arg_size()` 检查通过，但实际 LLVM 函数没有这么多参数

**情况 2：静态方法判断错误**
- 如果方法实际上是静态的，但 `is_static()` 返回 `false`
- 或者相反，导致参数索引计算错误

**情况 3：LLVM 函数生成时参数数量不正确**
- 在生成 LLVM 函数时，参数数量少于预期
- 例如，应该有 3 个参数但只生成了 2 个

## 为什么第一次成功第二次失败？

**可能的原因：**

1. **不同的方法特征：**
   - 第一次编译的 `Matrix.multiply` 方法可能局部变量较少
   - 第二次的 `encodeArrayLoop` 有 12 个局部变量槽位，触发了边界情况

2. **编译上下文不同：**
   - 可能在某些情况下（如 OSR、去优化等）函数签名生成方式不同
   - 第二次编译可能处于不同的编译模式

3. **状态污染：**
   - 第一次编译后留下的某些状态影响了第二次编译
   - 例如，某些静态变量或缓存没有正确重置

## 调试方向

### 1. 检查 `arg_size()` 的值
- 确认 `arg_size()` 返回的值是否正确
- 对比 LLVM 函数实际的参数数量

### 2. 检查 `is_static()` 的值
- 确认方法的静态/非静态属性是否正确识别

### 3. 检查 LLVM 函数生成
- 在函数生成时打印参数数量
- 确认生成的 LLVM 函数参数数量与预期一致

### 4. 检查 `process_local_slot` 的调用
- 打印每次调用时的 `index` 值
- 确认哪个 `index` 导致了失败

### 5. 对比第一次和第二次编译
- 记录两次编译时的关键参数
- 找出差异点

## 解决方案建议

### 短期解决方案：增强边界检查

在 `get_function_arg` 中增加更详细的错误信息：

```cpp
llvm::Argument* YuhuNormalEntryCacher::get_function_arg(int local_index) {
  llvm::Function* func = function()->function();
  int actual_arg_count = func->arg_size();
  
  tty->print_cr("get_function_arg: local_index=%d, actual_arg_count=%d, is_static=%d", 
                local_index, actual_arg_count, is_static());
  
  llvm::Function::arg_iterator ai = func->arg_begin();
  int skip_count = is_static() ? 1 : 0;
  int required_args = local_index + skip_count;
  
  if (required_args >= actual_arg_count) {
    tty->print_cr("ERROR: Attempting to access arg %d but function only has %d args", 
                  required_args, actual_arg_count);
    tty->print_cr("Method: %s", method()->name()->as_C_string());
    tty->print_cr("arg_size=%d, max_locals=%d", arg_size(), max_locals());
    ShouldNotReachHere();
  }
  
  // ... 原有代码
}
```

### 中期解决方案：修复参数数量不一致

1. **确保 LLVM 函数参数数量正确：**
   - 在函数创建时，参数数量应该等于 `arg_size`
   - 对于非静态方法：`arg_size` 包括 `this`
   - 对于静态方法：需要在 i2c adapter 中处理 NULL 参数

2. **修正 `arg_size` 的计算：**
   - 确认 `arg_size()` 返回的是正确的值
   - 可能需要区分"Java 方法参数数量"和"LLVM 函数参数数量"

### 长期解决方案：重构参数处理

1. **明确区分不同类型的"参数"：**
   ```cpp
   int java_arg_size();     // Java 方法参数数量（包括 this）
   int llvm_arg_count();    // LLVM 函数参数数量
   int max_locals();        // 局部变量表大小
   ```

2. **使用更安全的参数访问方式：**
   ```cpp
   llvm::Value* get_local_value(int local_index) {
     if (local_index < arg_size()) {
       return get_function_arg(local_index);
     } else {
       // 对于非参数的局部变量，从栈帧中读取
       return read_from_frame(local_index);
     }
   }
   ```

## 相关代码位置

- 错误发生位置：`yuhuCacheDecache.cpp:174`
- 相关方法：
  - `YuhuNormalEntryCacher::get_function_arg()`
  - `YuhuNormalEntryCacher::process_local_slot()`
  - `arg_size()` 的实现
  - `is_static()` 的实现
  - LLVM 函数创建代码

## 总结

这个问题揭示了两个独立的问题：

### 问题 1：yuhuonly 参数的语义误解

**现状：**
- `yuhuonly` 只控制**编译器选择**（Yuhu vs C1/C2），不控制**是否编译**
- 即使使用 `yuhuonly,Matrix.multiply`，其他方法仍然可以被 C1/C2 编译
- 这导致 `encodeArrayLoop` 被 C1/C2 或 Yuhu 编译（取决于触发路径）

**解决方案：**
```bash
# 方案 1：同时使用 yuhuonly 和 exclude（推荐）
java -XX:+UseYuhuCompiler \
     -XX:CompileCommand=yuhuonly,com/example/Matrix.multiply \
     -XX:CompileCommand=exclude,*.* \
     -XX:CompileCommand=compileonly,com/example/Matrix.multiply \
     com.example.Matrix

# 方案 2：修改 yuhuonly 的实现，增加排他性检查
# 在 should_exclude() 中添加：
# if (lists[YuhuOnlyCommand] != NULL && 
#     !lists[YuhuOnlyCommand]->match(method)) {
#   return true;  // 排除所有非 yuhuonly 的方法
# }
```

### 问题 2：Yuhu 编译器的参数处理 bug

**根本原因：**
- Java 局部变量表大小（max_locals）大于函数参数数量（arg_size）
- 代码错误地尝试将所有局部变量槽位映射到 LLVM 函数参数
- 但 LLVM 函数只有函数参数，不包括方法内部局部变量
- 当访问 `local[arg_size]` 或更高索引时，LLVM 函数参数不足，触发断言

**`encodeArrayLoop` 的具体情况：**
- `max_locals=12`：局部变量表有 12 个槽位
- `arg_size=3`：只有 3 个函数参数（this + 2个对象参数）
- 当尝试访问 `local[3]` 到 `local[11]` 时失败

**解决方案见下一节的代码修复建议**

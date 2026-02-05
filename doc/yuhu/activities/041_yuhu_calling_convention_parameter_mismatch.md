# 041 Yuhu 调用约定参数不一致问题修复

## 问题描述

在调试 Yuhu 编译器时发现，Yuhu 编译的方法在接收和传递参数时与 HotSpot 的 Java 调用约定不一致，导致参数错位。

### 发现过程

1. 调试 Yuhu 编译的 `ComplexNumber.multiply(ComplexNumber)` 方法
2. 发现 x0 寄存器包含错误值 0x1f8（应该是 dummy）
3. 通过 lldb 确认调用者传递的参数是正确的（x1=this, x2=arg0）
4. 问题在于 Yuhu 函数签名定义与实际调用约定不匹配

## 根本原因

### HotSpot Java 调用约定（AArch64）

根据 `hotspot/src/cpu/aarch64/vm/assembler_aarch64.hpp` 定义：

```
|--------------------------------------------------------------------|
| c_rarg0  c_rarg1  c_rarg2 c_rarg3 c_rarg4 c_rarg5 c_rarg6 c_rarg7  |
|--------------------------------------------------------------------|
| r0       r1       r2      r3      r4      r5      r6      r7       |
|--------------------------------------------------------------------|
| j_rarg7  j_rarg0  j_rarg1 j_rarg2 j_rarg3 j_rarg4 j_rarg5 j_rarg6  |
|--------------------------------------------------------------------|
```

HotSpot 的 Java 调用约定使用：
- **x0 (j_rarg7)**: dummy/返回值槽（输入时未使用）
- **x1 (j_rarg0)**: 第一个实际参数（非静态方法为 this，静态方法为第一个参数）
- **x2-x7 (j_rarg1-j_rarg6)**: 剩余参数

这与标准 AArch64 调用约定（x0=第一个参数）不同！

### Yuhu 的错误

Yuhu 编译器使用标准 LLVM/AArch64 约定定义函数签名：

**错误签名（非静态）:**
```cpp
void foo(Klass* this, arg0, arg1, ..., Method*, base_pc, JavaThread*)
  x0        x1        x2    x3
```

**错误签名（静态）:**
```cpp
void foo(arg0, arg1, ..., Method*, base_pc, JavaThread*)
  x0    x1    x2
```

这导致：
1. **接收参数时错位**: Yuhu 期望 x0=this，但实际 x1=this
2. **调用其他方法时错位**: Yuhu 传递 x0=this，但被调用者期望 x1=this

## 修复方案

### 1. 修改函数签名定义 (`yuhuFunction.cpp`)

**函数**: `YuhuFunction::generate_normal_entry_point_type()`

**修改后的签名（非静态）:**
```cpp
void foo(void* dummy, Klass* this, arg0, arg1, ..., Method*, base_pc, JavaThread*)
  x0           x1         x2        x3
```

**修改后的签名（静态）:**
```cpp
void foo(void* dummy, arg0, arg1, ..., Method*, base_pc, JavaThread*)
  x0           x1     x2
```

**关键代码:**
```cpp
std::vector<llvm::Type*> params;

if (is_static()) {
    // 静态方法：x0 = dummy（i2c adapter 传递 NULL）
    params.push_back(YuhuType::intptr_type());  // void* dummy (x0)
} else {
    // 非静态方法：x0 = dummy，x1 = this
    params.push_back(YuhuType::intptr_type());  // Dummy in x0
    params.push_back(YuhuType::oop_type());     // this in x1
}

// 添加 Java 方法参数（从 sig->type_at(i)）
ciSignature* sig = target()->signature();
int param_count = sig->count();  // 不包括 this
for (int i = 0; i < param_count; i++) {
    ciType* param_type = sig->type_at(i);
    llvm::Type* llvm_type = YuhuType::to_stackType(param_type);
    params.push_back(llvm_type);
}
```

**注意**: `sig->count()` 不包括 this，但 `args_size()` 包括 this（对于非静态方法）。

### 2. 修改调用其他方法 (`yuhuTopLevelBlock.cpp`)

**函数**: `YuhuTopLevelBlock::do_call()`

#### 2.1 修改函数类型签名构建

```cpp
std::vector<llvm::Type*> param_types;

if (is_static) {
    // 静态方法：x0 = dummy，x1 = 第一个参数（无需 NULL 占位）
    param_types.push_back(YuhuType::intptr_type());  // Dummy in x0
} else {
    // 非静态方法：x0 = dummy，x1 = receiver
    param_types.push_back(YuhuType::intptr_type());  // Dummy in x0
    param_types.push_back(YuhuType::oop_type());     // receiver in x1
}

// 添加参数类型（ciSignature* sig = call_method->signature()）
for (int i = 0; i < sig->count(); i++) {
    param_types.push_back(YuhuType::to_stackType(sig->type_at(i)));
}
```

#### 2.2 修改调用参数收集

```cpp
std::vector<Value*> call_args;
int arg_slots = call_method->arg_size();

// 添加 x0 的 dummy 值
call_args.push_back(LLVMValue::intptr_constant(0));

if (is_static) {
    // 静态方法：x1 = 第一个参数，x2+ = 剩余参数
    // 从操作数栈收集所有 Java 参数（逆序）
    for (int i = arg_slots - 1; i >= 0; i--) {
        YuhuValue* v = xstack(i);
        call_args.push_back(v->j*_value());  // 根据类型转换
    }
} else {
    // 非静态方法：x1 = receiver
    YuhuValue* recv_val = xstack(arg_slots - 1);
    check_null(recv_val);
    call_args.push_back(recv_val->jobject_value());  // receiver in x1

    // 收集剩余 Java 参数（不包括 receiver）
    for (int i = arg_slots - 2; i >= 0; i--) {
        YuhuValue* v = xstack(i);
        call_args.push_back(v->j*_value());  // 根据类型转换
    }
}
```

**注意**: 操作数栈的参数顺序是从右到左压入的，所以逆序收集（从高索引到低索引）。

### 3. 修改参数读取 (`yuhuCacheDecache.cpp`)

**函数**: `YuhuNormalEntryCacher::get_function_arg()`

**问题**: 原来的逻辑是非静态方法直接从 arg0 开始读取，静态方法跳过 arg0。但修改后，**两种方法都需要跳过 dummy (arg0)**。

**修改前:**
```cpp
llvm::Argument* YuhuNormalEntryCacher::get_function_arg(int local_index) {
  llvm::Function* func = function()->function();
  llvm::Function::arg_iterator ai = func->arg_begin();

  if (is_static()) {
    // 静态方法：跳过 function_arg[0] 的 NULL
    ai++;  // Skip NULL argument
  }

  // 前进到请求的 local index
  for (int i = 0; i < local_index; i++) {
    ai++;
    // ...
  }

  return &*ai;
}
```

**修改后:**
```cpp
llvm::Argument* YuhuNormalEntryCacher::get_function_arg(int local_index) {
  llvm::Function* func = function()->function();
  llvm::Function::arg_iterator ai = func->arg_begin();

  // 两种方法都跳过 dummy at function_arg[0]
  // 非静态：(dummy, this, arg0, ...) → local[0] = this (arg 1)
  // 静态：(dummy, arg0, arg1, ...) → local[0] = arg0 (arg 1)
  ai++;  // Skip dummy argument at arg 0

  // 前进到请求的 local index
  for (int i = 0; i < local_index; i++) {
    ai++;
    if (ai == func->arg_end()) {
      ShouldNotReachHere();
      return NULL;
    }
  }

  return &*ai;
}
```

**映射关系（修改后）:**

非静态方法 `(dummy, this, arg0, arg1, ...)`:
- `get_function_arg(0)` → 跳过 dummy → 返回 arg1 (this, x1) ✓
- `get_function_arg(1)` → 跳过 dummy，前进1 → 返回 arg2 (arg0, x2) ✓

静态方法 `(dummy, arg0, arg1, ...)`:
- `get_function_arg(0)` → 跳过 dummy → 返回 arg1 (arg0, x1) ✓
- `get_function_arg(1)` → 跳过 dummy，前进1 → 返回 arg2 (arg1, x2) ✓

## 验证方法

### 测试用例

```java
// test/ComplexNumber.java
public class ComplexNumber {
    private final double real;
    private final double imag;

    public ComplexNumber(double real, double imag) {
        this.real = real;
        this.imag = imag;
    }

    public ComplexNumber multiply(ComplexNumber other) {
        double newReal = this.real * other.real - this.imag * other.imag;
        double newImag = this.real * other.imag + this.imag * other.real;
        return new ComplexNumber(newReal, newImag);
    }

    public static void main(String[] args) {
        ComplexNumber c1 = new ComplexNumber(1.0, 2.0);
        ComplexNumber c2 = new ComplexNumber(3.0, 4.0);
        ComplexNumber result = c1.multiply(c2);
        System.out.println("Result: " + result.real + " + " + result.imag + "i");
        // 预期输出: Result: -5.0 + 10.0i
    }
}
```

### 运行命令

```bash
# 编译 JDK
cd /Users/liuanyou/CLionProjects/jdk8
make hotspot

# 运行测试
java -XX:+UseYuhuCompiler \
     -XX:CompileCommand=yuhuonly,ComplexNumber.multiply \
     -XX:+PrintCompilation \
     -cp test ComplexNumber
```

### lldb 调试验证

```bash
lldb -- java -XX:+UseYuhuCompiler \
             -XX:CompileCommand=yuhuonly,ComplexNumber.multiply \
             -cp test ComplexNumber

(lldb) b YuhuCompiledEntry
(lldb) c

# 检查寄存器
(lldb) register read x0  # 应该是 0（dummy）
(lldb) register read x1  # 应该是 this 对象
(lldb) register read x2  # 应该是 arg0 (other.real)
(lldb) register read x3  # 应该是 arg1 (other.imag)
```

## 相关文件

### 修改的文件

1. **hotspot/src/share/vm/yuhu/yuhuFunction.cpp**
   - 函数: `generate_normal_entry_point_type()`
   - 行数: ~56-72

2. **hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp**
   - 函数: `do_call()`
   - 修改位置:
     - 函数签名构建 (~1586-1592)
     - 调用参数收集 (~1500-1565)

3. **hotspot/src/share/vm/yuhu/yuhuCacheDecache.cpp**
   - 函数: `YuhuNormalEntryCacher::get_function_arg()`
   - 行数: ~162-184

### 参考文件

- **hotspot/src/cpu/aarch64/vm/assembler_aarch64.hpp** (lines 77-82)
  - HotSpot Java 调用约定定义

## 关键知识点

1. **sig->count() vs args_size()**
   - `ciSignature::count()`: 签名中的参数个数（**不包括 this**）
   - `args_size()`: 总参数个数（非静态方法**包括 this**）

2. **LLVM 函数参数映射**
   - LLVM 函数签名定义了参数到寄存器的映射
   - 第一个参数 → x0，第二个参数 → x1，依此类推

3. **操作数栈顺序**
   - Java 字节码从右到左压入参数到操作数栈
   - 调用时从栈顶（最后一个参数）开始弹出

4. **局部变量映射**
   - 局部变量区域存储参数（local[0], local[1], ...）
   - 修改后需要跳过 dummy，从 LLVM arg 1 (x1) 开始读取

## 经验教训

1. **HotSpot 调用约定与标准 ABI 不同**
   - 不能假设使用平台标准调用约定
   - 必须查看并遵循 HotSpot 的约定定义

2. **三个地方必须保持一致**
   - 函数签名定义（接收参数）
   - 调用时参数传递
   - 函数入口参数读取
   - 任何一处不一致都会导致参数错位

3. **sig->count() 不包括 this**
   - 容易误以为包括 this
   - 需要 args_size() 来获取包括 this 的参数个数

4. **静态方法不需要 NULL 占位符**
   - 早期错误认为静态方法需要在 x1 放 NULL
   - 实际上 x0 是 dummy，x1 直接放第一个参数

## 时间线

- **2025-02-05**: 发现问题，通过 lldb 确认参数传递正确但 Yuhu 处理错误
- **2025-02-05**: 定位根本原因（函数签名不匹配调用约定）
- **2025-02-05**: 修复三处代码，确保一致性
- **2025-02-05**: 创建本文档记录修复过程

## 状态

✅ 已修复，待编译测试

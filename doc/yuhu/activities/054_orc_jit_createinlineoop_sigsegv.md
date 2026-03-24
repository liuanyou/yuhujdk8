# Activity 054: ORC JIT 模式下 CreateInlineOop 地址计算错误导致 SIGSEGV

## 问题描述

Yuhu 编译器在 ORC JIT 模式下运行时发生 SIGSEGV，崩溃地址位于访问静态字段的代码处。根本原因是 `CreateInlineOop()` 方法在 IR 生成阶段错误地计算 oop 地址，使用了纯偏移量而非运行时地址。

### 崩溃现场

**错误信息**：
```
SIGSEGV (0xb) at pc=0x000000010b0b3b70
pc info:
  0x000000010b0b3b70:   ldr    x8, [x8]
Register to hex:
  x8: 0x20
```

**崩溃指令**：
```assembly
ldr x8, [x8]  ; 尝试从地址 0x20 读取 → SIGSEGV
```

### LLVM IR 问题

生成的 LLVM IR 包含错误的地址转换：
```llvm
bci_0:
  %16 = load ptr, ptr inttoptr (i64 32 to ptr), align 8
  ;                        ^^^^ 错误：把整数 32 当作地址！
  %17 = ptrtoint ptr %16 to i64
  %field_addr_int12 = add i64 %17, 100
  %addr = inttoptr i64 %field_addr_int12 to ptr
  %18 = load i32, ptr %addr, align 4
```

**问题指令解析**：
- `inttoptr (i64 32 to ptr)`：将整数 32 直接转换为指针
- 运行时访问地址 `0x20`（十进制 32），这不是有效的内存地址

---

## 根本原因分析

### 触发字节码

```java
// com.example.NineParameterTest.testNineParameters()
0: getstatic     #2    // Field java/lang/System.out:Ljava/io/PrintStream;
```

`getstatic` 字节码触发 Yuhu 生成访问静态字段的代码。

### 问题代码链

#### 1. YuhuBlock::do_getstatic()
```cpp
// hotspot/src/share/vm/yuhu/yuhuBlock.cpp:1063
object = builder()->CreateInlineOop(field->holder()->java_mirror());
// field->holder() = java/lang/System
// java_mirror() = System.class 这个对象
```

#### 2. YuhuBuilder::CreateInlineOop()
```cpp
// hotspot/src/share/vm/yuhu/yuhuBuilder.cpp:847-855
Value* YuhuBuilder::CreateInlineOop(jobject object, const char* name) {
  return CreateLoad(
    YuhuType::oop_type(),
    CreateIntToPtr(
      code_buffer_address(code_buffer()->inline_oop(object)),  // ❌ 问题在这
      PointerType::getUnqual(YuhuType::oop_type())),
    name);
}
```

#### 3. 问题分解

**第一步**：`code_buffer()->inline_oop(object)`
- 在 CodeBuffer 中为 oop 分配一个位置
- 返回**偏移量**（例如 32，表示从 CodeBuffer 起点往后 32 字节）

**第二步**：`code_buffer_address(32)`
- 期望将偏移量转换为**运行时绝对地址**
- **实际行为**：在 ORC JIT 模式下，CodeBuffer 的基地址是未知的或为 0
- 结果：`code_buffer_address(32)` 直接返回 `32`（纯偏移量，不是地址！）

**第三步**：`CreateIntToPtr(32, ...)`
- 生成 IR：`inttoptr (i64 32 to ptr)`
- 语义：把整数 32 当作内存地址

**第四步**：运行时崩溃
- CPU 执行 `ldr x8, [x8]`，其中 `x8 = 0x20`
- 访问地址 `0x20` → SIGSEGV

---

## C1 vs Yuhu 架构对比

### C1 的做法（正确）

#### 1. 编译时生成占位符
```cpp
// c1_GraphBuilder.cpp:1552-1558
if (code == Bytecodes::_getstatic || code == Bytecodes::_putstatic) {
  obj = new Constant(new InstanceConstant(holder->java_mirror()), state_before);
}
```
- 创建 `InstanceConstant`，只是一个**引用**，不是真正的地址
- 不计算绝对地址

#### 2. 运行时动态解析
```cpp
// c1_Runtime1.cpp:851-855
case Bytecodes::_getstatic:
  { Klass* klass = resolve_field_return_klass(caller_method, bci, CHECK);
    init_klass = KlassHandle(THREAD, klass);
    mirror = Handle(THREAD, klass->java_mirror());
  }
```
- 第一次执行时调用 runtime stub 解析 `java_mirror()`
- 获取真实的 oop 地址

#### 3. Patching 机制回填
```cpp
// c1_Runtime1.cpp:1021
n_copy->set_data(cast_from_oop<intx>(mirror()));
```
- Runtime stub 解析出正确的 oop 地址后，**直接修改机器码**
- 初始代码里放的是零或者无效值
- 运行时动态替换为真实地址

---

### Yuhu/Shark 的做法（错误）

```cpp
// yuhuBuilder.cpp:847-855
Value* YuhuBuilder::CreateInlineOop(jobject object, const char* name) {
  return CreateLoad(
    YuhuType::oop_type(),
    CreateIntToPtr(
      code_buffer_address(code_buffer()->inline_oop(object)),  // ❌ 依赖编译期地址
      ...),
    name);
}
```

**核心问题**：
- 假设 `code_buffer_address()` 能返回正确的运行时地址
- 在传统 MCJIT 模式下，这个假设成立（CodeBuffer 基地址已知）
- 在 ORC JIT 模式下，这个假设**不成立**（ORC 自己管理内存分配）

---

### 关键区别

| 方面 | C1 | Yuhu (Shark) |
|------|-----|--------------|
| **oop 解析时机** | 运行时（第一次执行时） | 编译时（IR 生成时）❌ |
| **oop 存储位置** | 全局 oop 表 / constantPool | CodeBuffer 内部 |
| **地址计算** | runtime stub 动态回填 ✅ | 编译期静态计算 ❌ |
| **对 ORC JIT 兼容性** | ✅ 兼容（不依赖编译期地址） | ❌ 不兼容（依赖 CodeBuffer 基地址） |
| **支持类卸载** | ✅ 支持 | ❌ 不支持 |
| **支持 GC 移动** | ✅ 支持（通过句柄表） | ❌ 不支持（硬编码地址） |

---

## 解决方案（最终方案）

### 问题根因

`yuhu_resolve_static_field` 返回的是**已经解压缩的完整 oop 指针**（转换为 `jlong`），但在 IR 生成时使用了错误的目标类型进行转换。

**错误的代码流程**：
```cpp
// yuhuBlock.cpp - 错误的做法
llvm::Type* field_type = YuhuType::to_arrayType(basic_type);
// 当 UseCompressedOops=true 时，field_type = jint_type() (i32)

if (basic_type == T_OBJECT) {
  field_value = builder()->CreateIntToPtr(field_result, field_type);
  // ❌ 将 jlong 转成 i32 pointer，丢失了完整的 oop 指针
}
```

**为什么会出错**：
1. `YuhuContext::to_arrayType(T_OBJECT)` 在 compressed oops 模式下返回 `jint_type()` (i32)
2. 这是为了从堆对象字段中加载 **32 位压缩指针**而设计的
3. 但 `yuhu_resolve_static_field` 返回的是**已经解压缩的完整指针**
4. 用 `inttoptr` 转成 i32 pointer 后，YuhuValue 的类型标志错误
5. 后续 `check_null()` 调用 `is_jobject()` 断言失败

### 正确的修复

对于 GETSTATIC 的对象引用字段，应该直接使用 `oop_type()` 作为转换目标类型：

```cpp
// hotspot/src/share/vm/yuhu/yuhuBlock.cpp:1066-1098
void YuhuBlock::do_field_access(bool is_get, bool is_field) {
  // ...
  
  if (!is_field) {
    // === GETSTATIC: Call runtime helper to get static field value ===
    int cp_index = iter()->get_field_index();
    Value* field_result = builder()->CreateInlineOopForStaticField(cp_index);
    
    // yuhu_resolve_static_field returns jlong (64-bit container)
    // We need to convert it to the actual field type
    BasicType basic_type = field->type()->basic_type();
    llvm::Type* field_type = YuhuType::to_arrayType(basic_type);
    
    Value* field_value;
    if (basic_type == T_OBJECT || basic_type == T_ARRAY) {
      // For static field object references, yuhu_resolve_static_field returns
      // the decoded oop pointer directly (not compressed), so use oop_type()
      field_value = builder()->CreateIntToPtr(field_result, YuhuType::oop_type());
      // ✅ 正确：使用完整的 oop pointer 类型
    } else if (basic_type == T_LONG || basic_type == T_DOUBLE) {
      // Already jlong/jdouble (64-bit), may need bitcast for double
      if (basic_type == T_DOUBLE) {
        field_value = builder()->CreateBitCast(field_result, field_type);
      } else {
        field_value = field_result;
      }
    } else {
      // Truncate jlong to smaller types (int, short, byte, char, boolean, float)
      if (basic_type == T_FLOAT) {
        field_value = builder()->CreateBitCast(
          builder()->CreateTrunc(field_result, YuhuType::jint_type()), 
          field_type);
      } else {
        field_value = builder()->CreateTrunc(field_result, field_type);
      }
    }
    
    value = YuhuValue::create_generic(field->type(), field_value, false);
  }
  
  // ...
}

---

## 技术说明

### 为什么 `to_arrayType(T_OBJECT)` 返回 `jint_type()`

查看 `YuhuContext::yuhuContext()` 初始化代码：

```cpp
// hotspot/src/share/vm/yuhu/yuhuContext.cpp:151-163
case T_OBJECT:
case T_ARRAY:
  // For object/array references, we need to consider compressed oops
  if (UseCompressedOops) {
    // When compressed oops are enabled, object fields store 32-bit compressed pointers
    _to_stackType[i] = oop_type();      // Stack operations still use full pointer type
    _to_arrayType[i] = jint_type();    // Field access loads 32-bit compressed pointers
  } else {
    // When compressed oops are disabled, object fields store full pointers
    _to_stackType[i] = oop_type();
    _to_arrayType[i] = oop_type();
  }
  break;
```

**设计原因**：
- JVM 使用 compressed oops 时，堆中对象字段存储的是 **32 位压缩指针**
- `to_arrayType()` 用于从对象字段 **load** 值，所以返回 `jint_type()` (i32)
- 然后需要用 `CreateDecodeHeapOop` 把 32 位压缩指针 decode 成 64 位完整指针

**GETSTATIC 的特殊情况**：
- `yuhu_resolve_static_field` 返回的是**已经解压缩的完整 oop 指针**（转为 `jlong`）
- 不需要再从堆里 load 压缩指针再 decode
- 所以应该直接用 `oop_type()`，而不是 `jint_type()`

### 类型对比

| 场景 | 返回值类型 | 需要的转换 |
|------|-----------|-----------|
| **GETFIELD (对象字段)** | `jint` (compressed oop) | `inttoptr(i32)` → decode → `oop_type()` |
| **GETSTATIC (静态字段)** | `jlong` (full oop as i64) | `inttoptr(i64)` → `oop_type()` ✅ |

---

## 实现状态

✅ **已完成**：
1. 修改 `yuhu_resolve_static_field` 返回 `jlong`（对象指针或基本类型值）
2. 修改 `CreateInlineOopForStaticField` 返回 `jlong` 类型
3. 修改 `do_field_access` 中的 GETSTATIC 处理，使用 `oop_type()` 进行对象引用转换

---

---

## 参考文档

- LLVM ORC JIT 文档：https://llvm.org/docs/ORC.html
- C1 字段访问实现：`hotspot/src/share/vm/c1/c1_Runtime1.cpp`
- JVM 规范 §4.7.3：max_stack 计算规则
- Activity 053：前序调试文档（offset marker 扫描问题）
- Activity 055：后续 runtime call 问题

---

## 状态

✅ **已完成** (2026-03-24):
1. 修改 `yuhu_resolve_static_field` 返回 `jlong`
2. 修改 `CreateInlineOopForStaticField` 返回 `jlong` 类型
3. 修改 `do_field_access` 使用 `oop_type()` 进行对象引用转换

⚠️ **待处理**:
- Runtime call 问题（见 Activity 055）

---

**创建时间**: 2026-03-19  
**作者**: Liu Anyou  
**关联 Issue**: ORC JIT SIGSEGV @ 0x000000010b0b3b70

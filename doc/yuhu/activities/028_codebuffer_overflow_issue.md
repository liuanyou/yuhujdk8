# CodeBuffer 内存越界问题分析

## 问题现象

在编译 `encodeArrayLoop` 方法时，在开始生成 IR 代码（block 0, bci=0）后立即出现断言失败：

```
=== Yuhu: Emitting IR for block 0 (bci=0) ===
loop 602 c[0][0] 328350

Internal Error (/Users/liuanyou/CLionProjects/jdk8/hotspot/src/share/vm/asm/codeBuffer.hpp:176)
assert(allocates2(pc)) failed: not in CodeBuffer memory: 0x000000010a8172c0 <= 0x000000010a8574c1 <= 0x000000010a8574c0
```

## 错误信息解读

### 断言条件

```cpp
// codeBuffer.hpp:176
void set_end(address pc) { 
  assert(allocates2(pc), 
         err_msg("not in CodeBuffer memory: " PTR_FORMAT " <= " PTR_FORMAT " <= " PTR_FORMAT, 
                 _start, pc, _limit)); 
  _end = pc; 
}
```

### allocates2() 的含义

```cpp
// codeBuffer.hpp:174
bool allocates2(address pc) const { 
  return pc >= _start && pc <= _limit; 
}
```

**检查地址 `pc` 是否在分配的内存范围内（包括边界）：**
- `_start` ≤ `pc` ≤ `_limit`

### 错误的具体数值

从错误消息中提取：
```
_start  = 0x000000010a8172c0
pc      = 0x000000010a8574c1
_limit  = 0x000000010a8574c0
```

**检查：**
```
_start  = 0x10a8172c0
_limit  = 0x10a8574c0
pc      = 0x10a8574c1

CodeBuffer size = _limit - _start
                = 0x10a8574c0 - 0x10a8172c0
                = 0x40200
                = 262,656 bytes (~256 KB)

Overflow amount = pc - _limit
                = 0x10a8574c1 - 0x10a8574c0
                = 0x1
                = 1 byte
```

## 问题分析

### 1. 内存布局

CodeBuffer 有三个 section：
- **SECT_CONSTS** - 常量数据（浮点数、跳转表等）
- **SECT_INSTS** - 可执行指令（主要部分）
- **SECT_STUBS** - 出站跳板（调用点支持、去优化、异常处理）

当前使用的是 **SECT_INSTS** section。

### 2. 为什么只越界 1 字节？

**关键发现：**
- CodeBuffer 已分配 256 KB 空间
- 但尝试写入的地址 `pc` 超出边界仅 **1 字节**
- 这说明代码生成几乎填满了整个 CodeBuffer

**可能的原因：**

#### 原因 1：CodeBuffer 初始大小不足

在 Yuhu 编译器中，CodeBuffer 可能是为较小的方法设计的：
```cpp
// 某处的 CodeBuffer 初始化
CodeBuffer cb(...);
cb.initialize(code_start, initial_size);  // initial_size 可能太小
```

对于 `encodeArrayLoop` 这样的复杂方法：
- **max_locals = 12** - 局部变量表很大
- **可能包含循环和大量字节码指令**
- 生成的 LLVM IR 和机器码会非常多

#### 原因 2：在 set_end() 之前已经写入了数据

```cpp
// 典型的代码生成模式
address pos = cb.end();
// 写入一些数据到 pos
*((int32_t*)pos) = value;
// 然后尝试更新 end
cb.set_end(pos + 4);  // ← 如果 pos + 4 超出 _limit，这里失败
```

**问题可能发生在：**
1. 在调用 `set_end()` 之前，代码已经向 `end()` 位置写入了数据
2. 写入的数据大小计算错误，导致 `new_end = old_end + size` 超出边界

#### 原因 3：累积的小错误

如果代码生成过程中，每次都稍微多写一点，最终累积到边界：
```cpp
// 假设每个指令应该占用 4 字节，但实际写入了 5 字节
for (int i = 0; i < many_instructions; i++) {
  address pos = cb.end();
  write_instruction(pos);  // 实际写入 5 字节而不是 4 字节
  cb.set_end(pos + 4);     // 但只增加 4 字节
}
// 最终累积的额外字节会导致越界
```

### 3. 为什么在 "Emitting IR for block 0" 时失败？

**时间点分析：**
1. ✅ 创建 LLVM 函数（成功）
2. ✅ 创建基本块（成功）
3. ✅ 初始化入口状态（成功）
4. ❌ **开始生成 IR 代码时失败**

**这说明：**
- CodeBuffer 在方法的初始化阶段就已经接近满了
- 或者在生成第一个 block 的 IR 时，某个操作尝试写入大量数据

**可能的操作：**
- **栈帧初始化**：为 12 个局部变量分配空间
- **参数传递代码**：处理方法参数（this + 2 个对象参数）
- **Safepoint 设置**：插入 safepoint 相关的代码
- **OopMap 生成**：记录对象引用位置
- **内联汇编**：使用 inline assembly 读取寄存器或设置栈帧

### 4. "loop 602 c[0][0] 328350" 的含义

**这是来自测试代码 `Matrix.java` 的输出：**
```java
// test_yuhu/com/example/Matrix.java:36
System.out.println("loop " + i + " c[0][0] " + c[0][0]);
```

**解读：**
- `loop 602` - 矩阵乘法的第 602 次循环迭代
- `c[0][0]` - 结果矩阵第一个元素的值
- `328350` - 当前的累积值

**这说明：**
1. `Matrix.multiply` 方法已经成功执行了 602 次循环
2. 在循环过程中，JVM 检测到 `encodeArrayLoop` 是个热点方法
3. 触发了对 `encodeArrayLoop` 的编译
4. 编译 `encodeArrayLoop` 时出现 CodeBuffer 越界错误

**编译触发路径：**
```
Matrix.multiply (循环 602 次)
  ↓ 调用
System.out.println(...)
  ↓ 调用
PrintStream.println(...)
  ↓ 调用
String 相关操作
  ↓ 调用
CharBuffer.encodeArrayLoop  ← JVM 检测到这是热点，触发编译
  ↓ 尝试编译
Yuhu Compiler
  ↓ CodeBuffer 越界
编译失败！
```

**为什么是第 602 次循环？**
- JVM 的分层编译策略有编译阈值
- 当方法调用次数或回边计数达到阈值时，触发编译
- 602 次可能正好达到了某个编译阈值

## 触发场景

### 为什么这个方法会触发问题？

**`encodeArrayLoop` 的特点：**
```
Method: encodeArrayLoop
Signature: (Ljava/nio/CharBuffer;Ljava/nio/ByteBuffer;)Ljava/nio/charset/CoderResult;
Max locals: 12, arg_size: 3
```

1. **max_locals = 12**：局部变量很多
   - 需要生成大量的栈帧初始化代码
   - 每个局部变量可能需要 OopMap 条目

2. **方法名包含 "Loop"**：可能包含循环
   - 循环体可能被多次展开
   - 生成的机器码会成倍增长

3. **CharBuffer 和 ByteBuffer 操作**：可能涉及大量数组访问
   - 每次数组访问都需要边界检查
   - 需要生成异常处理代码

4. **可能是优化的热点方法**：
   - JVM 可能对这个方法进行了激进的内联和优化
   - 导致生成的代码量激增

## CodeBuffer, CodeBlob, BufferBlob, CodeCache, CodeHeap 的关系

### 概述

这些类构成了 HotSpot JVM 的**代码生成和管理体系**，从临时代码生成到最终代码存储的完整流程：

```
代码生成阶段                    代码存储阶段
┌─────────────┐              ┌─────────────┐
│ CodeBuffer  │  ─复制到─>   │  CodeBlob   │  ─存储到─>  CodeCache
│ (临时缓冲区)  │              │ (永久对象)   │                 ↓
└─────────────┘              └─────────────┘           ┌──────────┐
                                   ↑                   │ CodeHeap │
                                   │                   │ (内存堆)  │
                             ┌─────┴──────┐            └──────────┘
                             │ BufferBlob │
                             │ RuntimeStub│
                             │  nmethod   │
                             │    etc.    │
                             └────────────┘
```

### 1. CodeBuffer（临时代码缓冲区）

**定义：** 位于 `hotspot/src/share/vm/asm/codeBuffer.hpp`

**用途：**
- **临时的**代码生成缓冲区，仅在编译期间存在
- 存在于**栈**上（`StackObj`），生命周期短暂
- 用于**汇编器**（Assembler）逐步写入机器指令

**特点：**
```cpp
class CodeBuffer: public StackObj {
  // 三个独立的 section：
  CodeSection _consts;  // 常量数据（浮点数、跳转表等）
  CodeSection _insts;   // 可执行指令（主要部分）
  CodeSection _stubs;   // 出站跳板（调用点支持、去优化、异常处理）
  
  address _total_start; // 缓冲区起始地址
  csize_t _total_size;  // 总大小
};
```

**生命周期：**
```cpp
void compile_method() {
  CodeBuffer cb("my_method", 256*1024);  // 分配 256 KB
  Assembler masm(&cb);
  
  // 生成代码
  masm.mov(...);
  masm.add(...);
  // ...
  
  // 复制到永久存储（CodeBlob）
  CodeBlob* blob = CodeCache::allocate(cb.total_content_size());
  cb.copy_code_to(blob);
  
}  // CodeBuffer 在这里销毁（栈对象自动销毁）
```

**重要方法：**
- `insts()->end()` - 当前指令写入位置
- `insts()->remaining()` - 剩余可用空间
- `set_end(address pc)` - 更新指令结束位置（**028 问题就是这里断言失败**）

---

### 2. CodeBlob（代码块基类）

**定义：** 位于 `hotspot/src/share/vm/code/codeBlob.hpp`

**用途：**
- **永久的**代码对象，存储在 CodeCache 中
- 所有已编译代码的**抽象基类**
- 包含代码、重定位信息、OopMap 等元数据

**内存布局：**
```
CodeBlob 内存布局：
┌──────────────────┐ ← header_begin()
│   Header         │   (CodeBlob 对象本身)
├──────────────────┤ ← relocation_begin()
│   Relocation     │   (重定位信息)
├──────────────────┤ ← content_begin()
│   Constants      │   (常量数据)
├──────────────────┤ ← code_begin()
│   Instructions   │   (可执行代码)
│   Stubs          │
├──────────────────┤ ← data_begin()
│   Data           │   (其他数据)
└──────────────────┘ ← data_end()
```

**子类层次：**
```cpp
CodeBlob (抽象基类)
├── BufferBlob        // 非重定位代码（解释器、存根等）
│   ├── AdapterBlob   // C2I/I2C 适配器
│   └── MethodHandlesAdapterBlob
├── RuntimeStub       // 运行时存根（调用 C++ 运行时）
├── SingletonBlob     // 单例代码块
│   ├── DeoptimizationBlob    // 去优化代码
│   ├── ExceptionBlob         // 异常处理
│   └── SafepointBlob         // Safepoint 处理
└── nmethod           // 已编译的 Java 方法
```

**关键属性：**
```cpp
class CodeBlob {
  const char* _name;           // 名称
  int _size;                   // 总大小
  int _header_size;            // 头部大小
  int _relocation_size;        // 重定位信息大小
  int _code_offset;            // 代码区偏移
  int _data_offset;            // 数据区偏移
  int _frame_size;             // 栈帧大小
  OopMapSet* _oop_maps;        // OopMap 集合
};
```

---

### 3. BufferBlob（缓冲代码块）

**定义：** 继承自 `CodeBlob`

**用途：**
- 存储**非重定位的机器码**
- 用于解释器、存根例程等静态代码
- 不需要垃圾回收追踪的代码

**特点：**
```cpp
class BufferBlob: public CodeBlob {
  // 不包含 Java 对象引用
  // 不需要重定位信息
  // 生命周期通常与 JVM 相同
};
```

**典型用途：**
- **解释器代码**：字节码解释器的机器码
- **存根代码**：各种 JVM 运行时存根
- **适配器代码**：C2I（编译代码到解释器）、I2C（解释器到编译代码）适配器

---

### 4. CodeCache（代码缓存管理器）

**定义：** 位于 `hotspot/src/share/vm/code/codeCache.hpp`

**用途：**
- **全局单例**（`AllStatic`），管理所有 CodeBlob
- 提供 CodeBlob 的分配、查找、释放功能
- 管理底层的 CodeHeap 内存

**核心职责：**
```cpp
class CodeCache : AllStatic {
private:
  static CodeHeap* _heap;  // 底层内存堆
  static int _number_of_blobs;
  static int _number_of_nmethods;
  
public:
  // 分配 CodeBlob
  static CodeBlob* allocate(int size, bool is_critical = false);
  
  // 提交 CodeBlob（使其可被查找）
  static void commit(CodeBlob* cb);
  
  // 查找包含给定地址的 CodeBlob
  static CodeBlob* find_blob(void* start);
  static nmethod* find_nmethod(void* start);
  
  // 释放 CodeBlob
  static void free(CodeBlob* cb);
  
  // 遍历所有 CodeBlob
  static void blobs_do(CodeBlobClosure* f);
  static void nmethods_do(void f(nmethod* nm));
};
```

**典型使用场景：**
```cpp
// 1. 分配 CodeBlob
CodeBlob* blob = CodeCache::allocate(total_size);
if (blob == NULL) {
  // CodeCache 已满
  return NULL;
}

// 2. 复制代码到 CodeBlob
code_buffer.copy_code_to(blob);

// 3. 提交到 CodeCache（使其可被查找）
CodeCache::commit(blob);

// 4. 运行时查找
address pc = ...;  // 某个程序计数器值
CodeBlob* blob = CodeCache::find_blob(pc);
if (blob != NULL && blob->is_nmethod()) {
  nmethod* nm = (nmethod*)blob;
  // 找到了包含这个 PC 的 nmethod
}
```

---

### 5. CodeHeap（代码堆）

**定义：** 位于 `hotspot/src/share/vm/memory/heap.hpp`

**用途：**
- CodeCache 的**底层内存管理器**
- 管理大块的**虚拟内存**（VirtualSpace）
- 提供内存的分配、释放、碎片整理

**内存管理：**
```cpp
class CodeHeap : public CHeapObj<mtCode> {
private:
  VirtualSpace _memory;  // 实际存储代码的虚拟内存
  VirtualSpace _segmap;  // 段映射表（用于快速查找）
  
  size_t _segment_size;  // 段大小（最小分配单位）
  FreeBlock* _freelist;  // 空闲块链表
  
public:
  // 预留和提交内存
  bool reserve(size_t reserved_size, size_t committed_size, size_t segment_size);
  
  // 分配内存
  void* allocate(size_t size, bool is_critical);
  
  // 释放内存
  void deallocate(void* p);
  
  // 查找包含给定地址的块
  void* find_start(void* p) const;
};
```

**内存分配策略：**
```
CodeHeap 内存结构：
┌─────────────────────────────────┐ ← low_boundary()
│  Reserved but not committed     │
├─────────────────────────────────┤
│  Committed memory                │
│  ┌─────────┐ ┌─────────┐        │
│  │CodeBlob1│ │CodeBlob2│  Free  │
│  └─────────┘ └─────────┘        │
├─────────────────────────────────┤ ← high()
│  Reserved but not committed     │
└─────────────────────────────────┘ ← high_boundary()
```

**分段管理：**
- CodeHeap 将内存分成固定大小的**段**（segment）
- 每个 CodeBlob 占用一个或多个段
- 使用段映射表（segmap）快速定位块的起始位置

---

### 完整的代码生成流程

**示例：编译一个 Java 方法**

```cpp
// 1. 创建临时 CodeBuffer（栈上）
CodeBuffer code_buffer("MyMethod", 256*1024);  // 256 KB
MacroAssembler masm(&code_buffer);

// 2. 生成机器码
masm.push(rbp);
masm.mov(rbp, rsp);
// ... 更多指令
masm.pop(rbp);
masm.ret();

// 3. 从 CodeCache 分配永久存储
int total_size = code_buffer.total_content_size();
CodeBlob* blob = CodeCache::allocate(total_size);
if (blob == NULL) {
  // CodeCache 满了
  handle_code_cache_full();
  return;
}

// 4. 复制代码到 CodeBlob
code_buffer.copy_code_to(blob);

// 5. 设置元数据
blob->set_oop_maps(oop_maps);
blob->set_frame_size(frame_size);

// 6. 提交到 CodeCache
CodeCache::commit(blob);

// 7. CodeBuffer 自动销毁（离开作用域）
// 8. CodeBlob 永久存在，直到被显式释放
```

---

### 关键区别总结

| 类 | 用途 | 生命周期 | 内存分配 | 可修改性 |
|---|---|---|---|---|
| **CodeBuffer** | 临时代码生成缓冲区 | 编译期间 | 栈或临时堆 | 可修改 |
| **CodeBlob** | 永久代码对象（基类） | 长期（直到释放） | CodeCache | 不可修改 |
| **BufferBlob** | 非重定位代码块 | 长期 | CodeCache | 不可修改 |
| **CodeCache** | 代码缓存管理器 | JVM 生命周期 | 全局单例 | N/A |
| **CodeHeap** | 底层内存堆 | JVM 生命周期 | 虚拟内存 | N/A |

**类比理解：**
- **CodeBuffer** = 建筑工地的临时脚手架（用完就拆）
- **CodeBlob** = 建成的永久建筑物
- **BufferBlob** = 不需要装修的简易建筑（如仓库）
- **CodeCache** = 城市规划局（管理所有建筑）
- **CodeHeap** = 土地资源（提供建筑用地）

---

## CodeBuffer 动态扩展机制

### 是否可以动态增加 CodeBuffer 大小？

**答案：可以！** CodeBuffer 有完整的动态扩展机制。

### CodeBuffer::expand() 方法

```cpp
// codeBuffer.cpp:829-919
void CodeBuffer::expand(CodeSection* which_cs, csize_t amount) {
  // 1. 检查是否允许扩展（所有 section 都不能是 frozen 状态）
  for (int n = 0; n < (int)SECT_LIMIT; n++) {
    guarantee(!code_section(n)->is_frozen(), "resizing not allowed when frozen");
  }
  
  // 2. 计算新的容量
  //    - 当前 section：100% 增长，最小 4KB
  //    - INSTS section：25% 增长（更保守）
  csize_t exp = sect->size();  // 100% increase
  if ((uint)exp < 4*K)  exp = 4*K;  // minimum 4KB increase
  
  // 3. 创建新的 CodeBuffer（临时）
  CodeBuffer cb(name(), new_total_cap, 0);
  
  // 4. 将所有代码和重定位信息复制到新 buffer
  relocate_code_to(&cb);
  
  // 5. 用新 buffer 替换当前 buffer
  this->take_over_code_from(&cb);
}
```

**关键特性：**
- ✅ **自动触发**：当某个 CodeSection 空间不足时自动调用
- ✅ **增量扩展**：100% 增长（翻倍），最小增加 4KB
- ✅ **保守策略**：INSTS section 只增长 25%（因为是最大的 section）
- ✅ **保留历史**：通过 `_before_expand` 链保留扩展历史
- ⚠️ **可能失败**：如果 CodeCache 空间不足，扩展会失败

### C1 和 C2 的初始大小策略

#### C1 编译器

```cpp
// c1_Compiler.cpp:85-88
int code_buffer_size = Compilation::desired_max_code_buffer_size() +
                       Compilation::desired_max_constant_size();

BufferBlob* buffer_blob = BufferBlob::create("C1 temporary CodeBuffer", code_buffer_size);
```

**C1 初始大小计算：**
```cpp
// c1_Compilation.hpp:191-201
static int desired_max_code_buffer_size() {
  return (int) NMethodSizeLimit;  // 64*K*wordSize
}

static int desired_max_constant_size() {
  return desired_max_code_buffer_size() / 10;  // 10%
}
```

**C1 初始大小：**
- **NMethodSizeLimit** = 64K * wordSize = **256KB** (64位系统) 或 **128KB** (32位系统)
- **Constant size** = 25.6KB (64位) 或 12.8KB (32位)
- **总计** = ~**281KB** (64位) 或 ~**141KB** (32位)

#### C2 编译器

```cpp
// compile.cpp:680,760
Compile::Compile(...)
  : _code_buffer("Compile::Fill_buffer"),  // 使用默认构造，依赖自动扩展
    ...
{
  // 估算节点图大小（不是 CodeBuffer 大小）
  uint estimated_size = method()->code_size() * 4 + 64;
}
```

**C2 初始大小：**
- **使用 CodeBuffer 默认构造**（通常很小，如 4KB 或 8KB）
- **依赖自动扩展机制**
- C2 更信任 expand() 机制，不预先分配大空间

### C1 vs C2 vs Yuhu 的 CodeBuffer 策略对比

| 特性 | C1 | C2 | Yuhu (当前) |
|------|----|----|-------------|
| **初始大小** | ~281KB (64位) | 默认值 (~4KB) | ❓ 待检查 |
| **扩展机制** | ✅ 使用 expand() | ✅ 使用 expand() | ❓ 待检查 |
| **预分配策略** | 保守预分配 | 激进依赖扩展 | ❓ 待检查 |
| **BufferBlob** | 预先创建 | 使用 CodeCache | ❓ 待检查 |

### 为什么 Yuhu 遇到越界问题？

根据错误信息：
```
not in CodeBuffer memory: 0x000000010a8172c0 <= 0x000000010a8574c1 <= 0x000000010a8574c0
```

**问题分析：**
1. **越界 1 字节**：`0x8574c1 > 0x8574c0`
2. **空间大小**：`0x8574c0 - 0x8172c0 = 0x40200` = **256KB**
3. **可能原因**：
   - ❌ CodeBuffer 被标记为 `frozen`（不允许扩展）
   - ❌ 扩展机制未被正确触发
   - ❌ 某个写操作没有检查空间是否足够

---

### 028 问题的具体关联

在 028 问题中：
- **CodeBuffer** 在编译 `encodeArrayLoop` 时耗尽了空间（256 KB）
- 尝试调用 `CodeBuffer::insts()->set_end(pc)` 时越界 1 字节
- 错误发生在**临时代码生成阶段**，还没到复制到 CodeBlob 的步骤
- 解决方案是增加 CodeBuffer 的初始大小，或实现动态扩展

## 相关代码位置

### 触发断言的调用栈

```
codeBuffer.hpp:176  - CodeSection::set_end()
  ↑ 被调用于
  ? - 某个代码生成函数
  ↑ 被调用于
yuhu IR 生成阶段 - 在 "Emitting IR for block 0" 时
```

### 需要检查的代码

1. **CodeBuffer 初始化**：
   ```cpp
   // 在 YuhuCompiler 或 YuhuFunction 中
   // 查找 CodeBuffer 的创建和初始化
   CodeBuffer cb(...);
   cb.initialize(code_start, initial_size);
   ```

2. **代码生成调用**：
   ```cpp
   // 在生成 IR 时，哪些操作会调用 set_end()
   // 查找：
   // - emit_* 函数
   // - write_inst* 函数
   // - inline assembly 生成
   ```

3. **栈帧初始化**：
   ```cpp
   // YuhuStack::CreateBuildAndPushFrame
   // 或类似的栈帧设置函数
   ```

## 解决方向

### 短期解决方案：增加 CodeBuffer 大小

**方案 1：增加初始大小**
```cpp
// 找到 CodeBuffer 初始化的地方
// 原来可能是：
csize_t initial_size = 256 * 1024;  // 256 KB

// 改为：
csize_t initial_size = 512 * 1024;  // 512 KB
// 或者根据方法复杂度动态计算：
csize_t initial_size = estimate_code_size(method);
```

**方案 2：启用 CodeBuffer 自动扩展**
```cpp
// 检查 CodeBuffer 是否支持动态扩展
// 如果支持，确保在接近边界时触发扩展
if (cb.remaining() < threshold) {
  cb.expand();
}
```

### 中期解决方案：优化代码生成

1. **减少冗余代码**：
   - 检查是否有重复的指令生成
   - 优化栈帧初始化代码

2. **延迟代码生成**：
   - 某些代码可以延迟到真正需要时再生成
   - 避免提前生成大量未使用的代码

3. **使用更紧凑的编码**：
   - 检查 inline assembly 是否使用了最紧凑的指令编码
   - 优化常量池使用

### 长期解决方案：改进架构

1. **分阶段代码生成**：
   - 将 IR 生成和机器码生成分离
   - 使用多个较小的 CodeBuffer

2. **按需分配**：
   - 根据方法复杂度动态估算 CodeBuffer 大小
   - 实现更智能的内存管理

3. **代码压缩**：
   - 使用代码压缩技术减少生成的机器码大小

## 调试步骤

### 1. 确定 CodeBuffer 的初始大小

```cpp
// 在 CodeBuffer 初始化时添加日志
tty->print_cr("CodeBuffer initialized: start=%p, size=%d (0x%x)", 
              code_start, initial_size, initial_size);
```

### 2. 监控 CodeBuffer 使用情况

```cpp
// 在 IR 生成的关键点添加日志
tty->print_cr("CodeBuffer usage: used=%d, remaining=%d, total=%d",
              cb.insts_size(), cb.insts_remaining(), cb.total_content_size());
```

### 3. 找出哪个操作导致越界

```cpp
// 在 set_end() 调用之前添加检查
void safe_set_end(CodeBuffer* cb, address pc) {
  if (!cb->insts()->allocates2(pc)) {
    tty->print_cr("WARNING: About to overflow CodeBuffer!");
    tty->print_cr("  current end=%p, new end=%p, limit=%p",
                  cb->insts()->end(), pc, cb->insts()->limit());
    tty->print_cr("  overflow by %d bytes", pc - cb->insts()->limit());
    // 打印调用栈
    frame fr = os::current_frame();
    while (!fr.is_first_frame()) {
      fr.print_on(tty);
      fr = os::get_sender_for_C_frame(&fr);
    }
  }
  cb->insts()->set_end(pc);
}
```

### 4. 检查 "loop 602" 的来源

```cpp
// 搜索代码库中输出这个日志的位置
grep -r "loop.*c\[.*\]" hotspot/src/
// 或者
grep -r "328350" hotspot/src/
```

## 预期结果

修复后应该能够：
1. 成功为 `encodeArrayLoop` 方法分配足够的 CodeBuffer 空间
2. 完成整个方法的 IR 生成和机器码生成
3. 不再出现内存越界错误

## 相关问题

这个问题与 027 文档中的参数处理问题是**独立的**：
- **027 问题**：LLVM 函数参数数量不匹配（缺少 `this` 参数）
- **028 问题**：CodeBuffer 空间不足导致内存越界

两个问题都需要修复才能成功编译 `encodeArrayLoop` 方法。

---

## 根本原因：create_unique_offset() 破坏了 4 字节对齐

### 问题代码

```cpp
int create_unique_offset() const {
  int offset = masm().offset();
  // advance() is implemented by setting the end of the code section
  masm().code_section()->set_end(masm().code_section()->end() + 1);  // ❌ 每次只增加 1 字节
  return offset;
}
```

### 为什么导致 CodeBuffer 溢出？

#### 1. **破坏了 4 字节对齐**

AArch64 架构要求：
- **所有指令必须 4 字节对齐**
- `nop()` 指令占用 4 字节
- `offset()` 必须始终是 4 的倍数

但 `create_unique_offset()` 每次调用都增加 **1 字节**：
```
初始 offset: 0
调用 create_unique_offset(): offset = 1  (奇数！)
调用 create_unique_offset(): offset = 2
调用 create_unique_offset(): offset = 3
调用 create_unique_offset(): offset = 4
调用 create_unique_offset(): offset = 5  (奇数！)
...
```

#### 2. **触发无限循环对齐**

当 `malloc()` 调用 `align(8)` 时：
```cpp
void MacroAssembler::align(int modulus) {
  while (offset() % modulus != 0) nop();  // nop() 每次增加 4 字节
}
```

如果 `offset()` 是奇数（如 157）：
```
offset = 157
157 % 8 = 5  (不等于 0)
  → 调用 nop() (+4 字节)
offset = 161
161 % 8 = 1  (仍然不等于 0)
  → 调用 nop() (+4 字节)
offset = 165
165 % 8 = 5  (仍然不等于 0)
  → 调用 nop() (+4 字节)
...
永远无法对齐到 8 的倍数！
无限循环直到 CodeBuffer 溢出！
```

#### 3. **为什么恰好越界 1 字节？**

```
CodeBuffer size: 256 KB (0x40200 bytes)
Limit: 0x10a8574c0

无限循环填充 nop:
offset = 157 → 161 → 165 → 169 → ...
最终到达:
offset = 262653 (0x4021d)
下一个 nop 会写入到: 0x10a8574c1
超出 limit: 0x10a8574c0
越界 1 字节！
```

### 完整的问题链

```
1. create_unique_offset() 每次增加 1 字节
   ↓
2. offset() 变成奇数（如 157）
   ↓
3. malloc() 调用 align(8)
   ↓
4. align() 进入无限循环（157 → 161 → 165 → ...）
   ↓
5. 不断插入 nop 指令
   ↓
6. CodeBuffer 空间耗尽
   ↓
7. set_end() 越界 1 字节
   ↓
8. 断言失败！
```

### 解决方案

**修改 `create_unique_offset()` 保持 4 字节对齐：**

```cpp
int create_unique_offset() const {
  int offset = masm().offset();
  // 每次增加 4 字节（一条指令的大小），保持对齐
  masm().code_section()->set_end(masm().code_section()->end() + 4);  // ✅ 改为 4
  return offset;
}
```

**效果：**
```
初始 offset: 0
调用 create_unique_offset(): offset = 4   (4 的倍数 ✓)
调用 create_unique_offset(): offset = 8   (4 的倍数 ✓)
调用 create_unique_offset(): offset = 12  (4 的倍数 ✓)
...
align(8) 可以正常工作:
  offset = 12 → 12 % 8 = 4 → nop() → 16 (成功对齐！)
```

### 为什么之前没有发现这个问题？

可能的原因：
1. **简单方法测试不足**：`Matrix.multiply` 可能调用 `create_unique_offset()` 次数较少
2. **没有触发 `malloc()`**：某些代码路径不需要调用 `malloc()` 和 `align()`
3. **`encodeArrayLoop` 特别复杂**：
   - 12 个局部变量
   - 大量对象引用（需要大量 `inline_Metadata` 调用）
   - 每次 `inline_Metadata` 都可能调用 `create_unique_offset()`
   - 最终累积导致 offset 变成奇数

### 关键教训

**在机器码生成中，必须始终保持指令对齐：**
- AArch64: 4 字节对齐
- x86_64: 1 字节对齐（但推荐 16 字节对齐以提高性能）
- 任何增加 CodeBuffer offset 的操作都必须是指令大小的倍数
- 破坏对齐会导致无法预测的错误（如无限循环、段错误等）

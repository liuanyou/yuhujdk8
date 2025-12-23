# Yuhu 编译器栈帧遍历崩溃分析 (024)

## 问题背景

启用 Yuhu 编译器时，在 safepoint 期间栈帧遍历导致 SIGSEGV 崩溃：

```
SIGSEGV (0xb) at pc=0x0000000107c2d5e8
=== ERROR: pc is NULL in frame constructor ===
  sp=0x16dbea5d0, unextended_sp=0xdeadbeef, fp=0x16dbea5c0, pc=0x0
```

崩溃发生在 `frame::frame` 构造函数中，`pc` 为 NULL，`unextended_sp` 为 `0xdeadbeef`（Yuhu 写入的 frame marker）。

---

## 问题分析

经过代码分析，发现 **三个核心问题**：

### 问题 1: LLVM Prologue 与 frame_size 不一致

这个问题已拆分到单独文档：[025_frame_size_prologue_mismatch.md](file:///Users/liuanyou/CLionProjects/jdk8/doc/yuhu/activities/025_frame_size_prologue_mismatch.md)，本篇只讨论与栈帧遍历直接相关的对齐和 last_Java_fp 设置。

---

### 问题 2: frame_size 未对齐

#### 现象

**yuhuCompiler.cpp 第 548 行**（注册到 nmethod）：
```cpp
int frame_size = frame_words + locals_words + 2;  // 未对齐
```

**yuhuStack.cpp 第 65-69 行**（实际分配）：
```cpp
int frame_size_bytes = (extended_frame_size() + 2) * wordSize;
frame_size_bytes = align_size_up(frame_size_bytes, 16);  // 对齐到 16 字节
```

#### 问题

如果 `(extended_frame_size() + 2)` 是奇数，对齐后会多出 1 word (8 字节)，但注册的 `frame_size` 没有反映这个对齐。

例如：
- extended_frame_size() = 21
- 注册的 frame_size = 21 + 2 = 23 words
- 实际 frame_size_bytes = align_up(23 * 8, 16) = 192 bytes = 24 words
- 差 1 word，导致从错误位置读取 sender_pc

#### 解决方案 ✓

在 `yuhuCompiler.cpp` 中对齐 `frame_size`：

```cpp
int frame_size = frame_words + locals_words + 2;
frame_size = align_size_up(frame_size, 2);  // 对齐到 2 words (16 bytes)
```

---

### 问题 3: last_Java_fp 设置错误

#### 现象

**yuhuStack.cpp 第 96-100 行**：
```cpp
Value *prev_fp = builder()->CreateValueOfStructEntry(
  thread(),
  JavaThread::last_Java_fp_offset(),
  YuhuType::intptr_type(),
  "prev_fp");
```

然后第 263-265 行把 `prev_fp` 存回 `last_Java_fp`：
```cpp
store_fp_args.push_back(fp_addr_i64);
store_fp_args.push_back(prev_fp);  // 存的是前一帧的 fp
```

#### 问题

- `last_Java_fp` 应该存 **当前帧的 FP 值**（x29 寄存器的值）
- Yuhu 错误地读取 `thread->last_Java_fp`（前一帧的 fp），然后存回去（等于没变）

#### 解决方案 ✓

修改 `last_Java_fp` 的设置，存当前帧的 FP 值：

```cpp
// 读取当前帧的 FP（x29 寄存器）
Value *current_fp = builder()->CreateReadFramePointer();  // 或者用 add x29, sp, #offset 后的值

// 存到 last_Java_fp
std::vector<Value*> store_fp_args;
store_fp_args.push_back(fp_addr_i64);
store_fp_args.push_back(current_fp);  // 改为存当前帧的 fp
builder()->CreateCall(store_asm_type, store_sp_asm, store_fp_args);
```

---

## HotSpot 栈帧遍历机制说明

### 获取第一帧

```cpp
// thread_bsd_aarch64.cpp
frame JavaThread::pd_last_frame() {
  return frame(_anchor.last_Java_sp(), _anchor.last_Java_fp(), _anchor.last_Java_pc());
}
```

### 计算 sender 帧

对于 compiled frame，`frame::sender_for_compiled_frame()` 的逻辑：

```cpp
intptr_t* l_sender_sp = unextended_sp() + _cb->frame_size();
address sender_pc = (address) *(l_sender_sp - 1);  // LR 位置
intptr_t** saved_fp_addr = (intptr_t**) (l_sender_sp - 2);  // saved FP 位置
```

期望的栈帧布局：

```
高地址
+------------------+ <-- sender_sp (= unextended_sp + frame_size)
|       LR         |  <-- [sender_sp - 1]
+------------------+
|    saved FP      |  <-- [sender_sp - 2]
+------------------+
| 当前帧内容        |
+------------------+ <-- sp = unextended_sp
低地址
```

---

## 待办事项

- [ ] 问题 1：LLVM Prologue 与 frame_size 不一致（详见 025_frame_size_prologue_mismatch.md）
- [x] 问题 2：在 yuhuCompiler.cpp 中对齐 frame_size
  - 添加 `frame_size = align_size_up(frame_size, 2);`
- [x] 问题 3：修改 last_Java_fp 设置为当前帧的 FP 值
  - 将 `prev_fp` 改为 `current_fp = builder()->CreatePtrToInt(fp, YuhuType::intptr_type())`

---

## 相关文件

- `hotspot/src/share/vm/yuhu/yuhuStack.cpp` - 栈帧初始化和 last_Java_* 设置
- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - frame_size 计算和 nmethod 注册
- `hotspot/src/cpu/aarch64/vm/frame_aarch64.cpp` - AArch64 栈帧遍历逻辑
- `hotspot/src/cpu/aarch64/vm/frame_aarch64.inline.hpp` - frame 构造函数（崩溃点）

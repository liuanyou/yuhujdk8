# 活动 005: 帧大小不匹配错误 (offset != extended_frame_size)

## 日期
2025-12-06

## 问题描述

在运行 Yuhu 编译器时，遇到以下运行时错误：

```
Internal Error (/Users/liuanyou/CLionProjects/jdk8/hotspot/src/share/vm/yuhu/yuhuStack.cpp:115), pid=57409, tid=17923
assert(offset == extended_frame_size()) failed: should do
```

错误发生在 `YuhuStack::initialize()` 方法中，当尝试验证分配的帧 slot 数量与预期大小一致时。

## 错误调用栈

```
YuhuCompiler::compile_method
  → YuhuFunction::build
    → YuhuFunction::initialize
      → YuhuStack::CreateBuildAndPushFrame
        → YuhuStack::initialize
          → assert(offset == extended_frame_size()) ← 这里失败
```

## 问题根源分析

### 1. `extended_frame_size` 的计算

在 `yuhuStack.cpp` 第 47 行：

```cpp
_extended_frame_size = frame_words + locals_words;
```

其中：
- `frame_words = header_words + monitor_words + stack_words` (第 45 行)
- `header_words = 2` (第 42 行，注释说这是 "frame pointer + return address")

所以：
```
extended_frame_size = header_words + monitor_words + stack_words + locals_words
                    = 2 + monitor_words + stack_words + locals_words
```

### 2. 实际分配的 slot 数量

在 `initialize()` 方法中，`offset` 从 0 开始累加（第 76 行）：

| 步骤 | 操作 | offset 值 | 说明 |
|------|------|-----------|------|
| 初始 | `offset = 0` | 0 | |
| 1 | `offset += stack_words` | stack_words | 表达式栈 |
| 2 | `offset += monitor_words` | stack_words + monitor_words | 监视器 |
| 3 | `offset++` | stack_words + monitor_words + 1 | 临时 oop slot |
| 4 | `offset++` | stack_words + monitor_words + 2 | 方法指针 |
| 5 | `offset++` | stack_words + monitor_words + 3 | Unextended SP |
| 6 | `offset++` | stack_words + monitor_words + 4 | PC |
| 7 | `offset++` | stack_words + monitor_words + 5 | Frame marker |
| 8 | `offset++` | stack_words + monitor_words + 6 | Frame pointer address |
| 9 | `offset += locals_words` | stack_words + monitor_words + 6 + locals_words | 局部变量 |

**最终 offset 值**：`stack_words + monitor_words + 6 + locals_words`

### 3. 不匹配的原因

- **期望值**：`extended_frame_size = 2 + monitor_words + stack_words + locals_words`
- **实际值**：`offset = stack_words + monitor_words + 6 + locals_words`

**差异**：实际比期望多 **4 个 slot**（6 - 2 = 4）

这 4 个额外的 slot 是：
1. 临时 oop slot（第 87 行）
2. 方法指针（第 90 行）
3. Unextended SP（第 97 行）
4. PC（第 100 行）

### 4. `header_words` 的定义问题

在 `yuhuStack.cpp` 第 40-42 行：

```cpp
// For AArch64, header_words = 2 (frame pointer + return address)
// This matches the frame layout: [fp, lr, ...]
int header_words  = 2;
```

**问题**：注释说 `header_words = 2` 是 "frame pointer + return address"，但实际分配的帧头包含 6 个 slot：
1. 临时 oop slot
2. 方法指针
3. Unextended SP
4. PC
5. Frame marker
6. Frame pointer address

## Shark 的实现对比

### Shark 的 `header_words` 定义

在 `sharkFrame_zero.hpp` 中：

```cpp
class SharkFrame : public ZeroFrame {
  enum Layout {
    pc_off = jf_header_words,  // jf_header_words = 2 (from ZeroFrame)
    unextended_sp_off,
    method_off,
    oop_tmp_off,
    header_words  // 这是 SharkFrame 的 header_words
  };
};
```

**关键点**：
- `jf_header_words = 2`（来自 `ZeroFrame`，包含 `frame_type` 和 `next_frame`）
- `SharkFrame::header_words` 是枚举的最后一个值，等于 `oop_tmp_off + 1`
- 所以 `SharkFrame::header_words = jf_header_words + 4 = 2 + 4 = 6`

### Shark 的帧布局（从注释）

```
| stack slot n-1     |       low addresses
|  ...               |
| stack slot 0       |
| monitor m-1        |
|  ...               |
| monitor 0          |
| oop_tmp            |
| method             |
| unextended_sp      |
| pc                 |
| frame_type         |  ← jf_header_words (2 words)
| next_frame         |      high addresses
```

### Shark 的 `initialize` 方法

在 `sharkStack.cpp` 中：

```cpp
int header_words = SharkFrame::header_words;  // = 6
int frame_words = header_words + monitor_words + stack_words;
_extended_frame_size = frame_words + locals_words;

// 然后分配：
offset += stack_words;        // 表达式栈
offset += monitor_words;      // 监视器
offset++;                      // oop_tmp
offset++;                      // method
offset++;                      // unextended_sp
offset++;                      // pc
offset++;                      // frame_type (jf_header_words 的一部分)
offset++;                      // next_frame (jf_header_words 的一部分)
// 实际上 frame_type 和 next_frame 是在 ZeroFrame 中处理的
// 所以 SharkFrame 的 header_words = 6 包含了：
//   - oop_tmp
//   - method
//   - unextended_sp
//   - pc
//   - frame_type (2 words from ZeroFrame)
```

**注意**：Shark 使用的是 Zero 架构的栈，所以有 `jf_header_words`（ZeroFrame 的头部）。但 Yuhu 是为 AArch64 设计的，不使用 ZeroStack。

## Yuhu 的问题

### 问题 1：`header_words` 定义不正确

Yuhu 的 `header_words = 2` 只包含了：
- Frame marker（1 word）
- Frame pointer address（1 word）

但实际上，帧头应该包含 6 个 slot：
1. 临时 oop slot
2. 方法指针
3. Unextended SP
4. PC
5. Frame marker
6. Frame pointer address

### 问题 2：帧布局理解错误

Yuhu 的注释说 `header_words = 2` 是 "frame pointer + return address"，这是对 AArch64 ABI 的误解。实际上：
- AArch64 的 frame pointer 和 return address 是由硬件/ABI 管理的，不在 Java 帧布局中
- Java 帧布局中的 "header" 是指 Java 运行时需要的元数据，不是 ABI 的帧头

## 正确的帧布局

根据 Shark 的实现和 Yuhu 的实际分配，正确的帧布局应该是：

```
| locals[0..n-1]     |       low addresses (高地址方向)
|  ...               |
| frame_pointer_addr |
| frame_marker       |
| pc                 |
| unextended_sp      |
| method             |
| oop_tmp            |
| monitor[0..m-1]    |
|  ...               |
| stack[0..s-1]      |      high addresses (低地址方向)
```

**header_words 应该 = 6**，包含：
1. oop_tmp
2. method
3. unextended_sp
4. pc
5. frame_marker
6. frame_pointer_addr

## 解决方案

### 方案 1：修正 `header_words` 的值（推荐）

将 `header_words` 从 2 改为 6：

```cpp
// For AArch64, header_words includes:
//   - oop_tmp (1 word)
//   - method (1 word)
//   - unextended_sp (1 word)
//   - pc (1 word)
//   - frame_marker (1 word)
//   - frame_pointer_addr (1 word)
int header_words = 6;
```

### 方案 2：重新组织帧布局计算

如果 `header_words` 只指 Frame marker 和 Frame pointer address，那么需要将其他 4 个 slot 单独计算：

```cpp
int header_words = 2;  // Frame marker + Frame pointer address
int metadata_words = 4;  // oop_tmp + method + unextended_sp + pc
int frame_words = header_words + metadata_words + monitor_words + stack_words;
```

但这样会破坏与 Shark 的一致性，不推荐。

## 验证

修复后，应该满足：
```
offset = stack_words + monitor_words + 6 + locals_words
extended_frame_size = 6 + monitor_words + stack_words + locals_words
offset == extended_frame_size  ✅
```

## 相关文件

- `hotspot/src/share/vm/yuhu/yuhuStack.cpp` - 问题发生的位置
- `hotspot/src/share/vm/yuhu/yuhuStack.hpp` - 帧布局定义
- `hotspot/src/share/vm/shark/sharkStack.cpp` - Shark 的参考实现
- `hotspot/src/cpu/zero/vm/sharkFrame_zero.hpp` - Shark 的帧布局定义

## 经验教训

1. **帧布局必须与实际分配一致**：`extended_frame_size` 的计算必须与 `offset` 的累加过程完全匹配。

2. **`header_words` 的含义**：在 Java 帧布局中，`header_words` 指的是 Java 运行时需要的元数据 slot 数量，不是 ABI 的帧头。

3. **参考 Shark 实现**：Shark 是 Yuhu 的参考实现，帧布局应该保持一致（除了 ZeroStack 相关的部分）。

4. **注释的重要性**：代码注释应该准确反映实际实现，错误的注释会导致误解。

## 修复实现

### 修改文件

**`hotspot/src/share/vm/yuhu/yuhuStack.cpp`** (第 40-42 行)

修改前：
```cpp
// For AArch64, header_words = 2 (frame pointer + return address)
// This matches the frame layout: [fp, lr, ...]
int header_words  = 2;
```

修改后：
```cpp
// For AArch64, header_words includes all frame header metadata:
//   - oop_tmp (1 word)
//   - method (1 word)
//   - unextended_sp (1 word)
//   - pc (1 word)
//   - frame_marker (1 word)
//   - frame_pointer_addr (1 word)
// This matches SharkFrame::header_words = 6
int header_words  = 6;
```

### 修复效果

- ✅ `extended_frame_size` 现在正确计算为 `6 + monitor_words + stack_words + locals_words`
- ✅ 与实际分配的 `offset = stack_words + monitor_words + 6 + locals_words` 匹配
- ✅ `assert(offset == extended_frame_size())` 应该通过
- ✅ 与 Shark 的实现保持一致

## 状态

- ✅ 问题已定位
- ✅ 根本原因已分析
- ✅ 解决方案已确定
- ✅ 修复已实现
- ⏳ 等待测试验证


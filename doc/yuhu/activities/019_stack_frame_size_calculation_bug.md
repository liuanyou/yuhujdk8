# 019: 栈帧大小计算错误导致寄存器覆盖

## 问题描述

在执行编译后的代码时，发现保存的寄存器（x28, x27, x26, x25）在函数退出时被错误恢复，导致 `SIGSEGV`。

### 症状

从内存转储看：
- **进入 LLVM 函数时**（prologue 之后）：
  - `[sp, sp+16]` - x28 = `0x0000000132809800`, x27 = `0x0000000000000000`
  - `[sp+16, sp+32]` - x26 = `0x00000001047786b0`, x25 = `0x0000000000000000`

- **退出 LLVM 函数时**（epilogue 之前）：
  - `[sp, sp+16]` - x28 = `0x0000000100000064`（**被修改**）, x27 = `0x000000076ab981b8`（**被修改**）
  - `[sp+16, sp+32]` - x26 = `0x000000076ab98508`（**被修改**）, x25 = `0x000000076ab98368`（**被修改**）

### 汇编代码分析

从生成的汇编代码看：

```assembly
; LLVM prologue: 保存寄存器
0x000000010c6b68dc: stp x28, x27, [sp, #-96]!  ; sp = sp - 96, 保存 x28, x27 到 [sp, sp+16]
0x000000010c6b68e0: stp x26, x25, [sp, #16]    ; 保存 x26, x25 到 [sp+16, sp+32]
...

; Yuhu 栈帧分配
0x000000010c6b6900: sub x25, sp, #0x80         ; x25 = sp - 0x80 (128 字节)

; 写入局部变量（从 arg_base 读取参数并写入栈帧）
0x000000010c6b6974: ldr x11, [x3, #8]          ; 读取 arg[1]
0x000000010c6b6978: str x11, [x25, #152]       ; 写入到 x25 + 152 = sp + 0x18
0x000000010c6b697c: ldr x10, [x3, #16]         ; 读取 arg[2]
0x000000010c6b6980: str x10, [x25, #144]       ; 写入到 x25 + 144 = sp + 0x10
0x000000010c6b6984: ldr x9, [x3, #24]          ; 读取 arg[3]
0x000000010c6b6988: str x9, [x25, #136]        ; 写入到 x25 + 136 = sp + 0x8
0x000000010c6b698c: ldr w28, [x3, #32]         ; 读取 arg[4]
0x000000010c6b6990: str w28, [x25, #128]       ; 写入到 x25 + 128 = sp + 0x0
```

### 问题分析

1. **栈布局**（prologue 之后，`sp = old_sp - 96`）：
   ```
   高地址 (old_sp)
     [保存的寄存器区域: old_sp - 96 到 old_sp]
       [sp, sp+16]     - x28, x27
       [sp+16, sp+32] - x26, x25
       [sp+32, sp+48] - x24, x23
       [sp+48, sp+64] - x22, x21
       [sp+64, sp+80] - x20, x19
       [sp+80, sp+96] - x29, x30
     [Yuhu 栈帧: sp - frame_size_bytes 到 sp]
       表达式栈
       监视器
       局部变量 (locals_slots_offset 开始)
   低地址 (sp - frame_size_bytes)
   ```

2. **写入位置计算**：
   - `x25 = sp - 0x80`（frame base，0x80 = 128 字节）
   - `str x11, [x25, #152]` → `(sp - 0x80) + 152 = sp + 0x18`
   - `str x10, [x25, #144]` → `(sp - 0x80) + 144 = sp + 0x10`
   - `str x9, [x25, #136]` → `(sp - 0x80) + 136 = sp + 0x8`
   - `str w28, [x25, #128]` → `(sp - 0x80) + 128 = sp + 0x0`

3. **覆盖问题**：
   - 写入位置 `sp + 0x0` 到 `sp + 0x18` 正好覆盖了保存的寄存器区域 `[sp, sp+32]`
   - 这导致 x28, x27, x26, x25 的值被破坏

## 根本原因

**栈帧大小计算错误**：`frame_size_bytes` 的计算使用了 `(frame_words + extra_locals) * wordSize`，但实际需要的栈帧大小应该是 `extended_frame_size * wordSize`。

### 代码分析

在 `YuhuStack::initialize()` 中：

```cpp
int locals_words  = max_locals();
int extra_locals  = locals_words - arg_size();
int frame_words   = header_words + monitor_words + stack_words;

_extended_frame_size = frame_words + locals_words;  // 实际需要的栈帧大小

// BUG: frame_size_bytes 计算错误
int frame_size_bytes = (frame_words + extra_locals) * wordSize;
// = (frame_words + locals_words - arg_size()) * 8
// < (frame_words + locals_words) * 8
// = extended_frame_size * 8
```

**问题**：
- `frame_size_bytes = (frame_words + extra_locals) * 8`
- `extended_frame_size = frame_words + locals_words`
- 因为 `extra_locals = locals_words - arg_size()`，所以 `frame_size_bytes < extended_frame_size * 8`
- 差值 = `arg_size() * 8` 字节

**后果**：
- `x25 = sp - frame_size_bytes` 分配的栈帧空间不足
- 写入局部变量时（`locals_slots_offset` 到 `locals_slots_offset + locals_words - 1`）超出了分配范围
- 覆盖了保存的寄存器区域，导致函数退出时恢复错误的寄存器值

## 解决方案

将 `frame_size_bytes` 的计算改为基于 `extended_frame_size`：

```cpp
// 修复前
int frame_size_bytes = (frame_words + extra_locals) * wordSize;

// 修复后
int frame_size_bytes = extended_frame_size() * wordSize;
```

### 修改位置

`hotspot/src/share/vm/yuhu/yuhuStack.cpp:62`

### 修改内容

```cpp
// Calculate frame size in bytes
// FIXED: Use extended_frame_size instead of (frame_words + extra_locals)
// because we need space for all locals_words, not just extra_locals
// extended_frame_size = frame_words + locals_words
int frame_size_bytes = extended_frame_size() * wordSize;
```

## 验证

修复后，`frame_size_bytes` 应该等于 `extended_frame_size * 8`，确保：
1. 栈帧分配的空间足够容纳所有局部变量
2. 写入局部变量时不会超出分配范围
3. 不会覆盖保存的寄存器区域

## 相关文件

- `hotspot/src/share/vm/yuhu/yuhuStack.cpp` - 栈帧初始化
- `hotspot/src/share/vm/yuhu/yuhuStack.hpp` - 栈帧定义

## 参考

- Shark 的实现：`hotspot/src/share/vm/shark/sharkStack.cpp:48-50`
  ```cpp
  Value *stack_pointer = builder()->CreateSub(
    CreateLoadStackPointer(),
    LLVMValue::intptr_constant((frame_words + extra_locals) * wordSize));
  ```
  注意：Shark 使用的是 ZeroStack，栈帧布局不同，所以它可以使用 `extra_locals`。但 Yuhu 使用的是标准 AArch64 栈，需要为所有 `locals_words` 分配空间。

## 相关文档

- `020_safepoint_hang.md` - Safepoint 卡死问题（`yuhuCompiler.cpp` 中 `frame_size` 计算错误）


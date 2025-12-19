# 020: Safepoint 卡死问题

## 问题描述

当循环次数增加到 1000 时，程序卡死，无法退出。

### 症状

从线程 dump 看：
- **主线程**（thread #3）：在 `SafepointSynchronize::block()` 中等待 safepoint 完成
- **VM Thread**（thread #12）：在 `frame::sender()` 中执行 `NMethodSweeper::mark_active_nmethods()`，卡在 `frame_aarch64.cpp:557`
- **编译线程**（Yuhu CompilerThread0, C2 CompilerThread0/1/2 等）：都在 `CompileQueue::get()` 中等待，这是正常的

### 线程 Dump 关键信息

**第一次线程 dump（修复前）**：
```
thread #3 (主线程)
  frame #7: SafepointSynchronize::block(thread=0x0000000158019800) at safepoint.cpp:669:21
  frame #8-11: JavaCalls::call_helper -> jni_CallStaticVoidMethod -> JavaMain

thread #12 (VM Thread)
  frame #0: frame::sender(this=0x000000016e6d9e68, map=0x000000016e6d9e98) const at frame_aarch64.cpp:557:29
  frame #1: StackFrameStream::next(this=0x000000016e6d9e60) at frame.hpp:576:62
  frame #2: JavaThread::nmethods_do(this=0x0000000158019800, cf=0x000000010bcd2e00) at thread.cpp:2805:57
  frame #3: Threads::nmethods_do(cf=0x000000010bcd2e00) at thread.cpp:4289:8
  frame #4: NMethodSweeper::mark_active_nmethods() at sweeper.cpp:218:5
  frame #5: SafepointSynchronize::do_cleanup_tasks() at safepoint.cpp:523:5
  frame #6: SafepointSynchronize::begin() at safepoint.cpp:396:3
```

**第二次线程 dump（修复后，问题依然存在）**：
```
thread #3 (主线程)
  frame #7: SafepointSynchronize::block(thread=0x0000000136813800) at safepoint.cpp:669:21

thread #12 (VM Thread)
  frame #0: CodeCache::find_blob(start=0x013d70d0004b00ac) at codeCache.cpp:272:7
  frame #1: frame::sender(this=0x000000016cf2e0e8, map=0x000000016cf2e118) const at frame_aarch64.cpp:546:3
  frame #2: StackFrameStream::next(this=0x000000016cf2e0e0) at frame.hpp:576:62
  frame #3: JavaThread::nmethods_do(this=0x0000000136813800, cf=0x000000010d47ee00) at thread.cpp:2805:57
  frame #4: Threads::nmethods_do(cf=0x000000010d47ee00) at thread.cpp:4289:8
  frame #5: NMethodSweeper::mark_active_nmethods() at sweeper.cpp:218:5
  frame #6: SafepointSynchronize::do_cleanup_tasks() at safepoint.cpp:523:5
  frame #7: SafepointSynchronize::begin() at safepoint.cpp:396:3
```

**关键发现**：
- 修复后，VM Thread 卡在 `CodeCache::find_blob(start=0x013d70d0004b00ac)`
- 地址 `0x013d70d0004b00ac` 看起来异常，可能是错误的 PC 值
- 这说明 `frame::sender_for_compiled_frame()` 计算出的 `sender_pc` 是错误的

## 根本原因

### 问题 1: `frame_size` 计算错误（已修复，但不足以解决问题）

**`frame_size` 参数传递给 `register_method` 时计算错误**：

在 `yuhuCompiler.cpp:628` 中：
```cpp
int frame_size = frame_words + extra_locals;  // frame_size in words
```

但实际需要的栈帧大小应该是：
```cpp
int extended_frame_size = frame_words + locals_words;
```

因为 `extra_locals = locals_words - arg_size()`，所以：
- `frame_size = frame_words + locals_words - arg_size()`
- `extended_frame_size = frame_words + locals_words`
- `frame_size < extended_frame_size`，差值 = `arg_size()` words

**影响**：
- `nmethod::frame_size()` 返回值不正确（偏小）
- 栈帧遍历时 `l_sender_sp` 计算错误

**状态**：已修复，但问题依然存在。

### 问题 2: Yuhu 栈帧不符合 HotSpot 标准 AArch64 布局（根本原因）

**HotSpot 标准 AArch64 编译帧布局**：

从 `frame_aarch64.hpp` 和 `frame::sender_for_compiled_frame()` 看，HotSpot 期望的标准布局是：

```cpp
// frame_aarch64.hpp
enum {
  link_offset = 0,           // 相对于 fp
  return_addr_offset = 1,    // 相对于 fp
  sender_sp_offset = 2,      // 相对于 fp
};

// frame::sender_for_compiled_frame()
intptr_t* l_sender_sp = unextended_sp() + _cb->frame_size();
address sender_pc = (address) *(l_sender_sp - 1);  // 返回地址在 [sender_sp - 1]
intptr_t** saved_fp_addr = (intptr_t**) (l_sender_sp - sender_sp_offset);  // FP 在 [sender_sp - 2]
```

**C2 编译器的标准做法**（从 `macroAssembler_aarch64.cpp:build_frame()` 看）：

```cpp
void MacroAssembler::build_frame(int framesize) {
  if (framesize < ((1 << 9) + 2 * wordSize)) {
    sub(sp, sp, framesize);                              // 1. 先分配栈空间
    stp(rfp, lr, Address(sp, framesize - 2 * wordSize)); // 2. 保存 FP 和 LR
  }
}
```

这意味着：
- FP 和 LR 保存在 `[sp + framesize - 2*wordSize]` 和 `[sp + framesize - wordSize]`
- 当 `sender_sp = unextended_sp + frame_size` 时：
  - 返回地址在 `[sender_sp - 1]` = `[unextended_sp + frame_size - 1]`
  - FP 在 `[sender_sp - 2]` = `[unextended_sp + frame_size - 2]`

**Yuhu 栈帧的问题**：

Yuhu 的栈帧布局（从 `yuhuStack.cpp` 看）：
```cpp
// Yuhu 栈帧布局（从高地址到低地址）：
//   [expression stack]
//   [monitors]
//   [oop_tmp]
//   [method]
//   [unextended_sp]
//   [pc]                    // Yuhu 内部的 PC，不是返回地址
//   [frame_marker]          // 0xDEADBEEF
//   [frame_pointer_addr]    // 保存的 FP（但不是标准位置）
//   [local variables]
```

**关键问题**：
1. **没有在标准位置保存返回地址（LR）**：
   - Yuhu 没有在 `[sender_sp - 1]` 保存返回地址
   - `frame::sender_for_compiled_frame()` 读取 `*(l_sender_sp - 1)` 时得到的是错误的值（可能是 `0xDEADBEEF` 或其他垃圾值）

2. **没有在标准位置保存 FP**：
   - Yuhu 将 FP 保存在 `frame_pointer_addr`，但不在 `[sender_sp - 2]`
   - `frame::sender_for_compiled_frame()` 读取 `*(l_sender_sp - 2)` 时得到错误的值

3. **LLVM 生成的函数 prologue 不符合 AArch64 ABI**：
   - LLVM 可能没有生成标准的 `stp x29, x30, [sp, #-16]!` 指令
   - 或者生成的指令顺序不对（先分配栈空间，后保存寄存器）

**为什么会导致 safepoint 卡死**：

1. **错误的 `sender_pc`**：
   - `frame::sender_for_compiled_frame()` 计算 `sender_pc = *(l_sender_sp - 1)`
   - 由于 Yuhu 没有在标准位置保存返回地址，`sender_pc` 是错误的值（如 `0xb5a3e4e0eb770061`）

2. **`CodeCache::find_blob()` 失败**：
   - `CodeCache::find_blob(sender_pc)` 无法找到对应的 `CodeBlob`
   - 返回 `NULL`，导致栈帧遍历失败

3. **无限循环**：
   - VM Thread 在 `frame::sender()` -> `CodeCache::find_blob()` 中反复调用
   - 每次都得到错误的 `sender_pc`，无法继续遍历栈帧
   - safepoint 操作无法完成，主线程一直等待

### 为什么循环 100 次正常，1000 次卡死？

- **循环 100 次**：编译的方法较少，safepoint 操作能较快完成，即使有错误也可能侥幸通过
- **循环 1000 次**：编译的方法增多，safepoint 需要遍历更多栈帧，错误的栈帧布局导致遍历卡住或变慢

## 解决方案

### 方案 1: 修复 `frame_size` 计算（已实施）

将 `frame_size` 的计算改为使用 `extended_frame_size`：

```cpp
// 修复前
int frame_size = frame_words + extra_locals;  // frame_size in words

// 修复后
int frame_size = frame_words + locals_words;  // frame_size in words (extended_frame_size)
```

**修改位置**：`hotspot/src/share/vm/yuhu/yuhuCompiler.cpp:628`

**状态**：已修复，但不足以解决问题。

### 方案 2: 修复栈帧布局，符合 HotSpot 标准 AArch64 布局（已实施）

**修复思路**：

在 `YuhuStack::initialize()` 中，在分配栈帧**之前**，先保存返回地址（LR）和 FP 到标准位置：

```cpp
// 1. 读取 LR 寄存器（x30）并保存到 [current_sp - 1]
Value *lr = builder()->CreateReadLinkRegister();
Value *return_addr_addr = builder()->CreateIntToPtr(
  builder()->CreateGEP(YuhuType::intptr_type(),
                       builder()->CreateIntToPtr(current_sp, PointerType::getUnqual(YuhuType::intptr_type())),
                       LLVMValue::intptr_constant(-1)),
  PointerType::getUnqual(YuhuType::intptr_type()));
builder()->CreateStore(lr, return_addr_addr);

// 2. 读取之前的 FP（从 last_Java_fp）并保存到 [current_sp - 2]
Value *prev_fp = builder()->CreateValueOfStructEntry(
  thread(),
  JavaThread::last_Java_fp_offset(),
  YuhuType::intptr_type(),
  "prev_fp");
Value *saved_fp_addr = builder()->CreateIntToPtr(
  builder()->CreateGEP(YuhuType::intptr_type(),
                       builder()->CreateIntToPtr(current_sp, PointerType::getUnqual(YuhuType::intptr_type())),
                       LLVMValue::intptr_constant(-2)),
  PointerType::getUnqual(YuhuType::intptr_type()));
builder()->CreateStore(prev_fp, saved_fp_addr);

// 3. 然后分配栈帧（sub sp, sp, frame_size_bytes）
Value *stack_pointer = builder()->CreateSub(
  current_sp,
  LLVMValue::intptr_constant(frame_size_bytes),
  "new_sp");
```

**为什么这个修复可以成功**：

1. **符合 AArch64 ABI**：
   - 在分配栈帧之前保存 LR 和 FP，确保它们在 `[sender_sp - 1]` 和 `[sender_sp - 2]`
   - 这与 C2 编译器的做法一致（虽然 C2 是先分配后保存，但最终位置相同）

2. **满足 HotSpot 的期望**：
   - `frame::sender_for_compiled_frame()` 期望返回地址在 `[l_sender_sp - 1]`
   - `frame::sender_for_compiled_frame()` 期望 FP 在 `[l_sender_sp - 2]`
   - 修复后，这两个位置都有正确的值

3. **正确的栈帧遍历**：
   - `sender_pc` 现在指向正确的返回地址
   - `CodeCache::find_blob(sender_pc)` 可以找到对应的 `CodeBlob`
   - 栈帧遍历可以正常进行，safepoint 操作可以完成

**修改位置**：`hotspot/src/share/vm/yuhu/yuhuStack.cpp:71-97`

**相关修改**：
- 添加 `CreateReadLinkRegister()` 函数（`yuhuBuilder.cpp`）
- 声明 `CreateReadLinkRegister()`（`yuhuBuilder.hpp`）

**状态**：已实施，等待测试验证。

### 为什么修复可以成功

**修复前的栈帧布局**（不符合 HotSpot 标准）：
```
[sender_sp] = unextended_sp + frame_size
  [sender_sp - 1] = 垃圾值（可能是 0xDEADBEEF 或其他）
  [sender_sp - 2] = 垃圾值（不是正确的 FP）
  [sender_sp - 3] = 0xDEADBEEF (frame_marker)
  ...
  [unextended_sp] = Yuhu 栈帧开始
```

**修复后的栈帧布局**（符合 HotSpot 标准）：
```
[sender_sp] = unextended_sp + frame_size
  [sender_sp - 1] = 返回地址（LR，从 x30 读取）✓
  [sender_sp - 2] = 保存的 FP（从 last_Java_fp 读取）✓
  [sender_sp - 3] = 0xDEADBEEF (frame_marker)
  ...
  [unextended_sp] = Yuhu 栈帧开始
```

**修复后的执行流程**：

1. **栈帧分配**：
   - 在 `current_sp` 位置保存 LR 到 `[current_sp - 1]`
   - 在 `current_sp` 位置保存 FP 到 `[current_sp - 2]`
   - 分配栈帧：`sub sp, sp, frame_size_bytes`

2. **栈帧遍历**（safepoint 时）：
   - `l_sender_sp = unextended_sp() + frame_size()`
   - `sender_pc = *(l_sender_sp - 1)` → 得到正确的返回地址 ✓
   - `saved_fp = *(l_sender_sp - 2)` → 得到正确的 FP ✓
   - `CodeCache::find_blob(sender_pc)` → 找到对应的 `CodeBlob` ✓
   - 继续遍历下一个栈帧 ✓

3. **Safepoint 完成**：
   - VM Thread 可以正常遍历所有栈帧
   - `NMethodSweeper::mark_active_nmethods()` 正常完成
   - Safepoint 操作完成，主线程可以继续执行

### 验证

修复后，需要验证：
1. `nmethod::frame_size()` 返回正确的值（方案 1）
2. 栈帧遍历时 `sender_pc` 计算正确（方案 2）
3. `CodeCache::find_blob()` 能找到正确的 CodeBlob（方案 2）
4. safepoint 操作能正常完成，不会卡死（方案 2）
5. 循环 1000 次也能正常退出（方案 2）

## 总结

### 为什么 Yuhu 栈帧不符合 HotSpot 标准

1. **设计差异**：
   - Yuhu 使用 LLVM 生成代码，LLVM 的栈帧布局可能与 HotSpot 期望的不同
   - Yuhu 的栈帧布局是为解释器设计的，包含了 `frame_marker`、`pc` 等解释器特有的字段
   - HotSpot 的编译帧布局是标准化的，所有编译器（C1、C2）都必须遵循

2. **缺少标准 ABI 支持**：
   - LLVM 生成的函数 prologue 可能没有显式保存 LR 和 FP 到标准位置
   - 或者保存的位置不对（在帧内而不是在 `[sender_sp - 1]` 和 `[sender_sp - 2]`）

3. **栈帧遍历的假设**：
   - HotSpot 的 `frame::sender_for_compiled_frame()` 假设所有编译帧都遵循标准布局
   - 它直接从 `[sender_sp - 1]` 读取返回地址，从 `[sender_sp - 2]` 读取 FP
   - 如果这些位置没有正确的值，栈帧遍历就会失败

### 为什么修复可以成功

1. **显式保存到标准位置**：
   - 修复在分配栈帧之前，显式将 LR 保存到 `[current_sp - 1]`
   - 显式将 FP 保存到 `[current_sp - 2]`
   - 这样当 `sender_sp = unextended_sp + frame_size` 时，标准位置就有正确的值

2. **符合 AArch64 ABI**：
   - 虽然 C2 是先分配栈空间后保存寄存器，但最终位置相同
   - 我们的修复是先保存寄存器后分配栈空间，最终位置也相同
   - 两种方式都符合 AArch64 ABI 的要求

3. **与 HotSpot 期望一致**：
   - `frame::sender_for_compiled_frame()` 现在可以正确读取返回地址和 FP
   - `CodeCache::find_blob()` 可以找到对应的 `CodeBlob`
   - 栈帧遍历可以正常进行，safepoint 操作可以完成

## 相关文件

- `hotspot/src/share/vm/yuhu/yuhuStack.cpp` - 栈帧初始化，LR 和 FP 保存（修复位置）
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` - `CreateReadLinkRegister()` 实现
- `hotspot/src/share/vm/yuhu/yuhuBuilder.hpp` - `CreateReadLinkRegister()` 声明
- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - 编译方法注册，`frame_size` 计算（方案 1）
- `hotspot/src/cpu/aarch64/vm/frame_aarch64.cpp` - 栈帧遍历，`frame::sender_for_compiled_frame()`
- `hotspot/src/cpu/aarch64/vm/macroAssembler_aarch64.cpp` - C2 的 `build_frame()` 实现（参考）
- `hotspot/src/share/vm/runtime/safepoint.cpp` - Safepoint 同步
- `hotspot/src/share/vm/code/sweeper.cpp` - NMethod 清理

## 参考

- 问题 1（寄存器覆盖）的修复：`019_stack_frame_size_calculation_bug.md`
  - 修复了 `YuhuStack::initialize()` 中的 `frame_size_bytes` 计算
  - 修复了 `yuhuCompiler.cpp` 中的 `frame_size` 计算
  - 但还需要修复栈帧布局，才能解决 safepoint 卡死问题


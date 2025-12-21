# 021: 参数传递简化设计

## 日期
2025-12-XX

## 实施范围

**重要**：本设计先处理普通编译（normal entry），OSR 编译（on-stack replacement）暂时保持不变，等普通编译跑通后再处理。

- ✅ **第一阶段**：普通编译（normal entry）
- ⏸️ **第二阶段**：OSR 编译（等普通编译跑通后）

## 背景

在解决 safepoint 卡死问题（020）的过程中，我们发现当前的设计存在根本性问题：

1. **多次数据拷贝**：Java 参数从寄存器 → 临时栈区域 → LLVM 局部变量区域
2. **适配器代码复杂**：需要分配临时区域、打包参数、设置多个寄存器
3. **容易出错**：栈帧布局、参数位置计算等容易出错
4. **不符合 AArch64 调用约定**：没有充分利用 HotSpot 的寄存器约定

## 当前设计的问题

### 当前参数传递流程

```
i2c 适配器调用：
  x0-x7 = Java 方法参数（从调用者传递）
  x12 = Method* (rmethod, HotSpot 默认)
  x28 = Thread* (rthread, HotSpot 默认)
  ↓
适配器代码（generate_normal_adapter_into）：
  1. 保存 x0-x7 到临时栈区域（arg_base）
  2. 从 x12 读取 Method* → x8 → x0
  3. 从 x28 读取 Thread* → x2
  4. 计算 base_pc → x1
  5. 设置 arg_base → x3
  6. 设置 arg_count → x4
  ↓
LLVM 函数：
  签名：(Method*, base_pc, thread*, arg_base, arg_count) -> int
  1. 从参数 x0 读取 Method*
  2. 从参数 x2 读取 Thread*
  3. 从参数 x3 读取 arg_base
  4. 从 arg_base 读取 Java 参数
  5. 存储到 LLVM 局部变量区域
```

### 问题分析

1. **多次数据拷贝**：
   - 寄存器 → 临时栈区域（适配器）
   - 临时栈区域 → LLVM 局部变量（YuhuNormalEntryCacher）
   - 效率低，容易出错

2. **适配器代码复杂**：
   - 需要分配临时栈区域
   - 需要打包参数
   - 需要处理栈参数（> 8 个参数的情况）
   - 代码量大，维护困难

3. **不符合 HotSpot 约定**：
   - x12 (rmethod) 和 x28 (rthread) 是 HotSpot 的全局寄存器
   - 当前设计通过参数传递，浪费了这些寄存器
   - 没有充分利用 AArch64 调用约定

4. **栈帧布局问题**：
   - 临时栈区域和 LLVM 栈帧的交互复杂
   - 容易导致栈帧布局不符合 HotSpot 标准
   - 这是 safepoint 卡死的根本原因之一

## 简化设计方案

### 核心思想

**让 LLVM 函数的参数直接对应 Java 方法的参数，无需中间转换**

### 设计原则

1. **直接对应**：Java 方法有几个参数，LLVM 函数就有几个参数
2. **寄存器读取**：Method* 和 Thread* 通过寄存器读取，不通过参数传递
3. **简化适配器**：适配器只负责跳转，不负责参数打包

### 新的设计

#### 1. LLVM 函数签名

**当前设计**：
```cpp
// 对于 multiply(int[][] a, int[][] b, int[][] c, int n)
(Method*, base_pc, thread*, arg_base, arg_count) -> int
```

**简化后**：
```cpp
// 对于 multiply(int[][] a, int[][] b, int[][] c, int n)
(int[][] a, int[][] b, int[][] c, int n) -> int
```

**优势**：
- 参数直接对应，无需转换
- 减少参数数量（从 5 个减少到 4 个）
- 更符合 AArch64 调用约定

#### 2. Method* 和 Thread* 的获取

**当前设计**：通过函数参数传递

**简化后**：通过寄存器读取

```cpp
// 在 YuhuBuilder 中添加
CallInst* CreateReadMethodRegister() {
  // 读取 x12 (rmethod) 寄存器
  // 使用 @llvm.read_register intrinsic
  // 返回 Method*
}

CallInst* CreateReadThreadRegister() {
  // 读取 x28 (rthread) 寄存器
  // 使用 @llvm.read_register intrinsic
  // 返回 Thread*
}
```

**优势**：
- 直接使用 HotSpot 的全局寄存器
- 无需通过参数传递
- 减少寄存器使用

#### 3. base_pc 的处理

**选项 A**：通过 `@llvm.read_register("pc")` 读取当前 PC
- 优点：无需参数传递
- 缺点：需要处理 PC 的偏移（可能需要减去某个常量）

**选项 B**：保留为参数（但可以简化）
- 优点：简单直接
- 缺点：仍然需要一个参数

**选项 C**：从 nmethod 获取（需要 runtime 调用）
- 优点：最准确
- 缺点：需要 runtime 调用，性能开销

**推荐**：选项 B（保留为参数，但简化传递方式）

#### 4. 适配器代码简化

**当前适配器代码**（~120 行）：
```cpp
// 1. 保存 caller's SP
// 2. 读取 Method* 和 ConstMethod*
// 3. 计算 arg_count
// 4. 分配临时栈区域
// 5. 保存 x0-x7 到临时区域
// 6. 复制栈参数（如果 arg_count > 8）
// 7. 设置 x0 = Method*, x1 = base_pc, x2 = thread*, x3 = arg_base, x4 = arg_count
// 8. 跳转到 LLVM 函数
```

**简化后的适配器代码**（~10 行）：
```cpp
// 1. 直接跳转到 LLVM 函数
//    - x0-x7 已经是 Java 参数（对于静态方法，x0 是 NULL，需要跳过）
//    - Method* 和 Thread* 通过寄存器读取（x12, x28）
//    - 栈参数（> 8 个）通过 x20 (esp) 读取
```

**优势**：
- 代码量减少 80%+
- 逻辑简单，不易出错
- 无需临时栈区域
- 无需参数打包

#### 5. 参数读取逻辑简化

**当前设计**（YuhuNormalEntryCacher）：
```cpp
// 从 arg_base 读取参数
Value* base_ptr = builder()->CreateIntToPtr(_arg_base, ...);
Value* elem_ptr = builder()->CreateGEP(base_ptr, arg_index, ...);
Value* loaded = builder()->CreateLoad(elem_ptr, ...);
```

**简化后**：
```cpp
// 直接从函数参数读取
Function::arg_iterator ai = function()->arg_begin();
// 对于静态方法，跳过第一个参数（NULL）
if (is_static()) {
  ai++;  // 跳过 x0 (NULL)
}
// 直接使用参数作为局部变量
for (int i = 0; i < arg_size(); i++) {
  llvm::Argument *arg = ai++;
  // 直接使用 arg 作为 local[i]
}
```

**优势**：
- 无需从内存读取
- 无需类型转换
- 直接使用寄存器参数

## 栈帧布局设计

### 目的

简化参数传递后，仍需确保栈帧布局符合 HotSpot 标准，以支持 safepoint 的栈帧遍历。关键是 LR 和 FP 必须保存在标准位置。

### HotSpot 标准 AArch64 编译帧布局

从 `frame_aarch64.hpp` 和 `frame::sender_for_compiled_frame()` 看，HotSpot 期望的标准布局是：

```cpp
// frame::sender_for_compiled_frame()
intptr_t* l_sender_sp = unextended_sp() + _cb->frame_size();
address sender_pc = (address) *(l_sender_sp - 1);  // 返回地址在 [sender_sp - 1]
intptr_t** saved_fp_addr = (intptr_t**) (l_sender_sp - sender_sp_offset);  // FP 在 [sender_sp - 2]
```

这意味着：
- 当 `sender_sp = unextended_sp + frame_size` 时：
  - 返回地址（LR）必须在 `[sender_sp - 1]`
  - 保存的 FP 必须在 `[sender_sp - 2]`

### Yuhu 栈帧布局（简化后）

**栈帧布局（从高地址到低地址）**：

```
[sender_sp] = unextended_sp + frame_size = current_sp (分配栈帧之前的 SP)
  [sender_sp - 1] = 返回地址（LR，从 x30 读取）✓ 标准位置
  [sender_sp - 2] = 保存的 FP（从 last_Java_fp 读取）✓ 标准位置
  [sender_sp - 3] 及以下 = Yuhu 栈帧内容（从 stack_pointer 开始）
    [expression stack]
    [monitors]
    [oop_tmp]
    [method]
    [unextended_sp 槽位] = current_sp 的值（存储调用者的原始 SP）⚠️ 注意：当前代码存储的是 stack_pointer，可能需要修复
    [pc]
    [frame_marker] (0xDEADBEEF)
    [frame_pointer_addr] (保存的 FP，用于帧指针链)
    [local variables]
[stack_pointer] = unextended_sp (分配栈帧后的 SP，当前栈帧的起始地址)
```

**关键说明**：

1. **`unextended_sp` 作为值**：
   - `unextended_sp` = `stack_pointer` = 分配栈帧后的 SP = 当前栈帧的起始地址
   - 这是当前栈帧的边界，用于计算 `sender_sp = unextended_sp + frame_size`

2. **`[unextended_sp]` 作为槽位**（根据 HotSpot 标准和 025 文档）：
   - 这是栈帧内部的一个存储位置（在 frame header 中）
   - **应该存储的值**：`current_sp`（调用者的原始 SP，分配栈帧之前的 SP）
   - **当前代码存储的值**：`stack_pointer`（当前栈帧的起始地址）⚠️ **这可能是一个 bug**
   - **用途**：在栈帧遍历时，可以通过读取这个槽位来获取调用者的原始 SP

3. **为什么需要存储调用者的原始 SP**：
   - 在栈帧遍历时（safepoint、GC 等），需要知道调用者的原始 SP
   - 用于 OopMaps 的正确性（OopMaps 基于调用者的原始 SP 计算）
   - 用于返回地址的计算和栈帧遍历

4. **当前实现的问题**：
   - 代码第 142 行：`builder()->CreateStore(stack_pointer, slot_addr(offset++));`
   - 存储的是 `stack_pointer`（当前栈帧的起始地址）
   - 但根据 HotSpot 标准和 025 文档，应该存储 `current_sp`（调用者的原始 SP）
   - **需要确认**：这是 Yuhu 的特殊设计，还是需要修复的 bug？

5. **与 025 文档的一致性**：
   - 025 文档明确说明：`[unextended_sp]` 保存调用者的原始 SP
   - 当前代码实现不一致，需要进一步调查和确认

### LR 和 FP 的保存时机和位置

**关键**：必须在分配栈帧之前保存 LR 和 FP 到标准位置。

**实现**（在 `YuhuStack::initialize()` 中）：

```cpp
// 1. 读取当前 SP（分配栈帧之前）
Value *current_sp = builder()->CreateReadStackPointer();

// 2. 计算 frame_size_bytes
int frame_size_bytes = extended_frame_size() * wordSize;
frame_size_bytes = align_size_up(frame_size_bytes, 16);  // 16 字节对齐

// 3. 保存 LR 到 [current_sp - 1]（标准位置）
Value *lr = builder()->CreateReadLinkRegister();  // 读取 x30
Value *return_addr_addr = builder()->CreateIntToPtr(
  builder()->CreateGEP(YuhuType::intptr_type(),
                       builder()->CreateIntToPtr(current_sp, ...),
                       LLVMValue::intptr_constant(-1)),
  PointerType::getUnqual(YuhuType::intptr_type()));
builder()->CreateStore(lr, return_addr_addr);  // 保存到 [current_sp - 1]

// 4. 保存 FP 到 [current_sp - 2]（标准位置）
Value *prev_fp = builder()->CreateValueOfStructEntry(
  thread(),
  JavaThread::last_Java_fp_offset(),
  YuhuType::intptr_type(),
  "prev_fp");
Value *saved_fp_addr = builder()->CreateIntToPtr(
  builder()->CreateGEP(YuhuType::intptr_type(),
                       builder()->CreateIntToPtr(current_sp, ...),
                       LLVMValue::intptr_constant(-2)),
  PointerType::getUnqual(YuhuType::intptr_type()));
builder()->CreateStore(prev_fp, saved_fp_addr);  // 保存到 [current_sp - 2]

// 5. 分配栈帧（sub sp, sp, frame_size_bytes）
Value *stack_pointer = builder()->CreateSub(
  current_sp,
  LLVMValue::intptr_constant(frame_size_bytes),
  "new_sp");
```

**为什么这样保存是正确的**：

1. **保存时机**：在分配栈帧之前保存，确保 `[current_sp - 1]` 和 `[current_sp - 2]` 的位置正确
2. **位置计算**：
   - `sender_sp = unextended_sp + frame_size = stack_pointer + frame_size = current_sp`
   - 因此 `[sender_sp - 1] = [current_sp - 1]` ✓
   - 因此 `[sender_sp - 2] = [current_sp - 2]` ✓
3. **栈帧遍历**：
   - `frame::sender_for_compiled_frame()` 读取 `[sender_sp - 1]` 得到正确的返回地址
   - `frame::sender_for_compiled_frame()` 读取 `[sender_sp - 2]` 得到正确的 FP
   - `CodeCache::find_blob(sender_pc)` 可以找到对应的 CodeBlob
   - 栈帧遍历可以正常进行，safepoint 操作可以完成

### 第1个栈帧（当前栈帧）的获取

**如何获取第1个栈帧（最顶层的栈帧，当前正在执行的栈帧）的 sp、fp 和 lr**：

HotSpot 使用 `JavaThread` 中的 `last_Java_sp`、`last_Java_fp` 和 `last_Java_pc` 来保存当前栈帧的信息。

**`JavaThread::pd_last_frame()` 的实现**（`thread_bsd_aarch64.cpp:30-34`）：

```cpp
frame JavaThread::pd_last_frame() {
  assert(has_last_Java_frame(), "must have last_Java_sp() when suspended");
  assert(_anchor.last_Java_pc() != NULL, "not walkable");
  return frame(_anchor.last_Java_sp(), _anchor.last_Java_fp(), _anchor.last_Java_pc());
}
```

**关键点**：

1. **`last_Java_sp`**：当前栈帧的 SP（栈指针）
   - 在进入 Java 代码时，通过 `set_last_Java_frame()` 设置
   - 在离开 Java 代码时，通过 `reset_last_Java_frame()` 清除

2. **`last_Java_fp`**：当前栈帧的 FP（帧指针）
   - 在进入 Java 代码时，通过 `set_last_Java_frame()` 设置
   - 在离开 Java 代码时，通过 `reset_last_Java_frame()` 清除（可选）

3. **`last_Java_pc`**：当前栈帧的 PC（程序计数器，也就是 LR）
   - 在进入 Java 代码时，通过 `set_last_Java_frame()` 设置
   - 在离开 Java 代码时，通过 `reset_last_Java_frame()` 清除

**`set_last_Java_frame()` 的实现**（`macroAssembler_aarch64.cpp:305-330`）：

```cpp
void MacroAssembler::set_last_Java_frame(Register last_java_sp,
                                         Register last_java_fp,
                                         Register last_java_pc,
                                         Register scratch) {
  // 保存 last_Java_pc
  if (last_java_pc->is_valid()) {
    str(last_java_pc, Address(rthread,
                              JavaThread::frame_anchor_offset()
                              + JavaFrameAnchor::last_Java_pc_offset()));
  }
  
  // 保存 last_Java_sp
  if (last_java_sp == sp) {
    mov(scratch, sp);
    last_java_sp = scratch;
  } else if (!last_java_sp->is_valid()) {
    last_java_sp = esp;  // 使用 x20 (esp) 作为默认值
  }
  str(last_java_sp, Address(rthread, JavaThread::last_Java_sp_offset()));
  
  // 保存 last_Java_fp（可选）
  if (last_java_fp->is_valid()) {
    str(last_java_fp, Address(rthread, JavaThread::last_Java_fp_offset()));
  }
}
```

**栈帧遍历的完整流程**：

```
1. 获取第1个栈帧（当前栈帧）：
   frame current = thread->pd_last_frame();
   // 从 JavaThread::_anchor 获取：
   //   sp = _anchor.last_Java_sp()
   //   fp = _anchor.last_Java_fp()
   //   pc = _anchor.last_Java_pc()
   
2. 遍历栈帧（从当前栈帧向上遍历到调用者）：
   frame sender = current.sender();
   // 调用 frame::sender_for_compiled_frame()
   //   从当前栈帧计算调用者的 sp、fp 和 lr
   
3. 继续遍历：
   frame sender2 = sender.sender();
   // 重复步骤 2，直到遍历完所有栈帧
```

### 栈帧遍历过程详解：如何从当前栈帧获取上一层的 sp、fp 和 lr

**`frame::sender_for_compiled_frame()` 的实现**（`frame_aarch64.cpp:502-572`）：

```cpp
frame frame::sender_for_compiled_frame(RegisterMap* map) const {
  // 1. 获取当前栈帧的 unextended_sp（当前栈帧的起始地址）
  // unextended_sp() = stack_pointer（分配栈帧后的 SP）
  intptr_t* current_unextended_sp = unextended_sp();
  
  // 2. 计算调用者的 SP（sender_sp）
  // sender_sp = 当前栈帧的起始地址 + 当前栈帧的大小
  intptr_t* l_sender_sp = current_unextended_sp + _cb->frame_size();
  // 关键：l_sender_sp = current_sp（分配栈帧之前的 SP）
  
  // 3. 从 [l_sender_sp - 1] 读取返回地址（LR）
  address sender_pc = (address) *(l_sender_sp - 1);
  // 这就是我们保存在 [current_sp - 1] 的 LR
  
  // 4. 从 [l_sender_sp - sender_sp_offset] 读取保存的 FP
  // sender_sp_offset = 2（相对于 sender_sp）
  intptr_t** saved_fp_addr = (intptr_t**) (l_sender_sp - frame::sender_sp_offset);
  intptr_t* saved_fp = *saved_fp_addr;
  // 这就是我们保存在 [current_sp - 2] 的 FP
  
  // 5. 对于编译帧，sender_unextended_sp = sender_sp
  intptr_t* sender_unextended_sp = l_sender_sp;
  
  // 6. 构造调用者的 frame
  return frame(l_sender_sp, sender_unextended_sp, saved_fp, sender_pc);
}
```

**关键点说明**：

1. **`sender_sp` 的计算**（这是关键！）：
   - `sender_sp = unextended_sp() + frame_size()`
   - `unextended_sp()` 是当前栈帧的起始地址（`stack_pointer`，分配栈帧后的 SP）
   - `frame_size()` 是当前栈帧的大小（以 words 为单位）
   - **结果**：`sender_sp = current_sp`（分配栈帧之前的 SP，也就是调用者的 SP）

**为什么 `unextended_sp + frame_size` 等于调用者的 SP？**

这是栈帧布局的关键理解点。让我用图示说明：

**栈帧分配过程**：

```
步骤 1：分配栈帧之前
高地址
  [调用者的栈帧内容]
  [调用者的栈帧边界] <- current_sp（调用者的 SP，也是分配前的 SP）
低地址
```

```
步骤 2：保存 LR 和 FP 到标准位置（在分配栈帧之前）
高地址
  [调用者的栈帧内容]
  [调用者的栈帧边界] <- current_sp
    [current_sp - 1] = LR（返回地址）✓ 保存到这里
    [current_sp - 2] = FP（保存的 FP）✓ 保存到这里
低地址
```

```
步骤 3：分配栈帧（sub sp, sp, frame_size_bytes）
高地址
  [调用者的栈帧内容]
  [调用者的栈帧边界] <- current_sp（调用者的 SP）
    [current_sp - 1] = LR（返回地址）✓
    [current_sp - 2] = FP（保存的 FP）✓
    [current_sp - 3] 及以下 = Yuhu 栈帧内容（从 stack_pointer 开始）
      [expression stack]
      [monitors]
      [oop_tmp]
      [method]
      [unextended_sp 槽位]
      [pc]
      [frame_marker]
      [frame_pointer_addr]
      [local variables]
  [stack_pointer] = unextended_sp（当前栈帧的起始地址，分配栈帧后的 SP）
低地址
```

**关键理解**：

1. **`current_sp`**（分配栈帧之前的 SP）：
   - 这是调用者的 SP
   - 在分配栈帧之前，SP 指向这里
   - 我们在这里保存了 LR 和 FP（`[current_sp - 1]` 和 `[current_sp - 2]`）

2. **`stack_pointer`**（分配栈帧之后的 SP）：
   - 这是当前栈帧的起始地址
   - `stack_pointer = current_sp - frame_size_bytes`
   - 也就是 `unextended_sp()`

3. **`sender_sp` 的计算**：
   - `sender_sp = unextended_sp() + frame_size()`
   - `= stack_pointer + frame_size()`
   - `= (current_sp - frame_size_bytes) + frame_size()`
   - `= current_sp` ✓

**所以**：
- `unextended_sp + frame_size` 确实等于 `current_sp`（调用者的 SP）
- 虽然 `unextended_sp` 到 `unextended_sp + frame_size` 之间的区域是 Yuhu 栈帧的内容
- 但是 `unextended_sp + frame_size` 这个地址本身，就是调用者的 SP（分配栈帧之前的 SP）
- 在这个地址的 `-1` 和 `-2` 位置，我们保存了 LR 和 FP，用于栈帧遍历

**栈帧边界的概念**：

```
调用者的栈帧：
  [调用者的栈帧内容]
  [调用者的栈帧边界] <- sender_sp = current_sp（调用者的 SP）
  
当前栈帧（Yuhu）：
  [LR] <- [sender_sp - 1] = [current_sp - 1]
  [FP] <- [sender_sp - 2] = [current_sp - 2]
  [Yuhu 栈帧内容] <- 从 unextended_sp 开始
  [当前栈帧边界] <- unextended_sp = stack_pointer（当前栈帧的起始地址）
```

**总结**：
- `unextended_sp` 是当前栈帧的起始地址（栈帧内部）
- `unextended_sp + frame_size` 是当前栈帧的结束地址（栈帧外部）
- 这个结束地址就是调用者的 SP（`current_sp`）
- 在调用者的 SP 的 `-1` 和 `-2` 位置，我们保存了 LR 和 FP

2. **`sender_pc`（返回地址/LR）的获取**：
   - 从 `[sender_sp - 1]` 读取
   - 这就是我们保存在 `[current_sp - 1]` 的 LR（返回地址）
   - 用于 `CodeCache::find_blob(sender_pc)` 查找调用者的 CodeBlob
   - 如果 `sender_pc` 无效，`CodeCache::find_blob()` 返回 `NULL`，导致栈帧遍历失败

3. **`saved_fp`（保存的 FP）的获取**：
   - 从 `[sender_sp - sender_sp_offset]` 读取，其中 `sender_sp_offset = 2`
   - 也就是从 `[sender_sp - 2]` 读取
   - 这就是我们保存在 `[current_sp - 2]` 的 FP（调用者的 FP）
   - 用于构造调用者的 frame

4. **`sender_unextended_sp` 的计算**：
   - 对于编译帧，`sender_unextended_sp = sender_sp`
   - 这是调用者栈帧的 `unextended_sp`

**为什么必须在分配栈帧之前保存 LR 和 FP**：

- **如果我们在分配栈帧之后保存**：
  - `[current_sp - 1]` 和 `[current_sp - 2]` 会被覆盖（在栈帧内部）
  - 当 `sender_sp = unextended_sp + frame_size = current_sp` 时，读取的是错误的值（可能是栈帧内部的数据）
  - 导致 `sender_pc` 和 `saved_fp` 错误，栈帧遍历失败

- **如果我们在分配栈帧之前保存**：
  - `[current_sp - 1]` 和 `[current_sp - 2]` 在栈帧外部（不会被覆盖）
  - 当 `sender_sp = current_sp` 时，可以正确读取 LR 和 FP
  - 栈帧遍历可以正常进行

### 关键问题 1：适配器不能分配栈帧

**问题**：如果适配器分配了栈帧（即使是临时栈区域），`current_sp` 就不是调用者的 SP 了。

**当前代码的问题**（`yuhuCompiler.cpp:416`）：
```cpp
// 适配器分配了临时栈区域用于参数打包
masm.write_inst("sub sp, sp, x13");  // 分配 buffer_size_bytes
```

**问题分析**：
- 如果适配器执行了 `sub sp, sp, x13`，那么：
  - 适配器进入时的 SP = 调用者的 SP（`caller_sp`）
  - 适配器分配临时区域后的 SP = `caller_sp - buffer_size_bytes`
  - LLVM 函数读取的 `current_sp` = `caller_sp - buffer_size_bytes`（错误！）
  - 保存 LR 和 FP 到 `[current_sp - 1]` 和 `[current_sp - 2]`（错误的位置！）
  - `sender_sp = unextended_sp + frame_size = (caller_sp - buffer_size_bytes - frame_size) + frame_size = caller_sp - buffer_size_bytes`（错误！）

**解决方案**：
- 在简化设计中，适配器**不能分配任何栈帧**
- 适配器只负责跳转，不负责参数打包
- LLVM 函数直接接收 Java 参数（x0-x7），或从 x20 (esp) 读取栈参数

### 关键问题 2：frame_size_bytes 必须包括保存的 LR 和 FP

**问题**：如果先保存 LR 和 FP 到 `[current_sp - 1]` 和 `[current_sp - 2]`，然后再分配栈帧，那么 LR 和 FP 会被保存在 expression stack 区域内，函数执行期间可能会被覆盖。

**C2 的正确做法**（参考 `macroAssembler_aarch64.cpp:4054`）：

```cpp
void MacroAssembler::build_frame(int framesize) {
  if (framesize < ((1 << 9) + 2 * wordSize)) {
    sub(sp, sp, framesize);  // 1. 先分配栈空间（framesize 包括 LR 和 FP 的 2 个 word）
    stp(rfp, lr, Address(sp, framesize - 2 * wordSize));  // 2. 然后保存到栈帧顶部
  }
}
```

**关键点**：
1. **`framesize` 包括 LR 和 FP 的 2 个 word**
2. **先分配栈空间**，然后保存 LR 和 FP 到栈帧顶部
3. **LR 和 FP 保存在栈帧顶部（高地址）**，不会被 expression stack 覆盖

**Yuhu 当前代码的问题**：

```cpp
// 1. 先保存 LR 和 FP 到 [current_sp - 1] 和 [current_sp - 2]（错误！）
// 2. 然后分配栈帧：stack_pointer = current_sp - frame_size_bytes
// 问题：[current_sp - 1] 和 [current_sp - 2] 会在 expression stack 区域内，会被覆盖！
```

**正确的做法**：

```cpp
// 1. frame_size_bytes 必须包括 LR 和 FP 的 2 个 word
int frame_size_bytes = (extended_frame_size() + 2) * wordSize;  // +2 for LR and FP
frame_size_bytes = align_size_up(frame_size_bytes, 16);

// 2. 先分配栈帧（包括 LR 和 FP 的空间）
Value *stack_pointer = builder()->CreateSub(
  current_sp,
  LLVMValue::intptr_constant(frame_size_bytes),
  "new_sp");

// 3. 然后保存 LR 和 FP 到栈帧顶部
// [stack_pointer + frame_size_bytes - 2 * wordSize] = [current_sp - 2 * wordSize]
Value *lr_save_addr = builder()->CreateIntToPtr(
  builder()->CreateGEP(YuhuType::intptr_type(),
                       builder()->CreateIntToPtr(stack_pointer, PointerType::getUnqual(YuhuType::intptr_type())),
                       LLVMValue::intptr_constant(frame_size_bytes / wordSize - 2)),
  PointerType::getUnqual(YuhuType::intptr_type()));
builder()->CreateStore(lr, lr_save_addr);

Value *fp_save_addr = builder()->CreateIntToPtr(
  builder()->CreateGEP(YuhuType::intptr_type(),
                       builder()->CreateIntToPtr(stack_pointer, PointerType::getUnqual(YuhuType::intptr_type())),
                       LLVMValue::intptr_constant(frame_size_bytes / wordSize - 1)),
  PointerType::getUnqual(YuhuType::intptr_type()));
builder()->CreateStore(prev_fp, fp_save_addr);
```

**栈帧布局（正确）**：

```
高地址
  [调用者的栈帧内容]
  [调用者的栈帧边界] <- current_sp（调用者的 SP，也是 sender_sp）
    [current_sp - 1] = FP（保存的 FP）✓ 在栈帧顶部，不会被覆盖
    [current_sp - 2] = LR（返回地址）✓ 在栈帧顶部，不会被覆盖
    [current_sp - 3] 及以下 = Yuhu 栈帧内容（从 stack_pointer 开始）
      [expression stack]  ← 不会覆盖 LR 和 FP
      [monitors]
      [oop_tmp]
      [method]
      [unextended_sp 槽位]
      [pc]
      [frame_marker]
      [frame_pointer_addr]
      [local variables]
  [stack_pointer] = unextended_sp（当前栈帧的起始地址）
低地址
```

**关键点**：
- **`frame_size_bytes` 必须包括 LR 和 FP 的 2 个 word**
- **先分配栈帧，然后保存 LR 和 FP 到栈帧顶部**
- **LR 和 FP 保存在栈帧顶部（高地址），不会被 expression stack 覆盖**
- **`sender_sp = unextended_sp + frame_size = stack_pointer + frame_size = current_sp`** ✓
- **`[sender_sp - 1] = [current_sp - 1]`（FP）** ✓
- **`[sender_sp - 2] = [current_sp - 2]`（LR）** ✓

**栈帧遍历的完整流程**：

```
当前栈帧（Yuhu 编译的方法）：
  unextended_sp() = stack_pointer（当前栈帧的起始地址）
  frame_size() = 当前栈帧的大小（words）
  ↓
计算调用者的 SP：
  sender_sp = unextended_sp() + frame_size() = current_sp
  ↓
从栈上读取调用者的信息：
  sender_pc = [sender_sp - 1] = [current_sp - 1]（我们保存的 LR）✓
  saved_fp = [sender_sp - 2] = [current_sp - 2]（我们保存的 FP）✓
  ↓
查找调用者的 CodeBlob：
  CodeBlob* sender_blob = CodeCache::find_blob(sender_pc)
  ↓
构造调用者的 frame：
  frame sender(sender_sp, sender_unextended_sp, saved_fp, sender_pc)
  ↓
继续遍历下一个栈帧...
```

### 与 C2 编译器的对比

**C2 编译器的做法**（从 `macroAssembler_aarch64.cpp:build_frame()` 看）：

```cpp
void MacroAssembler::build_frame(int framesize) {
  if (framesize < ((1 << 9) + 2 * wordSize)) {
    sub(sp, sp, framesize);                              // 1. 先分配栈空间
    stp(rfp, lr, Address(sp, framesize - 2 * wordSize)); // 2. 保存 FP 和 LR
  }
}
```

**区别**：
- C2：先分配栈空间，后保存寄存器（保存到 `[sp + framesize - 2*wordSize]` 和 `[sp + framesize - wordSize]`）
- Yuhu（设计意图）：先分配栈空间，后保存寄存器（保存到 `[stack_pointer + framesize - 2*wordSize]` 和 `[stack_pointer + framesize - wordSize]`）

**实际实现问题**：
- LLVM 自动生成函数序言，已经分配了栈帧（如 `stp x28, x27, [sp, #-96]!`）
- Yuhu 代码在 LLVM 序言之后读取 `current_sp`，此时 SP 已经被 LLVM 修改
- Yuhu 代码又手动分配了一次栈帧，导致 `stp x30, x9, [sp, #-16]!` 又分配了 16 字节
- **问题**：Yuhu 代码应该保存 LR 和 FP 到 LLVM 已分配栈帧的顶部，而不是再次分配

**需要修复**：
- 方案 1：在 LLVM 序言之前读取调用者的 SP，然后保存 LR 和 FP 到 `[caller_sp - 2*wordSize]` 和 `[caller_sp - wordSize]`
- 方案 2：禁用 LLVM 的自动序言，手动生成完整的序言（包括 LR 和 FP 的保存）
- 方案 3：让 LLVM 负责栈帧分配和 LR/FP 保存，Yuhu 只负责 Yuhu 特定的栈帧内容

**当前状态**：
- 代码逻辑是先分配栈帧，后保存 LR 和 FP（符合 C2 的做法）
- 但由于 LLVM 已经自动分配了栈帧，实际执行时又额外分配了一次
- 需要修复以匹配 C2 的行为

### 简化设计中的栈帧布局优势

1. **减少临时区域**：
   - 不再需要 `arg_base` 临时栈区域
   - 栈帧更简单，更容易符合标准

2. **标准位置保存**：
   - LR 和 FP 保存在标准位置（`[sender_sp - 1]` 和 `[sender_sp - 2]`）
   - 与 C2 编译器兼容

3. **正确的栈帧遍历**：
   - `frame::sender_for_compiled_frame()` 可以正确读取返回地址和 FP
   - `CodeCache::find_blob()` 可以找到对应的 CodeBlob
   - safepoint 操作可以正常完成

## 实现细节

### 1. 函数签名生成

**当前实现**（yuhuContext.cpp）：
```cpp
std::vector<llvm::Type*> params;
params.push_back(Method_type());        // x0
params.push_back(intptr_type());        // x1 (base_pc)
params.push_back(thread_type());        // x2
params.push_back(intptr_type());        // x3 (arg_base)
params.push_back(jint_type());          // x4 (arg_count)
_entry_point_type = FunctionType::get(jint_type(), params, false);
```

**简化后**：
```cpp
std::vector<llvm::Type*> params;
// 根据 Java 方法的参数类型生成对应的 LLVM 类型
for (int i = 0; i < method->arg_size(); i++) {
  BasicType arg_type = method->arg_type(i);
  params.push_back(YuhuType::to_stackType(arg_type));
}
// 可选：如果需要 base_pc
// params.push_back(intptr_type());  // base_pc
_entry_point_type = FunctionType::get(jint_type(), params, false);
```

### 2. 适配器代码简化

**当前实现**（generate_normal_adapter_into）：
- 120+ 行代码
- 分配临时栈区域
- 打包参数
- 设置多个寄存器

**简化后**：
```cpp
static int generate_normal_adapter_into(CodeBuffer& cb,
                                       Method* method,
                                       address base_pc,  // 保留参数，但不使用
                                       YuhuLabel* llvm_label,
                                       address llvm_entry) {
  YuhuMacroAssembler masm(&cb);
  address start = masm.current_pc();

  // 直接跳转，无需任何参数设置
  // x0-x7 已经是 Java 参数（对于静态方法，x0 是 NULL）
  // Method* 和 Thread* 通过寄存器读取（x12, x28）
  // 栈参数（> 8 个）通过 x20 (esp) 读取
  if (llvm_label != NULL) {
    masm.write_inst_b(*llvm_label);
  } else {
    masm.write_inst_b(llvm_entry);
  }

  address end = masm.current_pc();
  return (int)(end - start);
}
```

### 3. 参数读取逻辑

**当前实现**（YuhuNormalEntryCacher::process_local_slot）：
```cpp
// 从 arg_base 读取
int arg_index = index;
if (is_static()) {
  arg_index = index + 1;  // 跳过 NULL
}
Value* base_ptr = builder()->CreateIntToPtr(_arg_base, ...);
Value* elem_ptr = builder()->CreateGEP(base_ptr, arg_index, ...);
Value* loaded = builder()->CreateLoad(elem_ptr, ...);
```

**简化后**（YuhuFunction::initialize）：
```cpp
// 直接从函数参数读取
Function::arg_iterator ai = function()->arg_begin();

// 对于静态方法，第一个参数是 NULL（x0），需要跳过
if (is_static()) {
  llvm::Argument *null_arg = ai++;  // 跳过 NULL
  null_arg->setName("null_arg");
}

// 读取 Java 方法参数
for (int i = 0; i < arg_size(); i++) {
  llvm::Argument *arg = ai++;
  arg->setName("arg" + std::to_string(i));
  // 直接使用 arg 作为 local[i]
  // 无需从内存读取
}
```

### 4. Method* 和 Thread* 的获取

**在 YuhuFunction::initialize 中**：
```cpp
// 不再从函数参数读取
// llvm::Argument *method = ai++;
// llvm::Argument *thread = ai++;

// 改为通过寄存器读取
Value *method = builder()->CreateReadMethodRegister();
Value *thread = builder()->CreateReadThreadRegister();
set_thread(thread);
```

## 优势分析

### 1. 简化代码

- **适配器代码**：从 120+ 行减少到 10-20 行（减少 80%+）
- **参数读取逻辑**：从内存读取改为直接使用参数（减少 50%+）
- **总体代码量**：减少 60%+

### 2. 提高性能

- **减少内存访问**：无需从临时栈区域读取参数
- **减少数据拷贝**：参数直接在寄存器中，无需拷贝
- **减少栈分配**：无需分配临时栈区域

### 3. 降低错误率

- **减少中间步骤**：参数直接对应，无需转换
- **减少计算**：无需计算 arg_index、arg_base 等
- **减少栈操作**：无需管理临时栈区域

### 4. 符合约定

- **AArch64 调用约定**：直接使用寄存器参数
- **HotSpot 寄存器约定**：使用 x12 (rmethod) 和 x28 (rthread)
- **标准栈帧布局**：减少临时区域，栈帧更标准

## 需要考虑的问题

### 1. 静态方法的第一个参数

**问题**：在 AArch64 上，静态方法的 x0 是 NULL（不是 Java 参数）

**解决方案**：
- 选项 A：在函数签名中保留 NULL 参数
  ```cpp
  // 对于静态方法 multiply(int[][] a, int[][] b, int[][] c, int n)
  (void* null, int[][] a, int[][] b, int[][] c, int n) -> int
  ```
- 选项 B：在适配器中跳过 x0
  ```cpp
  // 适配器：将 x1-x7 移动到 x0-x6
  // 但这样会覆盖参数，不推荐
  ```
- 选项 C：在 LLVM 函数中跳过第一个参数
  ```cpp
  // 函数签名：(int[][] a, int[][] b, int[][] c, int n) -> int
  // 适配器：不做任何处理，x0 保持为 NULL
  // LLVM 函数：跳过第一个参数（NULL），从第二个参数开始对应 Java 参数
  ```

**推荐**：选项 C（在 LLVM 函数中处理）

### 2. 参数超过 8 个的情况

**决定**：**参数超过 8 个时，直接从 x20 (esp) 寄存器读取**

**分析**：
- AArch64 调用约定：前 8 个参数在寄存器（x0-x7），超过 8 个的在栈上
- x20 (esp) 是 HotSpot 保留的 Java 栈指针寄存器（`REGISTER_DECLARATION(Register, esp, r20)`）
- 在解释器中，x20 用于 Java 表达式栈
- 对于编译代码，x20 仍然指向 Java 栈帧，栈参数可以从 x20 读取

**实施**：
- 在 LLVM 函数中，对于参数索引 >= 8 的参数：
  ```cpp
  // 读取 x20 (esp) 寄存器
  Value *esp = builder()->CreateReadRegister("x20");
  
  // 计算栈参数地址：esp + (arg_index - 8) * 8
  // 注意：栈参数在调用者的栈帧中，可能需要考虑返回地址和 FP
  // 对于 i2c adapter 调用，栈参数在 [esp + (arg_index - 8) * 8]
  Value *stack_arg_addr = builder()->CreateGEP(
    builder()->CreateIntToPtr(esp, PointerType::getUnqual(YuhuType::intptr_type())),
    LLVMValue::intptr_constant((arg_index - 8) * wordSize));
  
  // 从栈上加载参数
  Value *stack_arg = builder()->CreateLoad(stack_arg_addr);
  ```

**注意事项**：
- x20 (esp) 是 HotSpot 的全局寄存器，在编译代码中仍然有效
- 栈参数的位置可能需要根据调用约定调整（考虑返回地址、FP 等）
- 需要测试验证栈参数的读取是否正确

### 3. base_pc 的处理

**决定**：**base_pc 不需要，直接不处理**

**分析**：
- base_pc 原本用于计算 PC 偏移（用于异常处理、调试等）
- 但在简化设计中，如果不需要 base_pc，可以直接省略
- 如果将来需要，可以通过 `@llvm.read_register("pc")` 读取当前 PC，或从 nmethod 获取

**实施**：
- 函数签名中不包含 base_pc 参数
- 适配器代码中不计算或传递 base_pc
- 如果将来需要，再通过其他方式获取

### 4. 向后兼容性

**问题**：是否需要支持旧的函数签名？

**分析**：
- 这是内部实现，不涉及外部 API
- 可以一次性迁移，无需兼容

**推荐**：一次性迁移，无需兼容旧版本

## 实施策略

### 分阶段实施：先普通编译，后 OSR 编译

**设计决策**：先处理普通编译（normal entry），暂时不处理 OSR 编译，等普通编译跑通了再处理 OSR。

**原因**：
1. **降低复杂度**：普通编译和 OSR 编译的参数传递和栈帧布局有差异，分开处理可以降低复杂度
2. **快速验证**：先让普通编译工作，可以快速验证简化设计的正确性
3. **减少风险**：如果同时修改普通编译和 OSR 编译，出现问题难以定位
4. **迭代改进**：在普通编译上验证后，可以将经验应用到 OSR 编译

**实施范围**：
- **第一阶段**：只修改普通编译（normal entry）
  - `generate_normal_adapter_into()` - 简化适配器代码
  - `YuhuNormalEntryCacher` - 修改参数读取逻辑
  - `YuhuFunction::initialize()` - 修改参数读取逻辑（仅普通编译路径）
- **第二阶段**：等普通编译跑通后，再处理 OSR 编译
  - `generate_osr_adapter_into()` - 简化适配器代码
  - `YuhuOSREntryCacher` - 修改参数读取逻辑
  - OSR 特有的栈帧布局处理

**注意事项**：
- OSR 编译的适配器和参数传递逻辑暂时保持不变
- 确保普通编译和 OSR 编译可以共存（使用条件编译或运行时判断）
- 在修改普通编译时，不要破坏 OSR 编译的现有功能

## 实施计划

### 阶段 1：添加寄存器读取函数

1. 在 `YuhuBuilder` 中添加 `CreateReadMethodRegister()`（读取 x12）
2. 在 `YuhuBuilder` 中添加 `CreateReadThreadRegister()`（读取 x28）
3. 在 `YuhuBuilder` 中添加 `CreateReadRegister(const char* reg_name)`（通用版本，用于读取 x20）
4. 测试寄存器读取功能

### 阶段 2：修改函数签名生成

1. 修改 `YuhuContext::entry_point_type()` 生成逻辑
2. 根据 Java 方法参数生成对应的 LLVM 函数签名
3. 处理静态方法的 NULL 参数

### 阶段 3：简化适配器代码（仅普通编译）

1. 简化 `generate_normal_adapter_into()`（普通编译适配器）
2. **关键**：移除所有栈帧分配（包括临时栈区域）
   - 当前代码中，适配器分配了临时栈区域（`sub sp, sp, x13`）用于参数打包
   - 在简化设计中，适配器**不能分配任何栈帧**，必须直接跳转到 LLVM 函数
   - 原因：如果适配器分配了栈帧，`current_sp` 就不是调用者的 SP 了，会导致栈帧遍历错误
3. 移除参数打包逻辑（不再需要临时栈区域）
4. **注意**：暂时不修改 `generate_osr_adapter_into()`（OSR 编译适配器）
5. 测试普通编译适配器功能

### 阶段 4：修改参数读取逻辑（仅普通编译）

1. 修改 `YuhuFunction::initialize()` 参数读取逻辑（仅普通编译路径）
2. 修改 `YuhuNormalEntryCacher::process_local_slot()` 逻辑
3. 直接使用函数参数作为局部变量（对于前 8 个参数）
4. 对于参数索引 >= 8 的参数，从 x20 (esp) 读取栈参数
5. **注意**：暂时不修改 `YuhuOSREntryCacher`（OSR 编译参数读取逻辑）

### 阶段 5：测试和验证（仅普通编译）

1. 测试基本功能（参数传递正确，仅普通编译）
2. 测试静态方法（NULL 参数处理，仅普通编译）
3. 测试多参数方法（> 8 个参数，从 x20 读取栈参数，仅普通编译）
4. 测试 safepoint（栈帧遍历，仅普通编译）
5. 性能测试（减少数据拷贝的效果，仅普通编译）
6. 验证 x20 (esp) 读取栈参数的正确性（仅普通编译）
7. **确认**：OSR 编译仍然使用旧逻辑，功能不受影响

### 阶段 6：处理 OSR 编译（等普通编译跑通后）

1. 简化 `generate_osr_adapter_into()`（OSR 编译适配器）
2. 修改 `YuhuOSREntryCacher::process_local_slot()` 逻辑
3. 处理 OSR 特有的栈帧布局（monitor 等）
4. 测试 OSR 编译功能
5. 测试 safepoint（包含 OSR 编译的栈帧遍历）

## 预期效果

### 1. 代码简化

- 适配器代码减少 80%+
- 参数读取逻辑减少 50%+
- 总体代码量减少 60%+

### 2. 性能提升

- 减少内存访问（无需从临时栈区域读取）
- 减少数据拷贝（参数直接在寄存器中）
- 减少栈分配（无需临时栈区域）

### 3. 错误减少

- 减少中间步骤（参数直接对应）
- 减少计算（无需 arg_index、arg_base 等）
- 减少栈操作（无需管理临时栈区域）

### 4. 栈帧布局改善

- 减少临时区域，栈帧更标准
- 符合 HotSpot 标准 AArch64 布局
- 解决 safepoint 卡死问题

## 与 020 文档的关系

这个简化设计是 **020_safepoint_hang.md 中"问题 2: Yuhu 栈帧不符合 HotSpot 标准 AArch64 布局"的终极解决方案**。

### 020 文档中的方案 2

020 文档中的方案 2 是通过显式保存 LR 和 FP 到标准位置来修复栈帧布局问题。这是一个**临时修复**，解决了症状，但没有解决根本原因。

### 021 文档的简化设计

021 文档的简化设计是**根本性重构**，通过简化参数传递流程，从根本上解决栈帧布局问题：

1. **减少临时栈区域**：不再需要 arg_base，减少栈帧复杂度
2. **简化适配器代码**：减少栈操作，降低出错概率
3. **直接使用寄存器**：符合 AArch64 调用约定
4. **标准栈帧布局**：更容易符合 HotSpot 标准

### 实施顺序

1. **先实施 020 的方案 2**（临时修复）：快速解决 safepoint 卡死问题
2. **再实施 021 的简化设计**（根本重构）：从根本上解决问题，简化代码

## 相关文件

### 需要修改的文件

1. **`hotspot/src/share/vm/yuhu/yuhuContext.cpp`**
   - 修改 `entry_point_type()` 生成逻辑
   - 根据 Java 方法参数生成 LLVM 函数签名

2. **`hotspot/src/share/vm/yuhu/yuhuBuilder.cpp`**
   - 添加 `CreateReadMethodRegister()` 实现（读取 x12）
   - 添加 `CreateReadThreadRegister()` 实现（读取 x28）
   - 添加 `CreateReadRegister(const char* reg_name)` 实现（通用版本，用于读取 x20）

3. **`hotspot/src/share/vm/yuhu/yuhuBuilder.hpp`**
   - 声明 `CreateReadMethodRegister()`
   - 声明 `CreateReadThreadRegister()`
   - 声明 `CreateReadRegister(const char* reg_name)`

4. **`hotspot/src/share/vm/yuhu/yuhuFunction.cpp`**
   - 修改 `initialize()` 参数读取逻辑
   - 使用寄存器读取 Method* 和 Thread*

5. **`hotspot/src/share/vm/yuhu/yuhuStack.cpp`**
   - **关键**：确保 LR 和 FP 保存在标准位置（`[current_sp - 1]` 和 `[current_sp - 2]`）
   - 确认保存时机正确（在分配栈帧之前）
   - 这是 safepoint 栈帧遍历的关键

6. **`hotspot/src/share/vm/yuhu/yuhuCompiler.cpp`**
   - 简化 `generate_normal_adapter_into()`（普通编译适配器）
   - 移除临时栈区域分配和参数打包
   - 确认传递给 `nmethod::new_nmethod` 的 `frame_size` 正确（`frame_words + locals_words`）
   - **注意**：暂时不修改 `generate_osr_adapter_into()`（OSR 编译适配器）

7. **`hotspot/src/share/vm/yuhu/yuhuCacheDecache.cpp`**
   - 修改 `YuhuNormalEntryCacher::process_local_slot()`（普通编译参数读取）
   - 直接使用函数参数，而不是从 arg_base 读取
   - 对于参数索引 >= 8 的参数，从 x20 (esp) 读取栈参数
   - **注意**：暂时不修改 `YuhuOSREntryCacher`（OSR 编译参数读取）

### 参考文件

- `hotspot/src/cpu/aarch64/vm/assembler_aarch64.hpp` - AArch64 寄存器定义
- `hotspot/src/cpu/aarch64/vm/sharedRuntime_aarch64.cpp` - Java 调用约定
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` - `CreateReadStackPointer()` 和 `CreateReadLinkRegister()` 实现（参考）

## 总结

这个简化设计通过让 LLVM 函数参数直接对应 Java 方法参数，从根本上简化了参数传递流程，解决了当前设计中的多个问题：

1. **减少数据拷贝**：参数直接在寄存器中，无需多次拷贝
2. **简化适配器代码**：从 120+ 行减少到 10-20 行
3. **降低错误率**：减少中间步骤和计算
4. **符合约定**：充分利用 AArch64 调用约定和 HotSpot 寄存器约定
5. **改善栈帧布局**：减少临时区域，更容易符合 HotSpot 标准

这是解决 safepoint 卡死问题的**终极方案**，也是 Yuhu 编译器设计的重要改进。


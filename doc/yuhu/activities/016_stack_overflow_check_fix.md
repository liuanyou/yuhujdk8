# 016: 栈溢出检查逻辑修复

## 问题描述

当前实现存在一个根本问题：

### 问题：使用错误的栈指针（根本问题）⚠️

**关键问题**：`llvm.frameaddress` 返回的是**帧指针（FP）**，而不是**栈指针（SP）**！

**说明**：虽然检查公式 `current_sp - stack_bottom >= StackShadowPages + frame_size` 与 `stack_pointer >= stack_bottom + StackShadowPages` 在数学上等价，但真正的问题是 `current_sp` 的值本身是错误的（它是 FP，不是 SP）。无论使用哪个检查公式，如果 `current_sp` 的值是错误的，整个检查就失去了意义。

**关键问题**：`llvm.frameaddress` 返回的是**帧指针（FP）**，而不是**栈指针（SP）**！

在 AArch64 上：
- **FP (x29)**: 帧指针，指向当前函数的帧（保存的 FP 和 LR 的位置）
- **SP (x31)**: 栈指针，指向栈顶

当前实现：
```cpp
// yuhuStack.cpp:57-60
Value *current_sp = builder()->CreatePtrToInt(
  builder()->CreateGetFrameAddress(),  // ← 返回的是 FP，不是 SP！
  YuhuType::intptr_type(),
  "current_sp");

// 计算新栈指针
Value *stack_pointer = builder()->CreateSub(
  current_sp,  // ← 这是 FP，不是 SP！
  LLVMValue::intptr_constant(frame_size_bytes),
  "new_sp");
```

**问题**：
1. `CreateGetFrameAddress()` 返回的是 FP（帧指针），不是 SP（栈指针）
2. 使用 FP 作为 SP，然后计算 `stack_pointer = FP - frame_size`
3. 这个 `stack_pointer` 可能根本不在栈上！
4. 写入帧数据时，会写入到堆内存，导致 `memory stomp`

**为什么会出现 `memory stomp`？**
- `stack_pointer` 计算错误，指向了堆内存
- 写入帧数据（如 `builder()->CreateStore(...)`）时，写入到了堆内存
- 破坏了堆内存的 cushion 区域
- 后续分配 Java 对象时，`verify_block` 发现损坏，报告 `memory stomp`

### 关于检查公式的说明

**重要澄清**：检查公式 `current_sp - stack_bottom >= StackShadowPages + frame_size` 与 `stack_pointer >= stack_bottom + StackShadowPages` 在数学上等价，只是表达方式不同。真正的问题是 `current_sp` 的值本身是错误的（它是 FP，不是 SP）。

无论使用哪个检查公式，如果 `current_sp` 的值是错误的，整个检查就失去了意义，并且会导致 `stack_pointer` 计算错误，指向错误的内存位置。

### 其他实现的对比

#### Shark 的实现（Zero Stack）
```cpp
// sharkStack.cpp:127-136
Value *free_stack = builder()->CreateSub(
  builder()->CreatePtrToInt(
    builder()->CreateGetFrameAddress(),  // ← 使用 current_sp
    SharkType::intptr_type(),
    "abi_sp"),
  stack_top);
builder()->CreateCondBr(
  builder()->CreateICmpULT(
    free_stack,
    LLVMValue::intptr_constant(StackShadowPages * os::vm_page_size())),
  overflow, abi_ok);
```

Shark 检查的是 `current_sp`（通过 `CreateGetFrameAddress()` 获取），而不是 `stack_pointer`。

#### x86 的实现
```cpp
// cppInterpreter_x86.cpp:717-727
const int slop = 6 * K;  // 为 throw_StackOverflowError 预留空间
const int max_pages = StackShadowPages > (StackRedPages+StackYellowPages) ? 
                      StackShadowPages : (StackRedPages+StackYellowPages);
__ addptr(rax, slop + 2*max_pages * page_size);  // 加上 slop
__ cmpptr(rsp, rax);  // 检查当前 rsp
```

x86 实现：
1. 检查的是 `current_sp`（`rsp`）
2. 额外加上 `slop`（6KB）来为 `throw_StackOverflowError` 预留空间

#### Sparc 的实现
```cpp
// cppInterpreter_sparc.cpp:1106-1111
const int fudge = 6 * K;  // 为 throw_StackOverflowError 预留空间
__ set(fudge + (max_pages * os::vm_page_size()), O1);
__ add(O0, O1, O0);
__ sub(O0, Gtmp, O0);  // 减去新帧大小
__ cmp(SP, O0);  // 检查当前 SP
```

Sparc 实现：
1. 检查的是 `current_sp`（`SP`）
2. 额外加上 `fudge`（6KB）来为 `throw_StackOverflowError` 预留空间
3. 在计算时减去新帧大小，但检查的是当前 SP

## 根本问题：使用错误的栈指针 ⚠️

### 问题分析

**关键发现**：`llvm.frameaddress` 返回的是**帧指针（FP）**，而不是**栈指针（SP）**！

在 AArch64 上：
- **FP (x29)**: 帧指针，指向当前函数的帧（保存的 FP 和 LR 的位置）
- **SP (x31)**: 栈指针，指向栈顶

当前实现：
```cpp
// yuhuStack.cpp:57-73
Value *current_sp = builder()->CreatePtrToInt(
  builder()->CreateGetFrameAddress(),  // ← 返回的是 FP，不是 SP！
  YuhuType::intptr_type(),
  "current_sp");

Value *stack_pointer = builder()->CreateSub(
  current_sp,  // ← 这是 FP，不是 SP！
  LLVMValue::intptr_constant(frame_size_bytes),
  "new_sp");
```

**问题**：
1. `CreateGetFrameAddress()` 返回的是 FP（帧指针），不是 SP（栈指针）
2. 使用 FP 作为 SP，然后计算 `stack_pointer = FP - frame_size`
3. 这个 `stack_pointer` 可能根本不在栈上！
4. 写入帧数据时，会写入到堆内存，导致 `memory stomp`

**为什么会出现 `memory stomp`？**
- `stack_pointer` 计算错误，指向了堆内存
- 写入帧数据（如 `builder()->CreateStore(...)`）时，写入到了堆内存
- 破坏了堆内存的 cushion 区域
- 后续分配 Java 对象时，`verify_block` 发现损坏，报告 `memory stomp`

### 为什么 Shark 没有问题？

Shark 使用 `CreateGetFrameAddress()` 检查 ABI 栈，但它：
1. 使用 ZeroStack 管理 Java 栈（不依赖 ABI 栈指针）
2. 只在检查 ABI 栈溢出时使用 `CreateGetFrameAddress()`（用于检查，不用于分配）
3. 实际的栈帧分配使用 ZeroStack 的 `CreateLoadStackPointer()`

### 如何获取实际的栈指针（SP）？

在 LLVM IR 中，我们需要使用 `@llvm.read_register` intrinsic 来读取 SP 寄存器：

```cpp
// 读取 SP 寄存器（x31）
Value* get_stack_pointer() {
  // @llvm.read_register.i64(metadata !"sp")
  // 需要声明 metadata
  llvm::MDNode* md = llvm::MDNode::get(
    context(),
    llvm::MDString::get(context(), "sp"));
  llvm::Function* read_reg = llvm::Intrinsic::getDeclaration(
    module(),
    llvm::Intrinsic::read_register,
    {YuhuType::intptr_type()});
  return builder()->CreateCall(
    read_reg,
    {llvm::MetadataAsValue::get(context(), md)},
    "sp");
}
```

或者，我们可以使用 `alloca` 来获取栈指针，但这也不对，因为 `alloca` 返回的是分配的内存地址，不是 SP。

**更好的方法**：在函数入口处，从参数或调用约定中获取 SP。

## 修复方案

### 方案 0: 修复栈指针获取（根本修复）⚠️ **优先**

**原理**：
- 使用 `@llvm.read_register` intrinsic 读取 SP 寄存器（x31）
- 这是获取实际硬件栈指针的正确方法

**实现**：
```cpp
// 在 yuhuBuilder.hpp 中添加
llvm::CallInst* CreateReadStackPointer();

// 在 yuhuBuilder.cpp 中实现
CallInst* YuhuBuilder::CreateReadStackPointer() {
  // 创建 metadata 指定寄存器名称 "sp"
  llvm::MDNode* md = llvm::MDNode::get(
    context(),
    llvm::MDString::get(context(), "sp"));
  
  // 获取 read_register intrinsic 声明
  llvm::Function* read_reg = llvm::Intrinsic::getDeclaration(
    module(),
    llvm::Intrinsic::read_register,
    {YuhuType::intptr_type()});
  
  // 调用 read_register 读取 SP 寄存器
  return CreateCall(
    llvm::FunctionType::get(
      YuhuType::intptr_type(),
      {llvm::Type::getMetadataTy(context())},
      false),
    read_reg,
    {llvm::MetadataAsValue::get(context(), md)},
    "sp");
}

// 在 yuhuStack.cpp 中使用
void YuhuStack::initialize(Value* method) {
  // ... 计算 frame_size_bytes ...
  
  // 获取实际的栈指针（SP），而不是帧指针（FP）
  Value *current_sp = builder()->CreateReadStackPointer();
  
  // 计算新栈指针
  Value *stack_pointer = builder()->CreateSub(
    current_sp,
    LLVMValue::intptr_constant(frame_size_bytes),
    "new_sp");
  
  // ... 继续 ...
}
```

**注意**：
- `@llvm.read_register` 是 LLVM 的 intrinsic，用于读取硬件寄存器
- 在 AArch64 上，SP 寄存器的名称是 "sp"
- 这个方法返回的是实际的硬件栈指针，而不是帧指针

### 方案 1: 使用 current_sp 检查（已实施，但可能不够）

**原理**：
- 检查 `current_sp`（当前栈指针），而不是 `stack_pointer`（新栈指针）
- 确保 `current_sp - stack_bottom >= StackShadowPages + frame_size`
- 这样即使调用 `throw_StackOverflowError`，也有足够的空间

**实现**：
```cpp
void YuhuStack::CreateStackOverflowCheck(Value* sp) {
  BasicBlock *overflow = CreateBlock("stack_overflow");
  BasicBlock *ok       = CreateBlock("stack_ok");

  // 获取当前栈指针（不是传入的 stack_pointer）
  // ⚠️ 注意：CreateGetFrameAddress() 返回的是 FP，不是 SP！
  // 应该使用 CreateReadStackPointer() 获取实际的 SP 寄存器
  Value *current_sp = builder()->CreateReadStackPointer();  // 修复后

  // 计算栈底
  Value *stack_base = builder()->CreateValueOfStructEntry(
    thread(),
    Thread::stack_base_offset(),
    YuhuType::intptr_type(),
    "stack_base");
  Value *stack_size = builder()->CreateValueOfStructEntry(
    thread(),
    Thread::stack_size_offset(),
    YuhuType::intptr_type(),
    "stack_size");
  Value *stack_bottom = builder()->CreateSub(stack_base, stack_size, "stack_bottom");

  // 计算可用栈空间（从当前 SP 到栈底）
  Value *free_stack = builder()->CreateSub(current_sp, stack_bottom, "free_stack");

  // 计算最小所需空间：StackShadowPages + 新帧大小
  // 新帧大小通过传入的 sp 计算：frame_size = current_sp - sp
  Value *frame_size = builder()->CreateSub(current_sp, sp, "frame_size");
  Value *min_required = builder()->CreateAdd(
    LLVMValue::intptr_constant(StackShadowPages * os::vm_page_size()),
    frame_size,
    "min_required");

  // 检查：free_stack >= min_required
  builder()->CreateCondBr(
    builder()->CreateICmpULT(free_stack, min_required),
    overflow, ok);

  // Handle overflow
  builder()->SetInsertPoint(overflow);
  // ... throw_StackOverflowError ...
  builder()->CreateRet(LLVMValue::jint_constant(0));

  builder()->SetInsertPoint(ok);
}
```

**优点**：
- 与 Shark 的实现一致
- 逻辑清晰：检查当前栈指针是否有足够空间分配新帧

**缺点**：
- 需要计算 `frame_size = current_sp - sp`，可能增加复杂度

### 方案 2: 预留空间（如果方案 1 不行）

**原理**：
- 检查 `stack_pointer`，但额外加上 `slop`（6KB）来为 `throw_StackOverflowError` 预留空间
- 参考 x86 和 Sparc 的实现

**实现**：
```cpp
void YuhuStack::CreateStackOverflowCheck(Value* sp) {
  BasicBlock *overflow = CreateBlock("stack_overflow");
  BasicBlock *ok       = CreateBlock("stack_ok");

  // 计算栈底
  Value *stack_base = builder()->CreateValueOfStructEntry(
    thread(),
    Thread::stack_base_offset(),
    YuhuType::intptr_type(),
    "stack_base");
  Value *stack_size = builder()->CreateValueOfStructEntry(
    thread(),
    Thread::stack_size_offset(),
    YuhuType::intptr_type(),
    "stack_size");
  Value *stack_bottom = builder()->CreateSub(stack_base, stack_size, "stack_bottom");

  // 计算最小栈指针：stack_bottom + StackShadowPages + slop
  // slop 为 throw_StackOverflowError 预留空间（6KB，参考 x86/Sparc）
  const int slop = 6 * K;
  Value *min_stack = builder()->CreateAdd(
    stack_bottom,
    builder()->CreateAdd(
      LLVMValue::intptr_constant(StackShadowPages * os::vm_page_size()),
      LLVMValue::intptr_constant(slop),
      "shadow_plus_slop"),
    "min_stack");

  // 检查：sp >= min_stack
  builder()->CreateCondBr(
    builder()->CreateICmpULT(sp, min_stack),
    overflow, ok);

  // Handle overflow
  builder()->SetInsertPoint(overflow);
  // ... throw_StackOverflowError ...
  builder()->CreateRet(LLVMValue::jint_constant(0));

  builder()->SetInsertPoint(ok);
}
```

**优点**：
- 与 x86/Sparc 的实现一致
- 简单直接：在检查时加上固定的 slop

**缺点**：
- slop 是硬编码的，可能不够灵活

## 实施计划

1. **第一步**：尝试方案 1（使用 current_sp 检查）✅ **已完成**
   - 修改 `CreateStackOverflowCheck` 使用 `current_sp` 而不是 `sp`
   - 计算 `frame_size = current_sp - sp`
   - 检查 `current_sp - stack_bottom >= StackShadowPages + frame_size`
   - **实施日期**: 2025-01-XX
   - **文件**: `hotspot/src/share/vm/yuhu/yuhuStack.cpp`

2. **第二步**：如果方案 1 有问题，改用方案 2（预留空间）
   - 在检查时加上 `slop`（6KB）
   - 检查 `sp >= stack_bottom + StackShadowPages + slop`
   - **状态**: 待测试，如果方案 1 不工作则实施

## 实施详情

### 方案 1 实施（已完成）

**修改内容**：
- `CreateStackOverflowCheck` 现在使用 `current_sp`（通过 `CreateReadStackPointer()` 获取实际的 SP 寄存器）
- 计算 `frame_size = current_sp - sp`（传入的 `sp` 是新栈指针）
- 检查 `free_stack >= min_required`，其中：
  - `free_stack = current_sp - stack_bottom`
  - `min_required = StackShadowPages + frame_size`

**注意**：虽然检查公式与检查 `stack_pointer >= stack_bottom + StackShadowPages` 在数学上等价，但真正的问题是 `current_sp` 的值本身必须是正确的（必须是 SP，不能是 FP）。

**关键改进**：
1. 检查的是 `current_sp`（分配前），而不是 `stack_pointer`（分配后）
2. 确保有足够空间分配新帧，同时保留 `StackShadowPages` 空间给 `throw_StackOverflowError`
3. 与 Shark 的实现逻辑一致（Shark 也检查 `current_sp`）

**重要说明**：
- 检查公式 `current_sp - stack_bottom >= StackShadowPages + frame_size` 与 `stack_pointer >= stack_bottom + StackShadowPages` 在数学上等价
- 真正的问题是 `current_sp` 的值本身是错误的（它是 FP，不是 SP）
- 修复方案：使用 `CreateReadStackPointer()` 获取实际的 SP 寄存器，而不是使用 `CreateGetFrameAddress()` 返回的 FP

**代码变更**：
```cpp
// 之前：检查 stack_pointer（但 current_sp 是 FP，错误！）
Value *current_sp = CreateGetFrameAddress();  // ← 返回 FP，不是 SP！
Value *stack_pointer = current_sp - frame_size;
Value *min_stack = stack_bottom + StackShadowPages;
if (stack_pointer < min_stack) → overflow

// 现在：检查 current_sp（使用实际的 SP）
Value *current_sp = CreateReadStackPointer();  // ← 获取实际的 SP 寄存器
Value *stack_pointer = current_sp - frame_size;
Value *free_stack = current_sp - stack_bottom;
Value *frame_size = current_sp - stack_pointer;
Value *min_required = StackShadowPages + frame_size;
if (free_stack < min_required) → overflow
```

## 关于场景1和场景2的说明 ⚠️

**重要澄清**：场景1和场景2在数学上是等价的，只是表达方式不同：

- **场景1**：检查 `stack_pointer >= stack_bottom + StackShadowPages`
  - 其中 `stack_pointer = current_sp - frame_size`
  - 等价于：`current_sp - frame_size >= stack_bottom + StackShadowPages`
  - 移项后：`current_sp - stack_bottom >= StackShadowPages + frame_size`

- **场景2**：检查 `current_sp - stack_bottom >= StackShadowPages + frame_size`
  - 这是场景1的等价形式

**结论**：两个场景的检查公式在数学上完全等价，区别只是表达方式。

### 真正的问题：`current_sp` 的值本身是错误的 ⚠️

**关键问题**：无论使用场景1还是场景2的检查公式，如果 `current_sp` 的值本身是错误的，那么整个检查就失去了意义。

**当前实现的问题**：
- `current_sp` 是通过 `llvm.frameaddress` 获取的
- `llvm.frameaddress` 返回的是**帧指针（FP）**，而不是**栈指针（SP）**
- 在 AArch64 上，FP (x29) 和 SP (x31) 是不同的寄存器
- 使用 FP 作为 SP 计算 `stack_pointer = FP - frame_size`，这个地址可能根本不在栈上！

**为什么会出现 `memory stomp`？**
- `stack_pointer` 计算错误，指向了堆内存
- 写入帧数据（如 `builder()->CreateStore(...)`）时，写入到了堆内存
- 破坏了堆内存的 cushion 区域
- 后续分配 Java 对象时，`verify_block` 发现损坏，报告 `memory stomp`

**修复方案**：
- 使用 `@llvm.read_register` intrinsic 读取实际的 SP 寄存器（x31）
- 而不是使用 `llvm.frameaddress` 返回的 FP

## 为什么栈空间会不足？（历史分析，仅供参考）

以下内容是对之前错误理解的记录，真正的问题已在上文说明。

### 调用 `throw_StackOverflowError` 时的栈状态

让我们分析一下调用 `throw_StackOverflowError` 时的栈状态：

#### 场景 1：使用 `stack_pointer` 检查（数学等价形式1）

```
栈布局（向下增长）：
┌─────────────────────────┐
│   stack_base            │
├─────────────────────────┤
│   ...                   │
│   current_sp            │ ← 当前栈指针（检查时）
│   ...                   │
│   stack_pointer         │ ← 新栈指针（已减去新帧大小）
│   ...                   │
│   stack_bottom +        │
│   StackShadowPages      │ ← 最小安全栈指针
│   ...                   │
│   stack_bottom          │ ← 栈底
└─────────────────────────┘
```

**检查公式**：`stack_pointer >= stack_bottom + StackShadowPages`
- 其中 `stack_pointer = current_sp - frame_size`
- 等价于：`current_sp - stack_bottom >= StackShadowPages + frame_size`

#### 场景 2：使用 `current_sp` 检查（数学等价形式2）

```
栈布局（向下增长）：
┌─────────────────────────┐
│   stack_base            │
├─────────────────────────┤
│   ...                   │
│   current_sp            │ ← 当前栈指针（检查时）
│   ...                   │
│   stack_pointer         │ ← 新栈指针（已减去新帧大小）
│   ...                   │
│   stack_bottom +        │
│   StackShadowPages +    │
│   frame_size            │ ← 最小安全栈指针（修复后）
│   ...                   │
│   stack_bottom +        │
│   StackShadowPages      │
│   ...                   │
│   stack_bottom          │ ← 栈底
└─────────────────────────┘
```

**检查公式**：`current_sp - stack_bottom >= StackShadowPages + frame_size`
- 这是场景1的等价形式

**注意**：这两个场景的检查公式在数学上完全等价，区别只是表达方式。真正的问题是 `current_sp` 的值本身是错误的（它是 FP，不是 SP）。

### `JRT_ENTRY` 的栈帧需求

`JRT_ENTRY` 宏会创建以下对象（都需要栈空间）：

```cpp
#define JRT_ENTRY(result_type, header)                               \
  result_type header {                                               \
    Thread::WXWriteFromExecSetter __wx_write;                        \
    ThreadInVMfromJava __tiv(thread);                                \
    VM_ENTRY_BASE(result_type, header, thread)                       \
    debug_only(VMEntryWrapper __vew;)
```

1. **`Thread::WXWriteFromExecSetter`**：RAII 对象，约几十字节
2. **`ThreadInVMfromJava`**：RAII 对象，包含线程状态转换，约几百字节
3. **`HandleMarkCleaner`**（在 `VM_ENTRY_BASE` 中）：管理 Handle，约几KB
4. **函数局部变量**：`throw_StackOverflowError` 中的局部变量，约几KB

**总计**：约 6-8KB（这就是为什么 x86/Sparc 使用 `slop = 6KB`）

### 为什么 `StackShadowPages` 是 80KB？

从 `os.cpp` 的注释可以看出：

```cpp
// Check if we have StackShadowPages above the yellow zone.  This parameter
// is dependent on the depth of the maximum VM call stack possible from
// the handler for stack overflow.  'instanceof' in the stack overflow
// handler or a println uses at least 8k stack of VM and native code
// respectively.
```

`StackShadowPages` 需要足够大，以容纳：
1. `throw_StackOverflowError` 的栈帧（约 6KB）
2. 异常处理过程中的 VM 调用（如 `instanceof`、`println` 等，约 8KB）
3. 安全余量（防止边界情况）

在 AArch64 上，`StackShadowPages = 20 页 ≈ 80KB`，这远大于实际需求，提供了足够的安全余量。

### 总结

**为什么栈空间会不足？**

1. **修复前**：检查 `stack_pointer`（已减去新帧大小）
   - 如果 `stack_pointer` 刚好在边界上，检查失败
   - 但调用 `throw_StackOverflowError` 时，栈指针还是 `current_sp`
   - `throw_StackOverflowError` 需要新栈帧，可能会超出 `StackShadowPages` 范围

2. **修复后**：检查 `current_sp`（分配前的栈指针）
   - 确保 `current_sp - stack_bottom >= StackShadowPages + frame_size`
   - 这样即使调用 `throw_StackOverflowError`，也有足够的空间（`StackShadowPages` 远大于需求）

## 测试验证

修复后需要验证：
1. 栈溢出检查能正确触发
2. `throw_StackOverflowError` 有足够的栈空间执行
3. 不会出现 `SIGBUS` 或 `memory stomp` 错误
4. 不会出现 WX 状态错误

## 新问题：i2c adapter 参数不匹配导致空指针异常 ⚠️

### 问题描述

在修复栈溢出检查后，出现新的错误：

```
Internal Error (/Users/liuanyou/CLionProjects/jdk8/hotspot/src/share/vm/runtime/thread.hpp:677), pid=63203, tid=5123
assert(current->_wx_state == from) failed: wrong state
```

**调用栈分析**：
```
V  [libjvm.dylib+0x2c61e8]  ThreadStateTransition::ThreadStateTransition(JavaThread*)+0x38
V  [libjvm.dylib+0x261ee0]  ThreadInVMfromJava::ThreadInVMfromJava(JavaThread*)+0x34
V  [libjvm.dylib+0x98b454]  ThreadInVMfromJava::ThreadInVMfromJava(JavaThread*)+0x24
V  [libjvm.dylib+0xb37f28]  YuhuRuntime::throw_NullPointerException(JavaThread*, char const*, int)+0x2c
J 12 yuhu com.example.Matrix.multiply([[I[[I[[II)V (79 bytes) @ 0x000000010aa16a40
C  [libsystem_platform.dylib+0x3a24]  _sigtramp+0x38
```

### 根本原因

**问题**：`i2c adapter` 调用编译后的方法时，**没有按照 Yuhu 期望的参数顺序传递参数**！

#### Yuhu 期望的函数签名

从 `yuhuContext.cpp:94-98` 可以看到，Yuhu 定义的正常入口点类型：
```cpp
// 正常入口点参数顺序：
params.push_back(Method_type());        // x0: Method*
params.push_back(intptr_type());        // x1: base_pc
params.push_back(thread_type());        // x2: thread (JavaThread*)
_entry_point_type = FunctionType::get(jint_type(), params, false);
```

#### i2c adapter 的实际调用

从 `sharedRuntime_aarch64.cpp:536-629` 可以看到，`i2c adapter` 的工作流程：
1. 从解释器栈加载 Java 方法参数到寄存器（x0, x1, x2, ...）
2. 跳转到 `from_compiled_entry`（即 `verified_entry_point`）

**关键问题**：`i2c adapter` 跳转到编译后的方法时，x0、x1、x2 是 **Java 方法的参数**，而不是 `Method*`、`base_pc`、`thread`！

#### 参数不匹配

| 位置 | i2c adapter 实际传递 | Yuhu 期望接收 |
|------|---------------------|--------------|
| x0   | 第一个 Java 方法参数（receiver 或第一个参数） | Method*      |
| x1   | 第二个 Java 方法参数 | base_pc      |
| x2   | 第三个 Java 方法参数 | thread (JavaThread*) |

#### 触发 SIGSEGV 的流程

1. 编译后的代码调用 `YuhuRuntime::throw_NullPointerException(thread, file, line)`
   - 但 `thread` 参数是 x2，实际是 Java 方法的第三个参数（可能是 NULL 或无效值）

2. `JRT_ENTRY` 宏创建 `ThreadInVMfromJava __tiv(thread);`
   - 传入的 `thread` 是 NULL 或无效值

3. `ThreadInVMfromJava` 构造函数：
   - 先调用 `ThreadStateTransition(thread)`（只是保存指针，不访问）
   - 然后调用 `trans_from_java(_thread_in_vm)`

4. `trans_from_java` 调用 `transition_from_java(_thread, to)`
   - 访问 `thread->thread_state()`，如果 `thread` 是 NULL，**触发 SIGSEGV**

5. SIGSEGV 导致跳转到 `_sigtramp`

6. 在信号处理程序中，再次尝试创建 `ThreadInVMfromJava` 和 `WXWriteFromExecSetter`，但此时 WX 状态不正确，导致断言失败

### 解决方案：为正常入口点生成适配器

**原理**：在 LLVM 生成的函数之前，添加一小段汇编适配器代码，将 `i2c adapter` 传递的 Java 方法参数转换为 Yuhu 期望的参数格式。

**适配器功能**：
1. 保存 Java 方法参数到栈上（因为 x0, x1, x2 会被覆盖）
2. 从 `rmethod`（R12）获取 `Method*` → x0
3. 从当前 PC 获取 nmethod，然后获取 `code_begin()` → x1 (base_pc)
4. 从 `rthread`（R28）获取 `thread` → x2
5. 恢复 Java 方法参数到原始位置（x3, x4, ...）
6. 跳转到真正的 LLVM 生成的函数

**实现步骤**：
1. 在 `compile_method` 中，如果是正常入口点，生成适配器代码
2. 适配器代码放在 LLVM 代码之前
3. `verified_entry_point` 指向适配器入口
4. LLVM 代码的入口点在适配器之后

### 实施详情

**修改文件**：
- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - 添加正常入口点适配器生成
- `hotspot/src/cpu/aarch64/vm/yuhu/yuhu_macroAssembler_aarch64.cpp` - 添加适配器生成辅助函数

**关键代码**：
```cpp
// 生成正常入口点适配器
static int generate_normal_adapter_into(CodeBuffer& cb,
                                        Method* method,
                                        address base_pc,
                                        YuhuLabel* llvm_label,
                                        address llvm_entry,
                                        int java_arg_count) {
  YuhuMacroAssembler masm(&cb);
  address start = masm.current_pc();

  // 1. 保存 Java 方法参数到栈上（x0-x7 可能被使用）
  // 2. 从 rmethod (R12) 获取 Method* → x0
  // 3. 计算 base_pc → x1
  // 4. 从 rthread (R28) 获取 thread → x2
  // 5. 恢复 Java 方法参数
  // 6. 跳转到 LLVM 入口

  address end = masm.current_pc();
  return (int)(end - start);
}
```

## 相关文件

- `hotspot/src/share/vm/yuhu/yuhuStack.cpp` - 栈溢出检查实现
- `hotspot/src/share/vm/yuhu/yuhuStack.hpp` - 栈溢出检查声明
- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - 适配器生成
- `hotspot/src/cpu/aarch64/vm/yuhu/yuhu_macroAssembler_aarch64.cpp` - 适配器汇编生成
- `hotspot/src/share/vm/shark/sharkStack.cpp` - Shark 的参考实现
- `hotspot/src/cpu/x86/vm/cppInterpreter_x86.cpp` - x86 的参考实现
- `hotspot/src/cpu/sparc/vm/cppInterpreter_sparc.cpp` - Sparc 的参考实现


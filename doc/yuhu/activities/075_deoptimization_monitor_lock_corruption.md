# 活动 075: Deoptimization 期间 Monitor Lock 损坏 - Biased Locking 未撤销

## 日期
2025-02-XX

## 状态
**已解决** - acquire_lock 和 release_lock 均已实现 biased locking 处理

## 问题描述

在 Yuhu 编译代码执行 deoptimization 并切换到 interpreter 模式后，执行 `monitorexit` 时发生崩溃：

```
#  Internal Error (/Users/liuanyou/CLionProjects/jdk8/hotspot/src/share/vm/runtime/synchronizer.cpp:1297)
#  assert(dmw->is_neutral()) failed: invariant
```

崩溃发生在 `ObjectSynchronizer::inflate()` 函数中，该函数在将 thin lock 转换为 fat lock 时，期望 BasicLock 头部存储的 displaced mark word 是 neutral 状态，但实际读取到的是 biased locking pattern (`0x5`)。

## 错误原因分析

### 1. 根本原因

Yuhu 的 `acquire_lock()` 函数在获取锁时，没有实现完整的 biased locking 协议。当对象的 mark word 是 biased pattern (`0x5`) 时，代码直接执行 thin-lock CAS 操作，将 biased mark word 存储到 BasicLock 头部，而不是先撤销 bias。

### 2. HotSpot 锁定协议

在 HotSpot 中，对象 mark word 有以下几种状态：

- **Neutral/Unlocked** (`0x1`): 对象未锁定，mark word 包含 hash code 和 GC age
- **Biased** (`0x5`): 对象偏向某个线程，mark word 包含线程 ID、epoch 和 hash
- **Thin-locked** (`0x0`): 对象被轻量级锁定，mark word 指向栈上的 BasicLock
- **Fat-locked/Monitor** (`0x2`): 对象被重量级锁定，mark word 指向 ObjectMonitor

当 `UseBiasedLocking` 启用时，新创建的对象默认使用 `biased_locking_prototype()` 作为 mark word（即 `0x5`，anonymous bias，没有设置线程 ID）。

### 3. 正确的锁定协议

C1/C2 编译器在 `lock_object()` 中的实现：

```cpp
if (UseBiasedLocking) {
    biased_locking_enter(disp_hdr, obj, hdr, scratch, false, done, &slow_case);
} else {
    // 加载对象头部
    ldr(hdr, Address(obj, hdr_offset));
    // 标记为 unlocked
    orr(hdr, hdr, markOopDesc::unlocked_value);
    // 保存到 displaced header
    str(hdr, Address(disp_hdr, 0));
    // CAS 尝试 thin lock
    cmpxchgptr(hdr, disp_hdr, ...);
}
```

`biased_locking_enter()` 处理 biased locking 协议：
1. 如果对象已偏向当前线程 → 锁定完成，无需 CAS
2. 如果对象是 anonymous bias → 尝试 CAS 偏向当前线程
3. 如果对象偏向其他线程 → 撤销 bias，回退到 slow case
4. 如果 mark word 是 neutral → 继续 thin-lock 路径

### 4. Yuhu 的错误实现（原始代码）

Yuhu 的 `acquire_lock()` 跳过了 biased locking 检查，直接执行 thin-lock CAS：

```cpp
Value *mark = builder()->CreateLoad(YuhuType::intptr_type(), mark_addr, "mark");
Value *disp = builder()->CreateOr(
    mark, LLVMValue::intptr_constant(markOopDesc::unlocked_value), "disp");
builder()->CreateStore(disp, monitor_header_addr);

Value *lock = builder()->CreatePtrToInt(monitor_header_addr, YuhuType::intptr_type());
Value *check = builder()->CreateAtomicCmpXchg(mark_addr, disp, lock, ...);
```

问题在于：
1. 对象 mark word 是 `0x5` (biased pattern)
2. `CreateOr(mark, 1)` 结果是 `0x5 OR 1 = 0x5`（biased pattern 不变）
3. CAS 成功：对象 mark word 被替换为 BasicLock 地址
4. BasicLock 头部存储的是 `0x5` (biased pattern)，而不是 neutral mark word

### 5. Deoptimization 期间的崩溃链路

1. **Yuhu 编译代码执行 monitorenter**
   - 对象 mark: `0x5` (biased)
   - CAS: 替换为 BasicLock 指针
   - BasicLock 头部: `0x5` (biased)

2. **Deoptimization 发生**
   - 编译帧被解包
   - `vframeArrayElement::unpack_on_stack()` 被调用

3. **Monitor 状态重建**
   - `compiledVFrame::monitors()` 提取 monitor 信息
   - 断言检查（第 84 行）：`!monitor->owner()->has_bias_pattern()` 
   - 如果断言启用，此处会失败
   - 如果断言禁用（NDEBUG），继续执行

4. **BasicLock 移动**
   - `monitor->lock()->move_to(monitor->owner(), dest->lock())`
   - 检查 `displaced_header()->is_neutral()` (basicLock.cpp:62)
   - **失败**：displaced header 是 `0x5` (biased)，不是 neutral

5. **Interpreter 执行 monitorexit**
   - `InterpreterRuntime::monitorexit()` → `ObjectSynchronizer::slow_exit()`
   - → `fast_exit()` → 读取 BasicLock 头部 (`0x5`)
   - → `inflate()` → 检查 `dmw->is_neutral()`
   - **崩溃**：`0x5` 不是 neutral pattern

### 6. 为什么必须撤销 Bias

`fast_exit()` 和 `move_to()` 都期望 BasicLock 头部存储的是 neutral mark word，原因：

1. **fast_exit 的恢复逻辑**：解锁时需要将 displaced mark word CAS 回对象，恢复对象的 unlocked 状态。biased mark word 不能直接 CAS 回去，因为 biased locking 有专门的撤销协议。

2. **move_to 的 inflation 逻辑**：当在帧之间移动 BasicLock 时，如果 displaced header 是 neutral，HotSpot 选择 inflate 锁（转换为 ObjectMonitor），这是安全的方式。如果是 biased pattern，inflation 逻辑会失败。

### 7. 编译帧到解释帧的 Monitor 状态转移

在 deoptimization 期间，monitor 状态需要从编译帧转移到解释帧：

```
编译帧:
  对象 mark word → BasicLock 地址 (编译帧栈)
  BasicLock 头部 → displaced mark word (应该是 neutral)

解释帧:
  对象 mark word → BasicLock 地址 (解释帧栈)
  BasicLock 头部 → displaced mark word (从编译帧复制)
```

问题：如果编译帧的 BasicLock 头部存储的是 biased pattern (`0x5`)，这个错误值会被复制到解释帧，导致后续 `monitorexit` 崩溃。

## 解决方案

### 方案 1: 实现完整的 Biased Locking 协议（已采用）

**acquire_lock 修复**：

在 `acquire_lock()` 中 neutralize displaced header，清除 biased lock bits：

```cpp
Value *mark = builder()->CreateLoad(YuhuType::intptr_type(), mark_addr, "mark");
// Neutralize displaced header: clear biased lock bits, then set unlocked bit.
Value *disp = builder()->CreateOr(
    builder()->CreateAnd(mark, LLVMValue::intptr_constant(~markOopDesc::biased_lock_mask_in_place)),
    LLVMValue::intptr_constant(markOopDesc::unlocked_value),
    "disp");
builder()->CreateStore(disp, monitor_header_addr);
```

效果：
- 对于 biased mark `0x5`，displaced header 变为 `0x1`（neutral）
- CAS 失败（`0x5 != 0x1`），走 runtime monitorenter
- Runtime 的 `ObjectSynchronizer::fast_enter()` 正确处理 biased locking 协议
- BasicLock 头部存储 neutral mark `0x1`，deoptimization 时 `move_to()` 正确调用 `inflate_helper()`

**release_lock 修复**：

在 `release_lock()` 中添加 biased lock 检查，复制 C1 的 `biased_locking_exit` 逻辑：

```cpp
// Handle biased locking: if object is still biased, unlock is a no-op
if (UseBiasedLocking) {
  BasicBlock *check_bias = function()->CreateBlock("check_bias");
  builder()->CreateBr(check_bias);
  builder()->SetInsertPoint(check_bias);

  Value *mark_addr = builder()->CreateAddressOfStructEntry(
    lockee, in_ByteSize(oopDesc::mark_offset_in_bytes()),
    PointerType::getUnqual(YuhuType::intptr_type()),
    "mark_addr");

  Value *mark = builder()->CreateLoad(YuhuType::intptr_type(), mark_addr, "mark");
  Value *bias_bits = builder()->CreateAnd(
    mark, LLVMValue::intptr_constant(markOopDesc::biased_lock_mask_in_place), "bias_bits");
  Value *is_biased = builder()->CreateICmpEQ(
    bias_bits, LLVMValue::intptr_constant(markOopDesc::biased_lock_pattern), "is_biased");
  builder()->CreateCondBr(is_biased, released_fast, not_recursive);
} else {
  builder()->CreateBr(not_recursive);
}
```

效果：
- 如果对象仍 biased，直接跳转到 `released_fast`（no-op）
- 如果对象不 biased，继续 thin lock CAS 或 runtime 路径
- 避免 biased 对象进入 runtime `monitorexit` → `fast_exit` → assert 失败

### 方案 2: 禁用 Biased Locking（未采用）

添加 JVM 启动参数 `-XX:-UseBiasedLocking`。

优点：快速解决，无需修改代码。
缺点：性能损失，biased locking 是 HotSpot 的重要优化。

### 方案 3: 强制转换 Biased Mark 为 Neutral（未采用，有缺陷）

在存储 displaced mark word 时，清除 biased lock bit：

```cpp
Value *neutral_mark = builder()->CreateAnd(
    mark, 
    LLVMValue::intptr_constant(~markOopDesc::biased_lock_bit_in_place));
builder()->CreateStore(neutral_mark, monitor_header_addr);
```

优点：简单修改。
缺点：违反了 locking 协议，对象仍然是 biased 状态但 BasicLock 存储的是 neutral mark，可能导致其他问题。

## 参考代码

### C1 的 lock_object 实现

文件：`hotspot/src/cpu/aarch64/vm/c1_MacroAssembler_aarch64.cpp`

```cpp
int C1_MacroAssembler::lock_object(Register hdr, Register obj, Register disp_hdr, 
                                    Register scratch, Label& slow_case) {
    // ...
    if (UseBiasedLocking) {
        assert(scratch != noreg, "should have scratch register at this point");
        null_check_offset = biased_locking_enter(disp_hdr, obj, hdr, scratch, 
                                                  false, done, &slow_case);
    } else {
        null_check_offset = offset();
    }
    
    // Load object header
    ldr(hdr, Address(obj, hdr_offset));
    // and mark it as unlocked
    orr(hdr, hdr, markOopDesc::unlocked_value);
    // save unlocked object header into the displaced header location on the stack
    str(hdr, Address(disp_hdr, 0));
    // test if object header is still the same (i.e. unlocked), and if so, store the
    // displaced header address in the object header
    // ...
}
```

### Biased Locking 断言

文件：`hotspot/src/share/vm/runtime/vframeArray.cpp`

```cpp
GrowableArray<MonitorInfo*>* list = vf->monitors();
if (list->is_empty()) {
    _monitors = NULL;
} else {
    for (index = 0; index < list->length(); index++) {
        MonitorInfo* monitor = list->at(index);
        assert(monitor->owner() == NULL || 
               (!monitor->owner()->is_unlocked() && 
                !monitor->owner()->has_bias_pattern()), 
               "object must be null or locked, and unbiased");
        // ...
    }
}
```

### BasicLock::move_to 实现

文件：`hotspot/src/share/vm/runtime/basicLock.cpp`

```cpp
void BasicLock::move_to(oop obj, BasicLock* dest) {
    if (displaced_header()->is_neutral()) {
        ObjectSynchronizer::inflate_helper(obj);
    } else {
        // Typically the displaced header will be 0 (recursive stack lock) or
        // unused_mark.
    }
    dest->set_displaced_header(displaced_header());
}
```

## 调试信息

### 崩溃时的对象状态

```
对象地址: 0x000000076abf7f00
对象 mark word: 0x000000016bc5a490 (指向栈地址 - thin-locked 状态)

BasicLock 地址: 0x000000016bc5a490
BasicLock 头部: 0x0000000000000005 (biased pattern - 错误！应该是 neutral)
```

### Deoptimization 事件

```
Event: 81.747 DEOPT PACKING pc=0x000000010dae6454 sp=0x000000016fa123f0
Event: 81.747 DEOPT UNPACKING pc=0x000000010d9570a4 sp=0x000000016fa12290 mode 3
```

## 影响范围

- 所有使用 `monitorenter`/`monitorexit` 的 Yuhu 编译代码
- 仅在 `UseBiasedLocking` 启用时发生
- 仅在 deoptimization 后执行 `monitorexit` 时触发

## 优先级

**高** - 导致 JVM 崩溃，阻塞包含 synchronized 块的代码执行。

## 后续工作

~~1. 实现 `biased_locking_enter()` 的 LLVM IR 生成逻辑~~ ✅ 已完成 - acquire_lock neutralize displaced header
~~2. 在 `acquire_lock()` 中集成 biased locking 检查~~ ✅ 已完成
3. ~~测试 biased locking 的各种场景（anonymous bias、biased to current thread、biased to other thread）~~ ✅ 已完成
4. ~~验证 deoptimization 期间 monitor 状态正确转移~~ ✅ 已完成
5. ✅ 已完成 - release_lock 添加 biased_locking_exit 检查，防止 biased 对象进入 runtime monitorexit

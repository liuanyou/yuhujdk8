# 038: Yuhu 额外的 16 字节 alloca 导致 frame_size 计算错误

**Date**: 2026-01-25
**Status**: 🔴 New Issue - 发现新的问题
**Related Issues**:
- 037: Yuhu Deoptimization Crash - LLVM Spill Space 导致 frame_size 计算错误

## 问题摘要

在修复了 037 问题（LLVM spill 空间）之后，发现 **还有额外的 16 字节**没有被计算到 frame_size 中。

### 症状

从 `encodeArrayLoop` 的汇编代码可以看到：

```asm
0x000000010a409378: mov    sp, x28        ; SP = x28 (Yuhu frame SP)
0x000000010a40937c: mov    x8, sp         ; x8 = SP (保存原始 SP)
0x000000010a409380: sub    x27, x8, #0x10 ; x27 = SP - 16
0x000000010a409384: mov    sp, x27        ; SP = x27 ← 又分配了 16 字节！
0x000000010a409388: stur    x28, [x8, #-16] ; 使用这 16 字节
```

**关键**：这 16 字节是**持久的**，在整个函数执行期间 SP 都保持这个值。

## 根本原因

### 1. LLVM `alloca` 指令的位置

在 `yuhuStack.hpp:85-86` 中：

```cpp
void initialize_stack_pointers(llvm::Value* stack_pointer) {
  _stack_pointer = stack_pointer;
  // Create storage for stack pointer
  _sp_storage = builder()->CreateAlloca(YuhuType::intptr_type(), 0, "sp_storage");
  builder()->CreateStore(stack_pointer, _sp_storage);
  // ...
}
```

这个函数在 `yuhuStack.cpp:86` 被调用：

```cpp
void YuhuStack::initialize(Value* method) {
  // ...
  builder()->CreateWriteStackPointer(stack_pointer);

  // Initialize stack pointer storage
  initialize_stack_pointers(stack_pointer);  // ← 在这里调用 alloca！

  // Create the frame
  _frame = builder()->CreateIntToPtr(...);
  // ...
}
```

### 2. 问题所在

- **调用时机**：`initialize_stack_pointers()` 在 Yuhu frame 分配**之后**调用
- **LLVM 行为**：当 LLVM 看到在函数体中间有 `alloca` 指令时，会在那个位置生成 `sub sp, sp, #16`
- **结果**：SP 在 Yuhu frame 分配之后又被减少了 16 字节

### 3. 为什么 `YuhuPrologueAnalyzer` 检测不到

`YuhuPrologueAnalyzer` 只扫描函数的前 10 条指令（prologue 区域）：

```cpp
for (int i = 0; i < 10; i++) {
  uint32_t inst = *(uint32_t*)pc;
  // ...
  pc += 4;
}
```

但这个 `sub sp, sp, #16` 指令出现在：
- 指令 1-10: LLVM prologue (stp, add, sub sp, sp, #0x40)
- 指令 11-12: Yuhu frame 分配 (sub x28, sp, #0xc0; mov sp, x28)
- **指令 13-14**: 额外的 16 字节 (sub x27, x8, #0x10; mov sp, x27) ← 不在 prologue 中！

## 影响分析

### SP 的完整变化

```
1. LLVM prologue:
   stp x28, x27, [sp, #-96]!   ; SP = 初始 - 96
   add x29, sp, #0x50
   sub sp, sp, #0x40           ; SP = 初始 - 160 (LLVM spill)

2. Yuhu frame 分配:
   sub x28, sp, #0xc0          ; 计算 Yuhu frame
   mov sp, x28                 ; SP = 初始 - 352

3. 额外的 alloca (sp_storage):
   sub sp, sp, #0x10           ; SP = 初始 - 368 ← 实际的 SP！

4. 如果 frame_size 只计算了 352 (不包含这 16 字节):
   去优化时: sender_sp = (初始 - 368) + 352 = 初始 - 16 ❌
   正确应该是: sender_sp = 初始 ✅
```

### 差了 16 字节！

- 实际的 SP = 初始 SP - 368
- frame_size = 352
- sender_sp = unextended_sp + frame_size = (初始 - 368) + 352 = 初始 - 16
- **但实际的 sender SP 应该是 = 初始**

## 解决方案

### 最终方案：在 epilogue 中动态生成 SP 恢复指令 ✅

由于 alloca 导致的额外 16 字节（或其他大小的 LLVM spill 空间）无法在编译时确定，我们采用 **运行时动态生成** SP 恢复指令的方案。

#### 核心思路

1. **在 IR 生成时**：在所有 return path 的统一 exit block 中插入一个 **placeholder 指令**（`0xcafebabe`）
2. **在代码生成后**：扫描 LLVM prologue 获取实际 frame offset，然后生成正确的 `sub sp, x29, #offset` 指令替换 placeholder

#### 为什么这样可以解决问题？

- **不依赖编译时计算**：frame offset 是在 LLVM 生成代码后，通过分析实际汇编确定的
- **自动适应任何 alloca 大小**：无论 LLVM 分配多少 spill 空间，都能正确恢复 SP
- **统一的 exit point**：所有 return path 都跳转到统一的 exit block，只需要修复一个地方

### 实现细节

#### 1. IR 生成：CreateEpiloguePlaceholder

在 `yuhuBuilder.cpp` 中：

```cpp
void YuhuBuilder::CreateEpiloguePlaceholder() {
  YuhuContext& ctx = YuhuContext::current();
  llvm::FunctionType* asm_type = llvm::FunctionType::get(
    llvm::Type::getVoidTy(ctx), false);

  // Emit a marker instruction (0xcafebabe) that will be replaced later
  llvm::InlineAsm* asm_func = llvm::InlineAsm::get(
    asm_type,
    ".inst 0xcafebabe",  // Emit raw 4-byte instruction as placeholder
    "",
    true,
    false,
    llvm::InlineAsm::AD_ATT);

  CreateCall(asm_type, asm_func, std::vector<Value*>());
}
```

**关键点**：
- 使用 `.inst 0xcafebabe` 而不是 `.byte` 序列，确保指令正确对齐
- 在统一的 exit block 中调用，所有 return path 跳转到这个 block

#### 2. 统一 Exit Block 实现

在 `yuhuFunction.cpp` 中：

```cpp
void YuhuFunction::build_returns() {
  // Create unified exit block with epilogue placeholder
  llvm::BasicBlock* exit_block = create_unified_exit_block();

  // All return blocks jump to unified exit block
  for (auto ret_block : _return_blocks) {
    _builder->CreateBr(exit_block);
  }
}
```

#### 3. 运行时修复：fixup_prologue_epilogue_markers

在 `yuhuBuilder.cpp` 中：

```cpp
void YuhuBuilder::fixup_prologue_epilogue_markers(address code_start, size_t code_size) {
  // Step 1: Use YuhuPrologueAnalyzer to extract frame offset from "add x29, sp, #imm"
  int frame_offset_bytes = YuhuPrologueAnalyzer::extract_add_x29_sp_imm(code_start);

  // Step 2: Generate correct SP restore instruction
  uint32_t restore_sp_instruction;
  if (frame_offset_bytes == 0) {
    // No offset: add sp, x29, #0
    restore_sp_instruction = 0x91003D9F;
  } else {
    // Use SUB to restore SP: sub sp, x29, #frame_offset_bytes
    // Encoding: [31]=1(SF), [30:24]=0b1010010(SUB), [23:22]=00, [21:10]=imm12, [9:5]=29(x29), [4:0]=31(sp)
    restore_sp_instruction = (0xD1 << 24) | ((frame_offset_bytes & 0xFFF) << 10) | (29 << 5) | 31;
  }

  // Step 3: Find and replace 0xcafebabe marker
  const uint32_t marker_value = 0xcafebabe;
  unsigned char* code_ptr = (unsigned char*)code_start;
  unsigned char* code_end = code_ptr + code_size;

  for (unsigned char* pc = code_ptr; pc < code_end - 4; pc += 4) {
    uint32_t inst = *(uint32_t*)pc;
    if (inst == marker_value) {
      *(uint32_t*)pc = restore_sp_instruction;  // Replace marker
    }
  }
}
```

**关键点**：
- `0xD1 << 24`：64位 SUB 指令（不是 0x51，那是32位版本）
- 直接使用 `frame_offset_bytes`（AArch64 的 add/sub 立即数 **不是 scaled by 4**）
- 扫描时只检查 4 字节对齐的地址（`pc += 4`）

#### 4. 调用时机

在 `yuhuCompiler.cpp` 中：

```cpp
// After memcpy LLVM code to combined buffer
memcpy(combined_base + adapter_size, entry->code_start(), llvm_code_size);

// Fix up epilogue markers (now in writable BufferBlob, not CodeCache)
builder.fixup_prologue_epilogue_markers(combined_base, combined_size);
```

**为什么不需要修改内存权限？**
- LLVM 代码先复制到 **BufferBlob**（通过 `CodeBuffer`）
- BufferBlob 是可写的
- 最后才安装到 CodeCache（此时已经是正确的指令了）

### 方案对比

| 方案 | 优点 | 缺点 | 是否采用 |
|------|------|------|----------|
| 1. 把 alloca 放到函数入口 | 自动检测 | 需要重构 | ❌ |
| 2. 不使用 alloca | 不额外分配 | 破坏现有设计 | ❌ |
| 3. 硬编码 +16 字节 | 简单 | 不够通用 | ❌ |
| 4. 扩展扫描范围 | 自动检测 | 可能误检测 | ❌ |
| **5. 运行时动态生成** | **自动适应任何 alloca** | **需要 placeholder** | **✅ 采用** |

## 推荐方案

**采用方案 5**：运行时动态生成 SP 恢复指令。

## 修改文件

### 1. hotspot/src/share/vm/yuhu/yuhuBuilder.cpp

**添加方法**：`CreateEpiloguePlaceholder()`

```cpp
void YuhuBuilder::CreateEpiloguePlaceholder() {
  YuhuContext& ctx = YuhuContext::current();
  llvm::FunctionType* asm_type = llvm::FunctionType::get(
    llvm::Type::getVoidTy(ctx), false);

  llvm::InlineAsm* asm_func = llvm::InlineAsm::get(
    asm_type,
    ".inst 0xcafebabe",  // Marker to be replaced later
    "",
    true,
    false,
    llvm::InlineAsm::AD_ATT);

  CreateCall(asm_type, asm_func, std::vector<Value*>());
}
```

**添加方法**：`fixup_prologue_epilogue_markers()`

```cpp
void YuhuBuilder::fixup_prologue_epilogue_markers(address code_start, size_t code_size) {
  // Extract frame offset from LLVM prologue
  int frame_offset_bytes = YuhuPrologueAnalyzer::extract_add_x29_sp_imm(code_start);

  // Generate: sub sp, x29, #frame_offset_bytes
  uint32_t restore_sp_instruction;
  if (frame_offset_bytes == 0) {
    restore_sp_instruction = 0x91003D9F;  // add sp, x29, #0
  } else {
    // 64-bit SUB: [31]=1, [30:24]=1010010, [21:10]=imm12, [9:5]=29, [4:0]=31
    restore_sp_instruction = (0xD1 << 24) | ((frame_offset_bytes & 0xFFF) << 10) | (29 << 5) | 31;
  }

  // Replace 0xcafebabe marker
  const uint32_t marker_value = 0xcafebabe;
  for (unsigned char* pc = (unsigned char*)code_start;
       pc < (unsigned char*)(code_start + code_size) - 4; pc += 4) {
    if (*(uint32_t*)pc == marker_value) {
      *(uint32_t*)pc = restore_sp_instruction;
    }
  }
}
```

### 2. hotspot/src/share/vm/yuhu/yuhuFunction.cpp

**修改**：实现 unified exit block

```cpp
void YuhuFunction::build_returns() {
  // Create unified exit block
  llvm::BasicBlock* exit_block = create_unified_exit_block();
  _builder->SetInsertPoint(exit_block);

  // Insert epilogue placeholder
  builder()->CreateEpiloguePlaceholder();

  // Emit return
  llvm::Value* void_ret = builder()->CreateRetVoid();

  // All return blocks jump to exit block
  for (auto ret_block : _return_blocks) {
    _builder->SetInsertPoint(ret_block);
    _builder->CreateBr(exit_block);
  }
}
```

### 3. hotspot/src/share/vm/yuhu/yuhuCompiler.cpp

**修改**：在 memcpy 之后调用 fixup

```cpp
// Copy LLVM code to combined buffer
memcpy(combined_base + adapter_size, entry->code_start(), llvm_code_size);

// Fix up epilogue markers (generate correct SP restore instructions)
builder.fixup_prologue_epilogue_markers(combined_base, combined_size);
```

**关键**：必须在 memcpy 之后、安装到 CodeCache 之前调用。

## 详细修改

### 1. 删除 _sp_storage 成员

```cpp
// 删除第 79 行：
mutable llvm::AllocaInst* _sp_storage;  // Storage for stack pointer
```

### 2. 修改 initialize_stack_pointers()

```cpp
void initialize_stack_pointers(llvm::Value* stack_pointer) {
  _stack_pointer = stack_pointer;
  // Frame pointer address will be set in initialize() when frame is created
  // No longer using alloca for sp_storage!
}
```

### 3. 修改 stack_pointer_addr()

```cpp
llvm::Value* stack_pointer_addr() const {
  // Return the address of unextended_sp slot in the frame
  if (_frame != NULL) {
    return builder()->CreatePtrToInt(
      slot_addr(unextended_sp_offset()),
      YuhuType::intptr_type(),
      "sp_storage_addr");
  }
  // Fallback during frame initialization: use alloca temporarily
  // This alloca will be optimized away by LLVM since it's only used during init
  return builder()->CreatePtrToInt(
    builder()->CreateAlloca(YuhuType::intptr_type(), 0, "sp_addr"),
    YuhuType::intptr_type());
}
```

### 4. 修改 CreateStoreStackPointer()

```cpp
llvm::StoreInst* CreateStoreStackPointer(llvm::Value* value) {
  _stack_pointer = value;
  if (_frame != NULL) {
    // Store to unextended_sp slot in the frame
    return builder()->CreateStore(value, slot_addr(unextended_sp_offset()));
  }
  // Fallback: use alloca during initialization
  llvm::AllocaInst* alloca = builder()->CreateAlloca(
    YuhuType::intptr_type(), 0, "sp_storage");
  return builder()->CreateStore(value, alloca);
}
```

### 5. 修改 CreateLoadStackPointer()

保持不变，因为它调用 `stack_pointer_addr()`，会自动使用新的逻辑。

### 6. 添加 unextended_sp_offset() 方法

如果还没有这个方法，需要添加：

```cpp
int unextended_sp_offset() const {
  // unextended_sp is stored at offset: header + monitor + stack
  return header_words() + monitor_words() + stack_words();
}
```

或者在现有代码中，unextended_sp 已经在 initialize() 中存储了：

```cpp
// Unextended SP
builder()->CreateStore(stack_pointer, slot_addr(offset++));
_pc_slot_offset = offset;
```

所以 offset 在存储 unextended_sp 之前就是 `unextended_sp_offset`。

## 测试步骤

### 1. 编译修改后的代码

```bash
cd /Users/liuanyou/CLionProjects/jdk8
make clean
make configure
make
```

### 2. 运行测试

```bash
cd test_yuhu
java -XX:+UseYuhuCompiler \
     -XX:TieredStopAtLevel=6 \
     -XX:CompileCommand=yuhuonly,sun.nio.cs.*::* \
     -cp . \
     TestEncoder
```

### 3. 验证修复

#### 检查日志输出

```
Yuhu: Prologue analysis - code_start=<addr>, prologue_bytes=160, prologue_words=20
Yuhu: Found prologue offset: 96 bytes
Yuhu: Generating 'sub sp, x29, #96' (imm=96/0x60, inst=0xd18603bf)
Yuhu: Replaced 1 epilogue markers with SP restore instruction
```

#### 检查反汇编

```bash
# 获取编译后的代码地址
jhsdb clhsdb > attach <pid>
clhsdb> memstat
clhsdb> disassemble <code_start> <code_end>

# 应该看到：
0x<addr>: sub    sp, x29, #0x60  ← 正确的 SP 恢复指令！
0x<addr+4>: ret
```

#### 验证去优化不再崩溃

运行测试应该正常完成，不再出现 SIGSEGV 或 `assert(sender_sp == unextended_sp() + frame_size()) failed` 错误。

## 相关文件

### 需要修改的文件
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` - 添加 placeholder 和 fixup 方法
- `hotspot/src/share/vm/yuhu/yuhuFunction.cpp` - 实现 unified exit block
- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - 在 memcpy 后调用 fixup

### 相关的 HotSpot 文件
- `hotspot/src/share/vm/yuhu/yuhuPrologueAnalyzer.cpp` - 分析 LLVM prologue 提取 frame offset
- `hotspot/src/share/vm/runtime/os.hpp` - 内存保护 API（本方案不需要）
- `hotspot/src/share/vm/code/codeCache.hpp` - CodeCache 内存管理

## 相关文档

- `doc/yuhu/activities/037_yuhu_deoptimization_root_cause.md` - 之前的去优化问题
- `doc/yuhu/YUHU_FRAME_LAYOUT_DESIGN.md` - Yuhu 栈帧设计

## 经验总结

### 1. LLVM `alloca` 的位置问题
- `alloca` 如果不在函数入口点，会在代码体中间生成 `sub sp, sp, #N` 指令
- 这导致 SP 值在函数执行过程中发生变化
- 如果不在 epilogue 中正确恢复，会导致去优化失败

### 2. 为什么不能用编译时计算
- LLVM 的 spill 空间大小是 **LLVM 编译器决定的**
- Yuhu 在生成 IR 时无法知道 LLVM 会分配多少 spill 空间
- 必须在 LLVM 生成机器码后，通过分析实际汇编来确定

### 3. Placeholder + 动态替换方案的优势
- **灵活性**：自动适应任何 LLVM alloca 大小
- **准确性**：基于实际生成的汇编，不是估算
- **可维护性**：不需要手动调整 frame_size

### 4. AArch64 指令编码关键点
- **SUB vs ADD**：恢复 SP 要用 `sub sp, x29, #N`（不是 `add`）
- **64位 vs 32位**：
  - `0xD1...` = 64位寄存器（`sp`, `x29`）
  - `0x51...` = 32位寄存器（`wsp`, `w29`）
- **立即数 scaling**：
  - `add/sub` 的立即数 **不是 scaled**，直接使用字节数
  - `ldr/str` 的立即数 **是 scaled by 4**，需要除以 4

### 5. 内存权限问题的教训
- **CodeCache 是 `r-x`**（只读），不能直接写
- **BufferBlob 是可写的**，通过 `CodeBuffer` 管理
- **正确的时机**：在 memcpy 到 BufferBlob 后、安装到 CodeCache 前进行 patch
- **不需要 `mprotect`**：stub patching 不需要权限变更的原因也是如此

### 6. 统一 Exit Block 的好处
- **只有一个 marker**：避免 tail merging 导致多个 marker
- **易于维护**：所有 return path 跳转到同一个 exit point
- **性能更好**：编译器可以优化跳转指令

## 下一步工作

1. ✅ **实施 epilogue placeholder 方案** - 已完成
2. ✅ **测试修复** - 验证去优化不再崩溃
3. **完善自动化测试** - 添加回归测试确保 SP 恢复正确
4. **性能优化** - 考虑是否可以缓存生成的指令

---

**更新历史**：
- 2026-01-25: 初始版本，发现额外的 16 字节 alloca 问题
- 2026-01-27: 实现运行时动态生成 SP 恢复指令方案，彻底解决问题

# 017: 普通入口适配器参数存储问题

## 日期
2025-12-XX

## 问题描述

在实现普通入口适配器（normal entry adapter）后，发现编译的代码在执行时触发空指针异常（`NullPointerException`），但实际传入的参数并非空指针。通过分析汇编代码和 Yuhu 的栈帧布局，发现问题的根本原因是：**适配器没有将 Java 方法参数存储到栈帧的 locals 区域，而 Yuhu 的 LLVM 代码期望从栈帧读取参数**。

## 错误现象

### 1. 空指针检查失败

汇编代码中多处 `cbz`（Compare and Branch if Zero）指令检查失败：

```assembly
0x000000010ca6b1b0: cbz	x9, 0x000000010ca6b4ec  ; 检查 x9 (应该是 local[0] = a)
0x000000010ca6b2a8: cbz	x11, 0x000000010ca6b3e4 ; 检查 x11 (应该是 local[1] = b)
0x000000010ca6b2d0: cbz	x10, 0x000000010ca6b4d4 ; 检查 x10 (应该是 local[2] = c)
```

这些寄存器中的值来自栈帧的 locals 区域（如 `[x25, #144]`），但值不正确，导致空指针检查失败。

### 2. 适配器代码的问题

当前适配器代码（`yuhuCompiler.cpp:357-396`）的逻辑：

```cpp
// 1. 保存 Java 方法参数 (x0-x7) 到临时栈空间
masm.write_inst("sub sp, sp, #64");
masm.write_inst("stp x0, x1, [sp, #0]");
masm.write_inst("stp x2, x3, [sp, #16]");
masm.write_inst("stp x4, x5, [sp, #32]");
masm.write_inst("stp x6, x7, [sp, #48]");

// 2. 设置 Yuhu 期望的参数 (Method*, base_pc, thread)
masm.write_inst_mov_reg(x0, x12);  // x0 = Method*
masm.write_insts_mov_imm64(x1, base_pc);  // x1 = base_pc
masm.write_inst_mov_reg(x2, x28);  // x2 = thread

// 3. 恢复部分 Java 参数到寄存器 (x3-x7)
masm.write_inst("ldp x3, x4, [sp, #16]");  // x3, x4 = 原始 x2, x3
masm.write_inst("ldp x5, x6, [sp, #32]");  // x5, x6 = 原始 x4, x5
masm.write_inst("ldr x7, [sp, #48]");      // x7 = 原始 x6

// 4. 跳转到 LLVM 代码
masm.write_inst_b(llvm_entry);
```

**问题**：
- 适配器只恢复了部分寄存器（x3-x7），但丢失了 x0, x1（原始 Java 参数）
- **关键问题**：适配器没有将 Java 方法参数存储到栈帧的 locals 区域
- Yuhu 的 LLVM 代码期望从栈帧读取参数，而不是从寄存器

## 根本原因分析

### 1. Yuhu 的栈帧布局

从 `yuhuStack.cpp:90-126` 可以看到栈帧的布局（从低地址到高地址）：

```
offset = 0
├─ stack_slots_offset (表达式栈)
├─ monitors_slots_offset (监视器)
├─ oop_tmp_slot_offset (临时 oop)
├─ method_slot_offset (Method*)
├─ unextended_sp (未扩展的 SP)
├─ pc_slot_offset (PC)
├─ frame_marker (帧标记)
├─ frame_pointer_addr (帧指针地址)
└─ locals_slots_offset (局部变量区域) ← 参数应该存储在这里
```

### 2. 局部变量的偏移计算

从 `yuhuStateScanner.cpp:68-72` 可以看到局部变量的扫描逻辑：

```cpp
for (int i = 0; i < max_locals(); i++) {
  process_local_slot(
    i,
    state->local_addr(i),
    stack()->locals_slots_offset() + max_locals() - 1 - i);  // ← 关键！
}
```

**偏移计算公式**：
- `local[i]` 的偏移 = `locals_slots_offset + max_locals() - 1 - i`

对于 `multiply([[I[[I[[II)V` 方法：
- `max_locals() = 4` (a, b, c, n)
- `arg_size() = 4`

局部变量在栈帧中的位置（从高地址到低地址）：
- `local[0]` (a) → `locals_slots_offset + 3`
- `local[1]` (b) → `locals_slots_offset + 2`
- `local[2]` (c) → `locals_slots_offset + 1`
- `local[3]` (n) → `locals_slots_offset + 0`

### 3. YuhuNormalEntryCacher 如何读取参数

从 `yuhuCacheDecache.cpp:218-232` 可以看到：

```cpp
void YuhuCacher::process_local_slot(int index, YuhuValue** addr, int offset) {
  YuhuValue *value = *addr;
  if (local_slot_needs_read(index, value)) {
    *addr = YuhuValue::create_generic(
      value->type(),
      read_value_from_frame(  // ← 从栈帧读取！
        YuhuType::to_stackType(value->basic_type()),
        adjusted_offset(value, offset)),
      value->zero_checked());
  }
}
```

`read_value_from_frame` 使用 `stack()->slot_addr(offset, type)` 从栈帧读取值。

### 4. 问题总结

**核心问题**：
1. 适配器代码是纯汇编，在 LLVM 代码执行之前运行
2. 适配器无法直接访问 LLVM 分配的栈帧
3. 适配器只恢复了寄存器，但没有将参数写入栈帧的 locals 区域
4. Yuhu 的 LLVM 代码在 `YuhuNormalEntryCacher::scan()` 时从栈帧读取参数
5. 由于栈帧中 locals 区域未初始化，读取到的是垃圾值或 NULL，导致空指针检查失败

## 解决方案（最新）

### 思路
- 适配器不分配 Yuhu 栈帧；栈帧完全由 LLVM prologue 分配。
- 适配器只打包参数并把“参数基址 + 参数个数”传给 LLVM。
- LLVM 入口（`YuhuNormalEntryCacher`）从参数基址读取 Java 实参，写入 locals 区域。

### 适配器行为（generate_normal_adapter_into）
- 保存 x0–x7 到一块 64 字节、16 字节对齐的缓冲区（位于当前 sp 向下）。
- 设置寄存器：
  - `x0 = Method*`
  - `x1 = base_pc`（`adr` 到适配器起始 label，适配 CodeCache 重定位）
  - `x2 = thread*`
  - `x3 = arg_base`（上面缓冲区的地址）
  - `x4 = arg_count`（ConstMethod::size_of_parameters）
- 不做 `sub sp, sp, frame_size`；不写 locals。
- 跳转到 LLVM 入口，LLVM prologue 再分配自己的帧；arg_base 位于更高地址，不会被覆盖。
- 当前支持 arg_count ≤ 8（寄存器参数）；栈上传递的更大参数后续扩展。

### LLVM 入口处理
- 函数签名扩展为 `(Method*, base_pc, thread*, arg_base, arg_count)`。
- `YuhuFunction::initialize` 记录 `arg_base/arg_count`。
- `YuhuNormalEntryCacher::process_local_slot` 重写：
  - 对 `i < arg_size()`：从 `arg_base + i*8` 加载值（按 `to_stackType` 类型加载）。
  - 写入 `slot_addr(locals_slots_offset + max_locals - 1 - i)`，确保后续 locals 读取正确。
  - 更新 `local[i]` 的 `YuhuValue`。

### base_pc 修复（保留）
- 继续使用 `adr` 计算 `base_pc`，避免临时地址失效。

### 后续事项
- 支持 arg_count > 8 时从调用者栈复制（待做）。
- 回归测试多参数方法、重定位、异常路径。 

## base_pc 地址计算问题修复

### 问题描述

在实现适配器时，发现 `base_pc` 参数存在一个严重问题：

**问题**：适配器使用 `mov` + `movk` 序列加载 `base_pc` 的绝对地址（编译时的 BufferBlob 地址），但 `register_method` 会将代码复制到 CodeCache 中的 nmethod，导致：
- `base_pc` 指向的是编译时的临时地址，而不是 nmethod 的真实 `code_begin()`
- 所有基于 `base_pc` 的偏移计算（异常表、safepoint、调试信息）都会错位
- 这会导致运行时错误，如异常处理失败、调试信息不正确等

### 修复方案

**解决方案**：使用 PC 相对地址计算（`adr` 指令）而不是绝对地址。

**实现**：
1. 在适配器开头创建一个标签 `base_pc_label`
2. 使用 `adr x1, base_pc_label`（普通入口）或 `adr x2, base_pc_label`（OSR 入口）计算标签地址
3. 这样 `base_pc` 就是适配器开头的地址，也就是 nmethod 的 `code_begin()`

**优势**：
- ✅ `base_pc` 自动适应代码重定位，无需 patch
- ✅ 代码被复制到 CodeCache 后，`adr` 指令计算出的就是正确的地址
- ✅ 所有基于 `base_pc` 的偏移计算都会正确

**修改位置**：
- `generate_normal_adapter_into`: 将 `write_insts_mov_imm64(x1, base_pc)` 改为 `write_inst_adr(x1, base_pc_label)`
- `generate_osr_adapter_into`: 将 `write_insts_mov_imm64(x2, base_pc)` 改为 `write_inst_adr(x2, base_pc_label)`

**注意**：`base_pc` 参数仍然保留在函数签名中（为了兼容性），但实际不再使用。

## 处理超过 8 个参数的情况

### AArch64 调用约定

在 AArch64 的 Java 调用约定中：
- **前 8 个参数**通过寄存器传递（x0-x7）
- **超过 8 个的参数**通过栈传递

### 适配器实现

适配器需要处理两种情况：

1. **寄存器参数（x0-x7）**：
   - 直接保存到临时缓冲区

2. **栈参数（arg[8] 到 arg[arg_count-1]）**：
   - 从调用者的栈帧读取
   - 栈参数位置：`[caller_sp + (arg_index - 8) * 8]`
   - 复制到临时缓冲区

### 实现细节

```cpp
// 1. 保存调用者的 SP（在修改 SP 之前）
masm.write_inst("mov x15, sp");  // x15 = caller's SP

// 2. 计算缓冲区大小：max(8, arg_count) * 8 字节，对齐到 16 字节
masm.write_inst("mov x13, x12");  // x13 = arg_count
masm.write_inst("cmp x13, #8");
masm.write_inst("mov x14, #8");
masm.write_inst("csel x13, x14, x13, lt");  // x13 = max(arg_count, 8)
masm.write_inst("lsl x13, x13, #3");  // 转换为字节
masm.write_inst("add x13, x13, #15");
masm.write_inst("bic x13, x13, #15");  // 对齐到 16 字节

// 3. 分配缓冲区并保存寄存器参数（x0-x7）
masm.write_inst("sub sp, sp, x13");
masm.write_inst("mov x14, sp");  // x14 = arg_base
masm.write_inst("stp x0, x1, [x14, #0]");
// ... 保存 x2-x7 ...

// 4. 如果 arg_count > 8，复制栈参数
masm.write_inst("cmp x12, #8");
// 循环：从 caller_sp + (i - 8) * 8 读取，写入 buffer[i]
```

### 注意事项

1. **栈参数位置**：
   - 对于 i2c adapter 调用：栈参数在 i2c adapter 调整后的 SP 上
   - 对于编译到编译的调用：栈参数在调用者的栈帧中
   - 当前实现假设栈参数在 `[caller_sp + (i - 8) * 8]`，这对大多数情况都适用

2. **缓冲区对齐**：
   - 缓冲区大小必须对齐到 16 字节（AArch64 要求）
   - 确保栈指针在函数调用时保持 16 字节对齐

3. **性能考虑**：
   - 对于参数数量 <= 8 的方法，没有额外开销
   - 对于参数数量 > 8 的方法，需要额外的内存复制操作
   - 这是可以接受的，因为超过 8 个参数的方法相对较少

## 后续工作

1. ✅ 实现方案：适配器不分配栈帧，传递 arg_base/arg_count
2. ✅ 实现超过 8 个参数的处理
3. 测试各种参数数量的方法（0-8+ 个参数）
4. 验证参数传递的正确性
5. 验证 `base_pc` 修复后的异常处理和调试信息是否正确


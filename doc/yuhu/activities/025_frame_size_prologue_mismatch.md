# LLVM Prologue 与 frame_size 不一致问题 (025)

## 问题背景

在使用 Yuhu 编译器生成 AArch64 机器码时，LLVM 自动插入的 prologue 会保存部分 callee-saved 寄存器，并调整栈指针 (SP)。

同时，Yuhu 在注册 `nmethod` 时，需要提供一个 `frame_size`（单位是 words），用于 HotSpot 的栈帧遍历逻辑：

- `sender_sp = unextended_sp + frame_size`
- `sender_pc = *(sender_sp - 1)`  // LR
- `sender_fp = *(sender_sp - 2)`  // saved FP

如果 `frame_size` 没有包含 LLVM prologue 占用的那部分栈空间，HotSpot 会从错误的位置读取 `sender_pc` / `sender_fp`，导致栈帧遍历崩溃。

---

## 具体现象

以 `com/example/Matrix.multiply([[I[[I[[II)V` 为例，LLVM 生成的 prologue 如下：

```asm
stp x20, x19, [sp, #-32]!   ; sp -= 32，保存部分 callee-saved regs
stp x29, x30, [sp, #16]     ; 保存 FP/LR 到 [sp+16]/[sp+24]
add x29, sp, #0x10          ; fp = sp + 16（指向 saved x29）
sub sp, sp, #0x30           ; 进一步为栈帧分配空间
...
; [sp+0xb0]  (sp of caller)
```

可以看到：

- LLVM prologue 至少保存了：`x20, x19, x29, x30`
- 这 4 个寄存器总共占用 **32 字节 = 4 words** 的栈空间
- HotSpot 的注释 `[sp+0xb0] (sp of caller)` 表示：
  - 当前 `sp + 0xb0` 指向 caller 的 SP
  - 因此：`frame_size_bytes(actual) = 0xb0`

如果 Yuhu 只把自己的 frame body（locals + stack + header）算进 `frame_size`，而忽略了这 4 个 word 的 prologue 区域，就会导致：

- HotSpot 认为 `sender_sp = unextended_sp + frame_size` 处是 caller 的 SP
- 实际上那里仍在当前帧内部
- 于是从错误位置读取 `sender_pc`，最终在 `frame::frame` 中触发崩溃。

---

## 原始实现中的问题

### yuhuStack.cpp

Yuhu 在 [`yuhuStack.cpp`](file:///Users/liuanyou/CLionProjects/jdk8/hotspot/src/share/vm/yuhu/yuhuStack.cpp) 中，计算 frame body 大小：

```cpp
int header_words  = 6;
int monitor_words = max_monitors()*frame::interpreter_frame_monitor_size();
int stack_words   = max_stack();
int frame_words   = header_words + monitor_words + stack_words;

_extended_frame_size = frame_words + locals_words;

int frame_size_bytes = extended_frame_size() * wordSize;
frame_size_bytes = align_size_up(frame_size_bytes, 16);
```

这里的 `frame_size_bytes` 只包含：

- header + monitors + stack + locals
- **不包含** LLVM prologue 保存 callee-saved 寄存器的那部分空间。

### yuhuCompiler.cpp

在 [`yuhuCompiler.cpp`](file:///Users/liuanyou/CLionProjects/jdk8/hotspot/src/share/vm/yuhu/yuhuCompiler.cpp) 中，注册 `nmethod` 时计算 `frame_size`：

```cpp
int header_words = 6;
...
int frame_words   = header_words + monitor_words + stack_words;
int frame_size    = frame_words + locals_words + llvm_prologue_words;
frame_size        = align_size_up(frame_size, 2);  // 对齐到 16 bytes
```

其中：

- `llvm_prologue_words` 最初只按 2 words（x29/x30）来估计
- 但实际 prologue 可能保存更多寄存器（例如 x20/x19）
- 导致 HotSpot 使用的 `frame_size` 小于真实的帧大小

---

## 当前状态与约束

1. **LLVM prologue 的大小不是固定的**：
   - 取决于函数中实际使用的 callee-saved 寄存器
   - 可能是 2 words（只保存 x29/x30），也可能是 4 words（再加上 x20/x19），甚至更多

2. **我们目前不修改 LLVM 后端代码**：
   - 不在 `AArch64RegisterInfo::getReservedRegs` 中强制保留 x19-x28
   - 不自定义 calling convention

3. **Yuhu 侧的简单修复**：
   - 暂时将 `llvm_prologue_words` 设为 4，以匹配当前观测到的 prologue 行为
   - 这在当前版本 LLVM 和当前 IR 生成模式下是正确的
   - 但从长期看，这只是一个保守估计，而非严格保证

---

## 未来可能的改进方向

### 方向 1：修改 LLVM 后端（推荐长期方案）

在 LLVM 的 AArch64 backend 中：

- 在 `AArch64RegisterInfo::getReservedRegs(const MachineFunction &MF)` 中，为 Yuhu 模式增加：
  - 将 x19-x28 标记为 reserved
  - 确保 prologue 只保存 x29/x30（固定 16 bytes）
- 通过 `-mattr=+yuhu` 或类似方式启用该模式

优点：

- Prologue 大小固定为 2 words，`frame_size` 计算简单可靠
- 不需要 runtime 分析或 patch 机器码

缺点：

- 需要修改 LLVM 源码并重新编译 LLVM

### 方向 2：编译后分析 prologue + 调整 frame_size（复杂但不改 LLVM）

思路：

1. 先使用一个占位的 `llvm_prologue_words`（例如 4）编译生成机器码
2. 在 `generate_native_code` 之后，读取 `entry->code_start()`，对 prologue 区域进行反汇编
3. 计算实际的 prologue 栈空间（例如通过匹配 `stp` / `sub sp, ...` 指令）
4. 得到 `actual_prologue_words`，计算 `final_frame_size = frame_body_words + actual_prologue_words`
5. 在调用 `env->register_method(..., frame_size, ...)` 之前，使用 `final_frame_size` 替换原始值

前提：

- `frame_size` 只作为 HotSpot 元数据使用，不嵌入到 LLVM 生成的指令中

优点：

- 不需要修改 LLVM，也不需要重新编译 LLVM
- 能够自适应不同 prologue 大小

缺点：

- 需要实现 AArch64 机器码的 prologue 解析逻辑
- 对 LLVM prologue 形状的假设仍然是启发式的，可能随 LLVM 升级需要调整

---

## 小结

- **问题本质**：LLVM 自动 prologue 使用的栈空间必须计入 HotSpot 的 `frame_size`，否则栈帧遍历会从错误位置读取 `sender_pc` / `sender_fp`。
- **当前临时方案**：将 `llvm_prologue_words` 设为 4，与当前观测到的 `stp x20,x19` + `stp x29,x30` prologue 对齐，避免栈遍历崩溃。
- **后续优化方向**：
  - 要么在 LLVM 后端中为 Yuhu 定制寄存器保留策略，使 prologue 固定为 2 words；
  - 要么在编译后对 prologue 进行解析，动态修正 `frame_size`。
# 037: Yuhu Deoptimization Crash - LLVM Spill Space 导致 frame_size 计算错误

**Date**: 2026-01-24 ~ 2026-01-25
**Status**: 🟢 Fixed - 已找到根因并实现修复
**Related Issues**:
1. Yuhu 编译的 encodeArrayLoop 方法在去优化时崩溃
2. 去优化时 locals 数组为空导致断言失败

## 问题摘要

### 问题 1: frame_size 计算错误

当 Yuhu 编译的 `sun.nio.cs.UTF_8$Encoder.encodeArrayLoop` 触发去优化时，JVM 崩溃：

```
Internal Error (frame_aarch64.inline.hpp:99)
assert(pc != NULL) failed: no pc?
```

### 问题 2: locals 数组为空

修复问题 1 后，发现新的崩溃：

```
Internal Error (vframeArray.cpp:427)
assert(method()->max_locals() == locals()->size()) failed: just checking
method()->max_locals() = 12, locals()->size() = 0
```

## 根本原因

### 问题 1: LLVM Spill Space 没有被计算

**LLVM 生成的 spill 空间没有被 `YuhuPrologueAnalyzer` 计算到 frame_size 中！**

### 问题详解

#### 1. encodeArrayLoop 的 LLVM Prologue

```asm
stp	x28, x27, [sp, #-96]!   ; LLVM: 分配 96 字节保存 callee-saved
stp	x26, x25, [sp, #16]
stp	x24, x23, [sp, #32]
stp	x22, x21, [sp, #48]
stp	x20, x19, [sp, #64]
stp	x29, x30, [sp, #80]     ; LLVM: 保存 FP 和 LR
add	x29, sp, #0x50          ; LLVM: 设置 FP
sub	sp, sp, #0x40           ; LLVM: spill 空间！64 字节
```

#### 2. Matrix.multiply 的 LLVM Prologue（对比）

```asm
stp	x28, x27, [sp, #-96]!   ; LLVM: 分配 96 字节
stp	x26, x25, [sp, #16]
stp	x24, x23, [sp, #32]
stp	x22, x21, [sp, #48]
stp	x20, x19, [sp, #64]
stp	x29, x30, [sp, #80]     ; LLVM: 保存 FP 和 LR
add	x29, sp, #0x50          ; LLVM: 设置 FP
mov	x8, sp                  ; 没有 spill 空间！
```

#### 3. YuhuPrologueAnalyzer 的 Bug

**原始代码** (`yuhuPrologueAnalyzer.cpp:29-32`):
```cpp
// If we hit a sub sp, sp, #N instruction, that's Yuhu's frame allocation, not prologue
else if (is_sub_sp_imm(inst)) {
  break;  // End of LLVM prologue ← 直接停止，没有计算 64 字节！
}
```

**导致的问题**：
- `analyze_prologue_stack_bytes()` 返回：96 字节
- **实际 LLVM 分配了**：96 + 64 = 160 字节
- **少计算了**：64 字节 = 8 words

#### 4. frame_size 计算错误

在 `yuhuCompiler.cpp:589`:
```cpp
int frame_size = frame_words + locals_words + actual_prologue_words;
// encodeArrayLoop: 12 + 12 + 12 = 36 words ❌
// 应该是: 12 + 12 + 20 = 44 words ✅
```

其中 `actual_prologue_words = 96/8 = 12`，应该是 `160/8 = 20`！

#### 5. 去优化时崩溃

在 `frame::sender_for_compiled_frame` 中：
```cpp
intptr_t* l_sender_sp = unextended_sp() + _cb->frame_size();
address sender_pc = (address) *(l_sender_sp-1);
```

- 使用错误的 `frame_size = 36 words`
- 实际应该是 `frame_size = 44 words`
- `l_sender_sp` 计算错误，指向了错误的内存位置
- 读取到 NULL 的返回地址 → 崩溃！

## 调查历史

### 第一阶段：怀疑 LR 没有保存

**错误假设**：Yuhu 的 prologue 没有保存 LR (x30)

**纠正**：查看汇编代码发现 LR 确实被保存了：
```asm
stp	x29, x30, [sp, #80]  ; FP 和 LR 都被保存
```

### 第二阶段：分析去优化过程

**发现**：
- Matrix.multiply 正常工作（没有触发去优化）
- encodeArrayLoop 触发去优化时崩溃
- 问题在 `DeoptimizationBlob` 的处理中

**但不完整**：没有意识到是 frame_size 计算错误

### 第三阶段：发现 LLVM spill 空间

**关键发现**：
- encodeArrayLoop 有额外的 `sub sp, sp, #0x40` 指令
- multiply 没有这条指令
- 这是 LLVM 的寄存器分配器自动生成的 spill 空间

**根本原因**：
`YuhuPrologueAnalyzer` 在遇到 `sub sp, sp, #N` 时就停止扫描，没有把这部分空间计算到 prologue_bytes 中。

## 解决方案

### 修改 YuhuPrologueAnalyzer

修改 `yuhuPrologueAnalyzer::analyze_prologue_stack_bytes()` 函数：

**修改前**：
```cpp
else if (is_sub_sp_imm(inst)) {
  break;  // 停止扫描，丢失 spill 空间
}
```

**修改后**：
```cpp
// 检查是否设置了 FP
bool found_frame_setup = false;

// 扫描过程中
if (is_add_x29_sp_imm(inst)) {
  found_frame_setup = true;  // 检测到 FP 设置
}
else if (is_sub_sp_imm(inst)) {
  int imm = extract_sub_sp_immediate(inst);
  total_prologue_bytes += imm;  // 累加 spill 空间！
  if (found_frame_setup) {
    break;  // 在 FP 设置后停止
  }
}
```

### 新增 extract_sub_sp_immediate 函数

添加函数来提取 `sub sp, sp, #imm` 的立即数：

```cpp
int YuhuPrologueAnalyzer::extract_sub_sp_immediate(uint32_t inst) {
  // imm12 is bits [21:10], scaled by shift amount
  int imm12 = (inst >> 10) & 0xFFF;

  // Shift is bits [23:22]
  int shift_val = (inst >> 22) & 0x3;

  // Apply scaling based on shift
  switch (shift_val) {
    case 0:  // LSL #0
      return imm12;
    case 1:  // LSL #12
      return imm12 << 12;
    default:
      return imm12;
  }
}
```

## 修改的文件

1. **hotspot/src/share/vm/yuhu/yuhuPrologueAnalyzer.cpp**
   - 修改 `analyze_prologue_stack_bytes()` 函数
   - 添加 `extract_sub_sp_immediate()` 函数

2. **hotspot/src/share/vm/yuhu/yuhuPrologueAnalyzer.hpp**
   - 添加 `extract_sub_sp_immediate()` 声明

## 修复后的效果

### encodeArrayLoop 的 frame_size

**修复前**：
```
LLVM prologue: 96 字节（不包括 spill）
actual_prologue_words = 12
frame_size = 12 + 12 + 12 = 36 words ❌
```

**修复后**：
```
LLVM prologue: 96 + 64 = 160 字节（包括 spill）
actual_prologue_words = 20
frame_size = 12 + 12 + 20 = 44 words ✅
```

### 栈帧布局验证

```
LLVM Prologue (160 字节 = 20 words):
  stp x28, x27, [sp, #-96]!   ← 96 字节
  stp x29, x30, [sp, #80]
  add x29, sp, #0x50
  sub sp, sp, #0x40           ← 64 字节 spill（现在被计算）

Yuhu Frame Body (192 字节 = 24 words):
  sub x25, sp, #0xc0          ← 192 字节

Total frame_size = 20 + 24 = 44 words ✅
```

去优化时的栈展开现在能正确计算 sender 的位置！

## 为什么需要 spill 空间？

### Java 局面 vs LLVM IR 层面

**Java 局面**：
- `locals_words = 12`：字节码的局部变量（包括参数）

**LLVM IR 层面**：
- 编译过程中产生大量临时变量
- 数组索引、边界检查、循环变量等
- 这些临时变量需要寄存器

### 为什么 encodeArrayLoop 需要 spill，multiply 不需要？

- **encodeArrayLoop**：复杂的方法，LLVM 需要更多寄存器
  - 即使包括 callee-saved x19-x28（10个寄存器）也不够
  - LLVM 在栈上分配 spill 空间来存储临时值
  - 生成 `sub sp, sp, #0x40` 分配 64 字节

- **multiply**：相对简单，寄存器够用
  - 不需要额外的 spill 空间
  - 没有 `sub sp, sp, #N` 指令

### 为什么 spill 在 prologue 中？

**LLVM 的标准做法**：
1. 保存 callee-saved 寄存器
2. 设置 frame pointer
3. **然后**分配 spill 空间（如果需要）

这确保：
- Spill 空间在函数入口就分配好
- 在整个函数执行期间可用
- 在函数返回时自动释放（恢复 SP）

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

运行后应该看到：
```
Yuhu: Prologue analysis - code_start=<addr>, prologue_bytes=160, prologue_words=20
Yuhu: frame_size=44 words (header=6, monitor=0, stack=6, extra_locals=9)
```

去优化时不再崩溃！

### 4. 对比测试

**修复前**：
```
sender_pc=0x0 (from [l_sender_sp-1])  ← NULL！
#  A fatal error has been detected by the Java Runtime Environment:
#  Internal Error (frame_aarch64.inline.hpp:99)
```

**修复后**：
```
sender_pc=0x<valid_address> (from [l_sender_sp-1])  ← 正确的返回地址
# 正常完成去优化，切换到解释器执行
```

## 相关文件

### 修改的文件
- `hotspot/src/share/vm/yuhu/yuhuPrologueAnalyzer.cpp`
- `hotspot/src/share/vm/yuhu/yuhuPrologueAnalyzer.hpp`

### 相关的 HotSpot 文件
- `hotspot/src/cpu/aarch64/vm/frame_aarch64.cpp` - Frame unwinding 逻辑
- `hotspot/src/cpu/aarch64/vm/frame_aarch64.inline.hpp` - Frame 构造函数
- `hotspot/src/share/vm/runtime/deoptimization.cpp` - 去优化实现
- `hotspot/src/cpu/aarch64/vm/sharedRuntime_aarch64.cpp` - DeoptimizationBlob 生成

### Yuhu 相关文件
- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - frame_size 计算
- `hotspot/src/share/vm/yuhu/yuhuStack.cpp` - 栈帧初始化
- `hotspot/src/share/vm/yuhu/yuhuFunction.cpp` - LLVM 函数签名

## 相关文档

- `doc/yuhu/activities/035_deoptimization_scope_desc_missing.md` - 之前的去优化问题
- `doc/yuhu/activities/036_fix_x28_thread_register_corruption.md` - 寄存器保存问题
- `doc/yuhu/YUHU_FRAME_LAYOUT_DESIGN.md` - Yuhu 栈帧设计
- `doc/yuhu/activities/028_ir_generation_flow.md` - LLVM IR 生成流程

## 经验总结

1. **LLVM 会自动生成 spill 空间**
   - 不能假设 LLVM 只做标准 prologue
   - 必须考虑编译器的优化行为

2. **Prologue 分析必须完整**
   - 不能在遇到 `sub sp, sp, #N` 时就停止
   - 需要区分 LLVM 的 spill 和 Yuhu 的 frame allocation

3. **frame_size 必须准确**
   - 去优化、GC、异常处理都依赖正确的 frame_size
   - 少计算会导致栈展开错误
   - 多计算会导致栈溢出

4. **对比不同方法的汇编**
   - multiply 和 encodeArrayLoop 的差异很关键
   - 通过对比发现了 spill 空间的存在

5. **从崩溃点反推**
   - sender_pc = NULL 是关键线索
   - 反向追踪 frame_size 的计算链
   - 最终找到 analyzer 的 bug

## 下一步工作

1. **测试更多方法**
   - 确保所有 Yuhu 编译的方法都能正确去优化
   - 特别关注复杂的方法（可能有更多 spill）

2. **监控 frame_size**
   - 添加日志输出 frame_size 的计算过程
   - 验证 prologue 分析的准确性

3. **考虑禁用 spill（可选）**
   - 如果 spill 影响性能，可以配置 LLVM 使用更多寄存器
   - 或者调整编译选项来减少 spill

4. **完善自动化测试**
   - 添加去优化的回归测试
   - 确保未来不会引入类似问题

---

## 问题 2: locals 数组为空导致去优化失败

### 新的错误表现

修复了 frame_size 问题后，发现新的崩溃（`hs_err_pid70207.log`）：

```
# A fatal error has been detected by the Java Runtime Environment:
#  Internal Error (/Users/liuanyou/CLionProjects/jdk8/hotspot/src/share/vm/runtime/vframeArray.cpp:427)
#  assert(method()->max_locals() == locals()->size()) failed: just checking
#
# method()->max_locals() = 12
# locals()->size() = 0
```

### 根本原因

**Yuhu 没有为 `describe_scope` 提供 locals 数组！**

#### 问题详解

1. **Yuhu 的 debug 信息记录**

在 `yuhuFunction.cpp:483-492`，Yuhu 调用 `describe_scope`：

```cpp
_debug_info_recorder->describe_scope(
  pc_offset,
  target(),
  0,
  false, false, false,
  (GrowableArray<ScopeValue*>*)NULL,  // ← locals 是 NULL！
  (GrowableArray<ScopeValue*>*)NULL,  // ← expressions 是 NULL！
  (GrowableArray<MonitorValue*>*)NULL);
```

2. **ScopeDesc 的处理流程**

```
YuhuFunction::process_deferred_oopmaps()
  └─> describe_scope(NULL, NULL, NULL)
      └─> DebugInformationRecorder 记录到 stream
          └─> nmethod 创建
              └─> 去优化时 ScopeDesc 读取
                  └─> decode_scope_values() 返回 NULL
                      └─> locals()->size() = 0
                          └─> assert failed!
```

3. **去优化时的断言**

在 `vframeArray.cpp:427`：

```cpp
int vframeArrayElement::on_stack_size(...) const {
  assert(method()->max_locals() == locals()->size(), "just checking");
  // ↑ max_locals = 12, locals()->size() = 0
  int locks = monitors() == NULL ? 0 : monitors()->number_of_monitors();
  int temps = expressions()->size();
  // ...
}
```

### 为什么需要 locals 数组？

去优化时，JVM 需要：
1. **恢复局部变量**：从编译帧的栈位置读取局部变量的值
2. **构建解释器帧**：将局部变量复制到解释器帧
3. **继续执行**：在解释器中从正确的 bci 继续执行

如果没有 locals 信息：
- JVM 不知道每个局部变量在栈上的位置
- 无法正确恢复局部变量的值
- 去优化后的行为不确定

### 解决方案

#### 修改 yuhuFunction.cpp

**修改前**：
```cpp
_debug_info_recorder->describe_scope(
  pc_offset,
  target(),
  0,
  false, false, false,
  (GrowableArray<ScopeValue*>*)NULL,
  (GrowableArray<ScopeValue*>*)NULL,
  (GrowableArray<MonitorValue*>*)NULL);
```

**修改后**：
```cpp
// Build locals array for ScopeDesc
int max_locals = target()->max_locals();
GrowableArray<ScopeValue*>* locals = new GrowableArray<ScopeValue*>(max_locals);

// Create a LocationValue for each local variable
// Yuhu stores locals in an interpreter-compatible frame layout
// The offset for local[i] follows the interpreter convention:
//   local_offset_in_bytes(i) = -(i * wordSize)
for (int i = 0; i < max_locals; i++) {
  int stack_offset = -(i * wordSize);
  Location loc = Location::new_stk_loc(Location::normal, stack_offset);
  locals->append(new LocationValue(loc));
}

// Create DebugTokens
DebugToken* locals_token = _debug_info_recorder->create_scope_values(locals);
DebugToken* expressions_token = _debug_info_recorder->create_scope_values(NULL);
DebugToken* monitors_token = _debug_info_recorder->create_monitor_values(NULL);

_debug_info_recorder->describe_scope(
  pc_offset,
  target(),
  0,
  false, false, false,
  locals_token,
  expressions_token,
  monitors_token);
```

### 修改的文件（问题 2）

1. **hotspot/src/share/vm/yuhu/yuhuFunction.cpp**
   - 添加 `#include "code/debugInfo.hpp"`
   - 添加 `#include "code/location.hpp"`
   - 修改 `process_deferred_oopmaps()` 函数，创建 locals 数组

### 为什么使用解释器 convention？

根据 `Interpreter::local_offset_in_bytes()` 的定义：

```cpp
static int local_offset_in_bytes(int n) {
  return ((frame::interpreter_frame_expression_stack_direction() * n) * stackElementSize);
}
```

在 AArch64 上，`interpreter_frame_expression_stack_direction() = -1`，所以：

- `local[0]` 的 offset = `-(0 * wordSize) = 0`
- `local[1]` 的 offset = `-(1 * wordSize) = -wordSize`
- `local[2]` 的 offset = `-(2 * wordSize) = -2*wordSize`

这个约定与解释器帧的布局一致，确保去优化时能正确找到局部变量。

### 测试步骤（问题 2）

#### 1. 编译修改后的代码

```bash
cd /Users/liuanyou/CLionProjects/jdk8
make clean
make configure
make
```

#### 2. 运行测试

```bash
cd test_yuhu
java -XX:+UseYuhuCompiler \
     -XX:TieredStopAtLevel=6 \
     -XX:CompileCommand=yuhuonly,sun.nio.cs.*::* \
     -cp . \
     TestEncoder
```

#### 3. 验证修复

运行后应该看到：
```
Yuhu: Added OopMap to YuhuDebugInformationRecorder at virtual pc_offset=XXX with 12 locals
```

去优化时不再崩溃！断言通过：
```
assert(method()->max_locals() == locals()->size()) succeeded
// max_locals() = 12, locals()->size() = 12 ✅
```

### 相关文件（问题 2）

#### 修改的文件
- `hotspot/src/share/vm/yuhu/yuhuFunction.cpp` - 创建 locals 数组

#### 相关的 HotSpot 文件
- `hotspot/src/share/vm/code/debugInfo.hpp` - ScopeValue 定义
- `hotspot/src/share/vm/code/location.hpp` - Location 类
- `hotspot/src/share/vm/code/debugInfoRec.hpp` - DebugInformationRecorder
- `hotspot/src/share/vm/code/scopeDesc.cpp` - ScopeDesc 读取
- `hotspot/src/share/vm/runtime/vframeArray.cpp` - 去优化时使用 ScopeDesc

### 经验总结（问题 2）

1. **ScopeDesc 是去优化的关键**
   - 必须提供完整的 locals、expressions、monitors 信息
   - 否则去优化时无法恢复执行状态

2. **DebugInformationRecorder 的使用**
   - `describe_scope` 需要 `DebugToken*`，不是直接的 `GrowableArray*`
   - 必须先用 `create_scope_values()` 转换

3. **Location 的约定**
   - 栈位置使用 `Location::new_stk_loc(Type, offset)` 创建
   - offset 是字节数，会被除以 `LogBytesPerInt`

4. **解释器兼容的重要性**
   - Yuhu 使用解释器兼容的栈帧布局
   - locals 的 offset 必须遵循解释器 convention
   - 这样去优化时才能正确映射变量

### 综合：两个问题的关联

这两个问题都源于 Yuhu 在 debug 信息生成上的不完整：

| 问题 | 根因 | 影响 |
|------|------|------|
| **问题 1: frame_size 错误** | PrologueAnalyzer 没有计算 LLVM spill 空间 | 栈展开时 sender_sp 计算错误 |
| **问题 2: locals 为空** | describe_scope 没有提供 locals 数组 | 去优化时无法恢复局部变量 |

修复这两个问题后，Yuhu 的去优化流程现在应该能正常工作了！

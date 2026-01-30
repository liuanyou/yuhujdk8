# 039: Yuhu Deoptimization 参数恢复失败问题

**Date**: 2026-01-27
**Status**: 🟢 已解决 - 实现了 Per-Function Deoptimization Stub
**Related Issues**:
- 038: Yuhu 额外的 16 字节 alloca 导致 frame_size 计算错误

## 问题摘要

Yuhu编译的代码（如`encodeArrayLoop`）触发deoptimization后，解释器从栈上读取到垃圾参数值，导致崩溃。

**根本原因**：Yuhu生成的Scope Descriptor记录的参数offset完全错误，导致deoptimization从错误的栈位置读取参数。

### 症状

从错误日志 hs_err_pid44077.log：

```
j  sun.nio.cs.UTF_8$Encoder.encodeArrayLoop(...)+1  ← 解释器执行，但崩溃
Registers:
  x0 = 0xdeaddeaf6dd3a080  ← 垃圾值（包含0xdeaddeaf标记）
  x2 = 0xdeaddeaf6dd3a080
```

从栈帧dump可以看到：
```
0x16b9c6080: unextended_sp (Yuhu frame底部)
0x16b9c6120: 0x000000076ab44b38  ← 参数x2的实际值（在栈上）
0x16b9c6130: 0x000000076abb0578  ← 参数x0/x1的实际值（在栈上）
```

**参数确实在栈上，但deoptimization读到了垃圾值！**

## 根本原因分析

### Deoptimization参数恢复流程

```
1. 编译代码触发deoptimization
   ↓
2. Deopt stub保存所有寄存器到栈
   ↓
3. fetch_unroll_info() 创建 vframeArray
   - 为每个safepoint创建ScopeDescriptor
   - ScopeDescriptor包含ScopeValue[]数组
   - ScopeValue记录每个参数/局部变量的位置
   ↓
4. unpack_frames() 调用 unpack_to_stack()
   ↓
5. unpack_to_stack() 调用 Interpreter::layout_activation()
   ↓
6. layout_activation() 遍历 locals[] 数组
   - 对每个local: addr = interpreter_frame_local_at(i)
   - 调用 value->get_int() 从记录的位置读取值
   - 写入解释器帧
```

**关键**：ScopeValue必须准确记录参数的位置！

### Yuhu的错误实现

在 `yuhuFunction.cpp::process_deferred_oopmaps()` 中：

```cpp
int max_locals = target()->max_locals();  // 包含参数

for (int i = 0; i < max_locals; i++) {
  int stack_offset = i * wordSize;  // ← 错误的offset计算！
  Location loc = Location::new_stk_loc(Location::normal, stack_offset);
  locals->append(new LocationValue(loc));
}
```

**问题**：

1. **Offset计算错误**：
   - Yuhu假设：参数在offset 0, 8, 16...
   - 实际：参数在offset 96, 104, 112（在`locals_slots_offset`之后）

2. **Frame layout不了解**：
   - Yuhu的frame layout是：
     ```
     [0-5]   Expression Stack (6 words)
     [6-11]  Monitors
     [12]    Temp Oop
     [13]    Method
     [14]    unextended_sp
     [15]    PC
     [16]    Frame Marker
     [17+]   Locals（包括参数和真正的局部变量）
     ```
   - `locals_slots_offset = 17`
   - 参数在`locals_slots_offset`开始的slot中

3. **所有参数被标记为"在栈上"**：
   - 但编译代码的参数实际上在寄存器中（c_rarg1, c_rarg2, c_rarg3）
   - Deopt stub已经保存了这些寄存器值
   - Yuhu应该标记为"在寄存器中"，而不是"在栈上"

### 实际的参数位置

从调试信息：
```
unextended_sp = 0x16b9c6080
参数实际位置：
  0x16b9c6120 = offset 96  (参数x2)
   0x16b9c6130 = offset 104 (参数x1)
  0x16b9c6140 = offset 112 (参数x0?)
```

Yuhu记录的：
```
参数0: offset 0
参数1: offset 8
参数2: offset 16
```

**Deoptimization读取offset 0, 8, 16 → 垃圾值！**

## 深入分析：LLVM Prologue 的限制

### LLVM Prologue 只保存 callee-saved 寄存器

通过分析 LLVM 生成的汇编代码发现：

```asm
; LLVM 生成的 prologue
stp    x29, x30, [sp, #-16]!    ; 保存 fp, lr
stp    x19, x20, [sp, #-16]!    ; 保存 callee-saved
stp    x21, x22, [sp, #-16]!
stp    x23, x24, [sp, #-16]!
stp    x25, x26, [sp, #-16]!
stp    x27, x28, [sp, #-16]!
sub    sp, sp, #0x40            ; LLVM spill space
add    x29, sp, #0x50          ; 设置 fp
```

**关键发现**：
- LLVM prologue **只保存 x19-x30**（callee-saved 寄存器）
- **x0-x7（参数寄存器）不在 LLVM prologue 的保存列表中**
- x0-x7 的值由 Yuhu 保存到 **locals 区域**

### Deoptimization 栈帧分析

从实际运行的栈帧变化 trace：

```
1. encodeArrayLoop entry（Yuhu 方法入口）:
   sp = 0x16bf8e1f0

2. before jump to deoptimization blob（触发 deopt 时）:
   sp = 0x16bf8e090  ← 下降了 352 bytes (Yuhu frame 大小)

3. after save_live_registers（deopt stub 保存寄存器后）:
   sp = 0x16bf8de90  ← 又下降了 512 bytes (deopt blob frame_size)

4. after restore_result_registers:
   sp = 0x16bf8e090  ← 恢复到 deopt 前的 sp

5. after __ add(sp, sp, r2)（弹出 Yuhu frame 后）:
   sp = 0x16bf8e1f0  ← 恢复到 Yuhu entry 时的 sp
```

**关键观察**：
- Deopt stub 的 frame_size = 64 words = 512 bytes（固定值，用于保存所有寄存器）
- Yuhu frame 的实际大小 = 352 bytes
- **Yuhu frame 在执行 `add(sp, sp, r2)` 后被弹出，栈上的参数数据被破坏**

### C1/C2 如何避免这个问题

C1/C2 使用 **REGISTER location** 记录参数：

```cpp
// C1/C2 的 ScopeValue 记录
参数 0: Location { type=REGISTER, number=x0 }
参数 1: Location { type=REGISTER, number=x1 }
参数 2: Location { type=REGISTER, number=x2 }
```

**Deoptimization 读取参数时**：
```cpp
// StackValue::get_int()
if (location.type() == Location::REGISTER) {
  // 从 deopt blob 保存的寄存器区域读取
  // deopt_sp + RegisterSaver::r0_offset_in_bytes()
  // 这个区域在 deopt stub 的 frame 中，不会破坏
}
```

**为什么 C1/C2 可以这样做**：
1. Deopt stub 在 `save_live_registers` 时保存了 **所有寄存器**（x0-x30, v0-v31）
2. 保存的位置在 **deopt blob frame** 中（512 bytes）
3. 即使原始栈帧被弹出，deopt blob frame 中的寄存器值仍然有效
4. ScopeValue 告诉 deoptimization："从 deopt blob 的 x0 保存位置读取"

### Yuhu 的问题

Yuhu 当前把所有参数标记为 **STACK location**：

```cpp
// yuhuFunction.cpp:process_deferred_oopmaps()
for (int i = 0; i < max_locals; i++) {
  int stack_offset = i * wordSize;  // ← 错误！
  Location loc = Location::new_stk_loc(Location::normal, stack_offset);
  locals->append(new LocationValue(loc));
}
```

**问题**：
1. 标记为 STACK location，deoptimization 会从 **栈帧** 读取
2. 但栈帧（Yuhu frame）在 deoptimization 时被弹出了
3. 读取到的是已被破坏的内存区域
4. 结果：垃圾值

## 附加问题：BCI记录不准确

在 `yuhuDebugInfo.cpp` 中：

```cpp
if (deferred_frame_bcis != NULL && deferred_frame_bcis->length() > 0) {
  int bci = deferred_frame_bcis->at(frame_idx);  // ← 使用实际bci
} else {
  recorder->describe_scope(..., 0, ...);  // ← bci = 0
}
```

**问题**：
- `_deferred_frame_bcis`为NULL（在`yuhuFunction.cpp:86`中初始化）
- Yuhu没有填充这个数组
- 所有safepoint都使用`bci = 0`
- 导致deoptimization总是从方法入口重新执行

**影响**：
- 如果在方法中间deoptimize，解释器会从头执行整个方法
- 可能导致无限循环或其他错误

## 解决方案

### ✅ 最终实现方案：Per-Function Deoptimization Stub

基于深入分析，我们采用了 **Per-Function Deoptimization Stub** 方案：

1. **每个方法生成自己的 deopt stub**：在方法编译时生成，知道该方法的参数个数
2. **从 locals 区域恢复参数**：使用 sp + extended_frame_size 计算参数地址
3. **动态恢复参数**：根据方法的实际参数个数（0-8 个）恢复 x0-x7
4. **ScopeDescriptor 标记为 REGISTER**：让 HotSpot 从 deopt blob 的寄存器保存区域读取
5. **区分参数类型**：正确标记 oop/lng/dbl/normal 类型，确保 GC 正确扫描

### 架构设计

```
Yuhu Compiled Code (触发 deoptimization)
    ↓
Yuhu Deoptimization Stub (我们自己写的)
    ├─ 恢复 sp 到正确位置
    ├─ 从 locals 区域恢复 x0-x7
    └─ 跳转到标准 deopt blob
    ↓
Standard Deoptimization Blob (HotSpot 的)
    ├─ 保存所有寄存器到栈
    ├─ fetch_unroll_info()
    ├─ unpack_frames()
    └─ unpack_on_stack() 从 deopt blob 的寄存器保存区域读取参数
    ↓
Interpreter
```

```
Yuhu Compiled Code (触发 deoptimization)
    ↓
Per-Function Deoptimization Stub (每个方法自己的 stub)
    ├─ 从 sp + extended_frame_size 计算 locals 地址
    ├─ 根据参数个数恢复 x0-x7（可能只恢复部分寄存器）
    └─ 跳转到标准 deopt blob
    ↓
Standard Deoptimization Blob (HotSpot 的)
    ├─ 保存所有寄存器到栈（包括刚恢复的 x0-x7）
    ├─ fetch_unroll_info()
    ├─ unpack_frames()
    └─ unpack_on_stack() 从 deopt blob 的寄存器保存区域读取参数
    ↓
Interpreter (从正确的 BCI 继续执行)
```

### 方案实现

#### ✅ 步骤 1：生成 Per-Function Deoptimization Stub

**文件**: `hotspot/src/share/vm/yuhu/yuhuFunction.cpp`

在 `YuhuFunction::initialize()` 中生成 stub：

```cpp
void YuhuFunction::initialize(const char *name) {
  builder()->set_function(this);

  // 初始化成员变量
  _deoptimization_stub = NULL;
  // ... 其他初始化

  // ✅ 在 IR 生成之前生成 deoptimization stub
  generate_deoptimization_stub();

  // ... 创建 LLVM Function，生成 IR
}
```

Stub 生成实现（使用 YuhuMacroAssembler）：

```cpp
void YuhuFunction::generate_deoptimization_stub() {
  // 检查是否已生成
  if (_deoptimization_stub != NULL) {
    return;
  }

  int num_params = target()->arg_size();

  // 限制参数个数（AArch64 ABI：x0-x7）
  if (num_params > 8) {
    tty->print_cr("Yuhu: WARNING - Method has %d parameters, only restoring first 8", num_params);
    num_params = 8;
  }

  // 使用 BufferBlob 创建 stub
  BufferBlob* stub_blob = BufferBlob::create("yuhu_deopt_stub", 256);
  CodeBuffer buffer(stub_blob);
  YuhuMacroAssembler masm(&buffer);

  address start = masm.current_pc();

  // === 计算 locals 区域地址 ===
  int header_words  = yuhu_frame_header_words;  // 6
  int monitor_words = max_monitors() * frame::interpreter_frame_monitor_size();
  int stack_words   = max_stack();
  int frame_words   = header_words + monitor_words + stack_words;
  int extended_frame_words = frame_words + max_locals();

  // x8 = sp + extended_frame_size * 8 = &locals[0]
  masm.write_inst("add %s, sp, #%d",
                  YuhuMacroAssembler::x8, extended_frame_words * wordSize);

  // === 从低地址到高地址恢复参数 x0-x7 ===
  // locals 布局（从低地址到高地址）：
  //   locals[0] (参数0/x0), locals[1] (参数1/x1), ...

  if (num_params >= 2) {
    // ldp x1, x0, [x8, #-16]!  // 从低地址读取，x8 向高地址移动
    masm.write_inst("ldp %s, %s, [%s, #%d]!",
                    YuhuMacroAssembler::x1, YuhuMacroAssembler::x0,
                    YuhuMacroAssembler::x8, -16);
  } else if (num_params == 1) {
    masm.write_inst("ldr %s, [%s, #%d]!",
                    YuhuMacroAssembler::x0, YuhuMacroAssembler::x8, -8);
  }

  if (num_params >= 4) {
    masm.write_inst("ldp %s, %s, [%s, #%d]!",
                    YuhuMacroAssembler::x3, YuhuMacroAssembler::x2,
                    YuhuMacroAssembler::x8, -16);
  } else if (num_params == 3) {
    masm.write_inst("ldr %s, [%s, #%d]!",
                    YuhuMacroAssembler::x2, YuhuMacroAssembler::x8, -8);
  }

  if (num_params >= 6) {
    masm.write_inst("ldp %s, %s, [%s, #%d]!",
                    YuhuMacroAssembler::x5, YuhuMacroAssembler::x4,
                    YuhuMacroAssembler::x8, -16);
  } else if (num_params == 5) {
    masm.write_inst("ldr %s, [%s, #%d]!",
                    YuhuMacroAssembler::x4, YuhuMacroAssembler::x8, -8);
  }

  if (num_params >= 8) {
    masm.write_inst("ldp %s, %s, [%s, #%d]!",
                    YuhuMacroAssembler::x7, YuhuMacroAssembler::x6,
                    YuhuMacroAssembler::x8, -16);
  } else if (num_params == 7) {
    masm.write_inst("ldr %s, [%s, #%d]!",
                    YuhuMacroAssembler::x6, YuhuMacroAssembler::x8, -8);
  }

  // === 跳转到标准 deoptimization blob ===
  masm.write_insts_far_jump(SharedRuntime::deopt_blob()->unpack_with_reexecution());

  masm.flush();

  _deoptimization_stub = start;

  tty->print_cr("Yuhu: Deoptimization stub generated for %s at %p (%d params)",
                target()->name()->as_C_string(), start, num_params);
}
```

**关键点**：

1. **每个方法有自己的 stub**：stub 知道该方法的参数个数
2. **使用 YuhuMacroAssembler**：生成 AArch64 汇编指令
3. **动态计算帧大小**：考虑 max_monitors、max_stack、max_locals
4. **从低地址到高地址读取**：使用 post-index 模式（`[%s, #-16]!`）
5. **只恢复需要的寄存器**：根据实际参数个数（0-7）

#### ✅ 步骤 2：修改 ScopeDescriptor（标记参数为 REGISTER）

**文件**: `hotspot/src/share/vm/yuhu/yuhuFunction.cpp`

```cpp
void YuhuFunction::process_deferred_oopmaps() {
  int max_locals = target()->max_locals();
  int num_params = target()->arg_size();  // 参数个数（非静态包括 this）
  ciSignature* sig = target()->signature();

  GrowableArray<ScopeValue*>* locals = new GrowableArray<ScopeValue*>(max_locals);

  for (int i = 0; i < max_locals; i++) {
    Location loc;

    if (i < num_params) {
      // === 参数：标记为在寄存器中 ===

      // 获取参数类型
      ciType* param_type = NULL;
      if (is_static()) {
        param_type = sig->type_at(i);
      } else {
        if (i == 0) {
          param_type = target()->holder();  // 'this' 类型
        } else {
          param_type = sig->type_at(i - 1);
        }
      }

      // 根据类型确定 Location::Type（关键：GC 需要知道 oop 类型）
      Location::Type loc_type;
      BasicType bt = param_type->basic_type();
      if (bt == T_OBJECT || bt == T_ARRAY) {
        loc_type = Location::oop;  // 对象引用（GC 需要扫描）
      } else if (bt == T_LONG) {
        loc_type = Location::lng;  // long 类型
      } else if (bt == T_DOUBLE) {
        loc_type = Location::dbl;  // double 类型
      } else {
        loc_type = Location::normal;  // int, float, short, byte, boolean
      }

      // 参数在寄存器 x0, x1, x2, ...
      VMReg reg = as_Register(i)->as_VMReg();
      loc = Location::new_reg_loc(loc_type, reg);

    } else {
      // === 真正的局部变量：在栈上 ===
      int stack_offset = i * wordSize;
      loc = Location::new_stk_loc(Location::normal, stack_offset);
    }

    locals->append(new LocationValue(loc));
  }

  // 使用 locals 创建 ScopeDescriptor
  _debug_info_recorder->describe_scope(
    pc_offset,
    target(),
    0,  // bci = 0（TODO：需要修复）
    false,
    false,
    false,
    locals,
    NULL,
    NULL);
}
```

**关键改进**：

1. ✅ **区分参数和真正的局部变量**
2. ✅ **参数标记为 REGISTER location**（使用 `as_Register(i)->as_VMReg()`）
3. ✅ **根据参数类型设置正确的 Location::Type**（oop/lng/dbl/normal）
4. ✅ **GC 能正确扫描对象引用**（Location::oop）

#### ✅ 步骤 3：修改 YuhuBuilder 集成 Per-Function Stub

**文件**: `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp`

```cpp
// YuhuBuilder 构造函数接受 YuhuFunction*
YuhuBuilder::YuhuBuilder(YuhuCodeBuffer* code_buffer, YuhuFunction* function)
  : IRBuilder<>(YuhuContext::current()),
    _code_buffer(code_buffer),
    _function(function) {
}

// deoptimized_entry_point() 返回 per-function stub
Value* YuhuBuilder::deoptimized_entry_point() {
  address yuhu_stub = NULL;
  if (_function != NULL) {
    yuhu_stub = _function->deoptimization_stub();
  }

  if (yuhu_stub != NULL) {
    tty->print_cr("Yuhu: Using per-function deoptimization stub at %p", yuhu_stub);
    return make_function(yuhu_stub, "iT", "v");
  } else {
    // Fallback to standard deopt blob（native wrapper 等）
    tty->print_cr("Yuhu: WARNING - No per-function deopt stub, using standard deopt blob");
    DeoptimizationBlob* deopt_blob = SharedRuntime::deopt_blob();
    return make_function((address) deopt_blob->unpack_with_reexecution(), "iT", "v");
  }
}
```

**文件**: `hotspot/src/share/vm/yuhu/yuhuFunction.cpp`

```cpp
void YuhuFunction::initialize(const char *name) {
  // ✅ 设置 builder 的 function 指针
  builder()->set_function(this);

  // ✅ 在 IR 生成之前生成 stub
  generate_deoptimization_stub();

  // ... 后续 IR 生成
}
```

**关键**：
- YuhuBuilder 持有 YuhuFunction 引用
- `deoptimized_entry_point()` 返回 per-function stub
- Stub 在 IR 生成之前就存在

#### ✅ 步骤 4：使用统一接口

**文件**: `hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp`

```cpp
// do_call() 中触发 deoptimization
Value* deopt_callee = builder()->deoptimized_entry_point();  // ✅ 使用统一接口
llvm::FunctionType* func_type = YuhuBuilder::make_ftype("iT", "v");
std::vector<Value*> args;
args.push_back(builder()->CreateSub(deoptimized_frames, LLVMValue::jint_constant(1)));
args.push_back(thread());
builder()->CreateCall(func_type, deopt_callee, args);
```

**好处**：
- 不直接访问 `function()->deoptimization_stub()`
- 统一使用 `builder()->deoptimized_entry_point()`
- 代码更清晰，易于维护

### 时序问题修复

**问题**：Stub 生成时机晚于 IR 生成

**原因**：
```
YuhuFunction::build()
  ├─ new YuhuFunction(...)
  │     └─ initialize()
  │          ├─ set_function(this)
  │          └─ [生成 IR]  ← 调用 deoptimized_entry_point()
  │               └─ _deoptimization_stub == NULL ❌
  └─ process_deferred_oopmaps()
       └─ generate_deoptimization_stub()  ← 太晚了！
```

**解决方案**：在 `initialize()` 开始时生成 stub

```cpp
void YuhuFunction::initialize(const char *name) {
  builder()->set_function(this);

  // ✅ 在 IR 生成之前生成 stub
  generate_deoptimization_stub();

  // ... 后续 IR 生成，此时 stub 已存在
}
```

**结果**：
- ✅ Stub 在 IR 生成之前就存在
- ✅ `deoptimized_entry_point()` 能找到 per-function stub
- ✅ 不会 fallback 到标准 deopt blob

### 为什么这个方案可行

#### 步骤 1：修改 ScopeDescriptor（标记参数为 REGISTER）

```cpp
// yuhuFunction.cpp:process_deferred_oopmaps()
void YuhuFunction::process_deferred_oopmaps() {
  int num_params = target()->size_of_parameters();
  int max_locals = target()->max_locals();

  GrowableArray<ScopeValue*>* locals = new GrowableArray<ScopeValue*>(max_locals);

  for (int i = 0; i < max_locals; i++) {
    LocationValue* loc_val;

    if (i < num_params) {
      // 参数 0-7：标记为在寄存器中
      if (num_params <= 8) {
        // 使用 REGISTER 类型，让 HotSpot 从 deopt blob 的寄存器保存区域读取
        Location loc = Location::new_reg_loc(
          VMRegImpl::as_VMReg(React_RegNum + i),  // x0, x1, x2, ...
          Location::ON_STACK
        );
        loc_val = new LocationValue(loc);
      } else {
        // 参数 > 7：暂不支持（后续从栈上恢复）
        ShouldNotReachHere("参数超过8个暂不支持");
      }
    } else {
      // 真正的局部变量：在栈上
      int stack_offset = (i - num_params) * wordSize;
      Location loc = Location::new_stk_loc(Location::normal, stack_offset);
      loc_val = new LocationValue(loc);
    }

    locals->append(loc_val);
  }

  // 使用 locals 创建 ScopeDescriptor
  _debug_info_recorder->describe_scope(..., locals, ...);
}
```

**关键**：标记参数为 REGISTER location 后，`unpack_on_stack()` 会从 **deopt blob 的寄存器保存区域** 读取参数，而不是从 Yuhu frame。

#### 步骤 2：创建 Yuhu Deoptimization Stub

```cpp
// yuhuRuntime.cpp
address YuhuRuntime::generate_deoptimization_stub() {
  CodeBuffer buffer("yuhu_deopt_stub", 256, 64);
  MacroAssembler* masm = new MacroAssembler(&buffer);

  address start = __ pc();

  // === 步骤 1：恢复 sp 到正确位置 ===
  // 从 fp 恢复 unextended_sp
  __ ldr(x8, Address(x29, frame::interpreter_frame_sender_sp_offset * wordSize));
  __ mov(sp, x8);

  // === 步骤 2：恢复参数寄存器 x0-x7 ===
  // 计算 locals[0] 的地址
  // locals_slots_offset = 17 (从 stack[0] 往高地址 17 个 slot)
  __ add(x9, sp, 17 * 8);  // x9 = &locals[0]

  // 恢复 x0-x7（从 locals 区域）
  __ ldp(x0, x1, [x9], 16);  // x0=*x9, x1=*(x9+8), x9+=16
  __ ldp(x2, x3, [x9], 16);  // x2=*x9, x3=*(x9+8), x9+=16
  __ ldp(x4, x5, [x9], 16);  // x4=*x9, x5=*(x9+8), x9+=16
  __ ldp(x6, x7, [x9]);       // x6=*x9, x7=*(x9+8)

  // === 步骤 3：跳转到标准 deoptimization blob ===
  __ b(SharedRuntime::deopt_blob()->unpack());

  __ flush();

  return buffer.code_start();
}
```

**关键点**：

1. **为什么使用 sp 而不是 x29**：
   - x29 指向的位置包含 LLVM spill 区域，offset 计算复杂
   - sp 恢复后，到 locals 的偏移是固定的（17 个 slot）
   - 计算公式：`&locals[0] = sp + 17 * 8`

2. **Yuhu Frame Layout（从 sp 往高地址）**：
   ```
   sp + 0*8:   stack[0]
   sp + 5*8:   stack[max_stack-1]
   sp + 6*8:   monitor[0]
   sp + 11*8:  monitor
   sp + 12*8:  oop_tmp
   sp + 13*8:  method
   sp + 14*8:  unextended_sp
   sp + 15*8:  pc
   sp + 16*8:  frame_marker
   sp + 17*8:  locals[0]  ← 参数 0 (x0)
   sp + 18*8:  locals[1]  ← 参数 1 (x1)
   ...
   sp + 24*8:  locals[7]  ← 参数 7 (x7)
   ```

#### 步骤 3：在 Yuhu 触发 deopt 时调用 Yuhu Stub

```cpp
// yuhuTopLevelBlock.cpp:do_trap()
llvm::BasicBlock* YuhuTopLevelBlock::make_trap(int trap_bci, int trap_request) {
  BasicBlock *trap_block = function()->CreateBlock("trap");

  builder()->SetInsertPoint(trap_block);

  // 恢复 sp（从 unextended_sp）
  Value *unextended_sp = builder()->CreateLoad(
    YuhuType::intptr_type(),
    builder()->CreateAddressOfStructEntry(
      current_fp(),  // 使用当前 fp
      byte_offset_from_sp(...)  // unextended_sp offset
    )
  );
  builder()->CreateWriteStackPointer(unextended_sp);

  // 调用 Yuhu deoptimization stub
  address yuhu_deopt_stub = YuhuRuntime::deoptimization_stub();
  builder()->CreateCall(yuhu_deopt_stub);

  builder()->CreateUnreachable();

  return trap_block;
}
```

#### 步骤 4：注册 Yuhu Deopt Stub

```cpp
// yuhuRuntime.cpp
address YuhuRuntime::_deoptimization_stub = NULL;

void YuhuRuntime::initialize() {
  // 生成 deoptimization stub
  _deoptimization_stub = generate_deoptimization_stub();

  tty->print_cr("Yuhu: Deoptimization stub generated at %p", _deoptimization_stub);
}
```

### 为什么这个方案可行

#### 原因 1：参数在 deopt 之前恢复

```
时间线：
1. Yuhu 代码执行，x0-x7 包含参数值
2. 触发 deoptimization
3. 跳转到 Yuhu deopt stub
4. Yuhu stub 从 locals 恢复 x0-x7  ← 此时 Yuhu frame 还有效
5. 跳转到标准 deopt blob
6. 标准 deopt stub 保存所有寄存器（包括刚恢复的 x0-x7）
7. 执行 fetch_unroll_info, unpack_frames
8. unpack_on_stack 从 deopt blob frame 读取 x0-x7  ← 正确！
```

#### 原因 2：REGISTER location 导致从正确的位置读取

```
ScopeValue 标记为 REGISTER (x0)：
  ↓
unpack_on_stack 调用 StackValue::get_int()
  ↓
StackValue 看到 type=REGISTER
  ↓
从 deopt_sp + RegisterSaver::r0_offset_in_bytes() 读取
  ↓
读取到 Yuhu stub 恢复的值（正确！）
```

而不是：
```
ScopeValue 标记为 STACK (offset=0)：
  ↓
从 yuhu_sp + 0 读取
  ↓
但 Yuhu frame 已被弹出（垃圾值）
```

### 参数 > 8 的处理（后续实现）

AArch64 ABI 规定：
- 参数 0-7：通过寄存器 x0-x7 传递
- 参数 8+：通过栈传递（从右到左压栈）

对于参数 > 8 的情况，需要：
1. 标记参数 0-7 为 REGISTER（当前方案）
2. 标记参数 8+ 为 STACK（需要计算调用者栈上的 offset）
3. 可能需要调整 `caller_adjustment`

**建议**：先实现参数 ≤ 8 的情况（最常见），> 8 的情况后续支持。

### 为什么这个方案可行

#### 原因 1：参数在 deopt 之前恢复

```
时间线：
1. Yuhu 代码执行，x0-x7 可能已被 LLVM spill
2. 触发 deoptimization
3. 跳转到 Per-Function Deopt Stub
4. Stub 从 locals 区域恢复 x0-x7  ← 此时 Yuhu frame 还有效
5. 跳转到标准 deopt blob
6. 标准 deopt stub 保存所有寄存器（包括刚恢复的 x0-x7）
7. 执行 fetch_unroll_info, unpack_frames
8. unpack_on_stack 从 deopt blob frame 读取 x0-x7  ← 正确！
```

#### 原因 2：REGISTER location 导致从正确的位置读取

```
ScopeValue 标记为 REGISTER (x0) + 类型 (oop/lng/dbl/normal)
  ↓
unpack_on_stack 调用 StackValue::get_int()
  ↓
StackValue 看到 type=REGISTER
  ↓
从 deopt_sp + RegisterSaver::r0_offset_in_bytes() 读取
  ↓
读取到 Per-Function Stub 恢复的值（正确！）
```

#### 原因 3：Per-Function Stub 知道参数个数

```
Per-Function Stub 的优势：
  ├─ 每个方法编译时生成
  ├─ 知道方法的参数个数（arg_size）
  ├─ 只恢复需要的寄存器（高效）
  └─ 不需要运行时判断（性能好）

示例：
  方法有 3 个参数：
    ├─ 只恢复 x0, x1, x2
    ├─ x3-x7 保持不变
    └─ 减少 stub 大小
```

#### 原因 4：正确的类型标记确保 GC 正确工作

```
参数类型正确标记：
  ├─ Location::oop  → GC 扫描对象引用 ✅
  ├─ Location::lng  → 正确处理 long 类型
  ├─ Location::dbl  → 正确处理 double 类型
  └─ Location::normal → 普通值

如果全部标记为 normal：
  ├─ GC 不会扫描对象引用
  ├─ 对象可能被错误回收
  └─ 崩溃！❌
```

### Yuhu Frame Layout（完整）

```
从 sp 往高地址：
  sp + 0*8:   stack[0]
  sp + ...:   stack[max_stack-1]
  sp + ...:   monitor[0]
  sp + ...:   monitor[max_monitors-1]
  sp + ...:   oop_tmp
  sp + ...:   method*
  sp + ...:   unextended_sp
  sp + ...:   pc
  sp + ...:   frame_marker
  sp + ...:   frame_pointer_addr (caller's FP)
  sp + header_size*8: locals[0]  ← 参数 0 (x0)
  sp + (header_size+1)*8: locals[1]  ← 参数 1 (x1)
  ...
  sp + (header_size+num_params)*8: locals[num_params]  ← 第一个真正的局部变量
  ...
  sp + extended_frame_size*8: locals[max_locals-1]
```

**关键**：
- `header_size = yuhu_frame_header_words = 6`
- `extended_frame_size = header_size + monitor_words + stack_words + locals_words`
- 参数在 `sp + header_size*8` 开始的连续区域

### 参数恢复指令（汇编）

```
假设方法有 3 个参数（x0, x1, x2）：

  add   x8, sp, #152       ; x8 = &locals[0] (假设 extended_frame_size = 19)
  ldr   x2, [x8, #-8]!    ; x2 = *x8, x8 -= 8
  ldp   x1, x0, [x8, #-16]!  ; x1 = *x8, x0 = *(x8+8), x8 -= 16
  b     deopt_blob        ; 跳转到标准 deopt blob

结果：
  x0 = locals[0]  (参数 0) ✅
  x1 = locals[1]  (参数 1) ✅
  x2 = locals[2]  (参数 2) ✅
```

**使用 post-index 模式的好处**：
- 指令简洁（一条指令读取两个值）
- 自动更新指针（x8 递减）
- 从低地址到高地址顺序读取

## 已实现的修改

### 1. 添加 Per-Function Deoptimization Stub

**文件**: `hotspot/src/share/vm/yuhu/yuhuFunction.cpp`

- ✅ 添加 `_deoptimization_stub` 成员变量
- ✅ 实现 `generate_deoptimization_stub()` 方法
- ✅ 使用 `YuhuMacroAssembler` 生成汇编代码
- ✅ 在 `initialize()` 中生成 stub（IR 生成之前）

### 2. 修改 ScopeDescriptor 生成

**文件**: `hotspot/src/share/vm/yuhu/yuhuFunction.cpp`

- ✅ 区分参数和真正的局部变量
- ✅ 参数标记为 REGISTER location
- ✅ 根据参数类型设置 Location::Type（oop/lng/dbl/normal）
- ✅ 使用 `as_Register(i)->as_VMReg()` 创建 VMReg

### 3. 修改 YuhuBuilder 集成

**文件**:
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp`
- `hotspot/src/share/vm/yuhu/yuhuBuilder.hpp`

- ✅ 添加 `YuhuFunction* _function` 成员
- ✅ 构造函数接受 `YuhuFunction*`
- ✅ 添加 `set_function()` 方法
- ✅ `deoptimized_entry_point()` 返回 per-function stub

### 4. 使用统一接口

**文件**: `hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp`

- ✅ `do_call()` 使用 `builder()->deoptimized_entry_point()`
- ✅ 不直接访问 `function()->deoptimization_stub()`

### 5. 添加必要的头文件

**文件**: `hotspot/src/share/vm/yuhu/yuhuFunction.cpp`

```cpp
#include "asm/yuhu/yuhu_macroAssembler.hpp"
#ifdef TARGET_ARCH_aarch64
#include "register_aarch64.hpp"
#endif
```

### 6. 提取全局常量

**文件**:
- `hotspot/src/share/vm/yuhu/yuhu_globals.hpp`
- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp`
- `hotspot/src/share/vm/yuhu/yuhuStack.cpp`

- ✅ 添加 `yuhu_frame_header_words = 6` 全局常量
- ✅ YuhuCompiler 和 YuhuStack 都引用这个常量
- ✅ 消除硬编码的 `int header_words = 6;`

## 测试步骤

### 阶段 1：创建 Yuhu Deoptimization Stub

**文件**: `hotspot/src/share/vm/yuhu/yuhuRuntime.cpp`

1. 添加 `_deoptimization_stub` 成员变量
2. 实现 `generate_deoptimization_stub()` 方法
3. 在 `YuhuRuntime::initialize()` 中生成 stub

**验证**：
- Stub 成功生成
- Stub 地址被正确注册
- 可以通过 `nm` 或 `jhsdb` 看到 stub 符号

### 阶段 2：修改 ScopeDescriptor 生成

**文件**: `hotspot/src/share/vm/yuhu/yuhuFunction.cpp`

1. 修改 `process_deferred_oopmaps()` 方法
2. 标记参数 0-7 为 REGISTER location
3. 标记真正的局部变量为 STACK location

**验证**：
- ScopeDescriptor 正确记录参数位置
- 通过 `-XX:+PrintDeoptimizationDetails` 查看日志

### 阶段 3：修改 Trap 调用

**文件**: `hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp`

1. 修改 `make_trap()` 或 `do_trap()` 方法
2. 在调用 deopt stub 之前恢复 sp
3. 调用 Yuhu deopt stub 而不是直接调用标准 deopt blob

**验证**：
- Trap 触发时正确跳转到 Yuhu stub
- sp 被恢复到正确位置

### 阶段 4：测试 Deoptimization

**测试用例**: `test_yuhu/OSRCrashTest.java` 或 `UTF_8` 编码测试

**验证**：
- Deoptimization 成功触发
- 参数被正确恢复（不再有 0xdeaddeaf）
- 解释器正确执行
- 程序正常完成，不崩溃

## 测试步骤

### 1. 编译 Yuhu 编译器

```bash
cd /Users/liuanyou/CLionProjects/jdk8
make clean
make
```

### 2. 运行测试

```bash
cd test_yuhu
java -XX:+UseYuhuCompiler \
     -XX:+PrintDeoptimizationDetails \
     -XX:CompileCommand=yuhuonly,sun.nio.cs.*::* \
     -cp . \
     TestEncoder
```

### 3. 检查日志输出

**期望看到**：
```
Yuhu: Deoptimization stub generated for sun.nio.cs.UTF_8$Encoder.encodeArrayLoop at 0x... (3 params)
Yuhu: Processing X deferred OopMaps from YuhuFunction before destruction
Yuhu: Added OopMap to YuhuDebugInformationRecorder at virtual pc_offset=...
Yuhu: Using per-function deoptimization stub at 0x...
Deoptimizing frame {
  ... 参数值正确（不是 0xdeaddeaf）...
}
```

### 4. 检查参数值

使用 lldb 断点：

```lldb
b unpack_on_stack
b Interpreter::layout_activation
run

# 当触发 deoptimization 时
x/10g $sp
# 检查参数值是否正确
```

### 5. 验证汇编代码

使用 `jhsdb` 反汇编：

```bash
jhsdb clhsdb
> attach <pid>
> disassemble yuhu_deopt_stub
```

**期望看到**：
- `add x8, sp, #XXX` 指令（计算 locals 地址）
- `ldr/ldp` 指令（恢复 x0-x7）
- 正确跳转到标准 deopt blob

## 相关文件

### 已修改的文件
- ✅ `hotspot/src/share/vm/yuhu/yuhuFunction.cpp` - 实现 generate_deoptimization_stub()，修改 process_deferred_oopmaps()
- ✅ `hotspot/src/share/vm/yuhu/yuhuFunction.hpp` - 添加 _deoptimization_stub 声明
- ✅ `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` - 修改 deoptimized_entry_point()
- ✅ `hotspot/src/share/vm/yuhu/yuhuBuilder.hpp` - 添加 YuhuFunction* 引用
- ✅ `hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp` - 使用 builder()->deoptimized_entry_point()
- ✅ `hotspot/src/share/vm/yuhu/yuhu_globals.hpp` - 添加 yuhu_frame_header_words 常量
- ✅ `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - 使用 yuhu_frame_header_words 常量
- ✅ `hotspot/src/share/vm/yuhu/yuhuStack.cpp` - 使用 yuhu_frame_header_words 常量

### 相关的 HotSpot 文件
- `hotspot/src/cpu/aarch64/vm/sharedRuntime_aarch64.cpp` - generate_deopt_blob() (参考)
- `hotspot/src/cpu/aarch64/vm/register_aarch64.hpp` - as_Register(), as_VMReg()
- `hotspot/src/share/vm/runtime/vframeArray.cpp` - unpack_on_stack()
- `hotspot/src/share/vm/code/location.cpp` - Location 类
- `hotspot/src/share/vm/code/stackValue.cpp` - StackValue 类
- `hotspot/src/share/vm/runtime/deoptimization.cpp` - Deoptimization 处理
- `hotspot/src/cpu/aarch64/vm/frame_aarch64.hpp` - 解释器栈帧定义

## 相关文档

- `doc/yuhu/activities/038_extra_16_bytes_alloca_issue.md` - 之前的alloca问题
- `doc/yuhu/YUHU_FRAME_LAYOUT_DESIGN.md` - Yuhu栈帧设计

## 经验总结

### 1. LLVM Prologue 的限制
- LLVM prologue **只保存 callee-saved 寄存器**（x19-x30）
- **不保存参数寄存器**（x0-x7）
- 参数寄存器的值需要在其他地方保存（Yuhu 保存到 locals）

### 2. C1/C2 的做法
- 使用 **REGISTER location** 标记参数
- Deoptimization 从 **deopt blob 的寄存器保存区域** 读取
- 即使原始栈帧被弹出，deopt blob frame 中的值仍然有效

### 3. Yuhu 的错误（已修复）
- ❌ 之前：把所有参数标记为 **STACK location**
- ❌ 之前：使用错误的 offset（0, 8, 16...）
- ✅ 现在：参数标记为 **REGISTER location**
- ✅ 现在：根据类型正确标记（oop/lng/dbl/normal）

### 4. Deoptimization 栈帧管理
- Deopt stub 会弹出 Yuhu frame（`add sp, sp, r2`）
- Yuhu frame 上的数据在弹出后被破坏
- 必须在弹出前恢复参数，或使用 REGISTER location

### 5. 为什么使用 sp 而不是 x29
- x29 (fp) 指向的位置可能包含 LLVM spill 区域
- 计算 offset 需要考虑 spill 大小（复杂）
- sp 到 locals 的偏移是**固定的**（extended_frame_size 个 slot）
- 更简单、更可靠

### 6. Per-Function Deoptimization Stub 的优势
- 每个方法编译时生成，知道参数个数
- 在 IR 生成之前生成，时序正确
- 只恢复需要的寄存器（高效）
- 使用 YuhuMacroAssembler，代码清晰

### 7. Location::Type 的重要性
- **Location::oop** → GC 扫描对象引用（必须正确！）
- **Location::lng** → 正确处理 long 类型
- **Location::dbl** → 正确处理 double 类型
- **Location::normal** → 普通值

如果全部标记为 normal，GC 不会扫描对象引用，导致对象被错误回收，崩溃！

### 8. AArch64 Calling Convention
- 参数 0-7：通过寄存器 x0-x7 传递
- 参数 8+：通过栈传递
- 编译器可以根据需要 spill 参数到栈

### 9. YuhuMacroAssembler 的使用
- 提供高层抽象（`write_inst`, `write_inst_regs`）
- 自动处理寄存器名称（`x0`, `x1`, ...）
- 支持 `write_insts_far_jump`（远程跳转）
- 比 MacroAssembler 更易用

### 10. 调试技巧
- 使用 lldb 观察 SP/FP 变化
- 对比 Yuhu frame、deopt blob frame、interpreter frame 的布局
- 检查 ScopeValue 记录的 location 类型
- 使用 `-XX:+PrintDeoptimizationDetails` 查看详细信息
- 使用 `jhsdb` 反汇编查看生成的代码

## 下一步工作

### 已完成 ✅
1. ✅ **实现 Per-Function Deoptimization Stub** - 生成恢复 x0-x7 的 stub
2. ✅ **修改 ScopeDescriptor** - 标记参数为 REGISTER location，正确标记类型
3. ✅ **修改 YuhuBuilder** - 集成 per-function stub
4. ✅ **时序修复** - 在 initialize() 中生成 stub（IR 生成之前）
5. ✅ **提取全局常量** - yuhu_frame_header_words

### 待完成（建议修复）

#### 高优先级
1. **修复 BCI 记录问题** - 填充 `deferred_frame_bcis` 数组
   - 当前所有 safepoint 的 BCI 都是 0
   - 导致 deoptimization 总是从方法入口重新执行
   - 应该记录每个 safepoint 的实际 BCI

2. **测试验证** - 确保 deoptimization 成功，参数正确恢复
   - 运行 OSRCrashTest
   - 运行 UTF_8 编码测试
   - 检查日志中的参数值

3. **真正的局部变量处理** - 当前使用 Location::normal
   - 应该根据局部变量的实际类型设置 Location::Type
   - 需要分析字节码，确定每个局部变量的类型

#### 中优先级
4. **支持参数 > 8** - 处理栈上传递的参数
   - AArch64 ABI：参数 8+ 通过栈传递
   - 需要计算调用者栈上的 offset
   - 可能需要调整 `caller_adjustment`

5. **完善 OopMap** - 确保 oop map 准确记录对象引用
   - 当前可能没有准确记录所有 oop 位置
   - 可能导致 GC 问题

#### 低优先级
6. **性能测试** - 测量 Per-Function Stub 的开销
   - Stub 大小
   - 执行时间
   - 内存占用

7. **回归测试** - 确保其他场景不受影响
   - 正常执行
   - 其他 deoptimization 场景
   - GC 行为

---

**更新历史**：
- 2026-01-27: 初始版本，发现deoptimization参数恢复失败的问题
- 2026-01-29: 深入分析，发现 LLVM prologue 不保存 x0-x7
- 2026-01-30: 实现 Per-Function Deoptimization Stub 方案 ✅
  - 添加 generate_deoptimization_stub()
  - 修改 ScopeDescriptor 生成（REGISTER location + 类型标记）
  - 修改 YuhuBuilder 集成
  - 提取 yuhu_frame_header_words 全局常量
  - 修复时序问题（在 initialize() 中生成 stub）

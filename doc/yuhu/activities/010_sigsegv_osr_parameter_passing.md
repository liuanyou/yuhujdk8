# 活动 010: SIGSEGV OSR 方法参数传递错误

## 日期
2025-12-08

## 问题描述

在运行 Yuhu 编译器编译的 OSR 方法时，遇到以下运行时错误：

```
SIGSEGV (0xb) at pc=0x000000010e810868, pid=77943, tid=5635

Problematic frame:
J 9% yuhu com.example.Matrix.multiply([[I[[I[[II)V (79 bytes) @ 0x000000010e810868 [0x000000010e810840+0x28]

implicit exception happened at 0x000000010e810868
```

错误发生在执行生成的代码时，具体位置是：
```
0x000000010e810868: ldp x8, x9, [x3, #328]
```

## 错误分析

### 1. 成功的部分

从日志可以看到，之前的步骤都成功了：

1. ✅ **IR 验证通过**：`IR verification passed`
2. ✅ **代码生成成功**：`main code [0x000000010e810840,0x000000010e8111a8] = 2408` 字节
3. ✅ **代码已安装**：nmethod 已正确安装到 CodeCache

### 2. 关键观察

从反汇编代码可以看到：

```
0x000000010e810860: mov x19, x3         ; 保存 x3 到 x19
0x000000010e810864: sub x10, x29, #0x80 ; 栈溢出检查
0x000000010e810868: ldp x8, x9, [x3, #328]  ; ← SIGSEGV 发生在这里
```

**分析**：
- `x3` 被用作基址寄存器，从 `x3+328` 加载数据
- `x3` 可能是 NULL 或无效地址，导致 SIGSEGV
- 错误发生在方法入口处，说明参数传递可能有问题

**x3 的来源推测**：
- 根据 Yuhu 的函数签名，`x3` 应该是 `JavaThread*`（第四个参数）
- 但由于参数不匹配，`x3` 实际上是未初始化的值（可能是 0 或随机值）
- 当代码尝试从 `x3+328` 加载数据时（可能是访问 `JavaThread` 的某个字段），就会触发 SIGSEGV

**验证方法**：
- 在调试器中检查 `x3` 的值
- 检查 `x3+328` 是否指向有效的内存地址
- 确认 `x3` 是否应该是 `JavaThread*` 指针

### 3. OSR 方法参数传递机制

#### 3.1 HotSpot 的 OSR 调用约定

从 `templateTable_aarch64.cpp` 可以看到，OSR 方法的调用流程：

```cpp
// 1. 调用 OSR_migration_begin 获取 OSR buffer
call_VM(noreg, CAST_FROM_FN_PTR(address, SharedRuntime::OSR_migration_begin));

// 2. OSR buffer 在 r0 (x0) 中返回
// r0 is OSR buffer, move it to expected parameter location
__ mov(j_rarg0, r0);  // j_rarg0 = x0 (AArch64 第一个参数寄存器)

// 3. 跳转到 OSR entry point
__ ldr(rscratch1, Address(r19, nmethod::osr_entry_point_offset()));
__ br(rscratch1);
```

**关键点**：HotSpot 调用 OSR 方法时，**只传递了 OSR buffer**（通过 x0），没有传递 Method*、base_pc、thread 等参数。

#### 3.2 Yuhu 期望的函数签名

从 `yuhuContext.cpp` 可以看到，Yuhu 定义的 OSR entry point 类型：

```cpp
// OSR entry point 参数顺序：
params.push_back(Method_type());        // x0: Method*
params.push_back(PointerType::getUnqual(jbyte_type()));  // x1: osr_buf (jbyte*)
params.push_back(intptr_type());        // x2: base_pc
params.push_back(thread_type());        // x3: JavaThread*
_osr_entry_point_type = FunctionType::get(jint_type(), params, false);
```

从 `yuhuFunction.cpp` 可以看到参数解析顺序：

```cpp
Function::arg_iterator ai = function()->arg_begin();
llvm::Argument *method = ai++;      // 期望 x0 = Method*
llvm::Argument *osr_buf = ai++;      // 期望 x1 = osr_buf
llvm::Argument *base_pc = ai++;     // 期望 x2 = base_pc
llvm::Argument *thread = ai++;      // 期望 x3 = JavaThread*
```

#### 3.3 参数不匹配问题

**问题根源**：HotSpot 和 Yuhu 的参数传递约定不一致：

| 位置 | HotSpot 实际传递 | Yuhu 期望接收 |
|------|-----------------|--------------|
| x0   | osr_buf         | Method*      |
| x1   | (未设置)        | osr_buf      |
| x2   | (未设置)        | base_pc      |
| x3   | (未设置)        | JavaThread*  |

**结果**：
- Yuhu 将 x0（实际是 osr_buf）当作 Method* 使用
- Yuhu 将 x1（未初始化）当作 osr_buf 使用
- x3（未初始化）被用作基址寄存器，导致 SIGSEGV

### 4. Zero 架构的特殊调用机制（Shark 的设计基础）

从 `entry_zero.hpp` 可以看到，Zero 架构的 OSR 调用：

```cpp
void invoke_osr(Method* method, address osr_buf, TRAPS) const {
  maybe_deoptimize(
    ((OSREntryFunc) entry_point())(method, osr_buf, (intptr_t) this, THREAD),
    THREAD);
}
```

**Zero 架构传递了 3 个参数**：
- `method` (Method*) - 通过函数参数传递
- `osr_buf` (address) - 通过函数参数传递  
- `base_pc` ((intptr_t) this) - ZeroEntry 的地址，通过函数参数传递
- `THREAD` - 通过 TRAPS 宏传递（可能是全局变量或特殊寄存器）

**关键差异**：
- **Zero 架构**：`Interpreter::invoke_osr()` 可以传递多个参数（Method*, osr_buf）
- **AArch64 架构**：`templateTable` 只传递 OSR buffer（x0），这是 HotSpot 的标准约定

**结论**：Shark 的设计基于 Zero 架构的特殊调用机制，而 Yuhu 是为 AArch64 设计的，必须遵循 AArch64 的标准调用约定（只传递 OSR buffer）。

### 5. 正常方法 vs OSR 方法

#### 正常方法调用
- 通过 `call_stub` 调用
- 参数通过标准调用约定传递：x0=Method*, x1=base_pc, x2=thread
- 与 Yuhu 的期望一致

#### OSR 方法调用
- 直接从解释器跳转
- **只传递 OSR buffer**（x0）
- 需要适配器代码来补充其他参数

### 6. C1 编译器的 OSR 处理方式（参考）

从 `c1_LIRAssembler_aarch64.cpp` 可以看到，C1 的 OSR entry point：

1. **只接收 OSR buffer**（通过 x0）：
   ```cpp
   // r2: osr buffer (注释说r2，但实际是x0)
   Register OSR_buf = osrBufferPointer()->as_pointer_register();
   // osrBufferPointer() 返回 receiverOpr()，即 x0
   ```

2. **在 `osr_entry()` 中自己构建 frame**：
   - 不需要 Method*、base_pc、thread 作为参数
   - 从 `compilation()->method()` 获取 Method*
   - 从 `compilation()` 的其他字段获取所需信息

3. **直接处理 OSR buffer**：
   - 从 buffer 中读取 locals 和 monitors
   - 构建编译后的 frame

**关键差异**：C1 的 OSR entry point **函数签名只接收 OSR buffer**，而 Yuhu 期望接收 4 个参数。

## 根本原因分析

### 核心问题：函数签名不匹配

**HotSpot 的 OSR 调用约定**：
- OSR entry point **只接收一个参数**：OSR buffer（通过 x0）
- 这是 HotSpot 的标准约定，C1/C2 都遵循这个约定

**Yuhu 期望的函数签名**：
- OSR entry point 期望接收 4 个参数：Method*, osr_buf, base_pc, thread
- 这是从 Shark/Zero 架构继承的设计，但 Zero 可能有特殊的适配机制

### 解决方案选择

#### 方案 1：修改 Yuhu 的 OSR entry point 签名（与 C1/C2 一致，但实现复杂）

将 OSR entry point 改为只接收 OSR buffer，与 HotSpot 标准约定一致：

1. **修改函数签名**：
   - 修改 `yuhuContext.cpp` 中的 `_osr_entry_point_type`，只包含 `osr_buf` 参数：
     ```cpp
     params.clear();
     params.push_back(PointerType::getUnqual(jbyte_type()));  // osr_buf
     _osr_entry_point_type = FunctionType::get(jint_type(), params, false);
     ```

2. **在函数入口获取其他参数**：
   - 在 `yuhuFunction.cpp` 的 `initialize()` 中，OSR 方法只接收 `osr_buf` 参数
   - 在入口代码中通过 runtime 调用获取：
     - **Method***：可能需要通过特殊方式获取（检查是否有 runtime 函数）
     - **base_pc**：从 `code_buffer()->base_pc()` 获取（但需要确保在运行时可用）
     - **thread**：通过 `JavaThread::current()` 或全局变量获取

3. **实现细节**：
   - 可能需要添加 LLVM intrinsic 或 runtime 函数来获取这些值
   - 或者通过特殊的寄存器约定（如果 AArch64 有的话）

**优点**：
- ✅ 与 HotSpot 标准约定一致（C1/C2 都使用这种方式）
- ✅ 不需要适配器代码
- ✅ 符合 AArch64 架构的设计

**挑战**：
- ⚠️ 需要找到在 LLVM IR 中获取 Method* 的方式（可能需要 runtime 调用）
- ⚠️ base_pc 在运行时如何获取（可能需要从 nmethod 的 metadata 获取）
- ⚠️ thread 的获取方式（可能需要全局变量或特殊寄存器）

**可能的实现方式**：

1. **Method***：
   - 从 nmethod 获取：`nmethod::method()` 返回 `Method*`
   - 从 code address 获取 nmethod：`CodeCache::find_blob(code_address)` 可以找到对应的 nmethod
   - 需要添加 runtime 函数或 LLVM intrinsic 来调用这些方法

2. **base_pc**：
   - 可以从 nmethod 的 `code_begin()` 获取
   - 或者使用 LLVM 的 `llvm.frameaddress` 或类似机制获取当前 PC，然后计算偏移

3. **thread**：
   - 可以通过 `JavaThread::current()` runtime 调用获取
   - 或者使用全局变量（如果 AArch64 有特殊约定，如 Zero 架构可能有）

**关键洞察**：
- C1 在编译时就知道这些值（从 `compilation()` 对象），所以不需要在运行时获取
- Yuhu 使用 LLVM JIT，需要在生成的代码中通过 runtime 调用获取
- 可能需要添加新的 runtime 函数或 LLVM intrinsic 来支持这些操作

#### 方案 2：添加 OSR 适配器代码（**推荐**，更易实现）

在 LLVM 生成的函数之前，添加一小段汇编适配器代码：

**实现思路**：
1. **适配器代码位置**：在 `compile_method` 中，如果是 OSR 方法，在生成 LLVM 代码之前，先用 `YuhuMacroAssembler` 生成适配器代码
2. **适配器代码功能**：
   - 接收 OSR buffer（x0）
   - 通过 runtime 调用获取所需参数：
     - 使用 `CodeCache::find_blob(pc)` 从当前 PC 获取 nmethod
     - 从 nmethod 获取 Method*：`nmethod->method()`
     - 从 nmethod 获取 base_pc：`nmethod->code_begin()`
     - 获取 thread：`JavaThread::current()`
   - 重新排列参数：x0=Method*, x1=osr_buf, x2=base_pc, x3=thread
   - 跳转到真正的 LLVM 生成的函数

**实现步骤**：

1. **在 `compile_method` 中修改流程**：
   ```cpp
   void YuhuCompiler::compile_method(ciEnv*    env,
                                      ciMethod* target,
                                      int       entry_bci) {
     // ... 前面的代码不变 ...
     
     // Create the code buffer and builder
     CodeBuffer hscb("Yuhu", 256 * K, 64 * K);
     hscb.initialize_oop_recorder(env->oop_recorder());
     YuhuMacroAssembler masm(&hscb);
     YuhuCodeBuffer cb(masm);
     YuhuBuilder builder(&cb);
     
     // Emit the entry point
     YuhuEntry *entry = (YuhuEntry *) cb.malloc(sizeof(YuhuEntry));
     
     // ========== OSR 适配器代码生成 ==========
     address osr_adapter_start = NULL;
     int osr_adapter_size = 0;
     if (entry_bci != InvocationEntryBci) {
       // OSR 方法：先生成适配器代码
       osr_adapter_start = masm.current_pc();
       generate_osr_adapter(&masm, target, entry);
       osr_adapter_size = masm.current_pc() - osr_adapter_start;
     }
     // ========== End of OSR 适配器 ==========
     
     // Build the LLVM IR for the method
     Function *function = YuhuFunction::build(env, &builder, flow, name);
     if (env->failing()) {
       return;
     }
     
     // Generate native code
     {
       ThreadInVMfromNative tiv(JavaThread::current());
       generate_native_code(entry, function, name);
     }
     
     // ========== Patch OSR 适配器的跳转指令 ==========
     if (entry_bci != InvocationEntryBci) {
       // LLVM 代码已生成，现在可以 patch 跳转指令
       address llvm_entry = entry->entry_point();
       patch_osr_adapter_jump(&masm, llvm_entry);
     }
     // ========== End of patch ==========
     
     // Install the method into the VM
     CodeOffsets offsets;
     if (entry_bci != InvocationEntryBci) {
       offsets.set_value(CodeOffsets::OSR_Entry, 0);  // 适配器在开头
       offsets.set_value(CodeOffsets::Verified_Entry, osr_adapter_size);
     } else {
       offsets.set_value(CodeOffsets::Verified_Entry,
                         target->is_static() ? 0 : wordSize);
     }
     // ... 后续代码 ...
   }
   ```

2. **实现 `generate_osr_adapter` 函数**（使用 YuhuMacroAssembler 的方法）：
   ```cpp
   // 在 yuhuCompiler.hpp 中添加
   class YuhuCompiler {
     // ...
     YuhuLabel* _llvm_entry_label;  // 用于标记 LLVM 函数入口
     address _jump_to_llvm_pc;       // 保存跳转指令的地址，用于后续 patch
     
     void generate_osr_adapter(YuhuMacroAssembler* masm,
                                ciMethod* target,
                                YuhuEntry* entry);
     void patch_osr_adapter_jump(YuhuMacroAssembler* masm,
                                  address llvm_entry);
   };
   
   // 在 yuhuCompiler.cpp 中实现
   void YuhuCompiler::generate_osr_adapter(YuhuMacroAssembler* masm,
                                            ciMethod* target,
                                            YuhuEntry* entry) {
     // 适配器入口点（OSR entry point）
     address adapter_start = masm->current_pc();
     
     // 使用已知的 label 获取当前 PC
     YuhuLabel adapter_label;
     masm->pin_label(adapter_label);  // 绑定到当前位置
     
     // 1. 获取当前 PC（使用 adr 指令和已知的 label）
     masm->write_inst_adr(x16, adapter_label);  // x16 = 当前 PC (rscratch1)
     
     // 2. 保存 x0 (osr_buf) 到临时寄存器（x17 = rscratch2）
     masm->write_inst_mov_reg(x17, x0);  // 保存 osr_buf
     
     // 3. 调用 runtime 函数获取 nmethod
     // 设置参数：c_rarg0 = x0 = PC
     masm->write_inst_mov_reg(x0, x16);  // x0 = PC
     // 调用 CodeCache::find_nmethod(pc)
     masm->write_insts_final_call_VM_leaf(
       CAST_FROM_FN_PTR(address, CodeCache::find_nmethod),
       x0);
     // r0 现在包含 nmethod*
     
     // 4. 从 nmethod 获取 Method* 和 base_pc
     // nmethod->method() 偏移
     masm->write_inst_ldr(x19, YuhuAddress(x0, in_bytes(nmethod::method_offset())));
     // base_pc 就是 nmethod 的 code_begin()
     // nmethod 继承自 CodeBlob，code_begin() 就是 CodeBlob 的 content_begin()
     // 可以通过 nmethod 地址 + CodeBlob header 大小计算
     // 或者：nmethod 的 code_begin() 就是 nmethod 地址 + header_size
     // 简化：使用 nmethod 的 code_begin() 方法
     // 但 runtime 调用可能更简单：通过 nmethod->code_begin() 获取
     // 或者：base_pc 可以从 entry 中获取（如果已经设置）
     masm->write_inst_mov_reg(x20, x0);  // x20 = nmethod (临时保存)
     // 注意：base_pc 实际上就是 nmethod 的 code_begin()
     // 可以通过计算获取：nmethod + header_size
     // 或者调用 runtime 函数，但更简单的方法是直接计算
     // CodeBlob::header_size() 是固定的，可以编译时计算
     int header_size = CodeBlob::header_size();
     masm->write_inst("add %s, %s, #%d", x21, x0, header_size);  // x21 = base_pc
     
     // 5. 获取 thread
     masm->write_insts_get_thread(x21);  // x21 = thread
     
     // 6. 重新排列参数
     masm->write_inst_mov_reg(x0, x19);  // x0 = Method*
     masm->write_inst_mov_reg(x1, x17);  // x1 = osr_buf (保存的值)
     masm->write_inst_mov_reg(x2, x21);  // x2 = base_pc (从 nmethod 计算)
     masm->write_inst_mov_reg(x3, x22);  // x3 = thread (x21 被 base_pc 占用，改用 x22)
     
     // 修正：重新获取 thread（因为 x21 被 base_pc 占用了）
     masm->write_insts_get_thread(x22);  // x22 = thread
     masm->write_inst_mov_reg(x3, x22);  // x3 = thread
     
     // 7. 预留跳转指令（使用 label，后续 patch）
     _llvm_entry_label = new YuhuLabel();
     masm->write_inst_b(*_llvm_entry_label);  // 使用未绑定的 label，后续会自动 patch
     
     // 记录适配器代码大小
     address adapter_end = masm->current_pc();
     // adapter_size = adapter_end - adapter_start;
   }
   
   // 在 LLVM 代码生成后，patch 跳转指令
   void YuhuCompiler::patch_osr_adapter_jump(YuhuMacroAssembler* masm,
                                              address llvm_entry) {
     if (_llvm_entry_label != NULL) {
       // 设置 masm 的 PC 到 LLVM 函数入口
       // 注意：需要确保 masm 的 CodeBuffer 已经包含 LLVM 代码
       // 然后在该位置绑定 label
       
       // 方法：保存当前 PC，移动到 llvm_entry，绑定 label，恢复 PC
       address saved_pc = masm->current_pc();
       
       // 找到 llvm_entry 在 CodeBuffer 中的位置
       // 由于 LLVM 代码是通过 YuhuMemoryManager 分配的，可能不在同一个 CodeBuffer
       // 需要特殊处理：如果 LLVM 代码在另一个 CodeBuffer，可能需要使用绝对跳转
       
       // 简化方案：如果 LLVM 代码在同一个 CodeBuffer 中（通过合并），可以直接绑定
       // 否则：使用绝对跳转（adrp + add + br）
       
       // 这里假设 LLVM 代码已经合并到 CodeBuffer 中
       // 实际实现可能需要根据具体情况调整
     }
   }
   
   **注意**：由于 LLVM 代码是通过 `YuhuMemoryManager` 在 CodeCache 中分配的，
   而适配器代码在 `CodeBuffer` 中，两者可能不在同一块内存。
   可能需要：
   1. 将 LLVM 代码拷贝到 CodeBuffer 中（在适配器之后）
   2. 或者使用绝对跳转（adrp + add + br）跳转到 LLVM 代码地址
   ```
   
   **实现要点**：
   - **使用 `write_inst_adr` 和 label**：获取当前 PC，避免硬编码
   - **使用 `write_inst_mov_reg`**：寄存器间移动
   - **使用 `write_insts_final_call_VM_leaf`**：调用 runtime 函数
   - **使用 `write_insts_get_thread`**：获取当前 thread
   - **使用 YuhuLabel 和 `pin_label`**：实现延迟绑定的跳转
   - **两阶段生成**：先生成适配器（预留跳转），LLVM 生成后 patch

3. **设置 CodeOffsets**：
   ```cpp
   CodeOffsets offsets;
   if (entry_bci != InvocationEntryBci) {
     // OSR 方法：适配器代码在 CodeBuffer 的开头
     offsets.set_value(CodeOffsets::OSR_Entry, 0);  // 适配器在代码开头
     offsets.set_value(CodeOffsets::Verified_Entry, adapter_size);  // LLVM 代码在适配器之后
   } else {
     offsets.set_value(CodeOffsets::Verified_Entry, target->is_static() ? 0 : wordSize);
   }
   ```

4. **代码布局**：
   ```
   CodeBuffer 布局（OSR 方法）：
   [适配器代码] [LLVM 生成的代码]
   ^            ^
   |            |
   OSR_Entry    Verified_Entry (如果需要)
   
   注意：适配器代码和 LLVM 代码在同一个 CodeBuffer 中，适配器在前
   ```

5. **实现挑战和解决方案**（已解决）：
   - **挑战1**：适配器代码需要知道 LLVM 函数的入口地址
     - ✅ **解决**：使用 `YuhuLabel` 和 `pin_label` 机制
       - 在适配器中生成跳转时使用未绑定的 label
       - LLVM 代码生成后，在入口处 `pin_label` 绑定 label
       - label 会自动 patch 之前使用它的跳转指令
   
   - **挑战2**：获取当前 PC
     - ✅ **解决**：使用已知的 label 和 `write_inst_adr`
       - 在适配器开始处创建并绑定 label
       - 使用 `write_inst_adr(reg, label)` 获取当前 PC
       - 比使用 `lr` 或硬编码更可靠
   
   - **挑战3**：适配器代码和 LLVM 代码在同一个 CodeBuffer 中
     - ✅ **解决**：适配器代码放在 CodeBuffer 开头，LLVM 代码放在后面
       - 适配器代码先生成（在 LLVM IR 生成之前）
       - LLVM 代码生成到 CodeBuffer 的后续位置
       - 使用 `CodeOffsets` 正确设置入口点偏移

**优点**：
- ✅ **不需要修改 LLVM IR 生成逻辑**：适配器代码是独立的 HotSpot 代码
- ✅ **可以调用 runtime 函数**：适配器代码是 HotSpot 代码的一部分，可以调用 `CodeCache::find_blob`、`JavaThread::current()` 等
- ✅ **复用现有函数签名**：LLVM 生成的函数仍然使用 4 参数签名
- ✅ **实现相对简单**：使用 `YuhuMacroAssembler` 生成，类似于 C1 的 stub 代码生成

**缺点**：
- ⚠️ 需要手写适配器汇编代码（但可以使用 MacroAssembler，相对简单）
- ⚠️ 增加了代码复杂度（但比方案1简单）

**关键优势**：
- 适配器代码执行时，nmethod 已经创建并安装到 CodeCache
- 可以通过 `CodeCache::find_blob(pc)` 从当前 PC 获取 nmethod
- 不需要在 LLVM IR 中添加特殊的 runtime 调用或 intrinsic

#### 方案 3：检查 Shark/Zero 的实现

Shark 可能使用了特殊的机制来获取这些参数。需要深入调查：
- Zero 架构是否有特殊的寄存器或全局变量
- Shark 是否在某个地方添加了适配器代码
- 是否有 runtime 函数可以获取这些信息

## 需要检查的点

1. **Shark 的 OSR entry point 设置**：
   - Shark 如何设置 `osr_entry_point`？
   - 是否有适配器代码？

2. **C1/C2 的 OSR 处理**：
   - C1/C2 如何处理 OSR 参数传递？
   - 是否有适配器代码可以参考？

3. **nmethod 的 metadata**：
   - Method* 是否可以从 nmethod 中获取？
   - base_pc 是否可以从 nmethod 中获取？

4. **AArch64 调用约定**：
   - 是否有特殊的方式在运行时获取这些参数？

## 相关文件

- `hotspot/src/share/vm/yuhu/yuhuContext.cpp` - OSR entry point 类型定义
- `hotspot/src/share/vm/yuhu/yuhuFunction.cpp` - 参数解析
- `hotspot/src/cpu/aarch64/vm/templateTable_aarch64.cpp` - OSR 调用代码
- `hotspot/src/share/vm/shark/sharkCompiler.cpp` - Shark 的 OSR 处理（参考）
- `hotspot/src/share/vm/code/nmethod.cpp` - nmethod 的 OSR entry point 设置

## 推荐方案

**方案 2（添加 OSR 适配器代码）** 是推荐的实现方式，原因：

1. **实现更简单**：适配器代码是 HotSpot 代码，可以调用 runtime 函数，不需要在 LLVM IR 中添加特殊机制
2. **不需要修改 LLVM IR 生成**：保持现有的函数签名和 IR 生成逻辑不变
3. **符合 HotSpot 的设计模式**：C1/C2 也使用类似的 stub/adapter 机制
4. **易于调试**：适配器代码是独立的汇编代码，可以单独调试

**实现优先级**：
1. 首先实现方案 2（适配器代码）
2. 如果方案 2 遇到问题，再考虑方案 1

## 状态

- ✅ 问题已定位：OSR 方法参数传递不匹配
- ✅ 错误分析已完成
- ✅ 方案对比分析已完成
- ✅ **推荐方案 2（适配器代码）**
- ⏳ 等待实现适配器代码生成逻辑


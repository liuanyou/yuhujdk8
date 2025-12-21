# Invalid register name "x12" 错误分析 (023)

## 问题描述

在 `jit()->lookup(func_name)` 触发编译时，出现错误：
```
LLVM ERROR: Invalid register name "x12"
```

**重要更正**：错误发生在**编译时**（`lookup` 触发延迟编译时），而不是运行时！

### 日志分析

```
ORC JIT: Module added successfully        <-- addIRModule 成功
loop 3 c[0][0] 328350                    <-- 解释器执行（编译失败，回退到解释器）
loop 4 c[0][0] 328350                    <-- 解释器执行
loop 5 c[0][0] 328350                    <-- 解释器执行
LLVM ERROR: Invalid register name "x12". <-- 编译时错误（在 lookup 时触发）
```

**关键理解**：
- `lookup` 触发延迟编译
- 编译时验证寄存器名失败
- 编译失败后，程序回退到解释器执行
- 所以看到的 loop 3, 4, 5 是解释器执行的，不是编译后的代码

## 关键观察

### 1. 错误发生时机（正确理解）

- **`addIRModule` 成功**：模块添加成功（第 1158 行）
- **`lookup` 触发编译**：第 1168 行 `lookup` 触发延迟编译
- **编译时失败**：在代码生成阶段，LLVM 验证 `read_register` 的寄存器名时失败
- **回退到解释器**：编译失败后，HotSpot 回退到解释器执行，所以看到 loop 输出

这说明：
- IR 模块添加成功
- **编译阶段失败**（在 `lookup` 触发的延迟编译时）
- 错误发生在代码生成阶段，LLVM 验证寄存器名时

### 2. 延迟编译（Lazy Compilation）机制

LLVM ORC JIT 使用延迟编译：
- `addIRModule` 只是将模块添加到 JITDylib，不立即编译
- `lookup` 触发实际编译，此时会验证 IR 的正确性
- **在编译阶段（代码生成时），LLVM 验证 `read_register` 的寄存器名**
- 如果编译失败，程序回退到解释器执行

### 2. ORC JIT 的延迟编译机制

LLVM ORC JIT 使用延迟编译（lazy compilation）：
- `addIRModule` 只是将模块添加到 JITDylib，不立即编译
- `lookup` 触发实际编译，此时会验证 IR 的正确性
- 验证包括：寄存器名验证、类型检查、目标特定验证等

### 3. 寄存器名使用情况（测试结果）

代码中使用的寄存器名：
- `CreateReadStackPointer()` → `"sp"` ✅（标准 AArch64 寄存器名，工作正常）
- `CreateReadLinkRegister()` → `"x30"` ✅（标准 AArch64 寄存器名，工作正常）
- `CreateReadMethodRegister()` → `"x12"` ❌（报错）
- `CreateReadThreadRegister()` → `"x28"` ❌（报错）

**测试结果**：
- `x0` ✅ 可以编译通过
- `x1` ❌ 报错 "Invalid register name 'x1'"
- `x12` ❌ 报错 "Invalid register name 'x12'"
- `x28` ❌ 报错 "Invalid register name 'x28'"

**结论**：LLVM 的 `read_register` intrinsic 在 AArch64 上**只支持 `x0` 和特殊寄存器**（如 `sp`, `x30`），**不支持其他通用寄存器**（`x1`-`x30`，除了 `x0`）。

## 根本原因分析

### 假设 1：LLVM AArch64 后端在代码生成时验证寄存器名（最可能）

**可能性：非常高**

根据错误发生在编译时（`lookup` 触发延迟编译），问题在代码生成阶段：

1. **代码生成时的寄存器名验证**：
   - LLVM 在将 `read_register` intrinsic 转换为机器码时验证寄存器名
   - AArch64 后端可能只允许特定寄存器用于 `read_register`
   - `x12` 和 `x28` 可能不在允许列表中

2. **只允许特定寄存器**：
   - 可能只允许标准寄存器（如 `sp`, `x30`, `x29` 等）
   - 不允许通用寄存器（如 `x12`, `x28`）
   - 或者需要特殊配置才能使用通用寄存器

3. **需要 TargetMachine 配置**：
   - 寄存器名验证通过 `TargetMachine` 进行
   - 在 `lookup` 触发的编译过程中，`TargetMachine` 验证寄存器名
   - 可能需要显式配置哪些寄存器可以被 `read_register` 使用
   - 或者需要设置特定的 target features

### 假设 2：ThreadSafeContext 使用新 Context 导致问题

**可能性：中**

代码第 1149-1150 行：
```cpp
auto TSCtx = std::make_unique<llvm::orc::ThreadSafeContext>(
  std::make_unique<llvm::LLVMContext>());
```

问题：
- 创建了新的 `LLVMContext`，而不是使用原始模块的 context
- 虽然 `CloneModule` 保留了 metadata，但新的 context 可能没有正确的目标信息
- 在编译时（`lookup` 触发），可能无法正确获取目标信息来验证寄存器名

**注意**：代码生成主要依赖 TargetMachine，而不是 Context，所以这个假设的可能性中等

### 假设 3：TargetMachine 配置问题（最可能的原因）

**可能性：非常高**

代码第 235-240 行创建 LLJIT：
```cpp
auto JIT = llvm::orc::LLJITBuilder()
  .setJITTargetMachineBuilder(
    llvm::orc::JITTargetMachineBuilder(llvm::Triple(TripleStr))
      .setCPU(MCPU)
      .addFeatures(MAttrs))
  .create();
```

问题：
- `JITTargetMachineBuilder` 可能没有正确配置寄存器信息
- AArch64 后端在代码生成时（`lookup` 触发编译），可能需要显式配置哪些寄存器可以被 `read_register` 使用
- 或者需要设置特定的 target features 来启用通用寄存器的 `read_register` 支持
- **由于错误发生在编译时（代码生成阶段），这个假设最可能**

### 假设 4：LLVM 版本或配置问题

**可能性：低**

- LLVM 20.1.5 可能对 `read_register` 的寄存器名验证更严格
- 或者 Homebrew 编译的 LLVM 有特殊配置

## 验证方法

### 方法 1：检查 LLVM 文档和源码（优先）

需要查看：
1. **LLVM AArch64 后端对 `read_register` 的寄存器名要求**
   - 哪些寄存器可以被 `read_register` 使用
   - 是否有特殊配置要求
2. **`read_register` intrinsic 的代码生成逻辑**
   - 在 `AArch64ISelLowering.cpp` 或类似文件中
   - 如何验证和转换寄存器名
3. **ORC JIT 延迟编译的代码生成流程**
   - 何时验证寄存器名
   - 如何获取 TargetMachine 信息

### 方法 2：测试其他寄存器名（快速验证）

测试：
- 使用 `"x0"` 而不是 `"x12"`，看是否报错
- 使用 `"x30"`（已知工作），看是否仍然工作
- 使用 `"sp"`（已知工作），看是否仍然工作
- **如果 `x0` 也报错，说明问题可能是通用寄存器都不支持**
- **如果只有 `x12` 和 `x28` 报错，可能是特定寄存器的问题**

### 方法 3：检查 TargetMachine 配置（最可能解决）

检查：
- `JITTargetMachineBuilder` 是否正确配置
- 是否需要显式设置寄存器信息
- 是否需要调用 `TargetMachine` 的特定方法来验证寄存器名
- **可能需要设置 target features 来启用通用寄存器的 `read_register` 支持**

### 方法 4：使用内联汇编替代（备选方案）

如果 `read_register` 不支持 `x12` 和 `x28`，可以考虑：
- 使用内联汇编直接读取寄存器：
  ```cpp
  llvm::InlineAsm::get(
    llvm::FunctionType::get(intptr_type(), false),
    "mov $0, x12",  // 或 "mov $0, x28"
    "=r",
    true, false, false, false);
  ```
- 或者使用其他方式获取这些值（如通过函数参数传递）

## 解决方案：使用内联汇编读取寄存器（已实现）

### 实现方案

由于 LLVM 的 `read_register` intrinsic 不支持 `x12` 和 `x28`，我们使用内联汇编直接读取这些寄存器。

**位置**：`hotspot/src/share/vm/yuhu/yuhuBuilder.cpp`

**实现代码**：
```cpp
CallInst* YuhuBuilder::CreateReadMethodRegister() {
  // Read rmethod register (x12) on AArch64 using inline assembly
  // LLVM's read_register intrinsic doesn't support x12 (and other general-purpose registers except x0)
  // So we use inline assembly to directly read the register
  llvm::FunctionType* asm_type = llvm::FunctionType::get(YuhuType::intptr_type(), false);
  llvm::InlineAsm* asm_func = llvm::InlineAsm::get(
    asm_type,
    "mov $0, x12",  // AArch64 assembly: move x12 to output register
    "=r",           // Output constraint: =r means output to a register
    false,          // Has side effects: no
    false,          // Is align stack: no
    llvm::InlineAsm::AD_ATT    // Dialect: AT&T style (but for AArch64, this is ignored)
  );
  
  // LLVM 20+ requires FunctionType for CreateCall
  return CreateCall(asm_type, asm_func, std::vector<Value*>(), "rmethod");
}

CallInst* YuhuBuilder::CreateReadThreadRegister() {
  // Read rthread register (x28) on AArch64 using inline assembly
  // Similar implementation, using "mov $0, x28"
  // ...
}
```

**优点**：
- 不依赖 LLVM 的 `read_register` intrinsic
- 直接控制寄存器访问
- 可以读取任何寄存器

**缺点**：
- 需要平台特定代码（AArch64）
- 可能影响优化（但影响很小，因为只是简单的 mov 指令）

### 为什么 x0 可以但其他不行？

**可能的原因**：
1. **x0 是第一个参数寄存器**：在 AArch64 调用约定中，`x0` 是第一个参数/返回值寄存器，可能在 LLVM 中有特殊处理
2. **LLVM 的限制**：`read_register` intrinsic 可能只支持特定寄存器（如 `sp`, `x30`, `x0`），不支持其他通用寄存器
3. **目标特定的验证**：AArch64 后端在代码生成时，可能只允许读取特定寄存器，以防止破坏调用约定

## 其他可能的解决方案（未采用）

### 方案 2：通过 TargetMachine 验证寄存器名

**优点**：
- 使用 LLVM 的标准机制
- 保持代码可移植性

**缺点**：
- 需要了解 LLVM 内部 API
- 可能复杂
- **可能无法解决根本问题**（LLVM 可能根本不支持这些寄存器）

### 方案 2：通过 TargetMachine 验证寄存器名

**优点**：
- 使用 LLVM 的标准机制
- 保持代码可移植性

**缺点**：
- 需要了解 LLVM 内部 API
- 可能复杂

### 方案 3：使用寄存器别名或特殊格式

**优点**：
- 如果 LLVM 支持别名，可能简单

**缺点**：
- 需要确认 LLVM 是否支持
- 可能不适用于所有寄存器

### 方案 4：检查 LLVM 版本和配置

**优点**：
- 可能是配置问题，容易修复

**缺点**：
- 如果是 LLVM 限制，无法解决

## 下一步行动（按优先级）

1. **查阅 LLVM 源码**（最高优先级）：
   - 查看 `llvm/lib/Target/AArch64/AArch64ISelLowering.cpp`
   - 查找 `read_register` 的处理逻辑
   - 查看哪些寄存器名被允许

2. **测试其他寄存器**（快速验证）：
   - 临时将 `x12` 改为 `x0`，看是否仍然报错
   - 如果 `x0` 也报错，说明通用寄存器都不支持
   - 如果只有 `x12` 和 `x28` 报错，可能是特定问题

3. **检查 TargetMachine 配置**（最可能解决）：
   - 查看是否需要设置特定的 target features
   - 检查 `JITTargetMachineBuilder` 的配置选项
   - 可能需要显式启用通用寄存器的 `read_register` 支持

4. **考虑替代方案**（如果无法解决）：
   - 使用内联汇编直接读取寄存器
   - 或者通过函数参数传递这些值（回到旧设计）

## 相关文件

- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp:1168` - 错误发生位置
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp:531-558` - `CreateReadRegister()` 实现
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp:519-523` - `CreateReadMethodRegister()` 实现
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp:525-529` - `CreateReadThreadRegister()` 实现


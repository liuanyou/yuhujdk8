# 042: Yuhu编译器中x28寄存器在方法调用时被污染导致计算错误

## 问题描述

在Yuhu编译器编译的方法中，当执行方法调用时，`blr`指令跳转到适配器，适配器会修改x28和x12寄存器。由于x28寄存器被修改，导致后续使用x28作为源寄存器的指令计算错误，进而导致错误的跳转地址。

## 问题现象

在编译`sun/nio/cs/StreamEncoder.implWrite([CII)V`方法时，观察到以下错误行为：

```
0x000000010b1c371c: add    x8, x28, #0x540  // x28被污染，x8计算错误
0x000000010b1c372c: blr    x8              //到错误地址
```

## 问题分析

###根原因

Yuhu编译器与HotSpot运行时之间存在寄存器使用约定冲突：

1. **Yuhu编译器假设**：x28寄存器在方法执行期间保持不变，可以安全地用于计算跳转地址
2. **HotSpot运行时需求**：适配器需要使用x28寄存器存储Thread*指针
3. **调用流程冲突**：
   - Yuhu编译的代码通过`blr x8`调用方法
   - 控制流转到适配器
   - 适配器修改x28寄存器（如：`mov x28, #0x8500`）
   - 控制返回到Yuhu代码
   -后续使用x28的计算指令产生错误结果

### 详细调用流程

```
Yuhu编译代码:
    add x8, x28, #0x600     //正常：使用原始x28值计算
    blr x8                  //调用方法

适配器代码:
    mov x28, #0x8500        // 问题：修改了x28寄存器
    mov x12, <Method*>      // 修改x12寄存器
    ldr x8, [x12, #72]      // 加载_from_compiled_entry
    br x8                   //跳到目标方法

返回Yuhu代码:
    add x8, x28, #0x540     //错误：使用被修改的x28值计算
    blr x8                  //跳到错误地址
```

###代码证据

从`debug/sp_data_test.txt`中可以看到关键证据：

```assembly
; 适配器修改x28寄存器
0x000000010b1c3530: mov    x28, #0x8500                 // #34048
0x000000010b1c3534: movk   x28, #0xb1c, lsl #16
0x000000010b1c3538: movk   x28, #0x1, lsl #32

;后续使用被污染的x28寄存器导致错误
0x000000010b1c371c: add    x8, x28, #0x540              // x8 =错值
0x000000010b1c372c: blr    x8                           //跳到错误地址
```

##影响范围

###受的场景

1. **所有方法调用**：任何Yuhu编译的方法调用都可能遇到此问题
2. **间接跳转**：使用x28寄存器计算的跳转地址都可能错误
3. **运行时错误**：可能导致SIGSEGV、SIGILL或其他未定义行为

### 严重程度

- **严重性**：高 -导致程序执行错误
- **发生频率**：中 - 在有方法调用的Yuhu编译代码中都会出现
- **可重现性**：高 -可稳定重现

## 解决方案设计

### 方案一：LLVM JIT级别寄存器保留（推荐 -已实现）

在LLVM JIT TargetMachineBuilder中直接保留x28寄存器，从根本上防止LLVM使用该寄存器：

```cpp
// 在YuhuCompiler初始化时配置LLVM JIT
auto JTMB = llvm::orc::JITTargetMachineBuilder(llvm::Triple(TripleStr))
  .setCPU(MCPU)
  .addFeatures(MAttrs);

JTMB.addFeatures({"+reserve-x28"});  // 关键：保留x28寄存器

auto JIT = llvm::orc::LLJITBuilder()
  .setJITTargetMachineBuilder(JTMB)
  .create();
```

**优势**：
- **编译时解决**：LLVM在代码生成阶段就避免使用x28寄存器
- **零运行时开销**：无需额外的保存/恢复指令
- **根本性解决**：从源头防止寄存器冲突
- **性能最优**：避免了方案一的栈操作开销

### 方案二：寄存器保护机制（运行时保护）

在方法调用前后保护x28寄存器的值：

```cpp
//调用前保存x28
str x28, [sp, #-16]!    // 保存x28到栈上

//执行方法调用
add x8, x28, #0x600
blr x8

//调用后恢复x28
ldr x28, [sp], #16      // 从栈恢复x28
```

### 方案三：使用独立寄存器

为Yuhu编译器分配专用寄存器，避免与HotSpot运行时冲突：

```cpp
// 使用x19-x27寄存器存储Yuhu专用数据
// 保留x28给HotSpot运行时使用
add x19, x28, #0x600    // 使用x19而不是x8
blr x19
```

### 方案四：重新设计适配器接口

修改适配器设计，使其不修改x28寄存器：

```cpp
// 适配器使用临时寄存器而不是直接修改x28
mov x9, x28             // 临时保存x28
mov x28, #0x8500        // 修改x28
; ... 适配器逻辑 ...
mov x28, x9             //x28
```

## 实施建议

###已实现修复（推荐方案一）

该解决方案已在代码中实现：
1. **配置LLVM JIT**：在`YuhuCompiler::initialize()`中配置`JITTargetMachineBuilder`
2. **保留寄存器**：通过`JTMB.addFeatures({"+reserve-x28"})`保留x28寄存器
3. **编译时生效**：LLVM在代码生成时自动避免使用x28寄存器

###备方案（运行时保护）

如果LLVM JIT级别保留不生效，可采用运行时保护：

1. **识别关键位置**：在所有方法调用点前后插入寄存器保护代码
2. **修改代码生成**：更新YuhuBuilder中的方法调用生成逻辑
3. **测试验证**：确保修复后的方法调用正常工作

###长期重构建议

1. **寄存器分配优化**：为Yuhu编译器设计独立的寄存器使用策略
2. **适配器接口标准化**：建立清晰的编译器与运行时接口约定
3. **调用约定统一**：确保Yuhu编译器遵循HotSpot的寄存器使用规范

##相关问题

此问题与以下已知问题相关：
- [036: x28 thread register corruption fix](036_fix_x28_thread_register_corruption.md)
- [041: Yuhu calling convention parameter mismatch](041_yuhu_calling_convention_parameter_mismatch.md)

## 结论

这是一个由寄存器使用约定冲突导致的核心架构问题。通过在LLVM JIT TargetMachineBuilder中保留x28寄存器（`+reserve-x28`特性），从根本上解决了Yuhu编译器与HotSpot运行时的寄存器冲突问题。该解决方案具有零运行时开销、编译时生效的优势，是目前最优的解决策略。
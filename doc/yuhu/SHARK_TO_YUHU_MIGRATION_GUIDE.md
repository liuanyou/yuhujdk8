# Shark 到 Yuhu 迁移指南

## Shark 实现分析

### 核心架构

Shark 使用 LLVM 作为后端，将 Java 字节码编译为机器码。核心流程如下：

```
字节码 → ciTypeFlow → SharkFunction → LLVM IR → ExecutionEngine → 机器码
```

### 关键组件

#### 1. SharkCompiler（主编译器）

**职责**：
- 初始化 LLVM ExecutionEngine
- 管理编译上下文（normal_context 和 native_context）
- 协调整个编译流程

**关键代码**：
```cpp
SharkCompiler::SharkCompiler() {
  // 1. 创建执行引擎锁
  _execution_engine_lock = new Monitor(Mutex::leaf, "SharkExecutionEngineLock");
  
  // 2. 初始化 LLVM 多线程支持
  llvm_start_multithreaded();
  
  // 3. 初始化目标平台
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  
  // 4. 创建上下文
  _normal_context = new SharkContext("normal");
  _native_context = new SharkContext("native");
  
  // 5. 创建内存管理器
  _memory_manager = new SharkMemoryManager();
  
  // 6. 创建 ExecutionEngine
  EngineBuilder builder(_normal_context->module());
  builder.setJITMemoryManager(memory_manager());
  builder.setEngineKind(EngineKind::JIT);
  _execution_engine = builder.create();
}
```

#### 2. SharkContext（LLVM Context 管理）

**职责**：
- 管理 LLVM Module（每个上下文一个 Module）
- 管理函数队列（用于释放）
- 线程安全：normal_context 用于编译器线程，native_context 用于适配器生成

**关键代码**：
```cpp
class SharkContext {
private:
  llvm::LLVMContext _context;
  llvm::Module* _module;
  std::queue<llvm::Function*> _free_queue;
  
public:
  SharkContext(const char* name) {
    _module = new llvm::Module(name, _context);
  }
  
  llvm::Module* module() { return _module; }
  llvm::LLVMContext& context() { return _context; }
};
```

#### 3. SharkBuilder（LLVM IR 生成器）

**职责**：
- 基于 LLVM IRBuilder，提供 Java 特定的 IR 生成辅助方法
- 封装对 HotSpot 对象的访问（Thread、Method、OOP 等）

**关键代码**：
```cpp
class SharkBuilder : public IRBuilder<> {
public:
  // 访问 HotSpot 结构
  LoadInst* CreateValueOfStructEntry(Value* base, ByteSize offset, Type* type);
  Value* CreateArrayAddress(Value* arrayoop, BasicType basic_type, Value* index);
  
  // 调用 HotSpot 运行时函数
  Value* CreateCallToRuntime(SharkRuntime::Function function, ...);
};
```

#### 4. SharkFunction（字节码到 LLVM IR）

**职责**：
- 使用 ciTypeFlow 分析字节码
- 为每个字节码块创建 SharkTopLevelBlock
- 生成 LLVM Function

**关键流程**：
```cpp
Function* SharkFunction::build(ciEnv* env, SharkBuilder* builder, 
                               ciTypeFlow* flow, const char* name) {
  // 1. 创建 LLVM Function
  Function* function = Function::Create(entry_point_type(), 
                                       GlobalVariable::InternalLinkage, name);
  
  // 2. 为每个字节码块创建 Block
  for (int i = 0; i < block_count(); i++) {
    _blocks[i] = new SharkTopLevelBlock(this, flow->block_at(i));
  }
  
  // 3. 初始化所有进入的块
  for (int i = 0; i < block_count(); i++) {
    if (block(i)->entered())
      block(i)->initialize();
  }
  
  // 4. 生成 IR
  for (int i = 0; i < block_count(); i++) {
    if (block(i)->entered())
      block(i)->emit_IR();
  }
  
  return function;
}
```

#### 5. SharkTopLevelBlock（字节码块到 LLVM IR）

**职责**：
- 将单个字节码块转换为 LLVM BasicBlock
- 处理字节码指令（load、store、invoke 等）
- 管理状态（局部变量、操作数栈）

#### 6. SharkMemoryManager（代码内存管理）

**职责**：
- 管理 LLVM 生成的代码内存
- 跟踪函数到代码地址的映射
- 与 HotSpot 代码缓存集成

### 编译流程

```cpp
void SharkCompiler::compile_method(ciEnv* env, ciMethod* target, int entry_bci) {
  // 1. 类型流分析
  ciTypeFlow* flow = target->get_flow_analysis();
  
  // 2. 创建记录器（OopMap、调试信息等）
  env->set_oop_recorder(new OopRecorder(&arena));
  env->set_debug_info(new DebugInformationRecorder(...));
  
  // 3. 创建代码缓冲区和构建器
  CodeBuffer hscb("Shark", 256 * K, 64 * K);
  SharkCodeBuffer cb(masm);
  SharkBuilder builder(&cb);
  
  // 4. 生成 LLVM IR
  Function* function = SharkFunction::build(env, &builder, flow, name);
  
  // 5. 编译为机器码
  {
    ThreadInVMfromNative tiv(JavaThread::current());
    generate_native_code(entry, function, name);
  }
  
  // 6. 安装到 HotSpot
  env->register_method(target, entry_bci, &offsets, 0, &hscb, ...);
}

void SharkCompiler::generate_native_code(SharkEntry* entry, Function* function, 
                                          const char* name) {
  // 1. 将函数添加到模块
  context()->add_function(function);
  
  // 2. 在锁保护下编译
  {
    MutexLocker locker(execution_engine_lock());
    memory_manager()->set_entry_for_function(function, entry);
    code = (address) execution_engine()->getPointerToFunction(function);
  }
  
  // 3. 设置入口点
  entry->set_entry_point(code);
}
```

## 是否可以照搬到 Yuhu？

### ✅ 可以照搬的部分

1. **整体架构**：完全可以使用相同的架构
2. **核心组件**：所有核心组件都可以复用
3. **编译流程**：编译流程完全一致
4. **线程安全设计**：双上下文模式可以直接使用

### ⚠️ 需要适配的部分

#### 1. 类名和命名空间

```cpp
// Shark → Yuhu
SharkCompiler → YuhuCompiler
SharkContext → YuhuContext
SharkBuilder → YuhuBuilder
SharkFunction → YuhuFunction
SharkMemoryManager → YuhuMemoryManager
```

#### 2. LLVM 版本兼容性

**Shark 使用的 LLVM 版本**：
- JDK 8 中的 Shark 使用 LLVM 3.x - 6.x
- 使用旧的 JIT API（`getPointerToFunction`）

**Yuhu 需要考虑**：
- 如果使用 LLVM 10+，需要使用 ORC JIT API
- 如果使用 LLVM 6.x，可以完全照搬

**建议**：
```cpp
#if LLVM_VERSION_MAJOR >= 10
  // 使用 ORC JIT
  #include "llvm/ExecutionEngine/Orc/LLJIT.h"
#else
  // 使用传统 JIT（Shark 方式）
  #include "llvm/ExecutionEngine/JIT.h"
#endif
```

#### 3. 平台特定代码

**Shark 主要支持**：
- Zero 架构（软件模拟）
- 部分 x86 支持

**Yuhu 需要支持**：
- AArch64（主要目标）

**需要适配**：
```cpp
// Shark 中的平台特定代码
#ifdef TARGET_ARCH_zero
  // Zero 特定代码
#elif defined(TARGET_ARCH_x86)
  // x86 特定代码
#endif

// Yuhu 需要
#ifdef TARGET_ARCH_aarch64
  // AArch64 特定代码
#endif
```

#### 4. 代码生成细节

**Shark 的代码生成**：
- 使用 `SharkCodeBuffer` 包装 HotSpot 的 `CodeBuffer`
- 使用 `MacroAssembler` 生成适配代码

**Yuhu 可以**：
- 完全照搬 `SharkCodeBuffer` 的设计
- 使用 Yuhu 的 `MacroAssembler`（如果已有）

## 迁移步骤

### 步骤 1：复制并重命名核心文件

```bash
# 复制 Shark 文件
cp hotspot/src/share/vm/shark/sharkCompiler.* hotspot/src/share/vm/yuhu/
cp hotspot/src/share/vm/shark/sharkContext.* hotspot/src/share/vm/yuhu/
cp hotspot/src/share/vm/shark/sharkBuilder.* hotspot/src/share/vm/yuhu/
cp hotspot/src/share/vm/shark/sharkFunction.* hotspot/src/share/vm/yuhu/
cp hotspot/src/share/vm/shark/sharkMemoryManager.* hotspot/src/share/vm/yuhu/
cp hotspot/src/share/vm/shark/sharkTopLevelBlock.* hotspot/src/share/vm/yuhu/
# ... 其他文件

# 重命名
# 可以使用脚本批量替换类名
```

### 步骤 2：修改类名和命名

```cpp
// 全局替换
Shark → Yuhu
shark → yuhu
SHARK → YUHU
```

### 步骤 3：适配 LLVM 版本

```cpp
// yuhu_Compiler.cpp
#if LLVM_VERSION_MAJOR >= 10
  // 使用 ORC JIT
  auto JIT = llvm::orc::LLJITBuilder().create();
  // ...
#else
  // 使用 Shark 的方式
  EngineBuilder builder(_normal_context->module());
  builder.setJITMemoryManager(memory_manager());
  _execution_engine = builder.create();
#endif
```

### 步骤 4：适配平台

```cpp
// yuhu_Compiler.cpp
#ifdef TARGET_ARCH_aarch64
  // AArch64 特定初始化
  InitializeAArch64Target();
  InitializeAArch64TargetAsmPrinter();
#elif defined(TARGET_ARCH_zero)
  // Zero 特定初始化（如果需要）
  InitializeZeroTarget();
#endif
```

### 步骤 5：集成到 CompileBroker

```cpp
// compileBroker.cpp
void CompileBroker::compilation_init() {
  // ...
  
#ifdef YUHU_COMPILER
  if (UseYuhuCompiler) {
    _compilers[2] = new YuhuCompiler();
  }
#endif
}
```

## 关键差异总结

| 方面 | Shark | Yuhu（需要） |
|------|-------|--------------|
| **类名** | Shark* | Yuhu* |
| **LLVM 版本** | 3.x - 6.x | 6.x - 10+ |
| **主要平台** | Zero | AArch64 |
| **JIT API** | 传统 JIT | ORC JIT（如果 LLVM 10+） |
| **架构** | ✅ 可以完全照搬 | ✅ 可以完全照搬 |

## 建议

### ✅ 推荐做法

1. **完全照搬架构**：Shark 的架构设计很好，可以直接使用
2. **逐步迁移**：
   - 先复制所有文件并重命名
   - 然后适配 LLVM 版本
   - 最后适配平台特定代码
3. **保持兼容**：如果可能，同时支持旧版和新版 LLVM API

### ⚠️ 注意事项

1. **LLVM 版本**：如果使用 LLVM 10+，需要适配 ORC JIT API
2. **平台支持**：确保 AArch64 目标正确初始化
3. **测试**：每个步骤都要充分测试

## 结论

**可以照搬**！Shark 的实现非常完整，可以直接作为 Yuhu 的基础。主要工作是：

1. ✅ 复制并重命名文件
2. ✅ 适配 LLVM 版本（如果需要）
3. ✅ 适配平台（AArch64）
4. ✅ 集成到 CompileBroker

Shark 的架构设计已经考虑了线程安全、内存管理、错误处理等关键问题，这些都是可以直接复用的。


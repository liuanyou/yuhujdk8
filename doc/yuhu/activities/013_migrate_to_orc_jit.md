# 活动 013: 迁移到 ORC JIT 解决 Module 编译问题

## 日期
2025-12-09

## 问题背景

在活动 012 中，我们发现了一个关键问题：
- 第一次编译（OSR）：成功，`getPointerToFunction` 返回有效地址
- 第二次编译（普通入口）：失败，`getPointerToFunction` 返回 NULL
- Module 中确实包含了新添加的 Function（从 2 个增加到 3 个）
- 但 ExecutionEngine 无法处理新添加的 Function

**根本原因**：LLVM 20 的 MCJIT（通过 ExecutionEngine）在第一次编译后，可能将 Module 标记为"已完成"，不再处理新添加的 Function。

## MCJIT vs ORC JIT

### MCJIT 的问题

1. **全局符号表共享**：
   - 所有 Module 共享同一个全局符号表
   - 模块间可能出现符号冲突

2. **模块间符号冲突**：
   - 不同 Module 中的同名函数可能冲突
   - 难以管理多个编译会话

3. **难以管理多个编译会话**：
   - 一旦 Module 被编译，可能无法再添加新函数
   - 缺乏隔离机制

### ORC JIT 的优势

1. **完全隔离：JITDylib 天然支持隔离编译**
   - 每个编译会话使用独立的 JITDylib
   - 避免模块间符号冲突
   - 支持动态添加新函数

2. **更好的符号管理：不会出现 MCJIT 的全局符号冲突**
   - 灵活的符号解析策略
   - 支持自定义符号解析器

3. **aarch64 优化：LLVM 对 ORC 在 aarch64 上投入更多优化**
   - 更好的性能
   - 更稳定的实现

4. **未来证明：MCJIT 已进入维护模式，ORC 是未来方向**
   - MCJIT 不再积极开发
   - ORC JIT 是 LLVM JIT API 的未来

## 迁移方案

### 方案概述

**当前实现**（MCJIT）：
```cpp
// 使用 EngineBuilder 创建 ExecutionEngine
EngineBuilder builder(std::move(module_ptr));
builder.setMCJITMemoryManager(std::move(mm_ptr));
_execution_engine = builder.create();

// 编译函数
code = (address) execution_engine()->getPointerToFunction(function);
```

**目标实现**（ORC JIT）：
```cpp
// 使用 ORC JIT 创建 JITDylib
auto JIT = llvm::orc::LLJITBuilder().create();
auto &JD = JIT->getMainJITDylib();

// 添加 Module 到 JITDylib
JIT->addIRModule(JD, ThreadSafeModule(std::move(module), TSCtx));

// 查找函数地址
auto Sym = JIT->lookup(function_name);
code = (address) Sym->getAddress();
```

### 关键改动点

#### 1. 头文件包含

**当前**：
```cpp
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
```

**ORC JIT**：
```cpp
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/MemoryMapper.h>  // 如果需要自定义 MemoryMapper
```

#### 2. ExecutionEngine 替换为 ORC JIT

**当前**：
```cpp
class YuhuCompiler {
  llvm::ExecutionEngine* _execution_engine;
  RTDyldMemoryManager* _memory_manager;
};
```

**ORC JIT**：
```cpp
class YuhuCompiler {
  std::unique_ptr<llvm::orc::LLJIT> _jit;
  // MemoryManager 由 ORC JIT 内部管理
};
```

#### 3. 初始化代码

**当前**（MCJIT）：
```cpp
EngineBuilder builder(std::move(module_ptr));
builder.setMCJITMemoryManager(std::move(mm_ptr));
_execution_engine = builder.create();
```

**ORC JIT**：
```cpp
// 创建 LLJIT
auto JIT = llvm::orc::LLJITBuilder()
  .setJITTargetMachineBuilder(
    llvm::orc::JITTargetMachineBuilder(TT))
  .create();
  
if (!JIT) {
  fatal("Failed to create LLJIT");
}

_jit = std::move(*JIT);
```

#### 4. 添加 Module

**当前**（MCJIT）：
```cpp
execution_engine()->addModule(std::move(module_ptr));
```

**ORC JIT**：
```cpp
// 创建 ThreadSafeModule
auto TSCtx = std::make_unique<llvm::orc::ThreadSafeContext>(
  std::make_unique<llvm::LLVMContext>());
  
auto TSM = llvm::orc::ThreadSafeModule(
  std::move(module), TSCtx);

// 添加到 JITDylib
auto &JD = _jit->getMainJITDylib();
if (auto Err = _jit->addIRModule(JD, std::move(TSM))) {
  fatal("Failed to add IR module");
}
```

#### 5. 编译函数

**当前**（MCJIT）：
```cpp
code = (address) execution_engine()->getPointerToFunction(function);
```

**ORC JIT**：
```cpp
// 查找函数符号
std::string func_name = function->getName().str();
auto Sym = _jit->lookup(func_name);
if (!Sym) {
  std::string ErrMsg;
  llvm::handleAllErrors(Sym.takeError(), [&](const llvm::ErrorInfoBase &EIB) {
    ErrMsg = EIB.message();
  });
  fatal(err_msg("Failed to lookup function: %s", ErrMsg.c_str()));
}

// 在 LLVM 20 中，lookup 返回 Expected<ExecutorAddr>
// ExecutorAddr 可以直接转换为地址（getValue() 返回 uint64_t）
code = (address) Sym->getValue();
```

#### 6. MemoryManager 集成

**当前**（MCJIT）：
```cpp
class YuhuMemoryManager : public RTDyldMemoryManager {
  uint8_t* allocateCodeSection(...) {
    // 从 CodeCache 分配内存
  }
};
```

**ORC JIT**：
```cpp
// ORC JIT 使用 JITLink 或 RuntimeDyld
// 需要自定义 MemoryManager 或使用 ORC 的 MemoryMapper

class YuhuMemoryMapper : public llvm::orc::MemoryMapper {
  // 实现 allocate 和 deallocate
  // 从 CodeCache 分配内存
};
```

## 需要修改的类

### 1. **YuhuCompiler** (主要修改)

**文件**：
- `hotspot/src/share/vm/yuhu/yuhuCompiler.hpp`
- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp`

#### 1.1 类成员变量（`yuhuCompiler.hpp`）

**当前**：
```cpp
class YuhuCompiler {
private:
  Monitor*               _execution_engine_lock;
  YuhuMemoryManager*    _memory_manager;
  llvm::ExecutionEngine* _execution_engine;
  
  llvm::ExecutionEngine* execution_engine() const;
  YuhuMemoryManager* memory_manager() const;
};
```

**修改为**（已实施）：
```cpp
class YuhuCompiler {
private:
  Monitor*               _execution_engine_lock;  // 保留，用于保护 ORC JIT
  // ORC JIT (LLVM 11+) - recommended for LLVM 20
  std::unique_ptr<llvm::orc::LLJIT> _jit;
  // MemoryManager 由 ORC JIT 内部管理，或通过自定义 MemoryMapper
  // TODO: In stage 2, integrate CodeCache via MemoryMapper
  
  llvm::orc::LLJIT* jit() const {
    assert(execution_engine_lock()->owned_by_self(), "should be");
    return _jit.get();
  }
  
  // 析构函数声明（在 .cpp 中定义）
  ~YuhuCompiler();
};
```

**注意**：已移除所有 MCJIT 相关代码和条件编译。

#### 1.2 初始化代码（`yuhuCompiler.cpp` 构造函数）

**当前**：
```cpp
// 使用 EngineBuilder 创建 ExecutionEngine
EngineBuilder builder(std::move(module_ptr));
builder.setMCJITMemoryManager(std::move(mm_ptr));
_execution_engine = builder.create();
```

**修改为**：
```cpp
#if LLVM_VERSION_MAJOR >= 11
  // ORC JIT 初始化
  auto JIT = llvm::orc::LLJITBuilder()
    .setJITTargetMachineBuilder(
      llvm::orc::JITTargetMachineBuilder(TT))
    .create();
    
  if (!JIT) {
    fatal("Failed to create LLJIT");
  }
  
  _jit = std::move(*JIT);
#else
  // MCJIT 初始化（现有代码）
  EngineBuilder builder(std::move(module_ptr));
  builder.setMCJITMemoryManager(std::move(mm_ptr));
  _execution_engine = builder.create();
#endif
```

#### 1.3 添加 Module（`yuhuCompiler.cpp` 初始化）

**当前**：
```cpp
execution_engine()->addModule(std::move(native_module_ptr));
```

**修改为**：
```cpp
#if LLVM_VERSION_MAJOR >= 11
  // ORC JIT: 添加 Module 到 JITDylib
  auto &JD = _jit->getMainJITDylib();
  auto TSCtx = std::make_unique<llvm::orc::ThreadSafeContext>(
    std::make_unique<llvm::LLVMContext>());
  auto TSM = llvm::orc::ThreadSafeModule(
    std::move(native_module_ptr), TSCtx);
  if (auto Err = _jit->addIRModule(JD, std::move(TSM))) {
    fatal("Failed to add IR module");
  }
#else
  execution_engine()->addModule(std::move(native_module_ptr));
#endif
```

#### 1.4 编译函数（`yuhuCompiler.cpp::generate_native_code`）

**当前**：
```cpp
code = (address) execution_engine()->getPointerToFunction(function);
```

**修改为**：
```cpp
#if LLVM_VERSION_MAJOR >= 11
  // ORC JIT: 查找函数符号
  auto Sym = _jit->lookup(function->getName());
  if (!Sym) {
    fatal("Failed to lookup function");
  }
  code = (address) Sym->getAddress();
#else
  code = (address) execution_engine()->getPointerToFunction(function);
#endif
```

### 2. **YuhuMemoryManager** (可能需要适配或替换)

**文件**：
- `hotspot/src/share/vm/yuhu/yuhuMemoryManager.hpp`
- `hotspot/src/share/vm/yuhu/yuhuMemoryManager.cpp`

#### 选项 A：创建新的 YuhuMemoryMapper（推荐）

**新文件**：`hotspot/src/share/vm/yuhu/yuhuMemoryMapper.hpp`

```cpp
#if LLVM_VERSION_MAJOR >= 11
class YuhuMemoryMapper : public llvm::orc::MemoryMapper {
private:
  // 从 CodeCache 分配内存的逻辑
  // 类似于 YuhuMemoryManager::allocateCodeSection
  
public:
  // 实现 MemoryMapper 接口
  llvm::Expected<std::unique_ptr<llvm::orc::MappedMemoryBlock>>
  allocate(const llvm::orc::JITTargetAddress &Size, unsigned Alignment) override;
  
  llvm::Error deallocate(const llvm::orc::MappedMemoryBlock &Block) override;
  
  // 其他必要的方法
};
#endif
```

#### 选项 B：保留 YuhuMemoryManager，但适配 ORC JIT

如果 ORC JIT 支持自定义内存分配器，可以：
- 保留 `YuhuMemoryManager` 的 CodeCache 分配逻辑
- 通过 ORC JIT 的配置接口集成

### 3. **llvmHeaders.hpp** (添加 ORC JIT 头文件)

**文件**：
- `hotspot/src/share/vm/yuhu/llvmHeaders.hpp`

**修改内容**：

```cpp
#if LLVM_VERSION_MAJOR >= 11
  // ORC JIT (LLVM 11+)
  #include <llvm/ExecutionEngine/Orc/LLJIT.h>
  #include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
  #include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
  #include <llvm/ExecutionEngine/Orc/Mangling.h>
  #include <llvm/ExecutionEngine/Orc/MemoryMapper.h>  // 如果需要自定义 MemoryMapper
#else
  // MCJIT (LLVM < 11)
  #include <llvm/ExecutionEngine/ExecutionEngine.h>
  #include <llvm/ExecutionEngine/MCJIT.h>
  #include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
#endif
```

### 4. **YuhuContext** (可能需要调整)

**文件**：
- `hotspot/src/share/vm/yuhu/yuhuContext.hpp`
- `hotspot/src/share/vm/yuhu/yuhuContext.cpp`

**修改内容**：

可能需要调整 Module 的创建和管理方式，因为：
- ORC JIT 使用 `ThreadSafeModule`，需要 `ThreadSafeContext`
- Module 的添加时机可能不同

**注意**：如果保持向后兼容（支持 MCJIT），可能需要条件编译。

### 5. **其他可能受影响的类**

#### 5.1 **YuhuEntry** (可能不需要修改)

**文件**：
- `hotspot/src/share/vm/yuhu/yuhuEntry.hpp`

**说明**：
- `YuhuEntry` 主要用于存储代码地址和大小
- ORC JIT 返回的地址格式应该与 MCJIT 兼容
- 可能不需要修改

#### 5.2 **YuhuFunction** (可能不需要修改)

**文件**：
- `hotspot/src/share/vm/yuhu/yuhuFunction.hpp`
- `hotspot/src/share/vm/yuhu/yuhuFunction.cpp`

**说明**：
- Function 的创建方式不变（仍然使用 `Function::Create`）
- 只是编译方式改变（从 `getPointerToFunction` 改为 `lookup`）
- 可能不需要修改

## 实现步骤

### 步骤 1：添加 ORC JIT 头文件

修改 `hotspot/src/share/vm/yuhu/llvmHeaders.hpp`：
```cpp
#if LLVM_VERSION_MAJOR >= 11
  // ORC JIT (LLVM 11+)
  #include <llvm/ExecutionEngine/Orc/LLJIT.h>
  #include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
  #include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
  #include <llvm/ExecutionEngine/Orc/Mangling.h>
#else
  // MCJIT (LLVM < 11)
  #include <llvm/ExecutionEngine/ExecutionEngine.h>
  #include <llvm/ExecutionEngine/MCJIT.h>
#endif
```

### 步骤 2：修改 YuhuCompiler 类定义（已完成）

修改 `hotspot/src/share/vm/yuhu/yuhuCompiler.hpp`：
```cpp
// ORC JIT (LLVM 11+) - recommended for LLVM 20
std::unique_ptr<llvm::orc::LLJIT> _jit;
// MemoryManager 由 ORC JIT 内部管理，或通过自定义 MemoryMapper
// TODO: In stage 2, integrate CodeCache via MemoryMapper

// 添加析构函数声明（在 .cpp 中定义）
~YuhuCompiler();

// 添加访问器方法
llvm::orc::LLJIT* jit() const {
  assert(execution_engine_lock()->owned_by_self(), "should be");
  return _jit.get();
}
```

**注意**：已移除 MCJIT 相关成员变量和方法。

### 步骤 3：修改初始化代码（已完成）

修改 `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` 的构造函数：
```cpp
// ORC JIT 初始化 (LLVM 11+)
// Get target triple from module
std::string TripleStr = _normal_context->module()->getTargetTriple();
if (TripleStr.empty()) {
  TripleStr = llvm::sys::getDefaultTargetTriple();
}

// Create LLJIT with target machine builder
auto JIT = llvm::orc::LLJITBuilder()
  .setJITTargetMachineBuilder(
    llvm::orc::JITTargetMachineBuilder(llvm::Triple(TripleStr))
      .setCPU(MCPU)
      .addFeatures(MAttrs))
  .create();

if (!JIT) {
  std::string ErrMsg;
  llvm::handleAllErrors(JIT.takeError(), [&](const llvm::ErrorInfoBase &EIB) {
    ErrMsg = EIB.message();
  });
  fatal(err_msg("Failed to create LLJIT: %s", ErrMsg.c_str()));
}

_jit = std::move(*JIT);

// Add modules to JITDylib using ThreadSafeModule
// (具体实现见上面的代码示例)
```

**注意**：已移除 MCJIT 初始化代码。

### 步骤 4：修改编译函数代码（已完成）

修改 `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` 的 `generate_native_code`：
```cpp
// ORC JIT: 查找函数符号
std::string func_name = function->getName().str();
auto Sym = jit()->lookup(func_name);
if (!Sym) {
  // Function not found - may need to add module
  // (重新添加 Module 到 JITDylib 的逻辑)
  Sym = jit()->lookup(func_name);
  if (!Sym) {
    std::string ErrMsg;
    llvm::handleAllErrors(Sym.takeError(), [&](const llvm::ErrorInfoBase &EIB) {
      ErrMsg = EIB.message();
    });
    fatal(err_msg("Failed to lookup function: %s", ErrMsg.c_str()));
  }
}

// 在 LLVM 20 中，lookup 返回 Expected<ExecutorAddr>
// ExecutorAddr 可以直接转换为地址（getValue() 返回 uint64_t）
code = (address) Sym->getValue();

// 阶段 1：使用默认内存管理（mm_base=NULL, mm_size=0）
// 阶段 2：将从 CodeCache 获取实际地址和大小
```

**注意**：已移除 MCJIT 的 `getPointerToFunction()` 调用。

### 步骤 5：处理 MemoryManager（阶段 2 待实施）

**阶段 1**：暂时使用 ORC JIT 的默认内存管理
- `mm_base = NULL`, `mm_size = 0`
- 代码不从 CodeCache 分配
- 这是阶段 1 的预期行为

**阶段 2**：集成 CodeCache 内存管理
ORC JIT 使用不同的内存管理机制，需要：
1. 实现自定义 `MemoryMapper` 或使用 ORC 的默认实现
2. 确保从 CodeCache 分配内存
3. 处理内存权限（可执行）

### 步骤 6：更新链接配置（已完成）

修改 `hotspot/make/bsd/makefiles/yuhu.make`：
```makefile
# 添加 ORC JIT 相关组件
LLVM_LIBS_RAW := $(shell $(LLVM_CONFIG) --libs --link-static \
  executionengine mcjit interpreter aarch64 \
  orcjit orcshared orctargetprocess 2>/dev/null)
```

**注意**：ORC JIT 需要 `orcjit`, `orcshared`, `orctargetprocess` 组件。

## 修改优先级

### 高优先级（必须修改）

1. **YuhuCompiler** - 核心修改，替换 ExecutionEngine 为 ORC JIT
2. **llvmHeaders.hpp** - 添加 ORC JIT 头文件

### 中优先级（可能需要修改）

3. **YuhuMemoryManager** - 适配 ORC JIT 的内存管理机制
   - 选项 A：创建新的 `YuhuMemoryMapper`
   - 选项 B：保留现有逻辑，通过 ORC JIT 配置集成

### 低优先级（可能不需要修改）

4. **YuhuContext** - 可能需要调整 Module 管理
5. **YuhuEntry** - 可能不需要修改
6. **YuhuFunction** - 可能不需要修改

## 实施步骤建议

### 阶段 1：基础迁移（最小可行版本）

1. 修改 `llvmHeaders.hpp` 添加 ORC JIT 头文件
2. 修改 `YuhuCompiler` 类定义（添加 `_jit` 成员）
3. 修改 `YuhuCompiler` 初始化代码（创建 LLJIT）
4. 修改 `generate_native_code`（使用 `lookup` 替代 `getPointerToFunction`）
5. **暂时使用 ORC JIT 的默认内存管理**（不集成 CodeCache）

**目标**：验证 ORC JIT 可以成功编译函数

### 阶段 2：内存管理集成

6. 创建 `YuhuMemoryMapper` 或适配 `YuhuMemoryManager`
7. 配置 ORC JIT 使用自定义内存分配器
8. 确保从 CodeCache 分配内存

**目标**：确保代码从 CodeCache 分配，符合 HotSpot 的要求

### 阶段 3：完整测试和优化

9. 测试 OSR 编译
10. 测试普通入口编译
11. 测试多个方法编译
12. 性能测试和优化

**目标**：确保所有功能正常，性能可接受

## 优势分析

### 解决当前问题

1. **隔离编译**：
   - 每个编译会话使用独立的 JITDylib
   - 避免 Module 状态问题
   - 支持动态添加新函数

2. **符号管理**：
   - 不会出现全局符号冲突
   - 灵活的符号解析策略

3. **并发支持**：
   - 支持并发编译
   - 更好的性能

### 长期优势

1. **未来证明**：
   - MCJIT 已进入维护模式
   - ORC JIT 是未来方向

2. **更好的 aarch64 支持**：
   - LLVM 在 ORC 上投入更多优化
   - 更好的性能和稳定性

3. **更现代的 API**：
   - 更清晰的接口
   - 更好的错误处理

## 风险评估

### 工作量

- **中等**：需要修改多个文件
- **主要改动**：
  - `yuhuCompiler.hpp` - 类定义
  - `yuhuCompiler.cpp` - 初始化和编译逻辑
  - `llvmHeaders.hpp` - 头文件包含
  - `yuhuMemoryManager.*` - 可能需要适配 ORC JIT

### 兼容性

- **LLVM 版本要求**：LLVM 11+ 才支持 ORC JIT
- **当前版本**：LLVM 20，完全支持

### 测试

- 需要全面测试：
  - OSR 编译
  - 普通入口编译
  - 多个方法编译
  - 内存管理

## 注意事项

1. **版本要求**：
   - **已实施决策**：移除了 LLVM < 11 的 MCJIT 支持
   - 代码只支持 ORC JIT（LLVM 11+）
   - LLVM 20.1.5 完全支持

2. **线程安全**：
   - ORC JIT 本身是线程安全的
   - `_execution_engine_lock` 仍然保留（用于保护其他共享资源）

3. **错误处理**：
   - ORC JIT 使用 `Expected<T>` 进行错误处理
   - 已实现使用 `handleAllErrors` 正确处理错误情况

4. **符号解析**：
   - ORC JIT 使用默认符号解析器
   - 未来可能需要自定义符号解析策略（如果需要）

5. **内存管理**：
   - **阶段 1**：使用 ORC JIT 的默认内存管理
   - **阶段 2**：将通过 `MemoryMapper` 集成 CodeCache

## 相关文件清单

### 必须修改的文件

1. `hotspot/src/share/vm/yuhu/yuhuCompiler.hpp`
2. `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp`
3. `hotspot/src/share/vm/yuhu/llvmHeaders.hpp`

### 可能需要修改的文件

4. `hotspot/src/share/vm/yuhu/yuhuMemoryManager.hpp`
5. `hotspot/src/share/vm/yuhu/yuhuMemoryManager.cpp`
6. `hotspot/src/share/vm/yuhu/yuhuContext.hpp`（如果需要调整 Module 管理）

### 可能需要创建的新文件

7. `hotspot/src/share/vm/yuhu/yuhuMemoryMapper.hpp`（如果选择选项 A）
8. `hotspot/src/share/vm/yuhu/yuhuMemoryMapper.cpp`（如果选择选项 A）

## 推荐方案

**建议采用 ORC JIT 迁移方案**：

1. **解决根本问题**：ORC JIT 的 JITDylib 隔离机制可以完全解决 Module 状态问题
2. **未来证明**：MCJIT 已进入维护模式，ORC 是未来方向
3. **更好的支持**：LLVM 在 ORC 上投入更多优化，特别是 aarch64

**实施状态**：
1. ✅ **阶段 1 已完成**：实现了基础迁移，验证了基本功能
2. ✅ **已移除 MCJIT 代码**：代码只支持 ORC JIT（LLVM 11+）
3. ⏳ **阶段 2 待实施**：集成 CodeCache 内存管理
4. ⏳ **阶段 3 待实施**：完整测试和优化

## 参考资源

- [LLVM ORC JIT 文档](https://llvm.org/docs/ORCv2.html)
- [ORC JIT API 参考](https://llvm.org/doxygen/classllvm_1_1orc_1_1LLJIT.html)
- [从 MCJIT 迁移到 ORC JIT](https://llvm.org/docs/ORCv2.html#transitioning-from-mcjit-to-orc)

## 实施状态

### 阶段 1：基础迁移（已完成）

✅ **已完成的工作**：

1. **头文件更新** (`llvmHeaders.hpp`)
   - 添加了 ORC JIT 相关头文件
   - 移除了 MCJIT 的条件编译（只支持 LLVM 11+）
   - 添加了 `#include <llvm/Transforms/Utils/Cloning.h>` 用于 CloneModule
   - 添加了 `#include <llvm/Support/Error.h>` 用于错误处理

2. **类定义更新** (`yuhuCompiler.hpp`)
   - 移除了 `_execution_engine` 和 `_memory_manager` 成员
   - 添加了 `std::unique_ptr<llvm::orc::LLJIT> _jit` 成员
   - 添加了 `jit()` 访问器方法
   - 添加了析构函数声明（在 .cpp 中定义）

3. **初始化代码** (`yuhuCompiler.cpp`)
   - 移除了 MCJIT 的 `EngineBuilder` 初始化代码
   - 实现了 ORC JIT 的 `LLJITBuilder` 初始化
   - 使用 `ThreadSafeModule` 添加 Module 到 JITDylib
   - 注意：`ThreadSafeModule` 构造函数使用 `*TSCtx` 而不是 `std::move(TSCtx)`

4. **函数编译** (`generate_native_code`)
   - 移除了 `getPointerToFunction()` 调用
   - 实现了 `jit()->lookup()` 调用
   - 使用 `Sym->getValue()` 获取函数地址（LLVM 20 中返回 `uint64_t`）
   - 暂时使用默认内存管理（mm_base=NULL, mm_size=0）

5. **链接配置** (`yuhu.make`)
   - 添加了 ORC JIT 相关组件：`orcjit`, `orcshared`, `orctargetprocess`
   - 更新了静态链接和动态链接的库列表

6. **代码清理**
   - 移除了所有 `_memory_manager` 相关代码
   - 移除了所有 `execution_engine()` 相关代码
   - 移除了 LLVM < 11 的条件编译

### 已知问题和限制

1. **内存管理**（阶段 1 限制）
   - 当前使用 ORC JIT 的默认内存管理
   - 代码不从 CodeCache 分配（mm_base=NULL, mm_size=0）
   - 这将在阶段 2 中通过 MemoryMapper 集成解决

2. **Module 管理**
   - 每次编译新函数时，如果 lookup 失败，会重新添加整个 Module
   - 这可能不是最优方案，但可以工作
   - 未来可以考虑更细粒度的 Module 管理

### 阶段 2：内存管理集成（待实施）

⏳ **待实施的工作**：

1. 创建 `YuhuMemoryMapper` 类
   - 实现 `llvm::orc::MemoryMapper` 接口
   - 从 CodeCache 分配内存
   - 处理内存权限（可执行）

2. 配置 ORC JIT 使用自定义 MemoryMapper
   - 在 `LLJITBuilder` 中设置 MemoryMapper
   - 确保代码从 CodeCache 分配

3. 更新 `generate_native_code` 以使用 MemoryManager
   - 获取实际的内存地址和大小
   - 正确设置 `entry->set_entry_point()` 和 `entry->set_code_limit()`

### 阶段 3：完整测试和优化（待实施）

⏳ **待实施的工作**：

1. 测试 OSR 编译
2. 测试普通入口编译
3. 测试多个方法编译
4. 性能测试和优化

## 实施细节

### ThreadSafeModule 使用方式

在 LLVM 20 中，`ThreadSafeModule` 的构造函数签名是：
```cpp
ThreadSafeModule(std::unique_ptr<Module> M, ThreadSafeContext TSCtx)
```

注意：第二个参数是 `ThreadSafeContext` 对象（不是 `std::unique_ptr<ThreadSafeContext>`），所以需要使用 `*TSCtx` 而不是 `std::move(TSCtx)`。

### lookup 返回值处理

在 LLVM 20 中，`jit()->lookup()` 返回 `Expected<ExecutorAddr>`，而 `ExecutorAddr` 可以直接转换为 `uint64_t`（地址值）。使用方式：
```cpp
auto Sym = jit()->lookup(func_name);
code = (address) Sym->getValue();  // getValue() 返回 uint64_t
```

### 链接配置

ORC JIT 需要以下 LLVM 组件：
- `orcjit` - ORC JIT 核心库
- `orcshared` - ORC 共享库
- `orctargetprocess` - ORC 目标进程库

在 `yuhu.make` 中，这些组件已添加到链接配置中。

## 状态

- ✅ 阶段 1：基础迁移已完成
- ⏳ 阶段 2：内存管理集成待实施
- ⏳ 阶段 3：完整测试和优化待实施

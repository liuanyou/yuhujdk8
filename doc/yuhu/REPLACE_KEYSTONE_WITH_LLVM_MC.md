# 使用 LLVM MC 框架替代 Keystone

## 问题

Keystone 库与 LLVM 库存在符号冲突，导致链接错误：
- `LLVMInitializeAArch64TargetMC` 和 `LLVMInitializeAArch64TargetInfo` 在 Keystone 和 LLVM 中都有定义
- macOS 链接器不支持 `-allow-multiple-definition` 选项
- `-flat_namespace` 无法解决重复符号问题

## 解决方案

使用 LLVM 的 MC (Machine Code) 框架替代 Keystone。由于 Yuhu 编译器已经依赖 LLVM，这样可以：
1. 避免额外的依赖（Keystone）
2. 避免符号冲突
3. 使用统一的 LLVM 工具链

## LLVM MC 框架简介

LLVM MC 框架提供了：
- `MCAsmParser`: 解析汇编字符串
- `MCCodeEmitter`: 将指令编码为机器码
- `MCTargetAsmParser`: 目标架构特定的汇编解析器

## 实现步骤

### 1. 修改 `yuhu_macroAssembler.hpp`

**关键点**：由于头文件会被 `precompiled.hpp` 包含（使用 C++11 编译），而 LLVM 20 头文件需要 C++17，因此**不能在头文件中直接包含 LLVM 头文件**，必须使用前向声明。

将 Keystone 相关的代码替换为 LLVM MC 前向声明：

```cpp
// 移除
#include <keystone/keystone.h>
ks_engine *ks;

// 添加前向声明（不包含 LLVM 头文件！）
// Forward declarations for LLVM MC types
namespace llvm {
  class Target;
  class MCRegisterInfo;
  class MCAsmInfo;
  class MCSubtargetInfo;
  class MCInstrInfo;
  class MCObjectFileInfo;
  class Triple;
  class MCTargetOptions;
}

class YuhuMacroAssembler: public MacroAssembler {
private:
    // 前向声明嵌套类（实际定义在 .cpp 文件中）
    class SimpleMCStreamer;
    
    // LLVM MC 可复用组件（在构造函数中创建一次，线程安全，只读）
    // 注意：使用指针或字符串存储，避免在头文件中包含 LLVM 类型
    const llvm::Target* _target;
    std::unique_ptr<llvm::MCRegisterInfo> _mri;
    std::unique_ptr<llvm::MCAsmInfo> _mai;
    std::unique_ptr<llvm::MCSubtargetInfo> _sti;
    std::unique_ptr<llvm::MCInstrInfo> _mcii;
    std::unique_ptr<llvm::MCObjectFileInfo> _mofi;
    std::string _triple_str;  // 使用字符串存储，在 .cpp 中转换为 llvm::Triple
    std::unique_ptr<llvm::MCTargetOptions> _mc_options;
    
    // 初始化标志
    bool _initialized;
    
    // 初始化目标（只调用一次）
    bool initializeTargets();
    
    // 创建可复用的组件（只调用一次）
    bool createReusableComponents();
    
    // 替换 machine_code 方法
    uint32_t machine_code(const char* assembly);
};
```

### 2. 实现 LLVM MC 版本的 `machine_code`

**关键点**：区分可复用组件和状态组件。可复用组件在构造函数中初始化一次，状态组件每次调用时创建新的。

#### 2.1 实现 SimpleMCStreamer 类（在 .cpp 文件中）

```cpp
// 在 yuhu_macroAssembler_aarch64.cpp 中
class YuhuMacroAssembler::SimpleMCStreamer : public llvm::MCStreamer {
private:
    llvm::MCCodeEmitter* CodeEmitter;
    const llvm::MCSubtargetInfo* STI;
public:
    llvm::SmallVector<char, 32> CodeBuffer;
    
    SimpleMCStreamer(llvm::MCContext &Ctx, 
                     llvm::MCCodeEmitter* CE, 
                     const llvm::MCSubtargetInfo* SubtargetInfo)
        : llvm::MCStreamer(Ctx), CodeEmitter(CE), STI(SubtargetInfo) {}
    
    // 关键方法：编码指令
    void emitInstruction(const llvm::MCInst &Inst, 
                        const llvm::MCSubtargetInfo &STI) override {
        if (CodeEmitter) {
            llvm::SmallVector<llvm::MCFixup, 4> Fixups;
            CodeEmitter->encodeInstruction(Inst, CodeBuffer, Fixups, STI);
        }
    }
    
    void emitBytes(llvm::StringRef Data) override {
        for (char C : Data) {
            CodeBuffer.push_back(C);
        }
    }
    
    // 其他必需的虚函数实现（最小化实现）
    bool emitSymbolAttribute(llvm::MCSymbol *Symbol, llvm::MCSymbolAttr Attribute) override {
        return true;
    }
    void emitCommonSymbol(llvm::MCSymbol *Symbol, uint64_t Size,
                         llvm::Align ByteAlignment) override {}
    void emitZerofill(llvm::MCSection *Section, llvm::MCSymbol *Symbol, uint64_t Size,
                     llvm::Align ByteAlignment, llvm::SMLoc Loc = llvm::SMLoc()) override {}
    void emitValueImpl(const llvm::MCExpr *Value, unsigned Size,
                      llvm::SMLoc Loc = llvm::SMLoc()) override {}
};
```

#### 2.2 实现 machine_code 方法

```cpp
uint32_t YuhuMacroAssembler::machine_code(const char* assembly) {
    if (!_initialized) {
        fatal(err_msg("Assembler not initialized"));
        return 0;
    }
    
    // 每次调用时创建新的状态组件（这些不是线程安全的）
    
    // 创建 Triple
    llvm::Triple TheTriple(_triple_str);
    
    // 创建新的 MCContext（每次都需要新的，因为管理符号表等状态）
    llvm::MCContext Ctx(TheTriple, _mai.get(), _mri.get(), _sti.get());
    
    // 初始化 MCObjectFileInfo（需要上下文）
    _mofi->initMCObjectFileInfo(Ctx, false, false);
    Ctx.setObjectFileInfo(_mofi.get());
    
    // 创建新的 MCCodeEmitter（依赖上下文，每次创建新的）
    std::unique_ptr<llvm::MCCodeEmitter> MCE(
        _target->createMCCodeEmitter(*_mcii, Ctx));
    if (!MCE) {
        fatal(err_msg("Unable to create code emitter"));
        return 0;
    }
    
    // 创建收集流（有状态，每次创建新的）
    SimpleMCStreamer Streamer(Ctx, MCE.get(), _sti.get());
    
    // 创建源管理器（有状态，每次创建新的）
    llvm::SourceMgr SrcMgr;
    SrcMgr.setDiagHandler(diagnosticHandler);
    
    // 创建内存缓冲区
    std::string asm_str = std::string(assembly);
    std::unique_ptr<llvm::MemoryBuffer> Buffer = 
        llvm::MemoryBuffer::getMemBuffer(asm_str);
    SrcMgr.AddNewSourceBuffer(std::move(Buffer), llvm::SMLoc());
    
    // 创建解析器（有状态，每次创建新的）
    std::unique_ptr<llvm::MCAsmParser> Parser(
        llvm::createMCAsmParser(SrcMgr, Ctx, Streamer, *_mai));
    if (!Parser) {
        fatal(err_msg("Unable to create asm parser"));
        return 0;
    }
    
    // 创建目标特定的解析器（有状态，每次创建新的）
    std::unique_ptr<llvm::MCTargetAsmParser> TAP(
        _target->createMCAsmParser(*_sti, *Parser, *_mcii, *_mc_options));
    if (!TAP) {
        fatal(err_msg("Unable to create target asm parser"));
        return 0;
    }
    Parser->setTargetParser(*TAP);
    
    // 解析汇编
    if (Parser->Run(false)) {
        fatal(err_msg("Failed to parse assembly: %s", assembly));
        return 0;
    }
    
    // 获取编码的字节
    const llvm::SmallVector<char, 32> &Code = Streamer.getCode();
    
    if (Code.size() >= 4) {
        // 将字节转换为32位值（小端序）
        uint32_t Result = 0;
        for (size_t i = 0; i < 4; ++i) {
            Result |= (static_cast<uint8_t>(Code[i]) << (i * 8));
        }
        return Result;
    } else if (!Code.empty()) {
        fatal(err_msg("Expected 4 bytes, got %zu bytes", Code.size()));
    } else {
        fatal(err_msg("No code generated for: %s", assembly));
    }
    
    return 0;
}
```

### 3. 初始化 LLVM MC 组件

**关键点**：区分可复用组件和状态组件。可复用组件在构造函数中初始化一次，状态组件每次 `machine_code` 调用时创建新的。

#### 3.1 构造函数（初始化可复用组件）

```cpp
YuhuMacroAssembler::YuhuMacroAssembler(CodeBuffer* code) : MacroAssembler(code),
    _target(nullptr),
    _triple_str("aarch64-apple-darwin"),
    _initialized(false) {
    
    _mc_options = std::make_unique<llvm::MCTargetOptions>();
    
    // 初始化目标
    if (!initializeTargets()) {
        return;
    }
    
    // 创建可复用的组件
    if (!createReusableComponents()) {
        return;
    }
    
    _initialized = true;
}
```

#### 3.2 初始化目标（只调用一次）

```cpp
bool YuhuMacroAssembler::initializeTargets() {
    static bool targetsInitialized = false;
    if (targetsInitialized) return true;
    
    // 只初始化我们需要的 AArch64 目标
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmPrinter();
    LLVMInitializeAArch64AsmParser();
    LLVMInitializeAArch64Disassembler();
    
    targetsInitialized = true;
    return true;
}
```

#### 3.3 创建可复用组件（只调用一次）

```cpp
bool YuhuMacroAssembler::createReusableComponents() {
    // 查找目标
    std::string Error;
    _target = llvm::TargetRegistry::lookupTarget("aarch64", Error);
    if (!_target) {
        fatal(err_msg("Error: %s", Error.c_str()));
        return false;
    }
    
    std::string CPU = "generic";
    std::string Features = "";
    
    // 创建必要的组件（这些是只读的，可以复用）
    llvm::Triple TheTriple(_triple_str);
    _mri.reset(_target->createMCRegInfo(TheTriple.str()));
    if (!_mri) {
        fatal(err_msg("Unable to create register info"));
        return false;
    }
    
    _mai.reset(_target->createMCAsmInfo(*_mri, TheTriple.str(), *_mc_options));
    if (!_mai) {
        fatal(err_msg("Unable to create asm info"));
        return false;
    }
    
    _sti.reset(_target->createMCSubtargetInfo(TheTriple.str(), CPU, Features));
    if (!_sti) {
        fatal(err_msg("Unable to create subtarget info"));
        return false;
    }
    
    _mcii.reset(_target->createMCInstrInfo());
    if (!_mcii) {
        fatal(err_msg("Unable to create instruction info"));
        return false;
    }
    
    // 创建 MCObjectFileInfo（这也是只读配置，可以复用）
    _mofi = std::make_unique<llvm::MCObjectFileInfo>();
    
    return true;
}
```

### 4. 清理资源

由于使用了 `std::unique_ptr`，析构函数可以很简单：

```cpp
YuhuMacroAssembler::~YuhuMacroAssembler() = default;
```

`std::unique_ptr` 会自动管理所有可复用组件的生命周期。

### 5. 更新 Makefile

#### 5.1 从 `gcc.make` 中移除 Keystone 头文件路径

```makefile
# 注释掉
# ifneq ($(strip $(KEYSTONE_INCLUDE_PATH)),)
#   CFLAGS += $(KEYSTONE_INCLUDE_PATH:%=-I%)
# endif
```

#### 5.2 从 `vm.make` 中移除 Keystone 链接

```makefile
# 移除整个 Keystone 链接块
# ifneq ($(strip $(KEYSTONE_LIB_PATH)),)
#   LFLAGS_VM += $(KEYSTONE_LIB_PATH:%=-L%) -lkeystone
# endif
```

#### 5.3 更新 `yuhu.make`

```makefile
# 恢复所有 LLVM 库，不再需要过滤 AArch64 库
LLVM_LIBS := $(LLVM_LIBS_RAW)

# 添加 C++17 编译标志（LLVM 20 需要）
CXXFLAGS/yuhu_macroAssembler_aarch64.o += -std=c++17 -Wno-reserved-user-defined-literal -Wno-format-nonliteral -Wno-error=format-nonliteral

# 禁用 PCH（避免 C++11/C++17 冲突）
PCH_FLAG/yuhu_macroAssembler_aarch64.o = $(PCH_FLAG/NO_PCH)
```

## 优势

1. **无符号冲突**: 完全使用 LLVM，避免与 Keystone 的冲突
2. **统一工具链**: 所有代码生成都使用 LLVM
3. **减少依赖**: 不需要额外的 Keystone 库
4. **更好的集成**: LLVM MC 与 Yuhu 的 LLVM IR 生成更紧密集成

## 注意事项

1. **C++11/C++17 兼容性**：
   - 头文件（`.hpp`）只使用前向声明，不包含 LLVM 头文件（因为会被 `precompiled.hpp` 包含，使用 C++11 编译）
   - 实现文件（`.cpp`）使用 C++17 编译，包含完整的 LLVM 头文件

2. **HotSpot 宏冲突**：
   - 在包含 LLVM 头文件之前 `#undef assert`
   - 在包含所有 LLVM 头文件之后重新定义 HotSpot 的 `assert` 宏
   - 使用 `err_msg` 格式化 `fatal` 和 `assert` 的消息

3. **组件生命周期**：
   - 可复用组件：在构造函数中初始化一次，使用 `std::unique_ptr` 管理
   - 状态组件：每次 `machine_code` 调用时创建新的，使用 `std::unique_ptr` 自动管理

4. **LLVM 20 API 变化**：
   - 头文件路径变化：`llvm/ADT/Triple.h` → `llvm/TargetParser/Triple.h`
   - `MCStreamer` 方法：某些方法在 LLVM 20 中不是虚函数，不能使用 `override`

5. **编译选项**：
   - 实现文件需要 `-std=c++17` 编译
   - 需要禁用 PCH（`PCH_FLAG/NO_PCH`）
   - 需要禁用格式字符串警告（`-Wno-format-nonliteral -Wno-error=format-nonliteral`）

## 参考

- LLVM MC 文档: https://llvm.org/docs/MCJITDesignAndImplementation.html
- LLVM MCAsmParser 示例: LLVM 源码中的 `llvm/tools/llvm-mc/llvm-mc.cpp`


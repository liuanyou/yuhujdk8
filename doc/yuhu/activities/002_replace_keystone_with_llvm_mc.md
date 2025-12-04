# 活动 002: 使用 LLVM MC 框架替代 Keystone

## 日期
2024-12-XX

## 问题描述

在链接 Yuhu 编译器时，遇到 Keystone 库与 LLVM 库的符号冲突错误：

```
duplicate symbol '_LLVMInitializeAArch64TargetMC' in:
    /opt/homebrew/lib/libkeystone.a(...)
    /opt/homebrew/Cellar/llvm/20.1.5/lib/libLLVMAArch64Desc.a(...)

duplicate symbol '_LLVMInitializeAArch64TargetInfo' in:
    /opt/homebrew/lib/libkeystone.a(...)
    /opt/homebrew/Cellar/llvm/20.1.5/lib/libLLVMAArch64Info.a(...)
```

### 问题根源

1. **符号冲突**：Keystone 库内部使用了 LLVM，导致与 Yuhu 编译器使用的 LLVM 库产生符号冲突。
2. **macOS 链接器限制**：macOS 的 `ld` 不支持 GNU `ld` 的 `-allow-multiple-definition` 选项。
3. **Flat Namespace 无效**：`-flat_namespace` 选项无法解决重复符号问题，因为符号在链接时就必须唯一。

## 解决方案

由于 Yuhu 编译器已经依赖 LLVM，使用 LLVM 的 MC (Machine Code) 框架替代 Keystone 是最佳选择：

1. **避免符号冲突**：完全使用 LLVM，不再依赖 Keystone
2. **减少依赖**：移除额外的第三方库
3. **统一工具链**：所有代码生成都使用 LLVM
4. **更好的集成**：LLVM MC 与 Yuhu 的 LLVM IR 生成更紧密集成

## 实现过程

### Phase 1: 在 Xcode 中验证 LLVM MC 功能

参考用户提供的成功实现：
- `/Users/liuanyou/XcodeProjects/HelloWord/HelloWord/llvm_mc_assembler.h`
- `/Users/liuanyou/XcodeProjects/HelloWord/HelloWord/llvm_mc_assembler.cpp`
- `/Users/liuanyou/XcodeProjects/HelloWord/LLVM_MC_ASSEMBLER_USAGE.md`

关键发现：
1. **可复用组件 vs 状态组件**：
   - 可复用（在构造函数中初始化一次）：`Target`, `MCRegisterInfo`, `MCAsmInfo`, `MCSubtargetInfo`, `MCInstrInfo`, `MCObjectFileInfo`
   - 状态组件（每次调用创建新的）：`MCContext`, `MCCodeEmitter`, `MCStreamer`, `SourceMgr`, `MCAsmParser`, `MCTargetAsmParser`

2. **自定义 MCStreamer**：需要实现一个 `SimpleMCStreamer` 类来收集编码后的机器码字节。

### Phase 2: 修改 YuhuMacroAssembler 头文件

**文件**: `hotspot/src/share/vm/asm/yuhu/yuhu_macroAssembler.hpp`

#### 关键修改：

1. **移除 Keystone 相关代码**：
   ```cpp
   // 移除
   #include <keystone/keystone.h>
   ks_engine *ks;
   ```

2. **添加 LLVM MC 前向声明**（避免在头文件中包含 LLVM 头文件，因为头文件会被 `precompiled.hpp` 包含，使用 C++11 编译）：
   ```cpp
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
   ```

3. **修改成员变量**：
   ```cpp
   // 使用指针或字符串存储，避免在头文件中包含 LLVM 类型
   const llvm::Target* _target;
   std::unique_ptr<llvm::MCRegisterInfo> _mri;
   std::unique_ptr<llvm::MCAsmInfo> _mai;
   std::unique_ptr<llvm::MCSubtargetInfo> _sti;
   std::unique_ptr<llvm::MCInstrInfo> _mcii;
   std::unique_ptr<llvm::MCObjectFileInfo> _mofi;
   std::string _triple_str;  // 使用字符串存储，在 .cpp 中转换为 llvm::Triple
   std::unique_ptr<llvm::MCTargetOptions> _mc_options;
   ```

4. **前向声明嵌套类**：
   ```cpp
   class SimpleMCStreamer;  // 实际定义在 .cpp 文件中
   ```

### Phase 3: 实现 LLVM MC 功能

**文件**: `hotspot/src/cpu/aarch64/vm/yuhu/yuhu_macroAssembler_aarch64.cpp`

#### 关键实现：

1. **SimpleMCStreamer 类**：
   ```cpp
   class YuhuMacroAssembler::SimpleMCStreamer : public llvm::MCStreamer {
   private:
     llvm::MCCodeEmitter *CodeEmitter;
     const llvm::MCSubtargetInfo *STI;
   public:
     llvm::SmallVector<char, 32> CodeBuffer;
     
     SimpleMCStreamer(llvm::MCContext &Ctx, 
                      llvm::MCCodeEmitter *CE, 
                      const llvm::MCSubtargetInfo *SubtargetInfo)
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
   };
   ```

2. **初始化可复用组件**（在构造函数中）：
   ```cpp
   bool YuhuMacroAssembler::initializeTargets() {
     static bool targetsInitialized = false;
     if (targetsInitialized) return true;
     
     LLVMInitializeAArch64Target();
     LLVMInitializeAArch64TargetInfo();
     LLVMInitializeAArch64TargetMC();
     LLVMInitializeAArch64AsmPrinter();
     LLVMInitializeAArch64AsmParser();
     LLVMInitializeAArch64Disassembler();
     
     targetsInitialized = true;
     return true;
   }
   
   bool YuhuMacroAssembler::createReusableComponents() {
     std::string Error;
     _target = llvm::TargetRegistry::lookupTarget("aarch64", Error);
     if (!_target) {
       fatal(err_msg("Error: %s", Error.c_str()));
       return false;
     }
     
     llvm::Triple TheTriple(_triple_str);
     _mri.reset(_target->createMCRegInfo(TheTriple.str()));
     _mai.reset(_target->createMCAsmInfo(*_mri, TheTriple.str(), *_mc_options));
     _sti.reset(_target->createMCSubtargetInfo(TheTriple.str(), "generic", ""));
     _mcii.reset(_target->createMCInstrInfo());
     _mofi = std::make_unique<llvm::MCObjectFileInfo>();
     
     return true;
   }
   ```

3. **machine_code 方法**（每次调用创建新的状态组件）：
   ```cpp
   uint32_t YuhuMacroAssembler::machine_code(const char* assembly) {
     // 创建新的 MCContext（每次都需要新的，因为管理符号表等状态）
     llvm::Triple TheTriple(_triple_str);
     llvm::MCContext Ctx(TheTriple, _mai.get(), _mri.get(), _sti.get());
     
     // 初始化 MCObjectFileInfo
     _mofi->initMCObjectFileInfo(Ctx, false, false);
     Ctx.setObjectFileInfo(_mofi.get());
     
     // 创建新的 MCCodeEmitter
     std::unique_ptr<llvm::MCCodeEmitter> MCE(
         _target->createMCCodeEmitter(*_mcii, Ctx));
     
     // 创建收集流
     SimpleMCStreamer Streamer(Ctx, MCE.get(), _sti.get());
     
     // 创建源管理器
     llvm::SourceMgr SrcMgr;
     SrcMgr.setDiagHandler(diagnosticHandler);
     
     // 创建内存缓冲区
     std::string asm_str = std::string(assembly);
     std::unique_ptr<llvm::MemoryBuffer> Buffer = 
         llvm::MemoryBuffer::getMemBuffer(asm_str);
     SrcMgr.AddNewSourceBuffer(std::move(Buffer), llvm::SMLoc());
     
     // 创建解析器
     std::unique_ptr<llvm::MCAsmParser> Parser(
         llvm::createMCAsmParser(SrcMgr, Ctx, Streamer, *_mai));
     
     // 创建目标特定的解析器
     std::unique_ptr<llvm::MCTargetAsmParser> TAP(
         _target->createMCAsmParser(*_sti, *Parser, *_mcii, *_mc_options));
     Parser->setTargetParser(*TAP);
     
     // 解析汇编
     if (Parser->Run(false)) {
       fatal(err_msg("Failed to parse assembly: %s", assembly));
       return 0;
     }
     
     // 获取编码的字节
     const llvm::SmallVector<char, 32> &Code = Streamer.getCode();
     if (Code.size() >= 4) {
       uint32_t Result = 0;
       for (size_t i = 0; i < 4; ++i) {
         Result |= (static_cast<uint8_t>(Code[i]) << (i * 8));
       }
       return Result;
     }
     
     return 0;
   }
   ```

### Phase 4: 处理 HotSpot 宏冲突

**问题**：HotSpot 的 `fatal` 和 `assert` 宏与 LLVM 的宏定义冲突。

**解决方案**：

1. **在 `yuhu_macroAssembler_aarch64.cpp` 中**：
   ```cpp
   // 在包含 LLVM 头文件之前
   #undef assert
   
   // 包含所有 LLVM 头文件
   #include <llvm/...>
   
   // 在所有 LLVM 头文件之后，重新定义 HotSpot 的 assert
   #define assert(p, msg) ...
   ```

2. **使用 `err_msg` 格式化消息**：
   ```cpp
   fatal(err_msg("Error: %s", Error.c_str()));
   assert(condition, err_msg("Message: %s", arg));
   ```

### Phase 5: 更新构建系统

#### 文件: `hotspot/make/bsd/makefiles/gcc.make`

**移除 Keystone 头文件路径**：
```makefile
# 注释掉
# ifneq ($(strip $(KEYSTONE_INCLUDE_PATH)),)
#   CFLAGS += $(KEYSTONE_INCLUDE_PATH:%=-I%)
# endif
```

#### 文件: `hotspot/make/bsd/makefiles/vm.make`

**移除 Keystone 链接**：
```makefile
# 移除整个 Keystone 链接块
# ifneq ($(strip $(KEYSTONE_LIB_PATH)),)
#   LFLAGS_VM += $(KEYSTONE_LIB_PATH:%=-L%) -lkeystone
# endif
```

#### 文件: `hotspot/make/bsd/makefiles/yuhu.make`

**恢复所有 LLVM 库**（不再需要过滤 AArch64 库）：
```makefile
# 不再过滤 -lLLVMAArch64Desc 和 -lLLVMAArch64Info
LLVM_LIBS := $(LLVM_LIBS_RAW)
```

**添加 C++17 编译标志**（LLVM 20 需要）：
```makefile
CXXFLAGS/yuhu_macroAssembler_aarch64.o += -std=c++17 -Wno-reserved-user-defined-literal -Wno-format-nonliteral -Wno-error=format-nonliteral
```

**禁用 PCH**（避免 C++11/C++17 冲突）：
```makefile
PCH_FLAG/yuhu_macroAssembler_aarch64.o = $(PCH_FLAG/NO_PCH)
```

## 关键技术点

### 1. C++11/C++17 兼容性

- **问题**：JDK 8 使用 C++11，但 LLVM 20 头文件需要 C++17
- **解决方案**：
  - 头文件（`.hpp`）只使用前向声明，不包含 LLVM 头文件
  - 实现文件（`.cpp`）使用 C++17 编译，包含完整的 LLVM 头文件

### 2. LLVM 20 API 变化

- **头文件路径变化**：
  - `llvm/ADT/Triple.h` → `llvm/TargetParser/Triple.h`
  - `llvm/MC/MCAsmParser.h` → `llvm/MC/MCParser/MCAsmParser.h`
  - `llvm/Support/Host.h` → `llvm/TargetParser/Host.h`

- **MCStreamer 方法**：某些方法在 LLVM 20 中不是虚函数，不能使用 `override`

### 3. 组件生命周期管理

- **可复用组件**：使用 `std::unique_ptr` 管理，在构造函数中创建一次
- **状态组件**：每次 `machine_code` 调用时创建新的，使用 `std::unique_ptr` 自动管理

### 4. 错误处理

- 使用 HotSpot 的 `fatal` 宏（通过 `err_msg` 格式化）
- 使用 HotSpot 的 `assert` 宏（需要 `#undef` 和重新定义）

## 验证方法

### 1. 编译测试

```bash
make hotspot-only
```

确保没有链接错误，特别是：
- 没有 Keystone 相关的符号冲突
- 没有 LLVM MC 相关的编译错误

### 2. 功能测试

运行一个使用 Yuhu 编译器的 Java 程序，验证机器码生成是否正常。

### 3. 调试输出

可以在 `machine_code` 方法中添加调试输出，验证生成的机器码是否正确。

## 经验总结

1. **参考成功实现**：用户的 Xcode 项目提供了很好的参考，直接参考可以避免很多错误。

2. **C++11/C++17 兼容性**：通过前向声明和分离编译标准，可以很好地解决兼容性问题。

3. **组件分类**：区分可复用组件和状态组件，可以优化性能和简化实现。

4. **宏冲突处理**：需要仔细处理 HotSpot 和 LLVM 的宏冲突。

5. **LLVM 版本适配**：LLVM 20 的 API 变化较大，需要仔细检查头文件路径和方法签名。

## 相关文件

- `hotspot/src/share/vm/asm/yuhu/yuhu_macroAssembler.hpp` - 头文件修改
- `hotspot/src/cpu/aarch64/vm/yuhu/yuhu_macroAssembler_aarch64.cpp` - 实现文件
- `hotspot/make/bsd/makefiles/gcc.make` - 移除 Keystone 头文件路径
- `hotspot/make/bsd/makefiles/vm.make` - 移除 Keystone 链接
- `hotspot/make/bsd/makefiles/yuhu.make` - 更新编译和链接选项
- `doc/yuhu/REPLACE_KEYSTONE_WITH_LLVM_MC.md` - 替换方案文档
- `doc/yuhu/LLVM_MC_IMPLEMENTATION_GUIDE.md` - 实现指南

## 参考文档

- LLVM MC 文档: https://llvm.org/docs/MCJITDesignAndImplementation.html
- LLVM MCAsmParser 示例: LLVM 源码中的 `llvm/tools/llvm-mc/llvm-mc.cpp`
- 用户提供的成功实现: `/Users/liuanyou/XcodeProjects/HelloWord/`


# LLVM MC 框架实现指南

## 目标
在 Xcode 中先实现一个独立的 LLVM MC 测试项目，验证机器码生成功能，然后再集成到 Yuhu 编译器中。

## 核心思路

### 1. LLVM MC 框架的基本流程

```
汇编字符串 → MCAsmParser → MCInst → MCCodeEmitter → 机器码字节
```

### 2. 关键组件

- **MCContext**: 管理符号、节等上下文信息
- **MCAsmParser**: 解析汇编字符串
- **MCCodeEmitter**: 将 MCInst 编码为机器码字节
- **MCStreamer**: 收集编码后的指令（自定义实现）
- **TargetMachine**: 目标机器信息

### 3. 实现步骤

#### 步骤 1: 在 Xcode 中创建测试项目

1. 创建新的 C++ 项目（选择 C++17 标准）
2. 配置 LLVM 路径：
   - Header Search Paths: `/opt/homebrew/Cellar/llvm/20.1.5/include`
   - Library Search Paths: `/opt/homebrew/Cellar/llvm/20.1.5/lib`
   - 链接库：使用 `llvm-config --libs --link-static mc aarch64` 获取

#### 步骤 2: 实现核心功能

**关键代码结构：**

```cpp
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCParser/MCAsmParser.h>
#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCStreamer.h>
#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Target/TargetMachine.h>

// 自定义 Streamer 收集编码后的指令
class SimpleMCStreamer : public llvm::MCStreamer {
private:
    std::vector<uint8_t> code;
    llvm::MCCodeEmitter* code_emitter;
    const llvm::MCSubtargetInfo* subtarget_info;
    
public:
    SimpleMCStreamer(llvm::MCContext &Ctx,
                     llvm::MCCodeEmitter* emitter,
                     const llvm::MCSubtargetInfo* sti)
        : MCStreamer(Ctx), code_emitter(emitter), subtarget_info(sti) {}
    
    // 关键方法：编码指令
    void EmitInstruction(const llvm::MCInst &Inst,
                        const llvm::MCSubtargetInfo &STI) {
        llvm::SmallVector<char, 16> buffer;
        llvm::raw_svector_ostream OS(buffer);
        code_emitter->encodeInstruction(Inst, OS, STI, llvm::SMLoc());
        for (char c : buffer) {
            code.push_back(static_cast<uint8_t>(c));
        }
    }
    
    std::vector<uint8_t>& getCode() { return code; }
};

// 主要函数
uint32_t assemble_instruction(const char* assembly) {
    // 1. 初始化 LLVM 目标
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllDisassemblers();
    
    // 2. 获取 AArch64 目标
    std::string error;
    const llvm::Target* target = 
        llvm::TargetRegistry::lookupTarget("aarch64", error);
    
    // 3. 创建 TargetMachine
    llvm::TargetOptions options;
    llvm::TargetMachine* TM = target->createTargetMachine(
        "aarch64", "generic", "", options, llvm::Reloc::PIC_);
    
    // 4. 创建 MCContext
    llvm::MCContext Ctx(llvm::Triple("aarch64-apple-darwin"), 
                       TM->getMCAsmInfo(),
                       TM->getRegisterInfo(),
                       TM->getSubtargetInfo());
    
    // 5. 创建 MCCodeEmitter
    llvm::MCCodeEmitter* code_emitter = 
        target->createMCCodeEmitter(*TM->getMCInstrInfo(),
                                    *TM->getMCRegisterInfo(),
                                    Ctx);
    
    // 6. 创建 MCSubtargetInfo
    const llvm::MCSubtargetInfo* STI = TM->getMCSubtargetInfo();
    
    // 7. 创建自定义 Streamer
    SimpleMCStreamer streamer(Ctx, code_emitter, STI);
    
    // 8. 创建 MCAsmParser
    llvm::SourceMgr SrcMgr;
    llvm::MCAsmParser* parser = 
        target->createMCAsmParser(*TM->getMCSubtargetInfo(), 
                                  streamer, Ctx);
    
    // 9. 解析汇编字符串
    std::string asm_str = assembly;
    llvm::MemoryBuffer* buffer = 
        llvm::MemoryBuffer::getMemBuffer(asm_str);
    SrcMgr.AddNewSourceBuffer(std::unique_ptr<llvm::MemoryBuffer>(buffer),
                              llvm::SMLoc());
    
    parser->Run(false);
    
    // 10. 提取机器码
    std::vector<uint8_t>& code = streamer.getCode();
    if (code.size() == 4) {
        return *reinterpret_cast<uint32_t*>(code.data());
    }
    return 0;
}
```

#### 步骤 3: 测试用例

```cpp
int main() {
    // 测试简单的 AArch64 指令
    uint32_t code1 = assemble_instruction("mov x0, #1");
    uint32_t code2 = assemble_instruction("add x1, x2, x3");
    
    printf("mov x0, #1 = 0x%08x\n", code1);
    printf("add x1, x2, x3 = 0x%08x\n", code2);
    
    return 0;
}
```

### 4. 常见问题和解决方案

#### 问题 1: 找不到头文件
- 确保 Header Search Paths 正确
- 检查 LLVM 版本（20.1.5 的头文件路径可能不同）

#### 问题 2: 链接错误
- 使用 `llvm-config --libs --link-static mc aarch64` 获取所有需要的库
- 确保链接顺序正确

#### 问题 3: MCStreamer 方法签名
- LLVM 20 中某些方法可能不是虚函数
- 只实现必需的方法，不要使用 `override` 除非确认是虚函数
- 必需的方法：`emitInstruction`, `emitBytes`, `emitSymbolAttribute`, `emitCommonSymbol`, `emitZerofill`, `emitValueImpl`

#### 问题 4: HotSpot 宏冲突
- HotSpot 的 `fatal` 和 `assert` 宏与 LLVM 的宏定义冲突
- 在包含 LLVM 头文件之前 `#undef assert`
- 在包含所有 LLVM 头文件之后重新定义 HotSpot 的 `assert` 宏
- 使用 `err_msg` 格式化 `fatal` 和 `assert` 的消息

#### 问题 5: C++11/C++17 兼容性
- JDK 8 使用 C++11，但 LLVM 20 头文件需要 C++17
- 解决方案：头文件只使用前向声明，实现文件使用 C++17 编译

#### 问题 6: 编码失败
- 检查汇编字符串格式是否正确
- 确保 TargetMachine 配置正确（特别是 relocation model）
- 检查 `MCObjectFileInfo` 是否正确初始化（需要调用 `initMCObjectFileInfo`）

#### 问题 7: 组件生命周期
- 区分可复用组件和状态组件
- 可复用组件：在构造函数中初始化一次
- 状态组件：每次 `machine_code` 调用时创建新的

### 5. 验证清单

- [ ] 项目在 Xcode 中编译通过
- [ ] 能够成功解析简单的 AArch64 指令（如 `mov x0, #1`）
- [ ] 能够生成正确的机器码字节
- [ ] 能够处理多种指令类型（算术、逻辑、内存访问等）

### 6. 集成到 Yuhu 编译器

一旦在 Xcode 中验证通过，集成步骤：

1. **复制核心逻辑**：将 `assemble_instruction` 函数的核心逻辑复制到 `yuhu_macroAssembler_aarch64.cpp` 的 `machine_code` 方法中

2. **适配 HotSpot 环境**：
   - 使用 HotSpot 的错误报告机制（`fatal` 和 `assert` 宏，通过 `err_msg` 格式化）
   - 处理 HotSpot 宏冲突（`#undef assert` 和重新定义）
   - 确保线程安全（区分可复用组件和状态组件）

3. **C++11/C++17 兼容性**：
   - 头文件（`.hpp`）只使用前向声明，不包含 LLVM 头文件
   - 实现文件（`.cpp`）使用 C++17 编译，包含完整的 LLVM 头文件

4. **组件生命周期管理**：
   - 可复用组件：在构造函数中初始化一次，使用 `std::unique_ptr` 管理
   - 状态组件：每次 `machine_code` 调用时创建新的，使用 `std::unique_ptr` 自动管理

5. **简化实现**：
   - 移除不必要的测试代码
   - 优化错误处理
   - 确保与现有代码风格一致

### 7. 关键 API 参考

**LLVM 20 中的关键变化：**
- `Triple.h` → `llvm/TargetParser/Triple.h`
- `TargetRegistry.h` → `llvm/MC/TargetRegistry.h`
- `MCAsmParser.h` → `llvm/MC/MCParser/MCAsmParser.h`

**必需的头文件：**
```cpp
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCParser/MCAsmParser.h>
#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCStreamer.h>
#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Target/TargetMachine.h>
```

### 8. 调试技巧

1. **使用 LLVM 的调试输出**：
   ```cpp
   llvm::setCurrentDebugType("mc");
   ```

2. **检查解析错误**：
   ```cpp
   if (parser->Run(false)) {
       // 解析失败
   }
   ```

3. **验证机器码**：
   - 使用 `llvm-objdump` 反汇编生成的机器码
   - 与预期指令对比

## 下一步

1. ✅ 在 Xcode 中创建测试项目（已完成）
2. ✅ 实现并验证核心功能（已完成）
3. ✅ 确认所有指令类型都能正确编码（已完成）
4. ✅ 将验证通过的代码集成到 Yuhu 编译器中（已完成）

## 实际实现参考

实际实现已经完成，参考以下文件：
- `hotspot/src/share/vm/asm/yuhu/yuhu_macroAssembler.hpp` - 头文件（使用前向声明）
- `hotspot/src/cpu/aarch64/vm/yuhu/yuhu_macroAssembler_aarch64.cpp` - 实现文件（使用 C++17）

详细实现过程请参考：
- `doc/yuhu/activities/002_replace_keystone_with_llvm_mc.md` - 完整的实现活动记录


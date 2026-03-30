# Activity 056: Yuhu 字符串常量嵌入问题

## 问题描述

Yuhu JIT 编译器在 LLVM IR 中嵌入 String 字面量时，无法正确处理 UTF-8 数据的地址。

## 问题现象

### 症状
- ORC JIT 编译后，`yuhu_lookup_string_by_utf8` 的参数地址指向错误的数据
- 运行时访问该地址看到的是 `0xdd` 填充数据，而非 UTF-8 字节
- 导致 String 字面量无法正确加载

### 调试信息
```
(lldb) memory read -s1 -c20 -fx (0x000000010f37f665)
0x10f37f665: 0xdd 0xdd 0xdd 0xdd 0xdd 0xdd 0xdd 0xdd
0x10f37f66d: 0xdd 0xdd 0xdd 0xdd 0xdd 0xdd 0xdd 0xdd
0x10f37f675: 0xdd 0xdd 0xdd 0xdd
```

## 根本原因

### 时间线分析

1. **IR 生成阶段**（`generate_native_code`）
   - 在临时 CodeBuffer 中嵌入 UTF-8 数据到 consts section
   - 获取 consts section 的绝对地址 `utf8_addr`
   - 将 `utf8_addr` 作为立即数嵌入 LLVM IR

2. **ORC JIT 编译阶段**
   - ORC JIT 将 LLVM Module 编译成机器码
   - `utf8_addr` 固化在机器码中（通过 `mov/movk` 序列）

3. **注册到 nmethod 阶段**（`register_method`）
   - 复制机器码到 nmethod 的 code section
   - **consts section 的数据也被复制，但地址变了**
   - 机器码中的地址仍然是旧的 consts section 地址

4. **运行时**
   - 执行 `mov x0, #utf8_addr` 时，`utf8_addr` 是旧地址
   - 该地址已被释放或重用 → 访问到 `0xdd` 填充数据

### 核心问题

**LLVM IR 中直接嵌入了 consts section 的绝对地址，但 CodeBuffer 的生命周期仅限于 IR 生成阶段。**

当方法被注册到 nmethod 时：
- 指令被复制到新地址
- consts section 数据也被复制到新地址
- **但指令中的绝对地址没有更新**

## 尝试过的方案

### 方案 1：LLVM GlobalVariable（已失败）
```cpp
GlobalVariable* str_utf8 = new GlobalVariable(
  *module, utf8_array_type, true,
  GlobalValue::PrivateLinkage,  // ❌ PrivateLinkage 导致符号冲突
  ConstantDataArray::get(ctx, utf8_bytes),
  "str_utf8");
```
**失败原因**：ORC JIT 解析 `private constant` GlobalVariable 时，地址指向 char[] 对象数据而非 UTF-8 字节

### 方案 2：CodeBuffer consts section（已失败）
```cpp
// IR 生成时嵌入绝对地址
int utf8_offset = code_buffer()->inline_data(utf8_bytes.data(), utf8_bytes.size());
address utf8_addr = consts->end() - utf8_bytes.size();

llvm::Value* utf8_ptr = CreateIntToPtr(
  llvm::ConstantInt::get(i64_ty, (uint64_t)(uintptr_t)utf8_addr),  // ❌ 绝对地址
  ptr_ty);
```
**失败原因**：CodeBuffer 在 register_method 后被释放，consts section 地址失效

## C1 的做法（详细代码层面）

### 第 1 步：LIR 层创建 oop constant

**文件**: `hotspot/src/share/vm/c1/c1_LIR.hpp`

```cpp
// LIR_OprFact::oopConst 创建 oop constant
static LIR_Opr oopConst(jobject o) {
  return (LIR_Opr)(new LIR_Const(o));
}
```

**使用示例** (`hotspot/src/share/vm/c1/c1_LIRGenerator.cpp`):
```cpp
void LIRGenerator::do_Constant(Constant* x) {
  LIR_Opr reg = rlock_result(x);
  __ oop2reg_patch(NULL, reg, info);  // 生成带 patching 的 move 指令
}
```

### 第 2 步：LIR → 机器码（AArch64）

**文件**: `hotspot/src/cpu/aarch64/vm/c1_LIRAssembler_aarch64.cpp`

```cpp
void LIR_Assembler::const2reg(LIR_Opr src, LIR_Opr dest, LIR_PatchCode patch_code, CodeEmitInfo* info) {
  LIR_Const* c = src->as_constant_ptr();
  
  switch (c->type()) {
    case T_OBJECT: {
      if (patch_code == lir_patch_none) {
        jobject2reg(c->as_jobject(), dest->as_register());
      } else {
        jobject2reg_with_patching(dest->as_register(), info);
      }
      break;
    }
  }
}

void LIR_Assembler::jobject2reg(jobject o, Register reg) {
  if (o == NULL) {
    __ mov(reg, zr);
  } else {
    __ movoop(reg, o, /*immediate*/true);  // 关键：调用 movoop
  }
}
```

### 第 3 步：MacroAssembler 生成指令 + Relocation

**文件**: `hotspot/src/cpu/aarch64/vm/macroAssembler_aarch64.cpp`

```cpp
void MacroAssembler::movoop(Register dst, jobject obj, bool immediate) {
  int oop_index;
  if (obj == NULL) {
    oop_index = oop_recorder()->allocate_oop_index(obj);
  } else {
    oop_index = oop_recorder()->find_index(obj);
  }
  
  // 创建 relocation specification
  RelocationHolder rspec = oop_Relocation::spec(oop_index);
  
  if (!immediate) {
    address dummy = address(uintptr_t(pc()) & -wordSize);
    ldr_constant(dst, Address(dummy, rspec));  // PC-relative load
  } else {
    // 生成 mov 指令，但带有 relocation 信息
    mov(dst, Address((address)obj, rspec));
  }
}
```

### 第 4 步：生成的机器码

**实际汇编代码**（来自 `debug/implWrite.txt`）:
```assembly
; C1 加载 java_mirror oop
0x000000010a79f138: movk x8, #0x1, lsl #32     ; 高 16 位
0x000000010a79f13c: mov  x0, #0x0               ; 低 16 位
; 结果：x8 = 0x100000000 (java_mirror 的地址)

; OopMap 记录这个位置包含 oop
; OopMap{[128]=Oop [120]=Oop [96]=Oop off=272}
```

### 第 5 步：Relocation 机制

**OopRecorder** (`hotspot/src/share/vm/code/oopRecorder.hpp`):
```cpp
class OopRecorder: public ResourceObj {
  GrowableArray<jobject>* _oops;  // 存储所有注册的 jobject
  
public:
  int allocate_oop_index(jobject o) {
    _oops->append(o);  // 添加到数组
    return _oops->length() - 1;  // 返回 index
  }
};
```

**nmethod 中的 oops section**:
```cpp
class nmethod : public NativeCodeBase {
  oop* oops_begin() const;   // oops section 起始
  oop* oops_end() const;     // oops section 结束
  
  // GC 时遍历
  void oops_do(OopClosure* f) {
    for (oop* p = oops_begin(); p < oops_end(); p++) {
      f->do_oop(p);  // GC 更新 oop 地址
    }
  }
};
```

### 关键点总结

1. **LIR 层**: `LIR_OprFact::oopConst(jobject)` 只保存 jobject 指针
2. **指令生成**: `MacroAssembler::movoop()` 通过 `OopRecorder` 分配 index
3. **Relocation**: 生成 `mov` 指令时附带 `oop_Relocation::spec(index)`
4. **nmethod**: CodeBuffer 复制时，根据 relocation 在 oops section 分配 slot
5. **GC**: 遍历 oops section 更新所有 slot 中的 oop 地址

**与 Yuhu 的关键区别**:
- C1 用 MacroAssembler 直接生成机器码 → 可以实时插入 relocation
- Yuhu 用 LLVM IR → ORC JIT 编译 → **无法直接使用 HotSpot 的 relocation**

## 待解决方案

### 方案 A：延迟嵌入（不推荐）
- IR 生成时不嵌入 UTF-8 数据，只记录"需要哪个字符串"
- register_method 时，在 nmethod 的 consts section 分配空间
- patch 指令中的地址

### 方案 B：使用 OopRecorder（不推荐）
- 在 IR 生成时使用 OopRecorder 注册 jobject
- register_method 时在 nmethod oops section 分配 slot
- 用 PC-relative load 从 slot 加载地址

### 方案 C：自定义 relocation（不推荐）
- ORC JIT 编译后扫描机器码中的占位符
- register_method 时 patch 所有占位符位置

### 方案 D：Marker + 占位符（最终方案）✅

**核心思想**：
1. IR 生成时用 inline asm 插入独特的 marker 指令序列
2. marker 中包含 oop_index（通过 OopRecorder 分配）
3. 直接生成占位符值作为返回值（不调用 runtime helper）
4. ORC JIT 编译后，代码被复制到 CodeCache
5. 调用 `builder.scan_for_oop_markers_and_generate_relocation()` 扫描 marker 并生成 HotSpot relocation 记录
6. register_method 时自动处理 patch

**实现步骤**：

```cpp
// 第 1 步：IR 生成 (YuhuBuilder::CreateInlineOop)
Value* YuhuBuilder::CreateInlineOop(ciObject* object, const char* name) {
  // 1. 分配 oop_index
  jobject jstring = JNIHandles::make_local(real_oop);
  int oop_index = code_buffer()->masm().code()->oop_recorder()->allocate_oop_index(jstring);
  
  // 2. 生成 marker 指令（inline asm）
  char asm_string[256];
  snprintf(asm_string, sizeof(asm_string),
           "mov w19, #0xCAFE\n"
           "movk w19, #0xBABE, lsl #16\n"
           "mov w20, #%d\n"  // oop_index
           "nop\n"
           "nop",
           oop_index & 0xFFFF);
  
  llvm::InlineAsm* marker_asm = llvm::InlineAsm::get(...);
  CreateCall(marker_asm);
  
  // 3. **直接返回占位符**（不嵌入 UTF-8，不调用 runtime helper）
  uint64_t placeholder = (0xDEAFULL << 48) | oop_index;
  return CreateIntToPtr(llvm::ConstantInt::get(i64_ty, placeholder), ptr_ty);
}

// 第 2 步：复制代码到 CodeCache 后扫描 (YuhuCompiler::compile_method)
memcpy(combined_base + adapter_size, entry->code_start(), effective_code_size);
builder.fixup_prologue_epilogue_markers(combined_base, combined_size);
builder.scan_for_oop_markers_and_generate_relocation(combined_base, combined_size);  // ← NEW!

// 第 3 步：scan_for_oop_markers_and_generate_relocation 实现 (YuhuBuilder.cpp)
void YuhuBuilder::scan_for_oop_markers_and_generate_relocation(address code_start, size_t code_size) {
  for (size_t i = 0; i < code_size / 4; i++) {
    uint32_t* instr = (uint32_t*)(code_start + i * 4);
    
    if (is_marker_pattern(instr)) {  // 检查 5 条指令序列
      int oop_index = extract_oop_index(instr);
      
      // 在后续指令中查找占位符
      for (size_t j = 0; j < 10; j++) {
        uint64_t* potential_placeholder = (uint64_t*)(code_start + (i + 5 + j) * 4);
        
        if (is_placeholder(*potential_placeholder)) {
          // 生成 HotSpot relocation 记录
          RelocationHolder rspec = oop_Relocation::spec(oop_index);
          code_buffer()->relocate((address)potential_placeholder, rspec);
          break;
        }
      }
    }
  }
}

// 第 4 步：register_method 自动处理
nmethod* nm = nmethod::new_nmethod(..., &combined_cb, ...);
// ↑ CodeBuffer 复制时会自动遍历 relocation 并 patch:
// for (RelocIterator iter(&combined_cb); !iter.done(); iter++) {
//   if (iter.type() == relocInfo::oop_type) {
//     oop_Relocation* reloc = iter.reloc<oop_Relocation>();
//     address placeholder_addr = nm->code_begin() + reloc.offset();
//     oop* slot = nm->oops_begin() + reloc.oop_index();
//     *(oop*)placeholder_addr = *slot;  // Patch 占位符为真实 oop 地址
//   }
// }
```

**优点**：
- ✅ 零误判（marker 模式独特：5 条连续指令）
- ✅ 精确定位（marker 后固定偏移处搜索占位符）
- ✅ 复用 C1 基础设施（OopRecorder、CodeBuffer、relocation）
- ✅ 扩展性强（可增加更多字段）
- ✅ 不需要 UTF-8 embedding 和 runtime lookup
- ✅ 符合 C1 语义（movoop → relocation → patch）

**缺点**：
- ⚠️ 需要 5 条额外指令 per string constant
- ⚠️ 需要验证 inline asm 不被 LLVM 优化

## 下一步行动

等待用户指示...

## 相关文件

- `/Users/liuanyou/CLionProjects/jdk8/hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` - CreateInlineOop
- `/Users/liuanyou/CLionProjects/jdk8/hotspot/src/share/vm/yuhu/yuhuCodeBuffer.hpp` - inline_data
- `/Users/liuanyou/CLionProjects/jdk8/hotspot/src/share/vm/asm/yuhu/yuhu_macroAssembler.hpp` - start_a_const/end_a_const

## 时间戳

2026-03-19

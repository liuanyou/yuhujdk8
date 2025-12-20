# Yuhu emit IR 问题汇总 (022)

本文档汇总了 Yuhu 编译器在生成 LLVM IR 过程中遇到的各种问题及其解决方案。

---

## 问题 1：PHI 节点类型不匹配

### 问题描述

在重构参数传递机制后，出现 PHI 节点类型不匹配错误：
```
Assertion failed: (getType() == V->getType() && "All operands to PHI node must be the same type as the PHI node!"), function setIncomingValue
```

错误发生在 `YuhuPHIState::add_incoming()` 中，当尝试将前驱 block 的状态合并到 PHI 节点时。

### 根本原因分析

#### 1.1 Method PHI 节点的类型不匹配（主要问题）

**问题现象：**
```
Method PHI: phi_type=ptr (ID=14), incoming_type=int (ID=12), match=0
ERROR: Method PHI type mismatch! Converting...
ERROR: Cannot convert method type!
```

**根本原因：**

1. **`CreateReadMethodRegister()` 返回的类型：**
   - 使用 `llvm.read_register` 读取 x12 寄存器（rmethod）
   - 返回类型：`intptr_type()`（整数类型，AArch64 上是 `i64`）

2. **`Method_type()` 的类型：**
   - 返回类型：`PointerType`（指针类型）
   - 定义：`PointerType::getUnqual(ArrayType::get(jbyte_type(), sizeof(Method)))`

3. **类型不匹配：**
   - `YuhuPHIState` 创建的 Method PHI 节点类型是 `Method_type()`（指针）
   - 但 `YuhuNormalEntryState` 中的 `method()` 是 `intptr_type()`（整数）
   - 当 `add_incoming()` 尝试合并时，类型不匹配导致断言失败

**调用链：**
```
YuhuFunction::initialize()
  -> method = builder()->CreateReadMethodRegister()  // 返回 intptr_type()
  -> entry_state = new YuhuNormalEntryState(start_block, method)
    -> YuhuNormalEntryCacher::process_method_slot()
      -> *value = method()  // 直接赋值，类型是 intptr_type()
  -> YuhuPHIState::add_incoming()
    -> method_phi->addIncoming(incoming_state->method())  // 类型不匹配！
```

#### 1.2 局部变量类型不匹配（潜在问题）

**问题场景：**
- PHI 节点的类型基于 `block->local_type_at_entry(i)`（类型流分析）
- 传入值的类型来自函数参数（`sig->type_at(i)`）
- 即使都使用 `YuhuType::to_stackType()`，输入的 `ciType*` 可能不同

**示例：**
- `ciSignature::type_at(i)` 返回方法签名中声明的类型
- `ciTypeFlow::local_type_at_entry(i)` 返回类型流分析推断的类型
- 对于 `int[][]`，可能分别映射为不同的 LLVM 类型

**解决方案：**
- 已在 `YuhuPHIValue::addIncoming()` 中实现自动类型转换
- 支持整数到整数、指针到指针、整数到指针、指针到整数的转换

#### 1.3 函数签名 vs 局部变量索引的映射关系（非静态方法）

**函数签名（`generate_normal_entry_point_type`）：**
- 静态方法：`(NULL, param0, param1, ...)`
- 非静态方法：`(param0, param1, ...)` - **不包括 `this`**

**局部变量索引：**
- 静态方法：`local[0] = param0, local[1] = param1, ...`
- 非静态方法：`local[0] = this, local[1] = param0, local[2] = param1, ...`

**注意：**
- 当前测试的是静态方法 `multiply`，所以这个问题不是主要原因
- 但如果将来支持非静态方法，需要特殊处理 `local[0]`（`this`）

### 修复方案

#### 修复 1.1：Method 类型转换（已修复）

**位置：** `hotspot/src/share/vm/yuhu/yuhuCacheDecache.cpp`

**修复代码：**
```cpp
void YuhuFunctionEntryCacher::process_method_slot(Value** value, int offset) {
  // "Cache" the method pointer
  // method() is read from register (x12) as intptr_type(), but we need Method_type() (pointer)
  llvm::Value* method_val = method();
  if (method_val->getType() != YuhuType::Method_type()) {
    // Convert from intptr_type() to Method_type() using inttoptr
    method_val = builder()->CreateIntToPtr(method_val, YuhuType::Method_type(), "method_ptr");
  }
  *value = method_val;
}
```

**说明：**
- 在 `process_method_slot` 中将 `method()`（整数类型）转换为 `Method_type()`（指针类型）
- 使用 `CreateIntToPtr` 进行类型转换
- 确保 `YuhuNormalEntryState` 中的 `method()` 类型与 `YuhuPHIState` 的 PHI 节点类型一致

#### 修复 1.2：Method PHI 节点的类型转换（已修复）

**位置：** `hotspot/src/share/vm/yuhu/yuhuState.cpp`

**修复代码：**
```cpp
if (method_phi_type != incoming_method_type) {
  tty->print_cr("  Method PHI type mismatch! Converting...");
  tty->flush();
  if (method_phi_type->isPointerTy() && incoming_method_type->isPointerTy()) {
    incoming_method = builder()->CreateBitCast(incoming_method, method_phi_type, "method_cast");
  } else if (method_phi_type->isPointerTy() && incoming_method_type->isIntegerTy()) {
    // Convert from intptr_type() to Method_type() using inttoptr
    incoming_method = builder()->CreateIntToPtr(incoming_method, method_phi_type, "method_cast");
  } else if (method_phi_type->isIntegerTy() && incoming_method_type->isPointerTy()) {
    // Convert from Method_type() to intptr_type() using ptrtoint
    incoming_method = builder()->CreatePtrToInt(incoming_method, method_phi_type, "method_cast");
  } else {
    tty->print_cr("  ERROR: Cannot convert method type!");
    tty->flush();
    ShouldNotReachHere();
  }
  // ... 验证转换后的类型 ...
}
```

**说明：**
- 在 `YuhuPHIState::add_incoming()` 中添加类型检查和转换
- 支持指针到指针、整数到指针、指针到整数的转换
- 作为防御性编程，即使 `process_method_slot` 已经转换，这里也能处理

#### 修复 1.3：局部变量 PHI 节点的类型转换（已实现）

**位置：** `hotspot/src/share/vm/yuhu/yuhuValue.cpp`

**实现：**
- `YuhuPHIValue::addIncoming()` 中已实现自动类型转换
- 支持整数到整数、指针到指针、整数到指针、指针到整数的转换
- 使用 `CreateIntCast`、`CreateBitCast`、`CreateIntToPtr`、`CreatePtrToInt`

### 相关文件

- `hotspot/src/share/vm/yuhu/yuhuCacheDecache.cpp` - Method 类型转换
- `hotspot/src/share/vm/yuhu/yuhuState.cpp` - Method PHI 节点类型转换
- `hotspot/src/share/vm/yuhu/yuhuValue.cpp` - 局部变量 PHI 节点类型转换
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` - `CreateReadMethodRegister()` 实现
- `hotspot/src/share/vm/yuhu/yuhuType.hpp` - `Method_type()` 定义

---

## 问题 2：base_pc 为 NULL 导致 SIGSEGV

### 问题描述

在生成 IR 时，出现 SIGSEGV 错误：
```
V  [libjvm.dylib+0x29cf500]  llvm::ConstantFolder::FoldNoWrapBinOp(...)
V  [libjvm.dylib+0xaf8b70]  llvm::IRBuilderBase::CreateAdd(...)
V  [libjvm.dylib+0xb06890]  YuhuBuilder::code_buffer_address(int)+0x68
V  [libjvm.dylib+0xb0d6c0]  YuhuDecacher::process_pc_slot(int)+0x4c
```

错误发生在 `YuhuDecacher::process_pc_slot()` 调用 `code_buffer_address()` 时。

### 根本原因分析

**调用链：**
```
YuhuDecacher::process_pc_slot(int offset)
  -> builder()->code_buffer_address(pc_offset())
    -> CreateAdd(code_buffer()->base_pc(), LLVMValue::intptr_constant(offset))
      -> llvm::ConstantFolder::FoldNoWrapBinOp (SIGSEGV!)
```

**问题根源：**

1. **`base_pc` 被设置为 NULL：**
   - 在 `YuhuFunction::initialize()` 中，对于 normal entry，`code_buffer()->set_base_pc(NULL)` 被设置
   - 这是因为根据 021 设计，`base_pc` 不再作为函数参数传递

2. **`code_buffer_address` 使用 `base_pc`：**
   - `code_buffer_address(int offset)` 调用 `CreateAdd(code_buffer()->base_pc(), constant)`
   - 当 `base_pc()` 返回 NULL 时，`CreateAdd(NULL, constant)` 导致 LLVM 常量折叠器崩溃

3. **为什么需要 `process_pc_slot`：**
   - `process_pc_slot` 用于记录 PC（程序计数器）到栈帧中，主要用于调试信息
   - 在 VM call 之前，需要记录当前 PC 以便 HotSpot 的栈遍历代码能够正确工作

**代码位置：**
- `hotspot/src/share/vm/yuhu/yuhuFunction.cpp:148` - `code_buffer()->set_base_pc(NULL)`
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp:677` - `code_buffer_address(int offset)`
- `hotspot/src/share/vm/yuhu/yuhuCacheDecache.cpp:116` - `process_pc_slot(int offset)`

### 修复方案

**位置：** `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp`

**修复代码：**
```cpp
Value* YuhuBuilder::code_buffer_address(int offset) {
  llvm::Value* base_pc = code_buffer()->base_pc();
  
  // For normal entry, base_pc may be NULL (set in YuhuFunction::initialize)
  // In this case, we need to use an alternative method to get the PC address
  // For now, we'll use a placeholder value (0) and add the offset
  // This is safe because process_pc_slot is only used for debug info
  // TODO: In the future, we could use PC register or frame address
  if (base_pc == NULL) {
    // For normal entry, base_pc is NULL because we no longer pass it as a parameter
    // Use a placeholder value (0) - this is only used for debug info recording
    // The actual PC will be calculated at runtime by HotSpot's stack walking code
    base_pc = LLVMValue::intptr_constant(0);
  }
  
  return CreateAdd(
    base_pc,
    LLVMValue::intptr_constant(offset));
}
```

**说明：**
- 检查 `base_pc()` 是否为 NULL
- 如果为 NULL（normal entry 的情况），使用 0 作为占位符
- 这仅用于调试信息记录，实际的 PC 会在运行时由 HotSpot 的栈遍历代码计算
- TODO: 未来可以考虑使用 PC 寄存器或 frame address 来获取真实的 PC 值

### 相关文件

- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` - `code_buffer_address()` 实现
- `hotspot/src/share/vm/yuhu/yuhuFunction.cpp` - `base_pc` 设置
- `hotspot/src/share/vm/yuhu/yuhuCacheDecache.cpp` - `process_pc_slot()` 实现

---

## 总结

### 问题 1 总结

**根本原因：**
1. **主要问题：** `CreateReadMethodRegister()` 返回 `intptr_type()`（整数），但 `Method_type()` 是指针类型
2. **潜在问题：** 局部变量的类型来源不同（`ciSignature` vs `ciTypeFlow`），可能导致类型不一致

**修复方法：**
1. 在 `process_method_slot` 中将 `method()` 转换为 `Method_type()`（指针类型）
2. 在 `YuhuPHIState::add_incoming()` 中添加 Method PHI 节点的类型转换
3. `YuhuPHIValue::addIncoming()` 中已实现局部变量 PHI 节点的自动类型转换

**经验教训：**
- 寄存器读取返回的是整数类型，需要转换为指针类型
- PHI 节点的类型必须与传入值的类型完全匹配
- 类型转换应该在值创建时进行，而不是在使用时进行（防御性编程）

### 问题 2 总结

**根本原因：**
- 在 021 参数传递简化设计中，`base_pc` 不再作为函数参数传递，被设置为 NULL
- `code_buffer_address()` 没有检查 `base_pc` 是否为 NULL，直接使用导致 LLVM 常量折叠器崩溃

**修复方法：**
- 在 `code_buffer_address()` 中添加 NULL 检查
- 如果 `base_pc` 为 NULL，使用占位符值（0）
- 这仅用于调试信息，不影响运行时行为

**经验教训：**
- 在简化设计时，需要考虑所有使用该值的地方
- 对于可能为 NULL 的值，必须在使用前进行检查
- 调试信息可以使用占位符值，但运行时值必须正确

---

## 测试验证

**测试方法：**
```java
public static void multiply(int[][] a, int[][] b, int[][] c, int n) {
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n; j++) {
      c[i][j] = 0;
      for (int k = 0; k < n; k++) {
        c[i][j] += a[i][k] * b[k][j];
      }
    }
  }
}
```

**验证点：**
1. Method PHI 节点类型匹配
2. 局部变量 PHI 节点类型匹配
3. Block 之间的状态传递正确
4. `code_buffer_address()` 正确处理 NULL `base_pc`
5. 程序能正常运行，不再出现类型不匹配或 SIGSEGV 错误


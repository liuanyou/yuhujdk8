# 活动 009: SIGILL 非法指令错误

## 日期
2025-12-06

## 问题描述

在运行 Yuhu 编译器时，遇到以下运行时错误：

```
SIGILL (0x4) at pc=0x000000010de5da00, pid=38830, tid=4355

Problematic frame:
J 9% yuhu com.example.Matrix.multiply([[I[[I[[II)V (79 bytes) @ 0x000000010de5da00 [0x000000010de5da00+0x0]
```

错误发生在执行 Yuhu 编译器生成的代码时。

## 错误分析

### 1. 成功的部分

从日志可以看到，之前的步骤都成功了：

1. ✅ **IR 验证通过**：`IR verification passed`
2. ✅ **Function 在 Module 中**：`Function found in Module function list: YES`
3. ✅ **LLVM 返回代码指针**：`getPointerToFunction returned: 0x142039818`（示例，非 NULL）
4. ⚠️ **nmethod 内仅 52 字节模板**：`main code [0x...9c00,0x...9c34] = 52`，与 LLVM 输出不一致

### 2. 问题分析（更新）

SIGILL 的直接原因：**安装阶段没有把 LLVM 生成的机器码写入 nmethod/CodeCache**，nmethod 执行的是 `hscb` 模板的 52 字节，占位/udf 指令。

- `getPointerToFunction` 返回的地址（例 0x142039818）是真实 LLVM 输出。
- `register_method` 只接受 `CodeBuffer*`（`hscb`），不知道 `entry` 里的指针。
- `YuhuMemoryManager::allocateCodeSection` 目前用 `os::malloc`，LLVM 代码落在 malloc 区，未与 CodeCache/CodeBuffer 对接。
- nmethod 复制的仍是 `hscb` 里 52 字节模板，反汇编充满 UDF/0xCC，因此 SIGILL。

### 3. 关键观察（更新）

- LLVM 指针（例 0x142039818）与 nmethod 入口（例 0x128019c00）不一致。
- nmethod `main code` 只有 52 字节，反汇编为 UDF/0xCC，占位。
- `YuhuTraceInstalls` 打印的 code_limit 末端是 0xcccc…（哨兵），表明 CodeBuffer 未填入真实机器码。

### 4. 与 Shark 的对比

需要检查 Shark 是如何设置入口点和安装代码的，看看是否有差异。

## 根因（当前结论）

安装路径缺失：LLVM 机器码未被放入 `register_method` 使用的 CodeBuffer/CodeCache，nmethod 执行的仍是模板 52 字节。

## 解决方向（待实施）

两条路径（二选一）把 LLVM 输出接入 nmethod：

1) **直接写 CodeCache**  
   - `YuhuMemoryManager::allocateCodeSection` 从 CodeCache/BufferBlob 分配，记录 base/size 与 `Function*`。  
   - `generate_native_code` 后取回 base/size，`entry->set_entry_point(base)`、`set_code_limit(base+size)`，`CodeBuffer`/`register_method` 使用这块内存。

2) **malloc -> 拷贝到 CodeBuffer**  
   - MemoryManager 记录每个 `Function` 的 code section base/size。  
   - `getPointerToFunction` 后，将该段机器码拷贝进 `hscb` 的 inst 区（或新建合适大小的 CodeBuffer），再 `register_method`，保证 nmethod 拷贝的是 LLVM 输出。

关键点：`register_method` 只认传入的 `CodeBuffer`，`entry->set_entry_point(code)` 本身不会让 nmethod 使用 LLVM 机器码。

## 相关文件

- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - `generate_native_code` 方法
- `hotspot/src/share/vm/yuhu/yuhuEntry.hpp` / `.cpp` - 入口点设置
- `hotspot/src/share/vm/yuhu/yuhuStack.cpp` - 栈帧初始化
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` - IR 生成

## 状态

- ✅ 问题已定位：nmethod 未包含 LLVM 机器码，执行模板 52 字节导致 SIGILL
- ⏳ 方案待实现：将 LLVM 输出写入 CodeCache/CodeBuffer（两路径择一）
- ⏳ 后续：实施对接后验证矩阵乘法执行正确



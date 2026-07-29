# 活动 076: HashMap.resize GC 崩溃 - 活跃 OOP 以整数形式逃逸 RS4GC 活跃性分析

## 日期
2026-06-27

## 状态
**已解决** - 前端寻址改为 addrspace(1) GEP，DataLayout 添加 ni:1，运行时验证通过

## 问题描述

Yuhu 编译的 `java.util.HashMap::resize()` 在 GC 压力下崩溃：

```
-XX:+UseYuhuCompiler -XX:TieredStopAtLevel=6
-XX:YuhuCompileOnlyOf=*java.util.HashMap*
-XX:+SafepointALot -XX:+GCALotAtAllSafepoints -Xmx16m
```

崩溃点在 `Klass::is_subtype_of`，`x0=0x48`（从垃圾 Klass 指针加载偏移 0x48 处的字段）。

崩溃链：

1. `resize()` 的旧 table 指针（解压后的 oop 地址）被溢出到栈槽 `[sp+32]`
2. 该栈槽**没有对应的 OopMap 条目**，safepoint 时 GC 不会更新它
3. GC 移动了旧 table 对象后，`[sp+32]` 中的指针变为悬空
4. 循环用悬空地址加载 compressed oop → 得到垃圾值 → 解码出垃圾对象地址
5. 从垃圾对象加载 Klass 指针 → 垃圾 Klass → `is_subtype_of` 访问 `0x48` 崩溃

## 错误原因分析

### 1. 根本原因

旧 table 的地址在 IR 中是一个**裸 i64 整数**，而不是 `ptr addrspace(1)` GC 指针。
RS4GC（`yuhuRewriteStatepointsForGC.cpp`）的活跃性分析 `computeLiveInValues` 只跟踪
`isHandledGCPointerType`（即 addrspace(1) 指针）类型的 SSA 值，i64 对它完全不可见：

```llvm
; RS4GC 输入 IR（修复前）
%17 = zext i32 %16 to i64
%18 = shl nuw nsw i64 %17, 3                 ; 解压后的 table 地址（i64！）
%19 = inttoptr i64 %18 to ptr addrspace(1)   ; GC 指针视图（仅少数使用点用它）

; 循环内（跨越 safepoint poll statepoint）全部基于 %18 寻址：
%80 = add nuw nsw i64 %18, %79               ; 元素地址 = i64 加法
```

`%19` 被 RS4GC 正常 relocate，但循环用的是 `%18`。i64 跨越 statepoint 时：
无 relocation → 无 stackmap 位置 → 无 OopMap 位 → GC 不修复 `[sp+32]`。

### 2. i64 寻址是如何产生的

两个因素叠加：

**(a) 前端故意用整数寻址。** `CreateAddressOfStructEntry` / `CreateArrayAddress`
对每次字段/数组元素访问生成：

```cpp
// yuhuBuilder.cpp（修复前）
Value* base_int = CreatePtrToInt(base, YuhuType::intptr_type());
Value* result_int = CreateAdd(base_int, byte_offset, "field_addr_int");
Value* result_ptr = CreateIntToPtr(result_int, type, name);
```

**(b) InstCombine 合法折叠。** Yuhu 的 pass 管线（`yuhuIRTransformer.cpp`）
在 RS4GC 之前运行 InstCombine。在 integral 地址空间中
`ptrtoint(inttoptr(x)) → x` 是合法折叠：由于 local 1（旧 table）从未被重新赋值，
所有基本块中的 `ptrtoint(%local_1_*)` 全部塌缩为 entry 块唯一的 `%18`，
被提升（hoist）跨越了所有 statepoint。

### 3. 为什么之前的手工修复无效

在 `maybe_add_safepoint` 中手工构造 `gc-live` operand bundle 是无效的（no-op）：
RS4GC **自己重新计算活跃性**（`analyzeParsePointLiveness`），只读取输入的
`deopt` bundle，完全忽略输入的 `gc-live` bundle。该手工代码已删除。

## 修复方案

### 修复 1: DataLayout 添加 `ni:1`（非整型地址空间声明）

文件：`hotspot/src/share/vm/yuhu/yuhuCompiler.cpp`

模块 DataLayout 和 LLJIT DataLayout 均追加 `-ni:1`：

```cpp
std::string DLStr = DL.getStringRepresentation() + "-ni:1";
_normal_context->module()->setDataLayout(DLStr);
_native_context->module()->setDataLayout(DLStr);
// LLJITBuilder 侧：
JITBuilder.setDataLayout(llvm::DataLayout(DLStrWithNI));
```

作用：声明 addrspace(1) 指针的整数表示不稳定，**禁止优化器折叠/引入**
`ptrtoint`/`inttoptr` 往返（正是第 2(b) 步的折叠）。这是 Azul Falcon、Julia
使用的标准机制。注意：LLVM verifier 只拒绝 ConstantExpr 形式的非法转换，
不拒绝指令形式，因此 ni:1 本身不产生报错——它是语义约束而非诊断工具。

### 修复 2: 自定义 GC 逃逸扫描

文件：`hotspot/src/share/vm/yuhu/yuhuIRTransformer.cpp`

新增 `reportNonIntegralPointerCasts(Module&)`，在 pass 管线运行前扫描所有
addrspace(1) 上的 `PtrToIntInst` / `IntToPtrInst`（含 ConstantExpr 操作数形式），
输出受 `YuhuTraceIRCompilation` 控制。用于持续捕获新引入的非法转换。

### 修复 3: 前端寻址迁移为 addrspace(1) GEP（真正的修复）

| 位置 | 修改 |
|---|---|
| `yuhuBuilder.cpp` `CreateAddressOfStructEntry` | base 为 addrspace(1) 时改用 `getelementptr i8, ptr addrspace(1) base, offset`；非 GC 指针（Thread*/Method* 等）保留整数路径 |
| `yuhuBuilder.cpp` `CreateArrayAddress` | 同上规则 |
| `yuhuBuilder.cpp` `CreateInlineOopForStaticField` | klass mirror（堆 oop）的静态字段地址同样改为 GEP |
| `yuhuTopLevelBlock.cpp` `do_if` | 对象比较改用 `jobject_value()` 直接做指针 `icmp`，删除 `intptr_value()`/`ptrtoint`（覆盖 ifnull/ifnonnull/if_acmpeq/if_acmpne） |
| `yuhuTopLevelBlock.cpp` `do_aload`（compressed oop 分支） | 删除元素地址上的 `CreateBitCast`（bitcast 不能跨地址空间，元素地址现为 addrspace(1)），直接从 GEP 地址加载 i32 narrowOop |
| `yuhuFunction.cpp` 统一出口（非 OSR + OSR 两处） | T_OBJECT/T_ARRAY 返回值直接按 `ptr addrspace(1)` 加载 return slot，删除 `load i64` + `inttoptr` |

GEP 产生的是**派生指针（derived pointer）**，RS4GC 的
`findBaseDefiningValue` 沿 GEP 指针操作数回溯到基指针，活跃性分析、
relocation、stackmap、OopMap 全链路正常工作。

### 保留不变的合法转换（白名单）

1. **解码**（`CreateDecodeHeapOop` 的 `inttoptr`）：RS4GC 明确将 `IntToPtrInst`
   视为 base pointer（`yuhuRewriteStatepointsForGC.cpp:528`），GC 安全。
   **注意：不可改用 `gep i8, ptr addrspace(1) null, i64` 惯用法** —— 本仓库的
   RS4GC 会将常量基指针的派生指针从 live set 中移除（`:2818-2821`），
   导致解码后的 oop 对 GC 完全不可见。
2. **编码**（`CreateEncodeHeapOop` 的 `ptrtoint`）：i64 立即死于
   `sub/lshr/trunc` → compressed store，不跨 statepoint。
3. **Card mark**（`CreateUpdateBarrierSet` 的 `ptrtoint`）：i64 立即死于
   `lshr 9` + card table 基址 → dirty byte store，不跨 statepoint。

## 验证

### 1. 逃逸扫描：46 → 19，且全部为白名单残留

修复前 `HashMap::resize` 有 46 处逃逸（约 35 处字段/数组寻址 + 5 处解码 +
编码/整数空指针检查 + return slot）。修复后剩 19 处，全部验证为白名单模式：

- 4 处解码 `inttoptr`（`shl 3` → `add 0` → `inttoptr`）
- 6 处编码 `ptrtoint`（`sub 0` → `lshr 3` → `trunc i32`）
- 9 处 card mark `ptrtoint`（`lshr 9` → `add 0x102638000`）

寻址、整数空指针检查、return slot 逃逸全部归零。

### 2. RS4GC 输出 IR

`_tmp_yuhu_ir_RS4GC_java.util.HashMap__resize.ll`：

```llvm
%17 = shl nuw nsw i64 %16, 3
%18 = inttoptr i64 %17 to ptr addrspace(1)      ; table 现在是 GC 指针
%20 = getelementptr i8, ptr addrspace(1) %18, i64 12   ; GEP 寻址
...
statepoint ... [ "gc-live"(ptr addrspace(1) %18, ptr addrspace(1) %12) ]
%320 = call ... @llvm.experimental.gc.relocate.p1(...)  ; (%18, %18)
```

table 指针出现在 `gc-live` 中并被 relocate（全函数 31 处 `gc.relocate`），
裸 i64 `%17` 立即死于 `inttoptr`，不再跨越任何 statepoint。

### 3. 运行时压力测试

```
$ java -Xbatch -XX:+UseYuhuCompiler -XX:TieredStopAtLevel=6 \
    -XX:YuhuCompileOnlyOf='*java.util.HashMap*' -XX:+PrintCompilation \
    -XX:+SafepointALot -XX:+GCALotAtAllSafepoints -Xmx16m com.example.HashMapTest

   1610   66    b  6       java.util.HashMap::resize (359 bytes)
   1698   47       3       java.util.HashMap::resize (359 bytes)   made not entrant
PASSED - completed in 817ms    (exit 0)
```

`resize` 以 tier 6（Yuhu）编译并实际执行（tier 3 版本 made not entrant），
在 SafepointALot + GCALotAtAllSafepoints + 16MB 堆下无 SIGSEGV。
修复前同配置稳定崩溃于 `Klass::is_subtype_of`（x0=0x48）。

## 修改文件汇总

- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - DataLayout/LLJIT 添加 ni:1
- `hotspot/src/share/vm/yuhu/yuhuIRTransformer.cpp` - GC 逃逸扫描
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` - 三处寻址改 GEP
- `hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp` - do_if 指针比较、do_aload 去 bitcast、删除无效 gc-live bundle
- `hotspot/src/share/vm/yuhu/yuhuFunction.cpp` - return slot 按 oop 类型加载

## 影响范围

- 所有 Yuhu 编译方法中跨 safepoint 存活的对象引用（字段/数组寻址是最普遍的模式）
- 此前任何"对象在循环中被访问 + 循环内有 safepoint poll + GC 移动对象"的场景都可能触发同类崩溃，HashMap.resize 只是最易复现的实例

## 经验教训

1. **GC 指针一旦转为整数，就脱离了 RS4GC 的世界。** 前端注释声称整数寻址
   "avoids potential GEP optimization issues"，实际上 GEP 派生指针才是
   statepoint 体系的正确形态。
2. **优化器折叠会放大前端的类型不诚实。** 逐访问点的 `ptrtoint→add→inttoptr`
   本身不跨 statepoint，但 InstCombine 的合法折叠把整数生命期拉长到整个函数。
3. **RS4GC 忽略输入的 gc-live bundle**，手工构造无效；活跃性只能靠正确的
   IR 类型（addrspace(1)）表达。
4. **ni:1 是护栏不是修复**：它阻止优化器重新引入逃逸，但前端自身的逃逸
   必须逐点迁移；且 verifier 不报指令级违规，需要自定义扫描兜底。

## 优先级

**高** - GC 正确性缺陷，导致 JVM 崩溃，影响所有含 safepoint 的编译方法。

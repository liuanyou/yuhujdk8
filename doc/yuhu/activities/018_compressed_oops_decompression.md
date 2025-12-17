# 018: 压缩指针解压缩问题

## 问题描述

在测试 `multiply` 方法时，发现 `c` 数组的值不正确：
- `c[0][0]` 一直是 `328350`，即使多次调用 `multiply` 也没有改变
- `a` 和 `b` 数组的值是正确的
- 说明 `multiply` 方法中对 `c[i][j]` 的写入没有生效

## 根本原因

Yuhu 编译器在处理对象数组（如 `int[][]`）时，**没有解压缩压缩指针（Compressed Oops）**。

### 压缩指针机制

在 HotSpot 中，如果启用了 `UseCompressedOops`：
- 对象引用（oop）被压缩为 32 位（`narrowOop`）
- 解压缩公式：`uncompressed = heap_base + (compressed << 3)`
- 如果 `heap_base == NULL`，则 `uncompressed = compressed << 3`

### 问题场景

对于二维数组 `int[][]`：
1. `c` 是 `int[][]` 类型（对象数组）
2. 访问 `c[i]` 时，得到的是 `int[]` 的压缩指针（32 位）
3. **Yuhu 没有解压缩这个指针**
4. 后续访问 `c[i][j]` 时，使用了错误的地址（压缩指针而不是解压缩后的地址）
5. 导致写入操作写入到错误的内存位置

### 字节码序列

```java
// c[i][j] = 0;
aload_2        // 加载 c (int[][])
iload          // 加载 i
aaload         // 访问 c[i]，得到 int[] 的压缩指针（32位）
iload          // 加载 j
iconst_0       // 加载 0
iastore        // 写入 c[i][j] = 0
```

在 `aaload` 时，Yuhu 的 `do_aload(T_OBJECT)` 返回的是压缩指针，但没有解压缩。

## 解决方案

在 `do_aload` 的 `T_OBJECT` 分支中添加压缩指针解压缩逻辑。

### 修改位置

`hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp:889-905`

### 修改内容

```cpp
case T_OBJECT:
  // Decompress compressed oops if UseCompressedOops is enabled
  // For int[][], c[i] returns a compressed pointer (32-bit) that needs to be
  // decompressed to a full pointer (64-bit) before accessing c[i][j]
  if (UseCompressedOops) {
    // value is currently a 32-bit compressed pointer (narrowOop)
    // Decompress: uncompressed = heap_base + (compressed << 3)
    // If heap_base is NULL, then: uncompressed = compressed << 3
    address heap_base = Universe::narrow_oop_base();
    int shift = Universe::narrow_oop_shift();
    
    // Cast to intptr for arithmetic
    Value* compressed_intptr = builder()->CreateZExt(
      value, YuhuType::intptr_type(), "compressed_intptr");
    
    // Shift left by 3 (or by Universe::narrow_oop_shift())
    Value* shifted = builder()->CreateShl(
      compressed_intptr,
      LLVMValue::intptr_constant(shift),
      "compressed_shifted");
    
    if (heap_base != NULL) {
      // Add heap base
      value = builder()->CreateAdd(
        shifted,
        LLVMValue::intptr_constant((intptr_t)heap_base),
        "decompressed_oop");
    } else {
      value = shifted;
    }
    
    // Cast back to pointer type
    value = builder()->CreateIntToPtr(
      value,
      llvm::PointerType::getUnqual(YuhuType::oop_type()),
      "decompressed_ptr");
  }
  
  // ... 原有的代码 ...
  push(YuhuValue::create_generic(...));
  break;
```

### 解压缩逻辑

1. **检查是否启用压缩指针**：`if (UseCompressedOops)`
2. **获取堆基址和位移**：
   - `heap_base = Universe::narrow_oop_base()`
   - `shift = Universe::narrow_oop_shift()`（通常是 3）
3. **扩展为 64 位**：`CreateZExt` 将 32 位压缩指针扩展为 64 位
4. **左移**：`CreateShl` 将压缩指针左移 `shift` 位（通常是 3）
5. **添加堆基址**：如果 `heap_base != NULL`，则加上堆基址
6. **转换回指针类型**：`CreateIntToPtr` 将整数转换为指针

### 生成的 LLVM IR 示例

```llvm
; 加载压缩指针（32位）
%compressed = load i32, i32* %c_i_addr

; 扩展为 64 位
%compressed_intptr = zext i32 %compressed to i64

; 左移 3 位
%shifted = shl i64 %compressed_intptr, 3

; 添加堆基址（如果存在）
%heap_base = inttoptr i64 0x700000000 to i8*
%heap_base_int = ptrtoint i8* %heap_base to i64
%decompressed_oop = add i64 %shifted, %heap_base_int

; 转换为指针
%decompressed_ptr = inttoptr i64 %decompressed_oop to i8*
```

## 验证方法

### 1. 检查解压缩后的地址

在 lldb 中检查 `c[i]` 解压缩后的地址是否正确：

```lldb
# 检查 c[0] 的压缩指针
(lldb) x/1wx 0x000000076aba0000+16  # c[0] 的压缩指针
# 假设输出是 0xed571067

# 解压缩：0xed571067 << 3 = 0x76ab88338
# 检查这个地址是否指向有效的 int[] 对象
(lldb) check_2d_array 0x000000076aba0000
```

### 2. 检查写入操作

在 `multiply` 方法中，`c[i][j] = 0;` 应该写入到解压缩后的地址：

```lldb
# 在 iastore 指令处设置断点
# 检查写入的地址是否是解压缩后的地址
```

### 3. 运行测试

```java
int[][] c = new int[n][n];  // 应该初始化为全 0
multiply(a, b, c, n);
System.out.println("c[0][0] = " + c[0][0]);  // 应该不是 328350
```

## 相关代码

### C1 编译器的实现

C1 编译器在 `LIR_Assembler::load` 中处理压缩指针：

```cpp
case T_ARRAY:
case T_OBJECT:
  if (UseCompressedOops && !wide) {
    __ lduw(base, disp, to_reg->as_register());  // 加载 32 位压缩指针
    __ decode_heap_oop(to_reg->as_register());  // 解压缩
  } else {
    __ ld_ptr(base, disp, to_reg->as_register());  // 加载 64 位指针
  }
  break;
```

### HotSpot 的解压缩函数

`hotspot/src/share/vm/oops/oop.inline.hpp`:

```cpp
inline oop oopDesc::decode_heap_oop_not_null(narrowOop v) {
  address base = Universe::narrow_oop_base();
  int    shift = Universe::narrow_oop_shift();
  return (oop)(void*)((uintptr_t)base + ((uintptr_t)v << shift));
}
```

## 影响范围

这个修复影响所有使用对象数组的代码：
- `int[][]`, `Object[][]` 等多维数组
- 任何包含对象引用的数组访问

## 测试建议

1. **基本测试**：`multiply` 方法应该能正确写入 `c` 数组
2. **边界测试**：测试 `c[0][0]` 和 `c[n-1][n-1]`
3. **性能测试**：确保解压缩操作不会显著影响性能

## 后续工作

1. **检查 `do_astore`**：确保存储操作也正确处理压缩指针（如果需要）
2. **优化**：如果 `heap_base == NULL`，可以简化解压缩逻辑
3. **测试覆盖**：添加更多测试用例覆盖不同的压缩指针场景


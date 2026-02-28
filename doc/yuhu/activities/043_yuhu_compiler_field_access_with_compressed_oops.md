# Yuhu编译器在AArch64平台上字段访问时未正确处理压缩指针问题

## 问题概述

Yuhu编译器在AArch64平台上处理对象引用类型的字段访问（getfield）时，未正确处理压缩指针（Compressed Oops），导致加载到错误的指针值。

## 问题详情

在StreamEncoder类的字段访问过程中，观察到以下现象：
- `ldur x1, [x23, #28]` 加载了错误的值 `0xed571b83ed571b79`
- `ldr x3, [x23, #32]` 加载了错误的值 `0xed571b48ed571b83`

这些值实际上是压缩指针的原始32位值，未经解压就被当作64位指针使用。

## 根本原因

对比JVM其他组件的实现发现：

### 模板解释器的正确实现
在AArch64的`macroAssembler_aarch64.cpp`中：
```cpp
void MacroAssembler::load_heap_oop(Register dst, Address src) {
  if (UseCompressedOops) {
    ldrw(dst, src);        // 读取32位压缩指针
    decode_heap_oop(dst);  // 解压成64位完整指针
  } else {
    ldr(dst, src);         // 直接读取64位指针
  }
}
```

### Yuhu编译器的问题实现
Yuhu编译器在`yuhuBlock.cpp`中的字段访问代码：
```cpp
// Yuhu编译器中的字段访问代码
Value* field_value = builder()->CreateLoad(field_type, addr);
```

Yuhu编译器只是直接加载了内存值，**没有处理压缩指针的解压过程**。

## 影响

- 对象引用类型的字段访问返回错误的指针值
- 导致后续的指针解引用操作失败
- 可能引发程序崩溃或数据错误

## 解决方案思路

Yuhu编译器在处理对象引用类型的字段访问时，应：
1. 检测是否启用了压缩指针（UseCompressedOops）
2. 如果启用，在加载32位压缩指针后，调用相应的解压函数
3. 确保生成的代码行为与模板解释器一致

## 相关代码位置

- `hotspot/src/share/vm/yuhu/yuhuBlock.cpp` - do_field_access方法
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` - CreateValueOfStructEntry方法
- `hotspot/src/cpu/aarch64/vm/macroAssembler_aarch64.cpp` - load_heap_oop方法
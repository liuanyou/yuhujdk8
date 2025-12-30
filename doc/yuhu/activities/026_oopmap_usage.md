# Yuhu 编译器 OopMap 使用指南

## 什么是 OopMap？

**OopMap** (对象对象指针映射) 是 HotSpot 的数据结构，对于给定的编译程序计数器 (PC) 偏移量，指定：

- **哪些栈槽**包含 oops (对象引用)
- **哪些寄存器**包含 oops

VM 在以下情况下使用 OopMap 信息：

- **垃圾收集 / Safepoint 栈遍历**：识别需要标记的对象
- **去优化 / 异常处理 / 非常见陷阱**：重建解释器帧
- **NMethod 清理器 / 性能分析**：遍历编译帧时

## OopMap 数据结构

OopMap 包含在特定程序点上对象引用位置的元数据。这允许 GC 正确追踪对象图，并在需要时让 VM 重建执行状态。

## find_map_at_offset 的工作原理

`OopMapSet::find_map_at_offset` 函数实现了一个关键查找算法：

```cpp
OopMap* OopMapSet::find_map_at_offset(int pc_offset) const {
  int i, len = om_count();
  assert( len > 0, "must have pointer maps" );

  // 遍历 oopmaps。当当前偏移量大于或等于查找的偏移量时停止。
  for( i = 0; i < len; i++) {
    if( at(i)->offset() >= pc_offset )
      break;
  }

  assert( i < len, "oopmap not found" );

  OopMap* m = at(i);
  assert( m->offset() == pc_offset, "oopmap not found" );
  return m;
}
```

### 算法细节：

- 所有 OopMap 条目按 **偏移量升序存储**
- 对于 **VM 可能需要遍历帧的每个 PC**，必须有 **完全匹配该偏移量的映射**
- 算法查找第一个 `offset() >= pc_offset` 的映射
- 然后断言 `offset() == pc_offset` - 如果失败，意味着没有完全匹配

### 关键约定：

**HotSpot 永远不能请求没有精确 OopMap 条目的偏移量。**

## Yuhu 如何创建 OopMaps

### Decacher OopMap 创建

在 `yuhuCacheDecache.cpp` 中，Yuhu decacher 为 safepoints 创建 OopMaps：

```cpp
void YuhuDecacher::start_frame() {
  _pc_offset = code_buffer()->create_unique_offset();
  _oopmap = new OopMap(
    oopmap_slot_munge(stack()->oopmap_frame_size()),
    oopmap_slot_munge(arg_size()));
  debug_info()->add_safepoint(pc_offset(), oopmap());
}
```

对于 PC 槽，Yuhu 记录代码地址：

```cpp
void YuhuDecacher::process_pc_slot(int offset) {
  // 记录 PC
  builder()->CreateStore(
    builder()->code_buffer_address(pc_offset()),
    stack()->slot_addr(offset));
}
```

### 过程：

1. 使用 `create_unique_offset()` 分配一个新的 `pc_offset`
2. 使用适当的帧大小和参数大小创建 `OopMap`
3. 通过 `add_safepoint(pc_offset(), oopmap())` 注册到 `DebugInformationRecorder`
4. 将 **代码地址** 存储到帧的 PC 槽

## Yuhu 特有的 OopMap 问题

### 问题1：IR 阶段与机器码阶段偏移量映射

在 Yuhu 中，OopMap 生成分为两个阶段：

- **IR 生成阶段**：
  - 在 IR 阶段就为每个 safepoint 生成虚拟偏移量
  - 使用 `create_unique_offset()` 创建虚拟 PC 位置
  - 此时还没有实际的机器码，偏移量是虚拟的

- **机器码生成阶段**：
  - LLVM IR 转换为实际机器码
  - 实际的 PC 偏移量确定
  - 需要创建**虚拟偏移到实际 PC 偏移的映射**

**问题**：IR 阶段生成的虚拟偏移量（如 1-51）与实际运行时的 PC 偏移量（如 180）不一致

**解决方案**：在机器码生成后，需要将虚拟偏移量重定位到实际的机器码偏移量

### 问题2：Adapter-LLVM 代码偏移量不匹配问题

最常见的 OopMap 问题由 adapter + LLVM 代码布局引起：

1. **LLVM 代码生成**：Yuhu 首先将 LLVM 代码生成到临时缓冲区
2. **OopMap 记录**：OopMap 使用相对于原始 LLVM 缓冲区的偏移量记录
3. **组合 Blob 创建**：对于普通方法，创建组合 blob：
   - 首先：adapter 代码 (大小：`adapter_size`)
   - 然后：LLVM 代码的副本在 adapter 之后
4. **偏移量不匹配**：安装的 nmethod 看到 `code_begin = combined_base`，所以返回地址的偏移量包含 `adapter_size`，但 OopMap 是使用来自原始 LLVM 缓冲区的偏移量记录的

### 详细问题

当 VM 遍历 Yuhu 编译的帧时：

1. VMThread 在 safepoint 时遍历编译帧
2. 获取组合 blob (adapter + LLVM) 内的 **返回地址**
3. 计算 `pc_offset_actual = return_address - code_begin` → 包含 `adapter_size`
4. 请求 `OopMapSet::find_map_at_offset(pc_offset_actual)`
5. Yuhu 的 OopMap 位于像 `k` 这样的偏移量，而不是 `adapter_size + k`
6. 没有完全匹配该偏移量的条目 → `assert(i < len)` / `assert(m->offset() == pc_offset)` 失败

### 问题3：last_Java_pc 设置时机问题

**问题描述**：
- 在函数入口处就设置了 `last_Java_pc`
- 但此时还没有到达任何 safepoint 调用点
- 即使虚拟偏移能够正确映射到实际偏移，实际的 PC 偏移量也是不正确的

**正确做法**：
- 应该在每个 safepoint 调用前才更新 `last_Java_pc`
- 而不是在函数入口处就设置
- 确保 `last_Java_pc` 总是指向有对应 OopMap 记录的位置

**根本原因**：
- `last_Java_pc` 指向的位置必须在 OopMapSet 中有对应的条目
- 函数入口位置可能没有 OopMap 记录
- 导致 `OopMapSet::find_map_at_offset()` 查找失败

**影响**：
- VM 在栈遍历时无法找到对应的 OopMap
- 触发 `assert(m->offset() == pc_offset, "oopmap not found")` 断言失败

## 解决方案

### 问题1 解决方案：IR 阶段与机器码阶段偏移量映射

为了解决 IR 阶段生成的虚拟偏移量与机器码阶段的实际偏移量不一致的问题，我们采用以下完整方案：

#### 1. YuhuOffsetMapper 类
创建 `YuhuOffsetMapper` 类来管理虚拟偏移量到实际偏移量的映射：

```cpp
// YuhuOffsetMapper 类定义
class YuhuOffsetMapper : public CHeapObj<mtCompiler> {
 private:
  std::map<int, int> _offset_map;  // 虚拟偏移到实际偏移的映射
  GrowableArray<OffsetMapping>* _mappings;  // 用于有序迭代的映射数组

 public:
  void add_mapping(int virtual_offset, int actual_offset);  // 添加映射
  int get_actual_offset(int virtual_offset) const;         // 获取实际偏移
  int relocate_offset(int virtual_offset) const;           // 重定位偏移
  // ... 其他方法
};
```

#### 2. 标记插入机制
- **IR 阶段**：在每个 safepoint 位置插入包含虚拟偏移量的特殊标记
- **标记形式**：插入特殊的 IR 指令或元数据标记，这些标记在机器码生成时可以被识别
- **标记内容**：包含 `create_unique_offset()` 生成的虚拟偏移量
- **标记识别**：在 LLVM 机器码生成过程中，这些特殊标记可以被识别并用于建立虚拟偏移到实际偏移的映射

**实现细节**：

1. **LLVM IR 标记插入**：在 IR 生成阶段，插入特殊的函数调用或内联汇编作为标记：

```cpp
// 在 IR 阶段插入标记
void YuhuDecacher::start_frame() {
  _pc_offset = code_buffer()->create_unique_offset();  // 虚拟偏移量
  
  // 插入特殊的标记指令，包含虚拟偏移量信息
  // 这可以通过特殊的 inline assembly 或函数调用来实现
  code_buffer()->insert_offset_marker(_pc_offset);
  
  _oopmap = new OopMap(...);
  debug_info()->add_safepoint(pc_offset(), oopmap());
}
```

2. **标记实现**：在 `YuhuCodeBuffer` 中实现标记插入：

```cpp
int YuhuCodeBuffer::insert_offset_marker(int virtual_offset) const {
  // 记录虚拟偏移到实际偏移的映射点
  int actual_offset = masm().offset();
  
  // 插入特殊的标记指令（例如空的 inline assembly）
  // 这个标记在机器码中占据位置，但不执行实际操作
  // 同时记录虚拟偏移量信息到映射器
  if (_offset_mapper != NULL) {
    _offset_mapper->add_mapping(virtual_offset, actual_offset);
  }
  
  return actual_offset;
}
```

3. **机器码生成时的处理**：LLVM 编译器在生成机器码时会保留这些标记的位置，使得实际偏移量可以与虚拟偏移量关联：

```cpp
// 在 LLVM IR 到机器码的转换过程中
// 特殊标记的位置对应实际的机器码偏移量
// 虚拟偏移量信息通过映射器保存，建立对应关系

// 在 YuhuDecacher::start_frame() 中：
void YuhuDecacher::start_frame() {
  _pc_offset = code_buffer()->create_unique_offset();  // 虚拟偏移量
  
  // 使用 builder 创建特殊的 LLVM IR 标记，该标记在机器码生成时可被识别
  builder()->CreateOffsetMarker(_pc_offset);
  
  _oopmap = new OopMap(...);
  debug_info()->add_safepoint(pc_offset(), oopmap());
}

// 在 YuhuBuilder::CreateOffsetMarker 中：
void YuhuBuilder::CreateOffsetMarker(int virtual_offset) {
  // 创建独特的内联汇编调用作为偏移标记
  // 这在生成的机器码中创建可识别的模式
  
  YuhuContext& ctx = YuhuContext::current();
  
  // 创建函数类型: void function(int32) - 以虚拟偏移量作为参数
  llvm::FunctionType* asm_type = llvm::FunctionType::get(
    llvm::Type::getVoidTy(ctx),
    {YuhuType::jint_type()},  // 以虚拟偏移量作为参数
    false);
  
  // 创建内联汇编，不做实际操作但可以被识别
  // 使用包含虚拟偏移量参数的空操作指令模式
  llvm::InlineAsm* marker_asm = llvm::InlineAsm::get(
    asm_type,
    "// YUHU_OFFSET_MARKER $0",  // 可识别的注释标记
    "r,~{memory}",  // 寄存器输入，影响内存
    true,            // 有副作用（防止优化）
    false,           // 不对齐堆栈
    llvm::InlineAsm::AD_ATT
  );
  
  // 用虚拟偏移量调用内联汇编
  std::vector<llvm::Value*> args;
  args.push_back(LLVMValue::jint_constant(virtual_offset));
  
  CreateCall(asm_type, marker_asm, args);
  
  // 同时在代码缓冲区的偏移映射器中记录映射
  code_buffer()->insert_offset_marker(virtual_offset);
}
```

#### 3. YuhuCodeBuffer 集成
在 `YuhuCodeBuffer` 中集成 `YuhuOffsetMapper`：

```cpp
// YuhuCodeBuffer 中的偏移映射功能
class YuhuCodeBuffer : public StackObj {
  // ... 
  YuhuOffsetMapper* _offset_mapper;  // 偏移映射器
  
 public:
  int insert_offset_marker(int virtual_offset) const {
    int actual_offset = masm().offset();
    
    // 添加映射到偏移映射器（如果可用）
    if (_offset_mapper != NULL) {
      _offset_mapper->add_mapping(virtual_offset, actual_offset);
    }
    
    return actual_offset;
  }
};
```

#### 4. 机器码扫描与映射建立
在 LLVM 生成机器码后，扫描代码以识别标记并建立虚拟-实际偏移映射：

```cpp
void YuhuBuilder::scan_and_update_offset_markers(address code_start, size_t code_size, 
                                                  YuhuOffsetMapper* mapper) {
  // 扫描生成的机器码查找偏移标记并更新映射器
  // 标记模式 (AArch64, 20 字节):
  //   mov w19, #0xBEEF             (4 字节)
  //   movk w19, #0xDEAD, lsl #16   (4 字节)
  //   mov w20, #<virtual_offset>   (4 字节)
  //   nop                          (4 字节: 0xD503201F)
  //   nop                          (4 字节: 0xD503201F)
  
  const uint32_t NOP_INSTRUCTION = 0xD503201F;
  int markers_found = 0;
  
  // 按 4 字节（指令对齐）扫描代码
  for (size_t offset = 0; offset + 20 <= code_size; offset += 4) {
    uint32_t* instructions = (uint32_t*)(code_start + offset);
    
    // 检查标记模式：
    // 1. 检查末尾的两个连续 NOP
    if (instructions[3] == NOP_INSTRUCTION && instructions[4] == NOP_INSTRUCTION) {
      // 2. 检查 magic number (0xDEADBEEF)
      uint32_t inst0 = instructions[0];
      uint32_t inst1 = instructions[1];
      uint32_t inst2 = instructions[2];
      
      // 验证 "mov w19, #0xBEEF"
      bool is_mov_w19 = ((inst0 & 0xFFE00000) == 0x52800000) && ((inst0 & 0x1F) == 19);
      uint32_t imm_low = (inst0 >> 5) & 0xFFFF;
      
      // 验证 "movk w19, #0xDEAD, lsl #16"
      bool is_movk_w19 = ((inst1 & 0xFFE00000) == 0x72A00000) && ((inst1 & 0x1F) == 19);
      uint32_t imm_high = (inst1 >> 5) & 0xFFFF;
      
      if (is_mov_w19 && is_movk_w19 && imm_low == 0xBEEF && imm_high == 0xDEAD) {
        // 3. 从 inst2 提取虚拟偏移量 (mov w20, #<virtual_offset>)
        bool is_mov_w20 = ((inst2 & 0xFFE00000) == 0x52800000) && ((inst2 & 0x1F) == 20);
        
        if (is_mov_w20) {
          int virtual_offset = (inst2 >> 5) & 0xFFFF;
          int actual_offset = offset;  // 实际偏移量
          
          // 更新映射器
          mapper->add_mapping(virtual_offset, actual_offset);
          markers_found++;
          offset += 16;  // 跳过标记的剩余部分
        }
      }
    }
  }
  
  tty->print_cr("找到 %d 个偏移标记", markers_found);
}
```
在机器码生成后，通过 `YuhuBuilder::relocate_oopmaps()` 方法重定位 OopMap：

```cpp
// 重定位 OopMap
void YuhuBuilder::relocate_oopmaps(YuhuOffsetMapper* offset_mapper, ciEnv* env) {
  if (offset_mapper != NULL && env != NULL) {
    // 获取 OopMapSet 从 DebugInformationRecorder
    OopMapSet* oopmaps = env->debug_info()->_oopmaps;
    if (oopmaps != NULL) {
      // 重定位每个 OopMap 的偏移量
      for (int i = 0; i < oopmaps->size(); i++) {
        OopMap* oopmap = oopmaps->at(i);
        if (oopmap != NULL) {
          int virtual_offset = oopmap->offset();  // 原来的虚拟偏移
          int actual_offset = offset_mapper->relocate_offset(virtual_offset);
          
          // 更新 OopMap 的偏移到实际偏移
          if (actual_offset != virtual_offset) {
            oopmap->set_offset(actual_offset);
          }
        }
      }
    }
  }
}
```

#### 5. 编译流程集成
在编译流程中自动调用扫描和重定位：

```cpp
// 在 YuhuCompiler::compile_method 中
{
  ThreadInVMfromNative tiv(JavaThread::current());
  generate_native_code(entry, function, func_name);
  
  // 机器码生成后，扫描代码查找偏移标记并更新映射器
  YuhuOffsetMapper* offset_mapper = cb.offset_mapper();
  if (offset_mapper != NULL) {
    // 1. 扫描生成的代码以查找标记并更新映射
    address code_start = entry->code_start();
    size_t code_size = entry->code_limit() - code_start;
    
    builder.scan_and_update_offset_markers(code_start, code_size, offset_mapper);
    
    // 2. 使用更新后的映射器重定位 OopMap 偏移量
    builder.relocate_oopmaps(offset_mapper, env);
  }
}
```

#### 6. OopMap 排序与适配器偏移量调整

在 OopMap 重定位完成后，需要对 OopMapSet 进行排序以确保 PC 偏移量线性递增，同时需要调整适配器区域的偏移量：

**OopMapSet 排序方法：**
```cpp
// OopMapSet::sort_by_offset() 方法通过 PC offset 对 OopMap 进行排序
// 确保 OopMapSet::find_map_at_offset 函数能够正确工作
void OopMapSet::sort_by_offset() {
  // 使用冒泡排序按 offset 升序排列 OopMaps
  int count = size();
  for (int i = 0; i < count - 1; i++) {
    for (int j = 0; j < count - 1 - i; j++) {
      OopMap* map1 = at(j);
      OopMap* map2 = at(j + 1);
      if (map1 != NULL && map2 != NULL && map1->offset() > map2->offset()) {
        // 交换 OopMap 指针
        set(j, map2);
        set(j + 1, map1);
      }
    }
  }
}
```

**适配器偏移量调整方法：**
```cpp
// YuhuBuilder::adjust_oopmaps_pc_offset 方法调整 OopMap 中的 PC 偏移量
// 由于 last_java_pc 包含适配器区域，OopMap 中的 PC 偏移量也需要加上适配器区域的大小
void YuhuBuilder::adjust_oopmaps_pc_offset(ciEnv* env, int plus_offset) {
  if (env != NULL) {
    OopMapSet* oopmaps = env->debug_info()->_oopmaps;
    if (oopmaps != NULL) {
      tty->print_cr("Yuhu: Adjusting OopMap PC offsets by +%d (adapter size)", plus_offset);
      
      for (int i = 0; i < oopmaps->size(); i++) {
        OopMap* oopmap = oopmaps->at(i);
        if (oopmap != NULL) {
          int old_offset = oopmap->offset();
          int new_offset = old_offset + plus_offset;
          oopmap->set_offset(new_offset);
          
          tty->print_cr("Yuhu: OopMap %d: offset %d -> %d", 
                        i, old_offset, new_offset);
        }
      }
    }
  }
}
```

**集成到编译流程：**
```cpp
// 在 YuhuCompiler::compile_method 中
{
  ThreadInVMfromNative tiv(JavaThread::current());
  generate_native_code(entry, function, func_name);
  
  // 机器码生成后，扫描代码查找偏移标记并更新映射器
  YuhuOffsetMapper* offset_mapper = cb.offset_mapper();
  if (offset_mapper != NULL) {
    // 1. 扫描生成的代码以查找标记并更新映射
    address code_start = entry->code_start();
    size_t code_size = entry->code_limit() - code_start;
    
    builder.scan_and_update_offset_markers(code_start, code_size, offset_mapper);
    
    // 2. 使用更新后的映射器重定位 OopMap 偏移量
    builder.relocate_oopmaps(offset_mapper, env);
    
    // 3. 对 OopMapSet 按 PC offset 进行排序
    OopMapSet* oopmaps = env->debug_info()->_oopmaps;
    if (oopmaps != NULL) {
      oopmaps->sort_by_offset();
    }
    
    // 4. 调整 OopMap PC 偏移量以包含适配器区域大小
    int adapter_size = entry->adapter_code_size();  // 适配器代码大小
    builder.adjust_oopmaps_pc_offset(env, adapter_size);
  }
}
```

#### 7. 验证机制
- **调试输出**：验证映射关系是否正确建立
- **一致性检查**：确保所有 OopMap 偏移量都得到正确更新
- **运行时验证**：通过 GC 和栈遍历验证 OopMap 的正确性

## 正确的 OopMap 使用要求

为了让 HotSpot 正常工作：

- 在每个 PC 处：
  - 线程可以在 **safepoint 被看到**，或
  - 调用 VM / 运行时，或
  - VM 可能遍历帧 (异常、去优化等)
- 必须使用 `CodeBlob::oop_map_for_return_address` 稍后将从真实 PC 计算的 **精确代码偏移量** 在 `DebugInformationRecorder` 中注册 OopMap

## 最佳实践

1. **一致的偏移量基址**：确保 OopMap 偏移量相对于安装的 nmethod 使用的相同基址记录
2. **完整覆盖**：为 VM 可能需要遍历帧的所有 safepoint 位置注册 OopMap
3. **准确的帧信息**：确保 OopMap 准确反映每个程序点上对象引用的位置
4. **Adapter 集成**：使用 adapter 代码时，在 OopMap 注册中考虑偏移量变化
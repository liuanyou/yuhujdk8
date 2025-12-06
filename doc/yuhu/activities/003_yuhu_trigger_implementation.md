# Yuhu 编译器触发机制实现（阶段 1）

## 概述

本文档描述了 Yuhu 编译器触发机制的阶段 1 实现：**调用次数 + 回边次数 + 复杂度评估**。

### 与编译策略的关系（默认 AdvancedThresholdPolicy）
- HotSpot 默认 `-XX:CompilationPolicyChoice=3`，使用 `AdvancedThresholdPolicy`。
- `AdvancedThresholdPolicy` 继承自 `SimpleThresholdPolicy`，复用了 `call_event/loop_event`；本次新增的 Yuhu 触发逻辑已放在这两个入口，因此 **在默认的 Advanced 策略下也会生效**，无需额外开关。
- 如果手动切换到 `-XX:CompilationPolicyChoice=2`（SimpleThresholdPolicy），同样会生效，因为逻辑位于基类实现。

## 实现策略

### 触发条件

方法满足以下**所有**条件时，将触发 Yuhu 编译器：

1. **Yuhu 编译器已启用**：`-XX:+UseYuhuCompiler`
2. **复杂度评估已启用**：`-XX:+YuhuUseComplexityBased`（默认启用）
3. **方法为热点方法**：
   - 调用次数 >= `Tier3InvocationThreshold` (默认 200)，**或**
   - 回边次数 >= `Tier3BackEdgeThreshold` (默认 60000)
4. **方法复杂度超过阈值**：
   - 复杂度分数 >= `YuhuComplexityThreshold` (默认 5000)

### 复杂度计算公式

```cpp
complexity = code_size * (num_blocks + 1) * (has_loops ? 2 : 1)
```

**参数说明**：
- `code_size`：方法的字节码大小（从 `method->code_size()` 获取）
- `num_blocks`：基本块数量（从 `MethodData->num_blocks()` 获取，如果不可用则默认为 1）
- `has_loops`：方法是否包含循环（从 `method->has_loops()` 获取）

**复杂度因子**：
- 字节码大小：代码越多，复杂度越高
- 基本块数量：控制流越复杂，基本块越多
- 循环：包含循环的方法复杂度翻倍（因为 LLVM 的循环优化很强）

## 代码实现

### 1. 命令行选项（`globals.hpp`）

```cpp
product(bool, UseYuhuCompiler, false,
        "Use Yuhu compiler for methods that meet complexity threshold")

product(intx, YuhuComplexityThreshold, 5000,
        "Complexity score threshold for Yuhu compilation. "
        "Complexity = code_size * (num_blocks + 1) * (has_loops ? 2 : 1)")

product(bool, YuhuUseComplexityBased, true,
        "Use complexity-based selection for Yuhu compiler")
```

### 2. 复杂度评估函数（`simpleThresholdPolicy.cpp`）

```cpp
int SimpleThresholdPolicy::calculate_complexity_score(Method* method) {
  if (method == NULL) {
    return 0;
  }
  
  // Get code size (bytecode size)
  int code_size = method->code_size();
  if (code_size == 0) {
    return 0;
  }
  
  // Get number of blocks and loops from MethodData if available
  int num_blocks = 1;  // Default to 1 if not available
  int num_loops = 0;
  bool has_loops = method->has_loops();
  
  MethodData* mdo = method->method_data();
  if (mdo != NULL) {
    num_blocks = mdo->num_blocks();
    num_loops = mdo->num_loops();
    // If num_blocks is 0, it means it hasn't been computed yet, use default
    if (num_blocks == 0) {
      num_blocks = 1;
    }
  }
  
  // Calculate complexity score
  int complexity = code_size * (num_blocks + 1);
  if (has_loops) {
    complexity *= 2;
  }
  
  return complexity;
}
```

### 3. Yuhu 编译决策函数（`simpleThresholdPolicy.cpp`）

```cpp
bool SimpleThresholdPolicy::should_compile_with_yuhu(Method* method) {
  if (method == NULL) {
    return false;
  }
  
  // Check if Yuhu compiler is enabled
  if (!UseYuhuCompiler) {
    return false;
  }
  
  // Check if complexity-based selection is enabled
  if (!YuhuUseComplexityBased) {
    return false;
  }
  
  // Check if method is hot (meets basic invocation/backedge thresholds)
  int invocation_count = method->invocation_count();
  int backedge_count = method->backedge_count();
  
  // Method must meet at least one of the basic thresholds
  bool is_hot = (invocation_count >= Tier3InvocationThreshold) ||
                (backedge_count >= Tier3BackEdgeThreshold);
  
  if (!is_hot) {
    return false;
  }
  
  // Calculate complexity score
  int complexity = calculate_complexity_score(method);
  
  // Check if complexity exceeds threshold
  if (complexity >= YuhuComplexityThreshold) {
    if (PrintTieredEvents) {
      ResourceMark rm;
      tty->print_cr("Yuhu compilation candidate: %s complexity=%d (threshold=%d) invocation=%d backedge=%d",
                    method->name_and_sig_as_C_string(),
                    complexity, YuhuComplexityThreshold,
                    invocation_count, backedge_count);
    }
    return true;
  }
  
  return false;
}
```

### 4. 集成到编译策略（`simpleThresholdPolicy.cpp`）

#### 调用事件（Call Event）

```cpp
CompLevel SimpleThresholdPolicy::call_event(Method* method, CompLevel cur_level) {
  // Check if method should be compiled with Yuhu compiler
  if (should_compile_with_yuhu(method)) {
    // Use Yuhu optimized compilation level
    return CompLevel_yuhu_optimized;
  }
  
  // ... 原有的 C1/C2 选择逻辑 ...
}
```

#### 回边事件（Loop Event / OSR）

```cpp
CompLevel SimpleThresholdPolicy::loop_event(Method* method, CompLevel cur_level) {
  // Check if method should be compiled with Yuhu compiler
  if (should_compile_with_yuhu(method)) {
    // Use Yuhu optimized compilation level for OSR as well
    return CompLevel_yuhu_optimized;
  }
  
  // ... 原有的 C1/C2 选择逻辑 ...
}
```

## 使用示例

### 启用 Yuhu 编译器

```bash
# 基本使用（使用默认阈值）
java -XX:+UseYuhuCompiler YourClass

# 自定义复杂度阈值
java -XX:+UseYuhuCompiler -XX:YuhuComplexityThreshold=10000 YourClass

# 启用详细日志
java -XX:+UseYuhuCompiler -XX:+PrintTieredEvents YourClass
```

### 禁用复杂度评估

```bash
# 如果只想使用调用次数+回边次数（未来实现）
java -XX:+UseYuhuCompiler -XX:-YuhuUseComplexityBased YourClass
```

## 复杂度计算示例

### 示例 1：简单方法

```java
public int add(int a, int b) {
    return a + b;
}
```

**计算**：
- `code_size` = 5（字节码：iload_1, iload_2, iadd, ireturn, return）
- `num_blocks` = 1（单个基本块）
- `has_loops` = false

**复杂度** = 5 * (1 + 1) * 1 = **10**

**结果**：不会触发 Yuhu 编译（10 < 5000）

### 示例 2：包含循环的方法

```java
public int sum(int[] array) {
    int sum = 0;
    for (int i = 0; i < array.length; i++) {
        sum += array[i];
    }
    return sum;
}
```

**计算**：
- `code_size` = 30（假设）
- `num_blocks` = 3（循环前、循环体、循环后）
- `has_loops` = true

**复杂度** = 30 * (3 + 1) * 2 = **240**

**结果**：不会触发 Yuhu 编译（240 < 5000）

### 示例 3：复杂方法（矩阵乘法）

```java
public void matrixMultiply(int[][] a, int[][] b, int[][] c, int n) {
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

**计算**：
- `code_size` = 150（假设）
- `num_blocks` = 10（多层循环产生多个基本块）
- `has_loops` = true

**复杂度** = 150 * (10 + 1) * 2 = **3300**

**结果**：如果调用次数 >= 200 或回边次数 >= 60000，会触发 Yuhu 编译（3300 < 5000，但接近阈值）

### 示例 4：非常复杂的方法

```java
public void complexAlgorithm(int[] data) {
    // 大量代码，多个循环，复杂控制流
    // code_size = 500
    // num_blocks = 20
    // has_loops = true
}
```

**计算**：
- `code_size` = 500
- `num_blocks` = 20
- `has_loops` = true

**复杂度** = 500 * (20 + 1) * 2 = **21000**

**结果**：如果调用次数 >= 200 或回边次数 >= 60000，会触发 Yuhu 编译（21000 > 5000）

## 调优建议

### 1. 调整复杂度阈值

**降低阈值**（更多方法使用 Yuhu）：
```bash
java -XX:+UseYuhuCompiler -XX:YuhuComplexityThreshold=2000 YourClass
```

**提高阈值**（只有非常复杂的方法使用 Yuhu）：
```bash
java -XX:+UseYuhuCompiler -XX:YuhuComplexityThreshold=10000 YourClass
```

### 2. 监控 Yuhu 编译

启用详细日志查看哪些方法被选中：
```bash
java -XX:+UseYuhuCompiler -XX:+PrintTieredEvents YourClass
```

**输出示例**：
```
Yuhu compilation candidate: com.example.Matrix.multiply complexity=21000 (threshold=5000) invocation=500 backedge=0
```

### 3. 性能测试

对比使用和不使用 Yuhu 编译器的性能：
```bash
# 不使用 Yuhu
java YourClass

# 使用 Yuhu
java -XX:+UseYuhuCompiler YourClass
```

## 限制和注意事项

### 1. MethodData 的可用性

- `num_blocks` 和 `num_loops` 只在方法被 C1 编译后才会在 MethodData 中设置
- 如果 MethodData 不存在或这些值未设置，使用默认值：
  - `num_blocks` = 1
  - `num_loops` = 0

**影响**：
- 首次编译时，复杂度计算可能不够准确
- 但对于热点方法，通常已经有 MethodData 了

### 2. 复杂度计算的简化

当前实现使用简化的复杂度模型：
- 不考虑循环嵌套深度
- 不考虑方法调用深度
- 不考虑数据类型复杂度

**未来改进**：
- 可以添加更精细的复杂度评估
- 可以考虑循环嵌套深度
- 可以考虑方法调用图复杂度

### 3. 与 C1/C2 的竞争

- Yuhu 编译器的选择在 C1/C2 选择之前进行
- 如果方法满足 Yuhu 条件，将优先使用 Yuhu
- 如果 Yuhu 编译失败，不会自动回退到 C2（需要未来实现）

## 未来改进方向

### 阶段 2：多指标综合评估

- 添加 CPU 时间采样
- 添加内存分配率
- 添加编译收益预测

### 阶段 3：自适应触发

- 根据 Yuhu 编译器负载动态调整阈值
- 根据 C2 编译器负载调整选择策略
- 根据代码缓存使用率调整

### 阶段 4：LLVM 优化潜力评估

- 评估方法的向量化潜力
- 评估方法的循环优化潜力
- 评估方法的内联潜力

## 相关文件

- `hotspot/src/share/vm/runtime/simpleThresholdPolicy.cpp` - 主要实现
- `hotspot/src/share/vm/runtime/simpleThresholdPolicy.hpp` - 接口定义
- `hotspot/src/share/vm/runtime/globals.hpp` - 命令行选项定义
- `hotspot/src/share/vm/utilities/globalDefinitions.hpp` - CompLevel 定义

## 参考文档

- [C1/C2 编译器选择过程](C1_C2_COMPILATION_SELECTION.md)
- [LLVM 优势与 Yuhu 触发条件设计](LLVM_ADVANTAGES_AND_YUHU_TRIGGER.md)
- [Yuhu 编译器触发机制](YUHU_COMPILATION_TRIGGER.md)


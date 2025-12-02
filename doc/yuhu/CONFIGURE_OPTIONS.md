# YUHU 编译器配置选项

## 概述

YUHU 编译器通过 configure 脚本的选项来控制启用和配置。本文档说明如何使用这些配置选项。

## 配置选项

### --enable-yuhu-compiler

启用或禁用 YUHU 编译器。

**用法**:
```bash
--enable-yuhu-compiler=yes   # 启用 YUHU 编译器
--enable-yuhu-compiler=no    # 禁用 YUHU 编译器（默认）
```

**说明**:
- 默认情况下，YUHU 编译器是禁用的
- 启用后，需要提供 LLVM 的路径配置（见下文）
- 如果启用但未提供 LLVM 路径，configure 会报错

### --with-llvm

指定 LLVM 的安装前缀路径。

**用法**:
```bash
--with-llvm=/opt/homebrew/Cellar/llvm/20.1.5
```

**说明**:
- 这会自动设置：
  - `LLVM_INCLUDE_PATH` = `$with_llvm/include`
  - `LLVM_LIB_PATH` = `$with_llvm/lib`
- 可以与 `--with-llvm-include` 和 `--with-llvm-lib` 一起使用来覆盖

### --with-llvm-include

指定 LLVM 头文件目录。

**用法**:
```bash
--with-llvm-include=/opt/homebrew/Cellar/llvm/20.1.5/include
```

**说明**:
- 如果使用 `--with-llvm`，这个选项可以覆盖 include 路径
- 如果未使用 `--with-llvm`，必须同时指定 `--with-llvm-include` 和 `--with-llvm-lib`

### --with-llvm-lib

指定 LLVM 库文件目录。

**用法**:
```bash
--with-llvm-lib=/opt/homebrew/Cellar/llvm/20.1.5/lib
```

**说明**:
- 如果使用 `--with-llvm`，这个选项可以覆盖 lib 路径
- 如果未使用 `--with-llvm`，必须同时指定 `--with-llvm-include` 和 `--with-llvm-lib`

## 配置示例

### 示例 1: 使用 --with-llvm（推荐）

```bash
./configure \
  --enable-yuhu-compiler=yes \
  --with-llvm=/opt/homebrew/Cellar/llvm/20.1.5
```

这会自动设置 include 和 lib 路径。

### 示例 2: 分别指定 include 和 lib 路径

```bash
./configure \
  --enable-yuhu-compiler=yes \
  --with-llvm-include=/opt/homebrew/Cellar/llvm/20.1.5/include \
  --with-llvm-lib=/opt/homebrew/Cellar/llvm/20.1.5/lib
```

### 示例 3: 使用 --with-llvm 并覆盖 include 路径

```bash
./configure \
  --enable-yuhu-compiler=yes \
  --with-llvm=/opt/homebrew/Cellar/llvm/20.1.5 \
  --with-llvm-include=/custom/path/to/llvm/include
```

### 示例 4: 禁用 YUHU 编译器（默认）

```bash
./configure
# 或者显式禁用
./configure --enable-yuhu-compiler=no
```

## 配置验证

配置完成后，可以检查生成的 `hotspot-spec.gmk` 文件来验证配置：

```bash
grep -E "USE_YUHU_COMPILER|LLVM_INCLUDE_PATH|LLVM_LIB_PATH" build/hotspot-spec.gmk
```

应该看到：
```makefile
USE_YUHU_COMPILER := true
LLVM_INCLUDE_PATH := /opt/homebrew/Cellar/llvm/20.1.5/include
LLVM_LIB_PATH := /opt/homebrew/Cellar/llvm/20.1.5/lib
```

## 错误处理

### 错误 1: 启用 YUHU 但未提供 LLVM 路径

**错误信息**:
```
Either specify --with-llvm or both --with-llvm-include and --with-llvm-lib when --enable-yuhu-compiler is enabled
```

**解决方案**:
- 使用 `--with-llvm=PREFIX` 指定 LLVM 安装路径
- 或同时使用 `--with-llvm-include` 和 `--with-llvm-lib`

### 错误 2: 只指定了 include 或 lib 之一

**错误信息**:
```
Either specify --with-llvm or both --with-llvm-include and --with-llvm-lib when --enable-yuhu-compiler is enabled
```

**解决方案**:
- 必须同时指定 `--with-llvm-include` 和 `--with-llvm-lib`
- 或使用 `--with-llvm` 自动设置两者

## 构建系统集成

配置变量会传递给构建系统：

1. **hotspot-spec.gmk**: 包含配置变量
   ```makefile
   USE_YUHU_COMPILER := true
   LLVM_INCLUDE_PATH := /path/to/llvm/include
   LLVM_LIB_PATH := /path/to/llvm/lib
   ```

2. **yuhu.make**: 使用这些变量
   ```makefile
   ifeq ($(USE_YUHU_COMPILER), true)
     CFLAGS += -DYUHU
     CFLAGS += -I$(LLVM_INCLUDE_PATH)
     LDFLAGS += -L$(LLVM_LIB_PATH)
   endif
   ```

## 与 llvm-config 的关系

`yuhu.make` 会优先使用 configure 设置的路径，但如果 `llvm-config` 可用，也会尝试使用它来获取额外的编译标志（如 `-stdlib=libc++`）。

**优先级**:
1. Configure 设置的路径（`LLVM_INCLUDE_PATH`, `LLVM_LIB_PATH`）
2. `llvm-config` 的额外标志（如果可用）

## 完整配置示例

```bash
# 配置并构建带 YUHU 编译器的 JDK
./configure \
  --enable-yuhu-compiler=yes \
  --with-llvm=/opt/homebrew/Cellar/llvm/20.1.5 \
  --with-debug-level=fastdebug

# 构建
make
```

## 相关文档

- [BUILD_INSTRUCTIONS.md](BUILD_INSTRUCTIONS.md) - 构建说明
- [LLVM_20_CONFIG.md](LLVM_20_CONFIG.md) - LLVM 20.1.5 配置
- [COEXISTENCE_IMPLEMENTATION.md](COEXISTENCE_IMPLEMENTATION.md) - YUHU 与 C1/C2 并存


# LLVM 20.1.5 配置指南

## 概述

本文档专门针对 LLVM 20.1.5 的配置说明。LLVM 20.1.5 是一个现代版本，需要 C++17 支持。

## 已验证的配置

- **LLVM 版本**: 20.1.5
- **安装方式**: Homebrew on macOS (Apple Silicon)
- **路径**: `/opt/homebrew/Cellar/llvm/20.1.5/`
- **C++ 标准**: C++17

## 自动配置（推荐）

`yuhu.make` 已配置为使用 `llvm-config` 自动检测路径：

```makefile
LLVM_CONFIG ?= llvm-config
LLVM_INCLUDE := $(shell $(LLVM_CONFIG) --includedir)
LLVM_LIB := $(shell $(LLVM_CONFIG) --libdir)
LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags)
LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags)
LLVM_LIBS := $(shell $(LLVM_CONFIG) --libs core executionengine jit native)
```

### 验证 llvm-config

```bash
# 检查版本
/opt/homebrew/opt/llvm/bin/llvm-config --version
# 输出: 20.1.5

# 检查路径
/opt/homebrew/opt/llvm/bin/llvm-config --includedir
# 输出: /opt/homebrew/Cellar/llvm/20.1.5/include

/opt/homebrew/opt/llvm/bin/llvm-config --libdir
# 输出: /opt/homebrew/Cellar/llvm/20.1.5/lib
```

### 设置 PATH 或 LLVM_CONFIG

如果 `llvm-config` 不在 PATH 中：

```bash
# 方法 1: 添加到 PATH
export PATH=/opt/homebrew/opt/llvm/bin:$PATH

# 方法 2: 设置 LLVM_CONFIG 环境变量
export LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config
```

## 手动配置

如果自动配置失败，可以手动配置：

```makefile
# 在 yuhu.make 中取消注释并设置：
LLVM_INCLUDE = -I/opt/homebrew/Cellar/llvm/20.1.5/include
LLVM_LIB = -L/opt/homebrew/Cellar/llvm/20.1.5/lib
CFLAGS += $(LLVM_INCLUDE) -std=c++17 -stdlib=libc++
LDFLAGS += $(LLVM_LIB) -lLLVM
```

## C++ 标准兼容性

**重要**: JDK 8 使用 **C++11**，而 `llvm-config --cxxflags` 会输出 `-std=c++17`。

**解决方案**: `yuhu.make` 已配置为过滤掉 LLVM 的 C++ 标准标志：

```makefile
# Filter out C++ standard flags from LLVM
LLVM_CXXFLAGS := $(filter-out -std=c++%,$(LLVM_CXXFLAGS_RAW))
```

这样：
- JDK 使用 C++11（由 JDK 构建系统控制）
- LLVM 头文件和库与 C++11 兼容
- 不会产生 C++ 标准冲突

**注意**: LLVM 20.1.5 的头文件和库完全兼容 C++11，虽然 LLVM 本身可以用 C++17 编译。

## 编译标志

LLVM 20.1.5 的编译标志包括：

```bash
llvm-config --cxxflags
# -I/opt/homebrew/Cellar/llvm/20.1.5/include
# -std=c++17
# -stdlib=libc++
# -fno-exceptions
# -funwind-tables
# -D__STDC_CONSTANT_MACROS
# -D__STDC_FORMAT_MACROS
# -D__STDC_LIMIT_MACROS
```

## 链接标志

```bash
llvm-config --ldflags
# -L/opt/homebrew/Cellar/llvm/20.1.5/lib
# -Wl,-search_paths_first
# -Wl,-headerpad_max_install_names
```

## 需要的库

```bash
llvm-config --libs core executionengine jit native
# 输出所需的 LLVM 库列表
```

## 常见问题

### 1. C++ 标准冲突

**错误**: `error: conflicting declaration` 或 C++ 标准相关的错误

**原因**: `llvm-config --cxxflags` 输出 `-std=c++17`，但 JDK 使用 C++11

**解决**: 
- `yuhu.make` 已自动过滤掉 `-std=c++17`
- 确保编译时使用 JDK 的 C++11 标准
- LLVM 20.1.5 的头文件和库与 C++11 兼容

### 2. 找不到头文件

**错误**: `fatal error: 'llvm/IR/Verifier.h' file not found`

**解决**: 
- 检查 `llvm-config --includedir` 输出
- 确保路径正确添加到 CFLAGS

### 3. 链接错误

**错误**: `undefined reference to 'llvm::...'`

**解决**:
- 检查 `llvm-config --libdir` 输出
- 确保库路径和库文件都正确链接

## 测试构建

```bash
# 1. 验证 LLVM 配置
llvm-config --version
llvm-config --includedir
llvm-config --libdir

# 2. 构建 Yuhu
cd hotspot/make
make TYPE=YUHU

# 3. 检查编译输出
# 应该看到 LLVM 相关的编译标志
```

## 相关文档

- [BUILD_INSTRUCTIONS.md](BUILD_INSTRUCTIONS.md) - 通用构建说明
- [COEXISTENCE_IMPLEMENTATION.md](COEXISTENCE_IMPLEMENTATION.md) - YUHU 与 C1/C2 并存


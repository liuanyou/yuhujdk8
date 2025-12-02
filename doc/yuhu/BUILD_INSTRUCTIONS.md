# Yuhu 编译器构建说明

## 前提条件

Yuhu 编译器基于 LLVM，因此需要：

1. **LLVM 开发库**（版本 3.0+，推荐 **LLVM 20** 或 LLVM 16+）
2. **LLVM 头文件路径配置**
3. **LLVM 库文件路径配置**
4. **C++17 支持**（仅 Yuhu 的 .cpp 文件需要 C++17，其他文件使用 C++11）

**重要提示**：
- **LLVM 16.0+ 需要 C++17**（头文件）
- **推荐使用 LLVM 20**（当前稳定版本，更好的 macOS 支持）
- **解决方案**：使用前向声明隔离，只有 Yuhu 的 .cpp 文件使用 C++17
- `compileBroker.cpp` 等文件仍使用 C++11（通过前向声明避免包含 LLVM 头文件）

## 安装 LLVM

### macOS (使用 Homebrew)

**推荐：安装 LLVM 20（当前稳定版本）**

```bash
brew install llvm
```

这会安装 LLVM 20.1.5（或更新版本），安装位置：
- **Apple Silicon**: `/opt/homebrew/opt/llvm/include` 和 `/opt/homebrew/opt/llvm/lib`
- **Intel**: `/usr/local/opt/llvm/include` 和 `/usr/local/opt/llvm/lib`

**验证安装**:
```bash
/opt/homebrew/opt/llvm/bin/llvm-config --version
# 输出: 20.1.5 (或类似版本)
```

**注意**：`llvm` 是 "keg-only"，不会自动链接到 PATH。需要：
1. 使用完整路径：`/opt/homebrew/opt/llvm/bin/llvm-config`
2. 或者在 configure 时指定路径：`--with-llvm=/opt/homebrew/opt/llvm`

**替代方案：安装 LLVM 16**

如果需要使用 LLVM 16：
```bash
brew install llvm@16
```

安装位置：
- **Apple Silicon**: `/opt/homebrew/opt/llvm@16/include` 和 `/opt/homebrew/opt/llvm@16/lib`
- **Intel**: `/usr/local/opt/llvm@16/include` 和 `/usr/local/opt/llvm@16/lib`

### Linux

```bash
# Ubuntu/Debian
sudo apt-get install llvm-dev

# Fedora/RHEL
sudo yum install llvm-devel
```

### 从源码编译

参考 [LLVM 官方文档](https://llvm.org/docs/GettingStarted.html)

## 配置构建系统

### 1. 自动配置（推荐）

`yuhu.make` 已配置为使用 `llvm-config` 自动检测 LLVM 路径和版本。

**使用 LLVM 20**（推荐）:
```bash
# 设置 llvm-config 路径（LLVM 是 keg-only，不在 PATH 中）
export LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config

# 验证
$LLVM_CONFIG --version
# 输出: 20.1.5
```

**或者使用完整路径在 configure 时指定**:
```bash
./configure \
  --enable-yuhu-compiler=yes \
  --with-llvm=/opt/homebrew/opt/llvm \
  ...
```

**如果使用 LLVM 16**:
```bash
export LLVM_CONFIG=/opt/homebrew/opt/llvm@16/bin/llvm-config
# 或
./configure \
  --enable-yuhu-compiler=yes \
  --with-llvm=/opt/homebrew/opt/llvm@16 \
  ...
```

### 2. 手动配置（如果自动配置失败）

编辑 `hotspot/make/bsd/makefiles/yuhu.make`，取消注释并设置路径：

```makefile
# 示例：Homebrew on macOS (Apple Silicon, LLVM 20 - 推荐)
LLVM_INCLUDE = -I/opt/homebrew/opt/llvm/include
LLVM_LIB = -L/opt/homebrew/opt/llvm/lib
CFLAGS += $(LLVM_INCLUDE) -stdlib=libc++
LDFLAGS += $(LLVM_LIB) -lLLVM
# 注意：LLVM 20 需要 C++17，但通过前向声明隔离，只有 Yuhu .cpp 文件使用 C++17

# 示例：Homebrew on macOS (Intel)
LLVM_INCLUDE = -I/usr/local/Cellar/llvm/VERSION/include
LLVM_LIB = -L/usr/local/Cellar/llvm/VERSION/lib

# 示例：系统安装
LLVM_INCLUDE = -I/usr/include
LLVM_LIB = -L/usr/lib
```

### 3. LLVM 版本兼容性

- **LLVM 3.8+**: 使用 `llvm/IR/Verifier.h`（默认）
- **LLVM < 3.8**: 需要定义 `-DYUHU_USE_OLD_LLVM_VERIFIER`（不推荐，已过时）

**推荐版本**:
- **LLVM 15 或更早**: 完全兼容 C++11，**强烈推荐**
- **LLVM 16.0+**: 需要 C++17，与 JDK 8 的 C++11 不兼容，**不推荐**

**版本选择说明**:
- LLVM 16.0 开始默认使用 C++17，头文件需要 C++17 支持
- JDK 8 使用 C++11 标准
- 使用 LLVM 15 可以避免 C++ 标准不兼容问题

### 4. C++ 标准兼容性

**重要**: JDK 8 使用 **C++11** 标准。

**LLVM 15 及更早版本**:
- 完全支持 C++11
- 头文件和库与 JDK 8 的 C++11 完全兼容
- 推荐使用，无需额外配置

**LLVM 16.0+**:
- 需要 C++17
- 头文件使用 C++17 特性（如 `namespace a::b`、`if constexpr` 等）
- 与 JDK 8 的 C++11 不兼容
- 如果必须使用，需要为 Yuhu 相关文件单独设置 `-std=c++17`（不推荐）

## 构建 Yuhu 版本

### 方法 1: 使用 configure + make（推荐）

```bash
# 1. 配置（启用 YUHU 编译器，使用 LLVM 15）
./configure \
  --enable-yuhu-compiler=yes \
  --with-llvm=/opt/homebrew/opt/llvm@15

# 2. 构建
make
```

### 方法 2: 直接使用 Makefile（不使用 configure）

```bash
cd hotspot/make
make TYPE=YUHU
```

**注意**: 此方法需要手动配置 `yuhu.make` 中的 LLVM 路径。

## 验证 LLVM 安装

### 检查 LLVM 版本

```bash
# 使用 LLVM 15（推荐）
/opt/homebrew/opt/llvm@15/bin/llvm-config --version
# 输出: 15.0.7 (或类似版本)
```

### 检查头文件路径

```bash
/opt/homebrew/opt/llvm@15/bin/llvm-config --includedir
# 输出: /opt/homebrew/opt/llvm@15/include (或您的路径)
```

### 检查库文件路径

```bash
/opt/homebrew/opt/llvm@15/bin/llvm-config --libdir
# 输出: /opt/homebrew/opt/llvm@15/lib (或您的路径)
```

### 检查编译标志

```bash
/opt/homebrew/opt/llvm@15/bin/llvm-config --cxxflags
# 输出: -I/opt/homebrew/opt/llvm@15/include -std=c++14 -stdlib=libc++ ...
# 注意：LLVM 15 使用 C++14，但头文件兼容 C++11
```

### 检查链接标志

```bash
/opt/homebrew/opt/llvm@15/bin/llvm-config --ldflags
# 输出: -L/opt/homebrew/opt/llvm@15/lib ...
```

## 常见问题

### 1. 找不到 LLVM 头文件

**错误**:
```
fatal error: 'llvm/IR/Verifier.h' file not found
```

**解决方案**:
- 确认 LLVM 已安装
- 检查 `yuhu.make` 中的 `LLVM_INCLUDE` 路径
- 使用 `llvm-config --includedir` 获取正确路径

### 2. 找不到 LLVM 库

**错误**:
```
undefined reference to `llvm::...`
```

**解决方案**:
- 检查 `yuhu.make` 中的 `LLVM_LIB` 路径
- 确认已链接 LLVM 库（`-lLLVM`）
- 使用 `llvm-config --libdir` 获取正确路径

### 3. LLVM 版本不兼容

**错误**:
```
'verifyFunction' was not declared
```

**解决方案**:
- LLVM 3.8+ 使用 `llvm/IR/Verifier.h`（默认）
- LLVM < 3.8 使用 `llvm/Analysis/Verifier.h`（已过时）
- LLVM 15 完全兼容，使用默认配置

### 4. C++ 标准冲突

**错误**: `error: conflicting declaration` 或 C++ 标准相关的错误

**原因**: `llvm-config --cxxflags` 输出 `-std=c++17`，但 JDK 使用 C++11

**解决方案**:
- **推荐**: 使用 LLVM 15 或更早版本，完全支持 C++11
- 如果使用 LLVM 16+，`yuhu.make` 会过滤掉 LLVM 的 `-std=c++17` 标志，但头文件仍需要 C++17
- JDK 的构建系统使用 C++11

**验证**: 
- 检查编译命令，确保使用 C++11
- 如果使用 LLVM 16+，会看到 C++17 相关的编译错误

### 5. 找不到 llvm-config

**错误**:
```
llvm-config: command not found
```

**解决方案**:
- 确保 LLVM 已正确安装
- 将 `llvm-config` 路径添加到 PATH
- 或在 `yuhu.make` 中设置 `LLVM_CONFIG=/path/to/llvm-config`
- 或使用手动配置（见上面的手动配置部分）

## 暂时不编译 Yuhu

如果现在不想编译 Yuhu（例如还没有安装 LLVM），可以：

1. **不定义 YUHU 宏**：所有 Yuhu 代码都会被条件编译排除
2. **使用默认构建**：构建系统会使用 C1/C2 编译器

Yuhu 代码已经使用 `#ifdef YUHU` 保护，不会影响正常构建。

## 快速开始（LLVM 15）

如果您已经安装了 LLVM 15（通过 Homebrew），可以快速开始：

```bash
# 1. 安装 LLVM 15（如果还没有安装）
brew install llvm@15

# 2. 验证 LLVM 安装
/opt/homebrew/opt/llvm@15/bin/llvm-config --version
# 应该输出: 15.0.7

# 3. 配置构建（使用 LLVM 15）
./configure \
  --enable-yuhu-compiler=yes \
  --with-llvm=/opt/homebrew/opt/llvm@15 \
  --with-freetype-include=/opt/homebrew/Cellar/freetype/2.13.3/include/freetype2 \
  --with-freetype-lib=/opt/homebrew/Cellar/freetype/2.13.3/lib \
  --with-debug-level=slowdebug

# 4. 构建
make
```

`yuhu.make` 已配置为自动使用 `llvm-config` 检测路径，通常不需要手动配置。

## 下一步

1. ✅ 安装 LLVM 20（推荐）或 LLVM 16
2. ✅ 验证 `llvm-config` 可用
3. 配置并构建 Yuhu 版本（使用 `--enable-yuhu-compiler=yes --with-llvm=...`）
4. 如果遇到问题，参考上面的常见问题部分

## LLVM 版本选择建议

| 版本 | 推荐度 | 原因 |
|------|--------|------|
| **LLVM 20** | ⭐⭐⭐⭐⭐ | 当前稳定版本，最佳 macOS 支持，性能最好 |
| LLVM 16 | ⭐⭐⭐⭐ | 稳定版本，功能完整 |
| LLVM 15 | ⭐⭐ | 在 macOS Sonoma 上可能无法安装 |

**结论**：推荐使用 **LLVM 20**，因为：
- 当前稳定版本，bug 修复最多
- 对 macOS 14.2.1（Sonoma）支持最好
- 性能优化更好
- API 兼容性良好（Yuhu 使用的 API 都支持）


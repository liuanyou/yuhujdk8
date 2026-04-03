#!/bin/bash

# Yuhu 编译器测试脚本
# 测试矩阵乘法方法

# 获取脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# 项目根目录（假设 test_yuhu 在项目根目录下）
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# 查找编译好的 JDK 路径
# 尝试多个可能的路径
JDK_BIN=""
for path in \
    "$PROJECT_ROOT/build/macosx-aarch64-normal-server-slowdebug/images/j2sdk-image/bin" \
    "$PROJECT_ROOT/build/macosx-aarch64-normal-server-slowdebug/images/j2sdk-bundle/jdk1.8.0.jdk/Contents/Home/bin" \
    "$PROJECT_ROOT/build/macosx-aarch64-normal-server-slowdebug/jdk/bin"
do
    if [ -f "$path/java" ] && [ -f "$path/javac" ]; then
        JDK_BIN="$path"
        break
    fi
done

if [ -z "$JDK_BIN" ]; then
    echo "错误: 找不到编译好的 JDK！"
    echo "请先编译 JDK，或手动设置 JDK_BIN 变量"
    echo ""
    echo "尝试的路径:"
    echo "  $PROJECT_ROOT/build/macosx-aarch64-normal-server-slowdebug/images/j2sdk-image/bin"
    echo "  $PROJECT_ROOT/build/macosx-aarch64-normal-server-slowdebug/images/j2sdk-bundle/jdk1.8.0.jdk/Contents/Home/bin"
    echo "  $PROJECT_ROOT/build/macosx-aarch64-normal-server-slowdebug/jdk/bin"
    exit 1
fi

JAVA="$JDK_BIN/java"
JAVAC="$JDK_BIN/javac"

echo "=== 使用编译好的 JDK ==="
echo "JDK 路径: $JDK_BIN"
echo "Java: $JAVA"
echo "Javac: $JAVAC"
echo ""

# 检查文件是否存在
if [ ! -f "$JAVA" ] || [ ! -f "$JAVAC" ]; then
    echo "错误: Java 或 Javac 不存在！"
    exit 1
fi

echo "=== 编译 Java 文件 ==="
cd "$SCRIPT_DIR"
"$JAVAC" com/example/Matrix.java

if [ $? -ne 0 ]; then
    echo "编译失败！"
    exit 1
fi

echo ""
echo "=== 测试 1: 强制使用 Yuhu 编译器 ==="
echo "命令: $JAVA -XX:+UseYuhuCompiler -XX:+YuhuTraceInstalls -XX:CompileCommand=yuhuonly,com/example/Matrix.multiply com.example.Matrix"
echo ""

"$JAVA" -XX:+UseYuhuCompiler \
     -XX:+YuhuTraceInstalls \
     -XX:TieredStopAtLevel=6 \
     -XX:CompileCommand=yuhuonly,com/example/Matrix.multiply \
     -XX:CompileCommand=exclude,*.* \
     -XX:CompileCommand=compileonly,com/example/Matrix.multiply \
     com.example.Matrix 2>&1 | grep -E "(Register method|successfully)"

echo ""
echo "=== 提示 ==="
echo "如果看到 'Register method *** successfully'，说明方法被 Yuhu 编译了"
echo ""
echo "=== 完整输出（不筛选）==="
echo "运行完整命令查看所有输出:"
echo "  $JAVA -XX:+UseYuhuCompiler \\
                   -XX:+YuhuTraceInstalls \\
                   -XX:TieredStopAtLevel=6 \\
                   -XX:CompileCommand=yuhuonly,com/example/Matrix.multiply \\
                   -XX:CompileCommand=exclude,\*.\* \\
                   -XX:CompileCommand=compileonly,com/example/Matrix.multiply \\
                   com.example.Matrix"


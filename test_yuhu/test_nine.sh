#!/bin/bash

# Yuhu 编译器简单测试脚本
# 用于测试包含方法调用、条件判断、循环的简单方法

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

# 编译测试类
echo "编译测试类..."
cd "$SCRIPT_DIR"
"$JAVAC" com/example/NineParameterTest.java

echo ""
echo "========================================="
echo "测试 1: 只编译 testNineParameters"
echo "========================================="
"$JAVA" \
  -XX:+UseYuhuCompiler \
  -XX:-YuhuUseComplexityBased \
  -XX:CompileCommand=yuhuonly,com/example/NineParameterTest.testNineParameters \
  -XX:TieredStopAtLevel=6 \
  com.example.NineParameterTest

echo ""
echo "========================================="
echo "测试 2: 只编译 testNineParametersStatic"
echo "========================================="
"$JAVA" \
  -XX:+UseYuhuCompiler \
  -XX:-YuhuUseComplexityBased \
  -XX:CompileCommand=yuhuonly,com/example/NineParameterTest.testNineParametersStatic \
  -XX:TieredStopAtLevel=6 \
  com.example.NineParameterTest

echo ""
echo "========================================="
echo "测试 3: 只编译 testNineParamsCallee"
echo "========================================="
"$JAVA" \
  -XX:+UseYuhuCompiler \
  -XX:-YuhuUseComplexityBased \
  -XX:CompileCommand=yuhuonly,com/example/NineParameterTest.testNineParamsCallee \
  -XX:YuhuCompileOnlyOf=com.example.NineParameterTest::testNineParamsCallee \
  -XX:TieredStopAtLevel=6 \
  com.example.NineParameterTest

echo ""
echo "========================================="
echo "测试 4: 编译所有测试方法"
echo "========================================="
"$JAVA" \
  -XX:+UseYuhuCompiler \
  -XX:CompileCommand=yuhuonly,com/example/NineParameterTest.* \
  -XX:TieredStopAtLevel=6 \
  com.example.NineParameterTest

echo ""
echo "测试完成！"
echo ""
echo "如果有错误，请检查:"
echo "  1. hs_err_pid*.log 文件"
echo "  2. _tmp_yuhu_ir_*.ll 文件（查看生成的 LLVM IR）"

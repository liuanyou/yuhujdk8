#!/bin/bash

# Yuhu tier-6 编译器断言复现脚本
#
# 目标崩溃 (来自 hs_err_pid62345.log):
#   Internal Error (.../yuhu/yuhuTopLevelBlock.cpp:874):
#   assert(slot_type == ciTypeFlow::StateVector::T_BOTTOM) failed: basic type should be T_CONFLICT
#   Current CompileTask: yuhu: ... 6  com.google.common.cache.LocalCache$Segment::lockedGetOrLoad
#
# 这是编译期断言(Yuhu CompilerThread 在 emit_IR -> do_monitorenter ->
# register_deopt_bundle 时触发), 只要让 Yuhu 编译 lockedGetOrLoad 即可复现。

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---------------------------------------------------------------
# 1. 定位编译好的 Yuhu JDK (slowdebug / assert 构建)
# ---------------------------------------------------------------
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
    echo "错误: 找不到编译好的 JDK！请先构建 slowdebug JDK, 或手动设置 JDK_BIN。"
    echo "尝试过的路径:"
    echo "  $PROJECT_ROOT/build/macosx-aarch64-normal-server-slowdebug/images/j2sdk-image/bin"
    echo "  $PROJECT_ROOT/build/macosx-aarch64-normal-server-slowdebug/images/j2sdk-bundle/jdk1.8.0.jdk/Contents/Home/bin"
    echo "  $PROJECT_ROOT/build/macosx-aarch64-normal-server-slowdebug/jdk/bin"
    exit 1
fi

JAVA="$JDK_BIN/java"
JAVAC="$JDK_BIN/javac"
echo "=== Yuhu JDK ==="
echo "  $JDK_BIN"

# ---------------------------------------------------------------
# 2. 定位 Guava 及 failureaccess jar (可用环境变量覆盖)
#    YuhuCompileOnlyOf/base_name 形如:
#      com.google.common.cache.LocalCache$Segment::lockedGetOrLoad
# ---------------------------------------------------------------
GUAVA_JAR="${GUAVA_JAR:-}"
if [ -z "$GUAVA_JAR" ]; then
    GUAVA_JAR="$(find "$HOME/.gradle/caches" -iname 'guava-*-jre.jar' 2>/dev/null \
        | grep -v -e sources -e javadoc | head -1)"
fi

FAILUREACCESS_JAR="${FAILUREACCESS_JAR:-}"
if [ -z "$FAILUREACCESS_JAR" ]; then
    FAILUREACCESS_JAR="$(find "$HOME/.gradle" "$HOME/.m2" -iname 'failureaccess-*.jar' 2>/dev/null \
        | grep -v -e sources -e javadoc | head -1)"
fi

if [ -z "$GUAVA_JAR" ] || [ ! -f "$GUAVA_JAR" ]; then
    echo "错误: 找不到 guava jar。请设置 GUAVA_JAR=/path/to/guava-XX-jre.jar"
    exit 1
fi

CP_COMPILE="$GUAVA_JAR"
CP_RUN=".:$GUAVA_JAR"
if [ -n "$FAILUREACCESS_JAR" ] && [ -f "$FAILUREACCESS_JAR" ]; then
    CP_COMPILE="$CP_COMPILE:$FAILUREACCESS_JAR"
    CP_RUN="$CP_RUN:$FAILUREACCESS_JAR"
else
    echo "警告: 未找到 failureaccess jar; 若运行期报 NoClassDefFoundError 请设置 FAILUREACCESS_JAR。"
fi

echo "=== 依赖 ==="
echo "  guava:          $GUAVA_JAR"
echo "  failureaccess:  ${FAILUREACCESS_JAR:-<none>}"

# ---------------------------------------------------------------
# 3. 编译复现类
# ---------------------------------------------------------------
echo ""
echo "=== 编译 com/example/GuavaLockedGetOrLoadReproducer.java ==="
cd "$SCRIPT_DIR"
"$JAVAC" -cp "$CP_COMPILE" com/example/GuavaLockedGetOrLoadReproducer.java
if [ $? -ne 0 ]; then
    echo "编译失败！"
    exit 1
fi

# ---------------------------------------------------------------
# 4. 运行: 强制 Yuhu 只编译 lockedGetOrLoad, 观察编译期断言
#    注意: '$Segment' 必须用单引号, 防止 shell 变量展开。
# ---------------------------------------------------------------
N="${1:-200000}"
echo ""
echo "=== 运行 (n=$N), 预期触发 yuhuTopLevelBlock.cpp:874 编译期断言 ==="
"$JAVA" \
    -XX:+UseYuhuInt \
    -XX:+UseYuhuCompiler \
    -XX:TieredStopAtLevel=6 \
    '-XX:YuhuCompileOnlyOf=com.google.common.cache.LocalCache$Segment::lockedGetOrLoad' \
    -cp "$CP_RUN" \
    com.example.GuavaLockedGetOrLoadReproducer "$N"

RC=$?
echo ""
if [ $RC -ne 0 ]; then
    echo "进程以退出码 $RC 结束 —— 若同目录生成了新的 hs_err_pid*.log 且其中含"
    echo "  assert(slot_type == ciTypeFlow::StateVector::T_BOTTOM) @ yuhuTopLevelBlock.cpp:874"
    echo "则复现成功。"
else
    echo "未崩溃: 请增大 n (例如 ./test_guava_lockedgetorload.sh 100000)。"
fi
exit $RC

#!/bin/bash

# Decimal Test Script for Yuhu Compiler

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JDK_HOME="$SCRIPT_DIR/../build/macosx-aarch64-normal-server-slowdebug/images/j2sdk-bundle/jdk1.8.0.jdk/Contents/Home"

echo "=== Yuhu Decimal Test ==="
echo "JDK Home: $JDK_HOME"
echo ""

# Compile the test
echo "Compiling DecimalTest.java..."
cd "$SCRIPT_DIR"
"$JDK_HOME/bin/javac" com/example/DecimalTest.java

if [ $? -ne 0 ]; then
    echo "Compilation failed!"
    exit 1
fi

echo "Compilation successful."
echo ""

# Run with Yuhu compiler and exception table tracing
echo "Running test with Yuhu compiler..."
echo "Command:"
echo "$JDK_HOME/bin/java -XX:+UseYuhuCompiler -XX:TieredStopAtLevel=6 -XX:YuhuComplexityThreshold=1 -XX:CompileCommand=yuhuonly,com/example/DecimalTest.simpleTryCatch -XX:+YuhuDumpIRToFile -XX:YuhuPrintTypeflowOf=com.example.DecimalTest::simpleTryCatch com.example.DecimalTest"
echo ""
echo "=== Output ==="

"$JDK_HOME/bin/java" \
    -XX:+UseYuhuCompiler \
    -XX:TieredStopAtLevel=6 \
    -XX:YuhuComplexityThreshold=1 \
    -XX:CompileCommand=yuhuonly,com/example/DecimalTest.simpleTryCatch \
    -XX:+YuhuDumpIRToFile \
    -XX:YuhuPrintTypeflowOf=com.example.DecimalTest::simpleTryCatch \
    com.example.DecimalTest

EXIT_CODE=$?

echo ""
echo "=== End of Output ==="
echo ""

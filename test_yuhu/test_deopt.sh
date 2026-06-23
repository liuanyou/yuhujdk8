#!/bin/bash

# Deoptimization Test Script for Yuhu Compiler

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JDK_HOME="$SCRIPT_DIR/../build/macosx-aarch64-normal-server-slowdebug/images/j2sdk-bundle/jdk1.8.0.jdk/Contents/Home"

echo "=== Yuhu Deoptimization Test ==="
echo "JDK Home: $JDK_HOME"
echo ""

# Compile the test
echo "Compiling DeoptTest.java..."
cd "$SCRIPT_DIR"
"$JDK_HOME/bin/javac" com/example/DeoptTest.java

if [ $? -ne 0 ]; then
    echo "Compilation failed!"
    exit 1
fi

echo "Compilation successful."
echo ""

# Run with Yuhu compiler and deoptimization tracing
echo "Running test with Yuhu compiler..."
echo "Command:"
echo "$JDK_HOME/bin/java -XX:+UseYuhuCompiler -XX:TieredStopAtLevel=6 -XX:YuhuComplexityThreshold=1 -XX:CompileCommand=yuhuonly,com/example/DeoptTest.complexDeoptTarget -XX:+YuhuDumpIRToFile -XX:YuhuPrintTypeflowOf=com.example.DeoptTest::complexDeoptTarget com.exampleDeoptTest"
echo ""
echo "=== Output ==="

"$JDK_HOME/bin/java" \
    -XX:+UseYuhuCompiler \
    -XX:TieredStopAtLevel=6 \
    -XX:YuhuComplexityThreshold=1 \
    -XX:CompileCommand=yuhuonly,com/example/DeoptTest.complexDeoptTarget \
    -XX:+YuhuDumpIRToFile \
    -XX:YuhuPrintTypeflowOf=com.example.DeoptTest::complexDeoptTarget \
    com.example.DeoptTest

EXIT_CODE=$?

echo ""
echo "=== End of Output ==="
echo ""

if [ $EXIT_CODE -eq 0 ]; then
    echo "Test completed successfully."
    echo ""
    echo "If you saw deoptimization messages above, the trap infrastructure works!"
    echo "Look for lines containing:"
    echo "  - 'Deoptimization event'"
    echo "  - 'reason='"
    echo "  - 'Unpack_reexecute' or 'Unpack_deopt'"
else
    echo "Test failed with exit code: $EXIT_CODE"
    echo "Check hs_err_pid*.log for crash details."
fi

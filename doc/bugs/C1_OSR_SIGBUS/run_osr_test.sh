#!/bin/bash
# Quick OSR crash regression script
# Runs the minimal test multiple times to catch the OSR bug

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

JDK_BIN="$REPO_ROOT/build/macosx-aarch64-normal-server-slowdebug/images/j2sdk-bundle/jdk1.8.0.jdk/Contents/Home/bin/java"
JAVAC_BIN="$REPO_ROOT/build/macosx-aarch64-normal-server-slowdebug/images/j2sdk-bundle/jdk1.8.0.jdk/Contents/Home/bin/javac"

cd "$SCRIPT_DIR"

echo "=== C1 OSR Bug Test Runner ==="
echo "Location: $SCRIPT_DIR"
echo ""

if [ ! -f OSRCrashTest.java ]; then
    echo "❌ Error: OSRCrashTest.java not found in $SCRIPT_DIR"
    exit 1
fi

echo "Compiling OSRCrashTest.java..."
"$JAVAC_BIN" OSRCrashTest.java
echo "✓ Compiled successfully"
echo ""

NUM_RUNS=20
echo "Running OSR crash test $NUM_RUNS times..."
echo "Press Ctrl+C to stop if it crashes"
echo ""

CRASH_COUNT=0
SUCCESS_COUNT=0

for i in $(seq 1 "$NUM_RUNS"); do
    echo "=== Run $i/$NUM_RUNS ==="

    BEFORE_COUNT=$(ls -1 "$SCRIPT_DIR"/hs_err_pid*.log 2>/dev/null | wc -l)

    TMP_OUT=$(mktemp)
    if "$JDK_BIN" \
        -cp "$SCRIPT_DIR" \
        -XX:+UseOnStackReplacement \
        -XX:CompileThreshold=100 \
        -XX:+PrintCompilation \
        -XX:+TraceOnStackReplacement \
        OSRCrashTest >"$TMP_OUT" 2>&1; then
        AFTER_CODE=0
    else
        AFTER_CODE=$?
    fi
    tail -10 "$TMP_OUT"
    rm -f "$TMP_OUT"

    AFTER_COUNT=$(ls -1 "$SCRIPT_DIR"/hs_err_pid*.log 2>/dev/null | wc -l)

    if [ "$AFTER_COUNT" -gt "$BEFORE_COUNT" ]; then
        CRASH_COUNT=$((CRASH_COUNT + 1))
        echo ""
        echo "❌ CRASH DETECTED (new hs_err log created)"
        LATEST_LOG=$(ls -t "$SCRIPT_DIR"/hs_err_pid*.log 2>/dev/null | head -1)
        if [ -n "${LATEST_LOG:-}" ]; then
            echo "Crash log: $LATEST_LOG"
            echo "PC address:"
            grep "pc=" "$LATEST_LOG" | head -1
            echo ""
        fi
        read -p "Continue testing? (y/n) " -n 1 -r
        echo ""
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            break
        fi
    elif [ "$AFTER_CODE" -ne 0 ]; then
        CRASH_COUNT=$((CRASH_COUNT + 1))
        echo ""
        echo "❌ CRASHED on run $i with exit code $AFTER_CODE"
        echo ""
        LATEST_LOG=$(ls -t "$SCRIPT_DIR"/hs_err_pid*.log 2>/dev/null | head -1)
        if [ -n "${LATEST_LOG:-}" ]; then
            echo "Crash log: $LATEST_LOG"
            echo "PC address:"
            grep "pc=" "$LATEST_LOG" | head -1
            echo ""
        fi
        read -p "Continue testing? (y/n) " -n 1 -r
        echo ""
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            break
        fi
    else
        SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
        echo "✓ Run $i completed successfully"
    fi

    echo ""
done

echo ""
echo "=== Test Summary ==="
echo "Total runs: $NUM_RUNS"
echo "Crashes: $CRASH_COUNT"
echo "Success: $SUCCESS_COUNT"
if [ "$NUM_RUNS" -gt 0 ]; then
    echo "Crash rate: $((CRASH_COUNT * 100 / NUM_RUNS))%"
fi
echo ""

if [ $CRASH_COUNT -eq 0 ]; then
    echo "✓✓✓ All runs successful - bug likely fixed or not triggered"
    echo "Suggestions:"
    echo "  - Lower CompileThreshold (e.g., -XX:CompileThreshold=50)"
    echo "  - Increase NUM_RUNS"
    echo "  - Modify OSRCrashTest.java to stress other patterns"
else
    echo "❌❌❌ Bug reproduced! Crash logs available for analysis"
fi

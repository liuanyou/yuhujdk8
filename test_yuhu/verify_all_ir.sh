#!/bin/bash

# Script to verify all LLVM IR files in test_yuzu folder
# Uses LLVM 20.1.5 opt -passes=verify

OPT_CMD="/opt/homebrew/Cellar/llvm/20.1.5/bin/opt"
TEST_DIR="/Users/liuanyou/CLionProjects/jdk8/test_yuhu"
PASS_COUNT=0
FAIL_COUNT=0
TOTAL_COUNT=0

echo "========================================="
echo "LLVM IR Verification Script"
echo "========================================="
echo ""

# Check if opt exists
if [ ! -x "$OPT_CMD" ]; then
    echo "ERROR: opt not found at $OPT_CMD"
    exit 1
fi

# Find all _tmp_yuhu_ir_*.ll files
for ir_file in "$TEST_DIR"/_tmp_yuhu_ir_*.ll; do
    if [ ! -f "$ir_file" ]; then
        echo "No IR files found in $TEST_DIR"
        exit 0
    fi
    
    TOTAL_COUNT=$((TOTAL_COUNT + 1))
    filename=$(basename "$ir_file")
    
    # Run verification
    if $OPT_CMD -passes=verify "$ir_file" > /dev/null 2>&1; then
        echo "[PASS] $filename"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "[FAIL] $filename"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        # Capture error output
        $OPT_CMD -passes=verify "$ir_file" 2>&1 | head -20 | sed 's/^/       /'
        echo ""
    fi
done

echo ""
echo "========================================="
echo "Verification Summary"
echo "========================================="
echo "Total:  $TOTAL_COUNT"
echo "Passed: $PASS_COUNT"
echo "Failed: $FAIL_COUNT"
echo "========================================="

if [ $FAIL_COUNT -gt 0 ]; then
    exit 1
else
    exit 0
fi

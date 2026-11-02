#!/bin/bash

# Reproducer for the tier-6 jump-table crash in
#   org.apache.log4j.helpers.ISO8601DateFormat.format(Date, StringBuffer, FieldPosition)
# (457 bytes, nmethod 8837 in hs_err_pid43806.log; SIGSEGV SEGV_ACCERR at the
#  "ldr x13, [x13]" after "ldr x13, [table, w0, sxtw #3]" of the 12-entry jump
#  table for the tableswitch {0..11} at bci 75).
#
# The goal is NOT only to catch the signal: even a clean run is informative,
# because -XX:+YuhuTraceMachineCode dumps, for this exact method,
#   - the '__const' section symbols (= jump tables) with their LLVM addresses,
#   - every registered edge (got_symbol / const_symbol),
#   - the LLVM function disassembly with the adrp/add pairs,
#   - the installed nmethod range (with -XX:+YuhuTraceInstalls),
# so we can check whether the jump table was copied into cb->consts() and
# whether its entries were rebased from LLVM-cache addresses to blob addresses.
#
#   ./test_iso8601_jumptable.sh [JDK_HOME] [LOG4J_JAR]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JDK_HOME="${1:-$SCRIPT_DIR/../build/macosx-aarch64-normal-server-slowdebug/images/j2sdk-bundle/jdk1.8.0.jdk/Contents/Home}"
LOG4J_JAR="${2:-$HOME/.m2/repository/log4j/log4j/1.2.17/log4j-1.2.17.jar}"
WORK_DIR="$SCRIPT_DIR/jumptable_work"
TRACE_LOG="$WORK_DIR/machine_code_trace.log"

echo "=== Yuhu ISO8601 jump-table reproducer ==="
echo "JDK Home:   $JDK_HOME"
echo "log4j jar:  $LOG4J_JAR"
echo ""

if [ ! -f "$LOG4J_JAR" ]; then
    echo "log4j 1.2.17 jar not found at $LOG4J_JAR"
    echo "pass it as the second argument, or point at any log4j 1.2.x jar"
    echo "whose ISO8601DateFormat.format(Date,StringBuffer,FieldPosition) is 457 bytes:"
    echo "  javap -c -p -classpath <jar> org.apache.log4j.helpers.ISO8601DateFormat"
    exit 1
fi

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
cd "$WORK_DIR" || exit 1

# A freshly relinked libjvm.dylib inside build/images only carries the linker's
# ad-hoc signature, which AMFI rejects on macOS/arm64 (every java process dies
# with "Killed: 9"). Re-signing it ad-hoc is what the JDK build would do for a
# real bundle.
if ! "$JDK_HOME/bin/java" -version >/dev/null 2>&1; then
    echo "java is not runnable, re-signing libjvm.dylib ad-hoc..."
    codesign --sign - --force "$JDK_HOME/jre/lib/server/libjvm.dylib" \
      || codesign --sign - --force "$JDK_HOME/lib/server/libjvm.dylib"
    "$JDK_HOME/bin/java" -version || exit 1
fi

cp -R "$SCRIPT_DIR/com" .

"$JDK_HOME/bin/javac" -cp "$LOG4J_JAR" com/example/ISO8601JumpTableRepro.java
if [ $? -ne 0 ]; then
    echo "Compilation failed!"
    exit 1
fi

echo ""
echo "Running (full stderr captured in $TRACE_LOG)..."
"$JDK_HOME/bin/java" \
    -XX:+UseYuhuInt \
    -XX:+UseYuhuCompiler \
    -XX:+YuhuLLVMInlining \
    -XX:TieredStopAtLevel=6 \
    -XX:YuhuComplexityThreshold=1 \
    -XX:YuhuCompileOnlyOf=org.apache.log4j.helpers.ISO8601DateFormat::format \
    -XX:CompileCommand=yuhuonly,org/apache/log4j/helpers/ISO8601DateFormat.format \
    -XX:+YuhuTraceInstalls \
    -XX:+YuhuTraceMachineCode \
    -XX:+YuhuDumpIRToFile \
    -Xbatch \
    -cp ".:$LOG4J_JAR" \
    com.example.ISO8601JumpTableRepro 2>&1 | tee "$TRACE_LOG"

EXIT_CODE=${PIPESTATUS[0]}

echo ""
if [ $EXIT_CODE -ne 0 ]; then
    echo "Run exited with code $EXIT_CODE - an hs_err_pid*.log should be in $WORK_DIR."
    echo "Check: problematic frame is the ISO8601DateFormat::format nmethod and the"
    echo "faulting pc follows the 'adrp/add + ldr [x13, w0, sxtw #3]' pattern."
else
    echo "No crash. Narrowing evidence for the jump-table copy path:"
    echo ""
    echo "--- const-section symbols registered (candidate jump tables) ---"
    grep -n "\[CallSite Extractor\] const," "$TRACE_LOG" | head -20
    echo "--- edges registered for the format function ---"
    grep -nE "Getting .* edge at|mark it as (const|got)" "$TRACE_LOG" | head -40
    echo "--- installed nmethod range for ISO8601DateFormat.format ---"
    grep -n "Register method.*ISO8601\|nmethod.*ISO8601" "$TRACE_LOG" | head -10
    echo ""
    echo "Then compare: every 'const, address:' value and the adrp targets in the"
    echo "LLVM disassembly vs. the installed nmethod [code_begin, code_end].  If an"
    echo "adrp pair in the installed blob still resolves to an address outside the"
    echo "blob (or a consts slot still holds an LLVM-cache label address), the copy/"
    echo "rebase in scan_and_generate_all_relocations missed this table."
fi

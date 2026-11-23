#!/bin/bash
# Yuhu CHA devirtualization reproducer.
#
# Validates the removal of the "inhibit cast" block from
# YuhuTopLevelBlock::improve_virtual_call.  The scenarios separate
# three concerns:
#
#   [1] stock JVM (baseline): fixture logic itself is correct; refSum
#       equals what runCha computes with plain vtable dispatch.
#   [2] Yuhu + CHA enabled (-YuhuDevirtualize): tier-6 promotes runCha
#       and the CHA path substitutes Mono.compute directly.  A result
#       mismatch here proves CHA is wrong.
#   [3] Dep invalidation via recompile count: after Class.forName
#       loads Tertius in phase 2, HotSpot's dependency tracker must
#       flush the phase-1 nmethod, causing runCha to be compiled at
#       level 6 at least twice.  One compile = the flush never fired.
#   [4] CHA + profile together (+YuhuDevirtualize): both mechanisms
#       must coexist; the profile path takes precedence in do_call
#       (find_child_by_bci fires before improve_virtual_call), and the
#       dep flush from phase 2 must still hold.
#
# Each scenario runs the two-phase main() so the dynamic-class-load
# transition is always exercised.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

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
    echo "ERROR: could not locate a compiled JDK image under $PROJECT_ROOT/build"
    exit 1
fi

JAVA="$JDK_BIN/java"
JAVAC="$JDK_BIN/javac"

echo "Using JDK at: $JDK_BIN"
cd "$SCRIPT_DIR"

# Fresh .class so we're sure the current source is what runs.
rm -f com/example/ChaDevirtTest*.class
"$JAVAC" com/example/ChaDevirtTest.java || { echo "javac failed"; exit 1; }

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

COMMON_YUHU_FLAGS=(
    -XX:+UseYuhuInt
    -XX:+UseYuhuCompiler
    -XX:TieredStopAtLevel=6
    -XX:+YuhuLLVMInlining
    -XX:-BackgroundCompilation
    -XX:+UnlockDiagnosticVMOptions
    -XX:+YuhuTraceInlining
    -XX:+PrintCompilation
)

overall_rc=0

# ---------------------------------------------------------------------
# Scenario 1: stock JVM, no Yuhu.  Proves the fixture's reference sums
# and its phase-1/phase-2 transitions are internally consistent.
# ---------------------------------------------------------------------
LOG1="$TMP_DIR/scenario1.log"
echo "=== [1/4] Baseline (stock interpreter + default tiering) ==="
"$JAVA" -cp . com.example.ChaDevirtTest > "$LOG1" 2>&1
rc=$?
if grep -q "ChaDevirtTest: ALL OK" "$LOG1" && [ "$rc" -eq 0 ]; then
    echo "PASS: baseline"
else
    echo "FAIL: baseline (exit=$rc)"
    tail -40 "$LOG1"
    overall_rc=1
fi
echo ""

# ---------------------------------------------------------------------
# Verify the -YuhuDevirtualize flag actually takes effect before
# treating the CHA-only scenario as evidence.
# ---------------------------------------------------------------------
"$JAVA" "${COMMON_YUHU_FLAGS[@]}" -XX:-YuhuDevirtualize \
    -XX:+PrintFlagsFinal -version 2>&1 \
    | grep -E 'YuhuDevirtualize[[:space:]]+:?=' > "$TMP_DIR/flags_off.log"
echo "Effective YuhuDevirtualize under -XX:-YuhuDevirtualize:"
cat "$TMP_DIR/flags_off.log"
if ! grep -qE 'YuhuDevirtualize[[:space:]]+:?=[[:space:]]+false' "$TMP_DIR/flags_off.log"; then
    echo "FAIL: -YuhuDevirtualize did not take effect; aborting"
    exit 1
fi
echo ""

# ---------------------------------------------------------------------
# Scenario 2: Yuhu + CHA enabled (profile devirtualization OFF).
# ---------------------------------------------------------------------
LOG2="$TMP_DIR/scenario2.log"
echo "=== [2/4] Yuhu + CHA only (-YuhuDevirtualize) ==="
"$JAVA" "${COMMON_YUHU_FLAGS[@]}" -XX:-YuhuDevirtualize \
    -cp . com.example.ChaDevirtTest > "$LOG2" 2>&1
rc=$?
if grep -q "ChaDevirtTest: ALL OK" "$LOG2" && [ "$rc" -eq 0 ]; then
    echo "PASS: CHA-only correctness"
else
    echo "FAIL: CHA-only correctness (exit=$rc)"
    tail -60 "$LOG2"
    overall_rc=1
fi

# Positive proof tier-6 fired.  Level column can be preceded by % (OSR)
# and/or b (break) flags, so match the level number + method loosely.
if grep -qE ' 6[[:space:]]+com\.example\.ChaDevirtTest::runCha' "$LOG2"; then
    echo "PASS: CHA-only tier-6 compile observed"
else
    echo "FAIL: CHA-only tier-6 compile missing"
    grep -E 'ChaDevirtTest::runCha' "$LOG2" || true
    overall_rc=1
fi

# Positive proof CHA substituted: with -YuhuDevirtualize, the ONLY way
# a virtual call becomes non-virtual is improve_virtual_call.  The
# planner's devirtualize-trace should NOT fire in this scenario.
if grep -q 'virtual_call_target.*devirtualiz' "$LOG2"; then
    echo "FAIL: profile planner fired even with -YuhuDevirtualize"
    overall_rc=1
else
    echo "PASS: profile planner correctly silent"
fi
echo ""

# ---------------------------------------------------------------------
# Scenario 3: dependency invalidation.  After Class.forName loads
# Tertius, HotSpot's dep tracker must flush runCha's tier-6 nmethod
# and recompile it.  Look for runCha compiled at level 6 at least
# twice in the PrintCompilation log.
# ---------------------------------------------------------------------
LOG3="$TMP_DIR/scenario3.log"
echo "=== [3/4] CHA dep invalidation on new subclass load ==="
"$JAVA" "${COMMON_YUHU_FLAGS[@]}" -XX:-YuhuDevirtualize \
    -cp . com.example.ChaDevirtTest > "$LOG3" 2>&1
rc=$?
if ! grep -q "ChaDevirtTest: ALL OK" "$LOG3" || [ "$rc" -ne 0 ]; then
    echo "FAIL: dep-invalidation run crashed or mismatched (exit=$rc)"
    tail -60 "$LOG3"
    overall_rc=1
else
    # Count unique level-6 runCha compile lines (the two phase-2 warm
    # loops may recompile again; we only require the phase-1 nmethod
    # was flushed at least once, i.e. we see >=2 level-6 entries).
    n=$(grep -cE ' 6[[:space:]]+com\.example\.ChaDevirtTest::runCha' "$LOG3")
    if [ "$n" -ge 2 ]; then
        echo "PASS: runCha recompiled at tier-6 after Tertius load (count=$n)"
    else
        echo "FAIL: runCha compiled only $n time(s) at tier-6; dep flush did not force recompile"
        grep -E 'ChaDevirtTest::runCha' "$LOG3" || true
        overall_rc=1
    fi
fi
echo ""

# ---------------------------------------------------------------------
# Scenario 4: CHA + profile both enabled.  Profile path takes
# precedence in do_call, but the CHA code path is still exercised by
# the recompile after dep invalidation (post-Tertius there are two
# impls so profile+guard becomes the driver; both paths must coexist
# without a correctness break).
# ---------------------------------------------------------------------
LOG4="$TMP_DIR/scenario4.log"
echo "=== [4/4] Yuhu + CHA + profile (+YuhuDevirtualize) ==="
"$JAVA" "${COMMON_YUHU_FLAGS[@]}" -XX:+YuhuDevirtualize \
    -cp . com.example.ChaDevirtTest > "$LOG4" 2>&1
rc=$?
if grep -q "ChaDevirtTest: ALL OK" "$LOG4" && [ "$rc" -eq 0 ]; then
    echo "PASS: CHA + profile coexistence"
else
    echo "FAIL: CHA + profile coexistence (exit=$rc)"
    tail -60 "$LOG4"
    overall_rc=1
fi
if grep -qE ' 6[[:space:]]+com\.example\.ChaDevirtTest::runCha' "$LOG4"; then
    echo "PASS: CHA+profile tier-6 compile observed"
else
    echo "FAIL: CHA+profile tier-6 compile missing"
    overall_rc=1
fi
echo ""

# ---------------------------------------------------------------------
# DeoptimizeALot stress: force every safepoint poll to trigger a deopt
# to make sure the CHA-substituted call site's PcDesc / dependency
# bookkeeping is consistent enough to survive constant deopt churn.
# ---------------------------------------------------------------------
LOG5="$TMP_DIR/scenario5.log"
echo "=== [5/5] Yuhu + CHA + DeoptimizeALot stress ==="
"$JAVA" "${COMMON_YUHU_FLAGS[@]}" -XX:-YuhuDevirtualize \
    -XX:+DeoptimizeALot \
    -cp . com.example.ChaDevirtTest > "$LOG5" 2>&1
rc=$?
if grep -q "ChaDevirtTest: ALL OK" "$LOG5" && [ "$rc" -eq 0 ]; then
    echo "PASS: CHA + DeoptimizeALot stress"
else
    echo "FAIL: CHA + DeoptimizeALot stress (exit=$rc)"
    tail -60 "$LOG5"
    overall_rc=1
fi
echo ""

if [ "$overall_rc" -eq 0 ]; then
    echo "=========================================="
    echo "All CHA devirtualization scenarios PASS."
    echo "=========================================="
else
    echo "=========================================="
    echo "At least one CHA scenario FAILED."
    echo "=========================================="
fi
exit $overall_rc

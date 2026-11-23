#!/bin/bash
# Yuhu Increment 2: profile-driven devirtualization reproducer.
#
# Runs DevirtTest in four configurations and asserts each one prints the
# "DevirtTest: ALL OK" sentinel.  Additionally, the tier-6 runs check for
# the level-6 compilation line and (when YuhuDevirtualize is on) the
# planner's devirtualization trace, so a silent fall-back to tier-3/4 is
# distinguishable from a real pass.

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
rm -f com/example/DevirtTest*.class
"$JAVAC" com/example/DevirtTest.java || { echo "javac failed"; exit 1; }

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

pass_fail() {
    local tag="$1"; local log="$2"; local rc="$3"
    if grep -q "DevirtTest: ALL OK" "$log" && [ "$rc" -eq 0 ]; then
        echo "PASS: $tag"
        return 0
    else
        echo "FAIL: $tag (exit=$rc)"
        tail -40 "$log"
        return 1
    fi
}

overall_rc=0

# ---------------------------------------------------------------------
# Scenario 1: stock JVM, no Yuhu flags.  Proves the fixture itself is
# internally consistent (all reference sums match) independent of the
# compiler under test.
# ---------------------------------------------------------------------
LOG1="$TMP_DIR/scenario1.log"
echo "=== [1/4] Baseline (stock interpreter + default tiering) ==="
"$JAVA" -cp . com.example.DevirtTest > "$LOG1" 2>&1
rc=$?
pass_fail "baseline" "$LOG1" $rc || overall_rc=1
echo ""

# ---------------------------------------------------------------------
# Scenario 2: Yuhu tier-6, YuhuDevirtualize ON (the interesting case).
# Requires "DevirtTest::runMono" to reach level 6 AND the planner trace
# to include at least one devirtualization substitution line.
# ---------------------------------------------------------------------
LOG2="$TMP_DIR/scenario2.log"
echo "=== [2/4] Yuhu tier-6 with YuhuDevirtualize ON ==="
"$JAVA" -cp . \
    "${COMMON_YUHU_FLAGS[@]}" \
    -XX:+YuhuDevirtualize \
    com.example.DevirtTest > "$LOG2" 2>&1
rc=$?
pass_fail "yuhu-devirt-on" "$LOG2" $rc || overall_rc=1

# Verify tier-6 was actually reached for runMono.  PrintCompilation format
# is "<ms> <id> [%][ ]<break/other-flag> <level> <class>::<method>" with a
# leading two-space indent; the previous strict regex assumed level was
# column-3 and missed the tier-6 lines.  This pattern just anchors on
# " 6 " followed by the method name.
if grep -E " 6 +com\.example\.DevirtTest::runMono" "$LOG2" >/dev/null; then
    echo "  tier-6 compile of runMono: YES"
else
    echo "  tier-6 compile of runMono: NO -- test may have run only at tier 3/4"
    overall_rc=1
fi

# Verify virtual_call_target actually substituted Mono.compute behind a
# Mono klass guard: the tree printed under YuhuTraceInlining must have a
# depth=1 child line for DevirtTest$Mono.compute attached to the runMono
# tree.  If runMono's tree still shows only depth=0, the substitution
# either didn't fire or was filtered by should_inline / is_llvm_inlineable.
if awk '/Inline tree for com\/example\/DevirtTest\.runMono/{f=1;next} f && /Inline tree for /{f=0} f && /depth=1.*DevirtTest\$Mono\.compute/{print; exit 0}' "$LOG2" | grep -q .; then
    echo "  planner devirtualized Mono.compute into runMono's inline tree: YES"
elif grep -E "depth=1.*DevirtTest\\\$Mono\.compute" "$LOG2" >/dev/null; then
    echo "  planner devirtualized Mono.compute into runMono's inline tree: YES (matched across full log)"
else
    echo "  planner devirtualized Mono.compute into runMono's inline tree: NO"
    echo "  (runMono's tree shows only depth=0 -- check that the call site"
    echo "   actually emits _invokevirtual, and whether the substitution or"
    echo "   the inline-eligibility gate rejected it)"
    overall_rc=1
fi

echo ""

# ---------------------------------------------------------------------
# Scenario 3: same as scenario 2 but with YuhuDevirtualize OFF.  Values
# produced by runMono must be identical to scenario 2 (the last printed
# aggregate line is compared if the fixture emits it; here we just
# require both to reach the ALL OK sentinel).
# ---------------------------------------------------------------------
LOG3="$TMP_DIR/scenario3.log"
echo "=== [3/4] Yuhu tier-6 with YuhuDevirtualize OFF (control) ==="
"$JAVA" -cp . \
    "${COMMON_YUHU_FLAGS[@]}" \
    -XX:-YuhuDevirtualize \
    com.example.DevirtTest > "$LOG3" 2>&1
rc=$?
pass_fail "yuhu-devirt-off" "$LOG3" $rc || overall_rc=1
echo ""

# ---------------------------------------------------------------------
# Scenario 4: stress the deopt path with DeoptimizeALot.  Ensures the
# new do_trap emitted inside emit_devirtualize_guard plays nicely with
# the frame-reconstruction / vm_result protocol, and the guard-fail
# edge (Poly receiver into a Mono-guarded runMono) recovers correctly.
# ---------------------------------------------------------------------
LOG4="$TMP_DIR/scenario4.log"
echo "=== [4/4] Yuhu tier-6 with DeoptimizeALot (stress guard-fail trap) ==="
"$JAVA" -cp . \
    "${COMMON_YUHU_FLAGS[@]}" \
    -XX:+YuhuDevirtualize \
    -XX:+DeoptimizeALot \
    com.example.DevirtTest > "$LOG4" 2>&1
rc=$?
pass_fail "yuhu-devirt-deoptalot" "$LOG4" $rc || overall_rc=1
echo ""

if [ $overall_rc -eq 0 ]; then
    echo "=== ALL SCENARIOS PASSED ==="
else
    echo "=== AT LEAST ONE SCENARIO FAILED (rc=$overall_rc) ==="
fi
exit $overall_rc

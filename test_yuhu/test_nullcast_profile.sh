#!/bin/bash
# Yuhu tier-6 BitData null_seen consumption test (YuhuProfileNullCast).
#
# Verifies that the profile-driven null-cast speculation:
#   1. ELIMINATES null-check blocks (zero:/not_zero:) in tier-6 IR
#      at checkcast and instanceof sites when null_seen==0.
#   2. REPLACES them with deopt-based speculation (checkcast_null_deopt
#      calling __llvm_deoptimize with per-site trap_request).
#   3. Preserves numerical semantics (oracle equality).
#   4. Is gated on tier-6 (tier-5 negative control: zero: present).
#   5. Survives DeoptimizeALot churn.
#
# IR discriminator:
#   - Control (-YuhuProfileNullCast): the tier-6 IR for run() contains
#     `zero:` block labels and `br ... label %zero ...` null-check patterns.
#   - Test (+YuhuProfileNullCast): those labels are ABSENT, replaced by
#     `checkcast_null_deopt:` labels calling `@__llvm_deoptimize(i32)`.

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

rm -f com/example/NullCastProfileTest*.class
"$JAVAC" com/example/NullCastProfileTest.java || { echo "javac failed"; exit 1; }

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"; find "$SCRIPT_DIR" -maxdepth 1 -name "_tmp_yuhu_ir_*NullCast*" -delete 2>/dev/null' EXIT

COMMON_YUHU_FLAGS=(
    -XX:+UseYuhuInt
    -XX:+UseYuhuCompiler
    -XX:TieredStopAtLevel=6
    -XX:-BackgroundCompilation
    -XX:+UnlockDiagnosticVMOptions
    -XX:+YuhuDumpIRToFile
    -XX:+PrintCompilation
)

# Tier-5 arm: same flags but ceiling at 5 to verify gate.
TIER5_YUHU_FLAGS=()
for _f in "${COMMON_YUHU_FLAGS[@]}"; do
    case "$_f" in
        -XX:TieredStopAtLevel=*) _f="-XX:TieredStopAtLevel=5" ;;
    esac
    TIER5_YUHU_FLAGS+=("$_f")
done

# glob helper: expand IR files for NullCastProfileTest.run
ir_glob() {
    find "$SCRIPT_DIR" -maxdepth 1 -type f \
        -name '_tmp_yuhu_ir_RS4GC_*NullCastProfileTest__run_*.ll' 2>/dev/null
}

wipe_ir() {
    while IFS= read -r f; do rm -f "$f"; done < <(ir_glob)
}

# Pick the first (and likely only) tier-6 RS4GC IR file for run().
pick_ir() {
    local files
    files=$(ir_glob)
    if [ -z "$files" ]; then
        echo ""
        return
    fi
    echo "$files" | sort | head -1
}

pass_fail() {
    local tag="$1"; local log="$2"; local rc="$3"
    if grep -q "NullCastProfileTest: ALL OK" "$log" && [ "$rc" -eq 0 ]; then
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
# Scenario 1: stock JVM baseline — fixture oracle self-consistent.
# ---------------------------------------------------------------------
LOG1="$TMP_DIR/scenario1.log"
echo "=== [1/5] Baseline (stock interpreter, no Yuhu) ==="
wipe_ir
"$JAVA" -cp . com.example.NullCastProfileTest > "$LOG1" 2>&1
rc=$?
pass_fail "baseline" "$LOG1" $rc || overall_rc=1
echo ""

# ---------------------------------------------------------------------
# Verify YuhuProfileNullCast flag toggles effectively.
# ---------------------------------------------------------------------
for want in false true; do
    [ "$want" = false ] && sign=- || sign=+
    "$JAVA" "${COMMON_YUHU_FLAGS[@]}" "-XX:${sign}YuhuProfileNullCast" \
        -XX:+PrintFlagsFinal -version 2>&1 \
        | grep -E 'YuhuProfileNullCast[[:space:]]+:?=' > "$TMP_DIR/flags_$want.log"
    if ! grep -qE "YuhuProfileNullCast[[:space:]]+:?=[[:space:]]+$want" "$TMP_DIR/flags_$want.log"; then
        echo "FAIL: YuhuProfileNullCast did not take effective value $want:"
        cat "$TMP_DIR/flags_$want.log"
        exit 1
    fi
done
echo "Flag check: YuhuProfileNullCast toggles confirmed effective"
echo ""

# ---------------------------------------------------------------------
# Scenario 2: CONTROL — Yuhu ON, -YuhuProfileNullCast (off).
# Expect: null-check blocks (zero:) PRESENT in tier-6 IR.
# ---------------------------------------------------------------------
LOG2="$TMP_DIR/scenario2.log"
IR2_DIR="$TMP_DIR/ir_off"
mkdir -p "$IR2_DIR"
echo "=== [2/5] CONTROL: -YuhuProfileNullCast ==="
wipe_ir
"$JAVA" "${COMMON_YUHU_FLAGS[@]}" -XX:-YuhuProfileNullCast \
    -cp . com.example.NullCastProfileTest > "$LOG2" 2>&1
rc=$?
while IFS= read -r f; do mv -f "$f" "$IR2_DIR/" 2>/dev/null; done < <(ir_glob)
pass_fail "null-cast profile off (control)" "$LOG2" $rc || overall_rc=1

# Tier-6 compile must have fired for run().
if grep -qE ' 6[[:space:]].*com\.example\.NullCastProfileTest::run' "$LOG2"; then
    echo "PASS: run() compiled at tier-6 (control)"
else
    echo "FAIL: run() never reached tier-6 (control)"
    grep -E 'NullCastProfileTest::run' "$LOG2" | head -5
    overall_rc=1
fi

# Control IR: MUST have zero: blocks (null-check throw path present).
CTRL_IR=$(find "$IR2_DIR" -name '*.ll' | sort | head -1)
if [ -z "$CTRL_IR" ]; then
    echo "FAIL: no IR files captured from control run"
    overall_rc=1
else
    CTRL_ZERO=$(grep -cE "^zero[0-9]*:" "$CTRL_IR" || true)
    if [ "$CTRL_ZERO" -ge 1 ]; then
        echo "PASS: control IR has null-check zero: blocks (count=$CTRL_ZERO)"
    else
        echo "FAIL: control IR missing expected zero: null-check blocks"
        overall_rc=1
    fi
    # Control must NOT have checkcast_null_deopt speculation blocks.
    if grep -q "checkcast_null_deopt" "$CTRL_IR"; then
        echo "FAIL: control IR unexpectedly has checkcast_null_deopt (speculation should be off)"
        overall_rc=1
    else
        echo "PASS: control IR has no checkcast_null_deopt (speculation inactive)"
    fi
fi
echo ""

# ---------------------------------------------------------------------
# Scenario 3: TEST — Yuhu ON, +YuhuProfileNullCast.
# Expect: null-check blocks (zero:) ABSENT, replaced by checkcast_null_deopt.
# Oracle values must match control.
# ---------------------------------------------------------------------
LOG3="$TMP_DIR/scenario3.log"
IR3_DIR="$TMP_DIR/ir_on"
mkdir -p "$IR3_DIR"
echo "=== [3/5] TEST: +YuhuProfileNullCast ==="
wipe_ir
"$JAVA" "${COMMON_YUHU_FLAGS[@]}" -XX:+YuhuProfileNullCast \
    -cp . com.example.NullCastProfileTest > "$LOG3" 2>&1
rc=$?
while IFS= read -r f; do mv -f "$f" "$IR3_DIR/" 2>/dev/null; done < <(ir_glob)
pass_fail "null-cast profile on (test)" "$LOG3" $rc || overall_rc=1

# Tier-6 compile must have fired.
if grep -qE ' 6[[:space:]].*com\.example\.NullCastProfileTest::run' "$LOG3"; then
    echo "PASS: run() compiled at tier-6 (test)"
else
    echo "FAIL: run() never reached tier-6 (test)"
    overall_rc=1
fi

# Test IR: zero: blocks must be ABSENT (null-check eliminated by speculation).
TEST_IR=$(find "$IR3_DIR" -name '*.ll' | sort | head -1)
if [ -z "$TEST_IR" ]; then
    echo "FAIL: no IR files captured from test run"
    overall_rc=1
else
    TEST_ZERO=$(grep -cE "^zero[0-9]*:" "$TEST_IR" || true)
    if [ "$TEST_ZERO" -eq 0 ]; then
        echo "PASS: test IR has ZERO null-check zero: blocks (speculation eliminated them)"
    else
        echo "FAIL: test IR still has zero: null-check blocks (count=$TEST_ZERO)"
        grep -nE "^zero[0-9]*:" "$TEST_IR" | head -5
        overall_rc=1
    fi

    # Test IR MUST have checkcast_null_deopt (speculation path).
    if grep -q "checkcast_null_deopt" "$TEST_IR"; then
        echo "PASS: test IR has checkcast_null_deopt speculation blocks"
    else
        echo "FAIL: test IR missing checkcast_null_deopt speculation blocks"
        overall_rc=1
    fi

    # Test IR MUST call __llvm_deoptimize (the deopt mechanism).
    DEOPT_CALLS=$(grep -c "__llvm_deoptimize" "$TEST_IR" || true)
    if [ "$DEOPT_CALLS" -ge 2 ]; then
        echo "PASS: test IR has __llvm_deoptimize calls (count=$DEOPT_CALLS, expect >=2)"
    else
        echo "FAIL: test IR has fewer than 2 __llvm_deoptimize calls (count=$DEOPT_CALLS)"
        overall_rc=1
    fi
fi

# Numerical oracle: both runs must print ALL OK.
if grep -q "ALL OK" "$LOG2" && grep -q "ALL OK" "$LOG3"; then
    echo "PASS: oracle equality (both runs print ALL OK)"
else
    echo "FAIL: oracle mismatch"
    overall_rc=1
fi
echo ""

# ---------------------------------------------------------------------
# Scenario 4: +YuhuProfileNullCast +DeoptimizeALot stress.
# Correctness must survive forced deopt churn.
# ---------------------------------------------------------------------
LOG4="$TMP_DIR/scenario4.log"
echo "=== [4/5] +YuhuProfileNullCast +DeoptimizeALot ==="
wipe_ir
"$JAVA" "${COMMON_YUHU_FLAGS[@]}" -XX:+YuhuProfileNullCast -XX:+DeoptimizeALot \
    -cp . com.example.NullCastProfileTest > "$LOG4" 2>&1
rc=$?
pass_fail "null-cast + DeoptimizeALot" "$LOG4" $rc || overall_rc=1
echo ""

# ---------------------------------------------------------------------
# Scenario 5: Tier-5 negative control.
# With ceiling at 5, speculation MUST NOT fire (gated on comp_level==6).
# Expect: zero: blocks PRESENT (same as control).
# ---------------------------------------------------------------------
LOG5="$TMP_DIR/scenario5.log"
IR5_DIR="$TMP_DIR/ir_tier5"
mkdir -p "$IR5_DIR"
echo "=== [5/5] Tier-5 negative control: +YuhuProfileNullCast, stop at 5 ==="
wipe_ir
"$JAVA" "${TIER5_YUHU_FLAGS[@]}" -XX:+YuhuProfileNullCast \
    -cp . com.example.NullCastProfileTest > "$LOG5" 2>&1
rc=$?
while IFS= read -r f; do mv -f "$f" "$IR5_DIR/" 2>/dev/null; done < <(ir_glob)
pass_fail "tier-5 negative" "$LOG5" $rc || overall_rc=1

# Tier-5 IR: zero: blocks must be PRESENT (speculation gated at tier 6).
TIER5_IR=$(find "$IR5_DIR" -name '*.ll' | sort | head -1)
if [ -n "$TIER5_IR" ]; then
    TIER5_ZERO=$(grep -cE "^zero[0-9]*:" "$TIER5_IR" || true)
    if [ "$TIER5_ZERO" -ge 1 ]; then
        echo "PASS: tier-5 IR has null-check zero: blocks (speculation suppressed: count=$TIER5_ZERO)"
    else
        echo "FAIL: tier-5 IR missing expected zero: blocks — speculation may have fired below tier-6"
        overall_rc=1
    fi
else
    echo "WARN: no tier-5 IR captured (compilation may not have triggered)"
fi
echo ""

# ---------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------
echo "==========================================="
if [ "$overall_rc" -eq 0 ]; then
    echo "ALL SCENARIOS PASSED"
else
    echo "SOME SCENARIOS FAILED (rc=$overall_rc)"
fi
echo "==========================================="
exit $overall_rc

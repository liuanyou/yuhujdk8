#!/bin/bash
# Yuhu Increment 2 extension: profile-driven devirtualization of
# _invokeinterface sites.
#
# The fixture loads BOTH Shape impls (Circle, Square) up front and warms
# runShape monomorphically on Circle, so:
#   - tier-6 substitutes Circle.area behind a receiver-klass guard even
#     though Shape.area has two loaded impls -- which means the binding is
#     the PROFILE path, not CHA (find_unique_concrete_method(Shape,area)
#     returns NULL here, and improve_virtual_call would leave the site
#     virtual).
#   - the guard-fail edge (runShape(square)) runs on the ORIGINAL,
#     never-invalidated nmethod, so the guard machine code is genuinely
#     executed.  This is the point of the DevirtTest-style fixture: an
#     early Class.forName would instead flush the nmethod and skip the
#     guard entirely.
#
# Scenarios:
#   [1]  stock JVM baseline (fixture oracle sums self-consistent).
#   [1b] javap precondition: runShape must call invokeinterface Shape.area.
#   [2]  -YuhuDevirtualize control: correct, and NO devirt child / trace.
#   [3]  +YuhuDevirtualize: correct + tier-6 + depth=1 Circle.area child
#        + "[Yuhu] Inlining area"; guard-fail (Square) result must match
#        the control's refSquare exactly.
#   [4]  +YuhuDevirtualize +DeoptimizeALot: guard/trap path survives
#        constant forced deopts.

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
JAVAP="$JDK_BIN/javap"

echo "Using JDK at: $JDK_BIN"
cd "$SCRIPT_DIR"

rm -f com/example/InterfaceDevirtTest*.class
"$JAVAC" com/example/InterfaceDevirtTest.java || { echo "javac failed"; exit 1; }

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
    if grep -q "InterfaceDevirtTest: ALL OK" "$log" && [ "$rc" -eq 0 ]; then
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
# Scenario 1: stock JVM, no Yuhu.  Fixture self-validation.
# ---------------------------------------------------------------------
LOG1="$TMP_DIR/scenario1.log"
echo "=== [1/4] Baseline (stock interpreter + default tiering) ==="
"$JAVA" -cp . com.example.InterfaceDevirtTest > "$LOG1" 2>&1
rc=$?
pass_fail "baseline" "$LOG1" $rc || overall_rc=1
echo ""

# ---------------------------------------------------------------------
# Scenario 1b: fixture precondition - the call site MUST be an
# invokeinterface bytecode (an abstract-class receiver would emit
# invokevirtual and silently test the wrong path).
# ---------------------------------------------------------------------
echo "=== [1b] Fixture precondition: invokeinterface in runShape ==="
"$JAVAP" -c -p com.example.InterfaceDevirtTest > "$TMP_DIR/javap.log" 2>&1
if grep -q 'invokeinterface.*Shape.area' "$TMP_DIR/javap.log"; then
    echo "PASS: runShape calls invokeinterface Shape.area"
else
    echo "FAIL: no invokeinterface Shape.area; fixture does not exercise the interface path"
    grep -n 'area' "$TMP_DIR/javap.log" | head -5
    overall_rc=1
fi
echo ""

# ---------------------------------------------------------------------
# Verify YuhuDevirtualize actually toggles before treating any A/B
# difference as evidence.
# ---------------------------------------------------------------------
for want in false true; do
    [ "$want" = false ] && sign=- || sign=+
    "$JAVA" "${COMMON_YUHU_FLAGS[@]}" "-XX:${sign}YuhuDevirtualize" \
        -XX:+PrintFlagsFinal -version 2>&1 \
        | grep -E 'YuhuDevirtualize[[:space:]]+:?=' > "$TMP_DIR/flags_$want.log"
    if ! grep -qE "YuhuDevirtualize[[:space:]]+:?=[[:space:]]+$want" "$TMP_DIR/flags_$want.log"; then
        echo "FAIL: YuhuDevirtualize did not take effective value $want:"
        cat "$TMP_DIR/flags_$want.log"
        exit 1
    fi
done
echo "Flag check: YuhuDevirtualize toggles confirmed effective"
echo ""

# ---------------------------------------------------------------------
# Scenario 2: A/B control - Yuhu ON, profile devirtualization OFF.
# Correctness holds; devirt-specific trace must be absent.
# ---------------------------------------------------------------------
LOG2="$TMP_DIR/scenario2.log"
echo "=== [2/4] Yuhu ON, -YuhuDevirtualize (control) ==="
"$JAVA" "${COMMON_YUHU_FLAGS[@]}" -XX:-YuhuDevirtualize \
    -cp . com.example.InterfaceDevirtTest > "$LOG2" 2>&1
rc=$?
pass_fail "interface off (control)" "$LOG2" $rc || overall_rc=1

# Literals copied verbatim from yuhuInlineTree.hpp print() and
# yuhuTopLevelBlock.cpp do_call.
if grep -q 'depth=1 .*InterfaceDevirtTest\$Circle.area' "$LOG2"; then
    echo "FAIL: devirtualized inline-tree child present with -YuhuDevirtualize"
    overall_rc=1
else
    echo "PASS: no devirtualized child (control)"
fi
if grep -q '\[Yuhu\] Inlining area' "$LOG2"; then
    echo "FAIL: '[Yuhu] Inlining area' trace present with -YuhuDevirtualize"
    overall_rc=1
else
    echo "PASS: no interface-inlining trace (control)"
fi
echo ""

# ---------------------------------------------------------------------
# Scenario 3: profile devirtualization ON.  Positive proof + the key
# claim that this is the profile path, not CHA.
# ---------------------------------------------------------------------
LOG3="$TMP_DIR/scenario3.log"
echo "=== [3/4] Yuhu ON, +YuhuDevirtualize (interface devirt) ==="
"$JAVA" "${COMMON_YUHU_FLAGS[@]}" -XX:+YuhuDevirtualize \
    -cp . com.example.InterfaceDevirtTest > "$LOG3" 2>&1
rc=$?
pass_fail "interface devirt correctness (incl guard-fail Square)" "$LOG3" $rc || overall_rc=1

if grep -qE ' 6[[:space:]]+com\.example\.InterfaceDevirtTest::runShape' "$LOG3"; then
    echo "PASS: runShape compiled at tier-6"
else
    echo "FAIL: runShape never reached tier-6"
    grep -E 'InterfaceDevirtTest::runShape' "$LOG3" | head -5
    overall_rc=1
fi

# Positive proof the planner recorded the devirtualized interface child:
# Circle.area bound behind a guard EVEN THOUGH Square (2nd impl) is loaded
# up front.  CHA would refuse here (Shape.area is bimorphic), so seeing
# this child is also proof the substitution is profile-driven.
if grep -q 'depth=1 .*InterfaceDevirtTest\$Circle.area' "$LOG3"; then
    echo "PASS: inline tree shows devirtualized Circle.area child (profile path, not CHA)"
else
    echo "FAIL: inline tree has no depth=1 Circle.area child (substitution did not fire)"
    grep -A6 'Inline tree for com.example.InterfaceDevirtTest.runShape' "$LOG3" | head -20
    overall_rc=1
fi

if grep -q '\[Yuhu\] Inlining area at bci' "$LOG3"; then
    echo "PASS: do_call inlined the devirtualized target"
else
    echo "FAIL: do_call never inlined area (guard/inline path not taken)"
    overall_rc=1
fi

# The fixture's guard-fail check("guard-fail#", s, refSquare(1000)) is
# the real proof the Square trap edge executed on the guarded nmethod;
# an "ALL OK" here (vs a FAIL exit) means it produced the exact
# Square reference sum, matching the control's value.  Both runs are the
# same deterministic oracle, so equality is: both printed ALL OK.
if grep -q "InterfaceDevirtTest: ALL OK" "$LOG2" && grep -q "InterfaceDevirtTest: ALL OK" "$LOG3"; then
    echo "PASS: control and devirt runs agree on all reference sums"
else
    echo "FAIL: control/devirt runs disagree"
    overall_rc=1
fi
echo ""

# ---------------------------------------------------------------------
# Scenario 4: DeoptimizeALot stress - the guard-fail reinterpret path and
# the statepoint/deopt bookkeeping survive constant forced deopts.
# ---------------------------------------------------------------------
LOG4="$TMP_DIR/scenario4.log"
echo "=== [4/4] +YuhuDevirtualize +DeoptimizeALot stress ==="
"$JAVA" "${COMMON_YUHU_FLAGS[@]}" -XX:+YuhuDevirtualize -XX:+DeoptimizeALot \
    -cp . com.example.InterfaceDevirtTest > "$LOG4" 2>&1
rc=$?
pass_fail "interface devirt + DeoptimizeALot" "$LOG4" $rc || overall_rc=1
echo ""

if [ "$overall_rc" -eq 0 ]; then
    echo "=========================================="
    echo "All invokeinterface devirtualization scenarios PASS."
    echo "=========================================="
else
    echo "=========================================="
    echo "At least one interface-devirt scenario FAILED."
    echo "=========================================="
fi
exit $overall_rc

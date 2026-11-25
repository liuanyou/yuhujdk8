#!/bin/bash
# Yuhu tier-6 two-way branch profile consumption (Option B):
# profile-driven predicate-invert + successor-swap + LLVM !prof
# branch_weights metadata.
#
# What we can grep in dumped LLVM IR:
#   - "branch_weights" string literal, present iff the helper emits the
#     metadata.  Under -YuhuBranchProbabilities the helper early-outs, so
#     the string must not appear anywhere in run()'s IR.  Under
#     +YuhuBranchProbabilities and mature tier-3 profile, it must appear
#     at least twice (once per asymmetric target branch, plus once per
#     noise branch that also has a BranchData slice).
#
# Yuhu writes IR files under a name like
#   _tmp_yuhu_ir_RS4GC_<mangled-func-name>.ll
# in the java process's CWD.  The leading "/tmp/yuhu_ir_RS4GC" is
# sanitized in saveIRToFile (yuhuIRTransformer.cpp ~line 118-122) which
# replaces ':' '/' ' ' with '_', so the initial '/' becomes '_'.  Files
# therefore land beside the java invocation, not under /tmp.
#
# Scenarios:
#   [1] stock JVM baseline (fixture oracle self-consistent).
#   [2] -YuhuBranchProbabilities control: correct + zero branch_weights
#       occurrences in the dumped IR.
#   [3] +YuhuBranchProbabilities test: correct + >=1 branch_weights
#       occurrence + numeric equality against control's oracle.
#   [4] +YuhuBranchProbabilities +DeoptimizeALot stress.
#   [5] Fast-tier negative control: +YuhuBranchProbabilities with the
#       tier ceiling at 5 must emit ZERO branch_weights metadata, because
#       branch_profile() is gated on CompLevel_yuhu_optimized.  Before
#       that gate, level 5 produced IR identical to level 6.

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

rm -f com/example/BranchProfileTest*.class
"$JAVAC" com/example/BranchProfileTest.java || { echo "javac failed"; exit 1; }

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"; rm -f "$SCRIPT_DIR"/_tmp_yuhu_ir_*BranchProfileTest*.ll' EXIT

COMMON_YUHU_FLAGS=(
    -XX:+UseYuhuInt
    -XX:+UseYuhuCompiler
    -XX:TieredStopAtLevel=6
    -XX:-BackgroundCompilation
    -XX:+UnlockDiagnosticVMOptions
    -XX:+YuhuDumpIRToFile
    -XX:+PrintCompilation
)

# Scenario 5 arm: same flags, ceiling pulled down to the fast tier.
# Derived from COMMON_YUHU_FLAGS rather than restated, so the two arms
# cannot drift apart when flags are added.
TIER5_YUHU_FLAGS=()
for _f in "${COMMON_YUHU_FLAGS[@]}"; do
    case "$_f" in
        -XX:TieredStopAtLevel=*) _f="-XX:TieredStopAtLevel=5" ;;
    esac
    TIER5_YUHU_FLAGS+=("$_f")
done

# glob helper: expand "any IR file for BranchProfileTest.run" safely
# (empty result returns nothing, no zsh/bash abort).
ir_glob() {
    find "$SCRIPT_DIR" -maxdepth 1 -type f \
        -name '_tmp_yuhu_ir_*BranchProfileTest*run*.ll' 2>/dev/null
}

wipe_ir() {
    while IFS= read -r f; do rm -f "$f"; done < <(ir_glob)
}

pass_fail() {
    local tag="$1"; local log="$2"; local rc="$3"
    if grep -q "BranchProfileTest: ALL OK" "$log" && [ "$rc" -eq 0 ]; then
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
echo "=== [1/5] Baseline (stock interpreter + default tiering) ==="
wipe_ir
"$JAVA" -cp . com.example.BranchProfileTest > "$LOG1" 2>&1
rc=$?
pass_fail "baseline" "$LOG1" $rc || overall_rc=1
echo ""

# ---------------------------------------------------------------------
# Verify YuhuBranchProbabilities actually toggles.
# ---------------------------------------------------------------------
for want in false true; do
    [ "$want" = false ] && sign=- || sign=+
    "$JAVA" "${COMMON_YUHU_FLAGS[@]}" "-XX:${sign}YuhuBranchProbabilities" \
        -XX:+PrintFlagsFinal -version 2>&1 \
        | grep -E 'YuhuBranchProbabilities[[:space:]]+:?=' > "$TMP_DIR/flags_$want.log"
    if ! grep -qE "YuhuBranchProbabilities[[:space:]]+:?=[[:space:]]+$want" "$TMP_DIR/flags_$want.log"; then
        echo "FAIL: YuhuBranchProbabilities did not take effective value $want:"
        cat "$TMP_DIR/flags_$want.log"
        exit 1
    fi
done
echo "Flag check: YuhuBranchProbabilities toggles confirmed effective"
echo ""

# ---------------------------------------------------------------------
# Scenario 2: A/B control - Yuhu ON, branch profile consumption OFF.
# ---------------------------------------------------------------------
LOG2="$TMP_DIR/scenario2.log"
IR2="$TMP_DIR/ir_off"
mkdir -p "$IR2"
echo "=== [2/5] Yuhu ON, -YuhuBranchProbabilities (control) ==="
wipe_ir
"$JAVA" "${COMMON_YUHU_FLAGS[@]}" -XX:-YuhuBranchProbabilities \
    -cp . com.example.BranchProfileTest > "$LOG2" 2>&1
rc=$?
# Move any produced IR into TMP_DIR so subsequent scenarios don't reuse it.
while IFS= read -r f; do mv -f "$f" "$IR2/" 2>/dev/null; done < <(ir_glob)
pass_fail "branch profile off (control)" "$LOG2" $rc || overall_rc=1

# tier-6 compile MUST have fired for run(); otherwise the metadata-grep
# absence is meaningless (nothing was compiled by Yuhu at level 6).
if grep -qE ' 6[[:space:]]+com\.example\.BranchProfileTest::run' "$LOG2"; then
    echo "PASS: run() compiled at tier-6 (control)"
else
    echo "FAIL: run() never reached tier-6 (control); no positive proof possible"
    grep -E 'BranchProfileTest::run' "$LOG2" | head -5
    overall_rc=1
fi

# IR files must exist; otherwise the negative grep below is a false PASS.
n_ir2=$(find "$IR2" -type f -name '*.ll' | wc -l | tr -d ' ')
if [ "$n_ir2" -ge 1 ]; then
    echo "PASS: at least one IR file captured from control run (count=$n_ir2)"
else
    echo "FAIL: no IR files captured from control run -- YuhuDumpIRToFile may not be producing files"
    overall_rc=1
fi

# The literal string is "branch_weights" in LLVM IR metadata defs.
if find "$IR2" -type f -name '*.ll' -exec grep -l '"branch_weights"' {} + 2>/dev/null | grep -q .; then
    echo "FAIL: 'branch_weights' string present in control IR"
    find "$IR2" -type f -name '*.ll' -exec grep -Hn '"branch_weights"' {} + | head -5
    overall_rc=1
else
    echo "PASS: no branch_weights metadata in control IR"
fi
echo ""

# ---------------------------------------------------------------------
# Scenario 3: branch profile ON.  Positive proof via metadata presence.
# ---------------------------------------------------------------------
LOG3="$TMP_DIR/scenario3.log"
IR3="$TMP_DIR/ir_on"
mkdir -p "$IR3"
echo "=== [3/5] Yuhu ON, +YuhuBranchProbabilities (branch profile) ==="
wipe_ir
"$JAVA" "${COMMON_YUHU_FLAGS[@]}" -XX:+YuhuBranchProbabilities \
    -cp . com.example.BranchProfileTest > "$LOG3" 2>&1
rc=$?
while IFS= read -r f; do mv -f "$f" "$IR3/" 2>/dev/null; done < <(ir_glob)
pass_fail "branch profile correctness" "$LOG3" $rc || overall_rc=1

if grep -qE ' 6[[:space:]]+com\.example\.BranchProfileTest::run' "$LOG3"; then
    echo "PASS: run() compiled at tier-6"
else
    echo "FAIL: run() never reached tier-6"
    grep -E 'BranchProfileTest::run' "$LOG3" | head -5
    overall_rc=1
fi

n_ir3=$(find "$IR3" -type f -name '*.ll' | wc -l | tr -d ' ')
if [ "$n_ir3" -ge 1 ]; then
    echo "PASS: at least one IR file captured from devirt run (count=$n_ir3)"
else
    echo "FAIL: no IR files captured from devirt run"
    overall_rc=1
fi

BW_HITS=$(find "$IR3" -type f -name '*.ll' -exec grep -h '"branch_weights"' {} + 2>/dev/null | wc -l | tr -d ' ')
if [ "${BW_HITS:-0}" -ge 1 ]; then
    echo "PASS: branch_weights metadata present in IR (hits=$BW_HITS)"
    find "$IR3" -type f -name '*.ll' -exec grep -h '"branch_weights"' {} + 2>/dev/null | head -3
else
    echo "FAIL: branch_weights metadata absent from IR despite +YuhuBranchProbabilities"
    overall_rc=1
fi

# Numeric equality: fixture's internal check() calls assert got==want on
# every iteration, so any divergence between flag settings aborts with
# FAIL <tag>: ... .  Both runs printing ALL OK proves the hint did not
# change semantics.
if grep -q "BranchProfileTest: ALL OK" "$LOG2" && grep -q "BranchProfileTest: ALL OK" "$LOG3"; then
    echo "PASS: control and devirt runs agree on all oracle sums"
else
    echo "FAIL: control/devirt runs disagree"
    overall_rc=1
fi
echo ""

# ---------------------------------------------------------------------
# Scenario 4: DeoptimizeALot stress.  Hint-driven layout must survive
# forced deopts.
# ---------------------------------------------------------------------
LOG4="$TMP_DIR/scenario4.log"
echo "=== [4/5] +YuhuBranchProbabilities +DeoptimizeALot stress ==="
wipe_ir
"$JAVA" "${COMMON_YUHU_FLAGS[@]}" -XX:+YuhuBranchProbabilities -XX:+DeoptimizeALot \
    -cp . com.example.BranchProfileTest > "$LOG4" 2>&1
rc=$?
wipe_ir
pass_fail "branch profile + DeoptimizeALot" "$LOG4" $rc || overall_rc=1
echo ""

# ---------------------------------------------------------------------
# Scenario 5: fast-tier negative control.  branch_profile() bails unless
# env()->comp_level() == CompLevel_yuhu_optimized, so the same bytecode
# compiled with the ceiling at 5 must carry no profile metadata at all
# even with +YuhuBranchProbabilities explicitly ON.  Locks down the rule
# that profiling consumption is an optimized-tier decision.
#
# Non-vacuity is asserted first: run() must really have compiled at level
# 5 and really have produced IR dumps, otherwise "zero hits" would only
# mean Yuhu never ran.
# ---------------------------------------------------------------------
LOG5="$TMP_DIR/scenario5.log"
IR5="$TMP_DIR/ir_tier5"
mkdir -p "$IR5"
echo "=== [5/5] Fast-tier negative control (+YuhuBranchProbabilities, StopAtLevel=5) ==="
wipe_ir
"$JAVA" "${TIER5_YUHU_FLAGS[@]}" -XX:+YuhuBranchProbabilities \
    -cp . com.example.BranchProfileTest > "$LOG5" 2>&1
rc=$?
while IFS= read -r f; do mv -f "$f" "$IR5/" 2>/dev/null; done < <(ir_glob)
pass_fail "tier-5 run produces correct results" "$LOG5" $rc || overall_rc=1

if grep -qE ' 5[[:space:]]+com\.example\.BranchProfileTest::run' "$LOG5"; then
    echo "PASS: run() compiled at tier-5 (control is not vacuous)"
else
    echo "FAIL: run() never compiled at tier-5; this arm proves nothing"
    grep -E 'BranchProfileTest::run' "$LOG5" | head -5
    overall_rc=1
fi

n_ir5=$(find "$IR5" -type f -name '*.ll' | wc -l | tr -d ' ')
if [ "$n_ir5" -ge 1 ]; then
    echo "PASS: IR files captured from tier-5 run (count=$n_ir5)"
else
    echo "FAIL: no IR files captured from tier-5 run -- absence of metadata is vacuous"
    overall_rc=1
fi

if find "$IR5" -type f -name '*.ll' -exec grep -l '"branch_weights"' {} + 2>/dev/null | grep -q .; then
    echo "FAIL: tier-5 IR carries branch_weights despite the level-6 gate"
    find "$IR5" -type f -name '*.ll' -exec grep -Hn '"branch_weights"' {} + | head -5
    overall_rc=1
else
    echo "PASS: no branch_weights metadata in tier-5 IR"
fi

# Semantics must not depend on the tier either.
if grep -q "BranchProfileTest: ALL OK" "$LOG3" && grep -q "BranchProfileTest: ALL OK" "$LOG5"; then
    echo "PASS: tier-5 and tier-6 runs agree on all oracle sums"
else
    echo "FAIL: tier-5/tier-6 runs disagree"
    overall_rc=1
fi
echo ""

if [ "$overall_rc" -eq 0 ]; then
    echo "=========================================="
    echo "All two-way branch profile scenarios PASS."
    echo "=========================================="
else
    echo "=========================================="
    echo "At least one branch-profile scenario FAILED."
    echo "=========================================="
fi
exit $overall_rc

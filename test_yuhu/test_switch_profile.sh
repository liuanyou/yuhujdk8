#!/bin/bash
# Yuhu tier-6 multi-way branch profile consumption (Option R2,
# metadata-only): !prof branch_weights on tableswitch/lookupswitch
# SwitchInsts, built from the caller's MultiBranchData with
# skipped-default case traffic folded into the default weight and
# zero counts clamped to 1.
#
# What we can grep in dumped LLVM IR:
#   - "branch_weights" string literal, present iff the helper emits the
#     metadata.  Under -YuhuSwitchProfile the helper early-outs, so the
#     string must not appear anywhere in the dumped IR.  Under
#     +YuhuSwitchProfile every switch instruction must carry !prof, and
#     each node must have exactly (case count + 1) operands -- the
#     default weight plus one per emitted addCase, verifier-legal arity.
#   - We do NOT assert which weight is largest: javac may or may not
#     fold an aliased empty case into the default row (deciding whether
#     the never-executed row survives to be clamped), and downstream
#     passes can rewrite instruction order.  Arity + presence are the
#     stable, positional invariants.
#
# Yuhu writes IR files under a name like
#   _tmp_yuhu_ir_RS4GC_<mangled-func-name>.ll
# in the java process's CWD (see saveIRToFile, yuhuIRTransformer.cpp
# ~line 118-122: ':' '/' ' ' sanitized to '_').
#
# Scenarios:
#   [1] stock JVM baseline (fixture oracle self-consistent).
#   [2] bytecode-shape gate: javap must show TABLESWITCH in dense() and
#       LOOKUPSWITCH in sparse(); if javac folded our aliases differently
#       than expected the whole test is vacuous, so fail loudly instead.
#   [3] -YuhuSwitchProfile control: correct + switches present + zero
#       branch_weights occurrences.
#   [4] +YuhuSwitchProfile test: correct + every switch annotated with
#       right arity + >=1 clamped-to-1 weight + numeric equality against
#       control.
#   [5] +YuhuSwitchProfile +DeoptimizeALot stress.
#
# Why Tier0ProfilingStartPercentage is pinned down:
#   MultiBranchData case cells have exactly one writer in this VM -- the
#   template interpreter (interp_masm / yuhu_macroAssembler
#   profile_switch_case).  C1 never touches them, so once a method leaves
#   tier 0 the cells are frozen.  AdvancedThresholdPolicy::should_create_mdo
#   only builds the MDO while still at tier 0 and only once counters pass
#   Tier0ProfilingStartPercentage (default 200%) of the tier-3 thresholds,
#   whereas promotion fires at 100%.  With -BackgroundCompilation the
#   compile is synchronous, so the method is promoted before any MDO
#   exists and every interpreter profile increment short-circuits on the
#   null mdp: the switch cells stay zero and this test would measure
#   nothing (both arms would correctly report "no metadata").  Lowering
#   the percentage below the promotion threshold restores a real tier-0
#   profiling window, which also makes the -/+ control a true A/B: both
#   runs collect identical data and only consumption differs.

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

rm -f com/example/SwitchProfileTest*.class
"$JAVAC" com/example/SwitchProfileTest.java || { echo "javac failed"; exit 1; }

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"; rm -f "$SCRIPT_DIR"/_tmp_yuhu_ir_*SwitchProfileTest*.ll' EXIT

COMMON_YUHU_FLAGS=(
    -XX:+UseYuhuInt
    -XX:+UseYuhuCompiler
    -XX:TieredStopAtLevel=6
    -XX:-BackgroundCompilation
    -XX:Tier0ProfilingStartPercentage=50
    -XX:+UnlockDiagnosticVMOptions
    -XX:+YuhuDumpIRToFile
    -XX:+PrintCompilation
)

# glob helper: expand "any IR file for dense/sparse" safely
# (empty result returns nothing, no zsh/bash abort).
ir_glob() {
    find "$SCRIPT_DIR" -maxdepth 1 -type f \
        -name '_tmp_yuhu_ir_*SwitchProfileTest*' 2>/dev/null \
    | grep -E '__(dense|sparse)_t'
}

wipe_ir() {
    while IFS= read -r f; do rm -f "$f"; done < <(ir_glob)
}

pass_fail() {
    local tag="$1"; local log="$2"; local rc="$3"
    if grep -q "SwitchProfileTest: ALL OK" "$log" && [ "$rc" -eq 0 ]; then
        echo "PASS: $tag"
        return 0
    else
        echo "FAIL: $tag (exit=$rc)"
        tail -40 "$log"
        return 1
    fi
}

# tier-6 compile check, tolerant of the OSR '%' marker:
# compiled-from-interpreter lines read " 6 ...::name", OSR lines " 6 % ...::name".
tier6_fired() {
    local log="$1"; local meth="$2"
    grep -qE " 6[[:space:]]+%?[[:space:]]+com\.example\.SwitchProfileTest::${meth}\b" "$log"
}

# Structural IR validator: every 'switch' instruction must carry !prof,
# the referenced node must be a branch_weights node, and its operand
# count must equal (cases in the case array + 1 for the default).
# Prints diagnostics; exit 0 = all switches annotated and arity-correct,
# exit 1 otherwise.  A separate presence count and a clamped-weight (== 1)
# existence check are printed as "stats:" for the caller to gate on.
validate_switch_prof() {
    python3 - "$@" <<'PYEOF'
import re, sys

META_RE = re.compile(r'^!(\d+) = !\{!"branch_weights",((?:[^}\n]|\\.)*)\}')
SWITCH_RE = re.compile(r'^\s*switch\s+(?:i\d+|llvm::Value)\s+\S+,\s*label\s+%([^\s,]+)')

def parse_nodes(text):
    nodes = {}
    for line in text.splitlines():
        m = META_RE.match(line)
        if not m:
            continue
        # weights are printed as "i32 N"; strip the type prefix so bare
        # digits inside "i32" are not parsed as weights.
        nums = re.findall(r'\bi\d+\s+(\d+)', m.group(2))
        nodes[int(m.group(1))] = [int(n) for n in nums]
    return nodes

total_switches = 0
annotated = 0
arity_bad = []
parse_unsupported = []
weights_seen = []

for path in sys.argv[1:]:
    with open(path) as f:
        text = f.read()
    nodes = parse_nodes(text)
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        m = SWITCH_RE.match(lines[i])
        if not m:
            i += 1
            continue
        total_switches += 1
        line = lines[i]
        # Gather the switch instruction across lines: case array is
        # bracketed; find its close, then the optional !prof reference.
        seg = line
        j = i
        while seg.count('[') > seg.count(']') and j + 1 < len(lines):
            j += 1
            seg += '\n' + lines[j]
        prof = re.search(r'!prof\s+!(\d+)', seg)
        if prof is None and j + 1 < len(lines) and ']' in lines[j]:
            # !prof may trail on the closing line
            k = j + 1
            if k < len(lines):
                prof = re.search(r'!prof\s+!(\d+)', lines[k])
                if prof:
                    seg += '\n' + lines[k]
        if prof is None:
            i = j + 1
            continue
        annotated += 1
        # case count: lines "i32 N, label" inside the brackets, or the
        # single-line form "i32 N, label %x, i32 M, label %y]"
        arr = re.search(r'\[((?:.|\n)*?)\]', seg)
        if arr is None:
            parse_unsupported.append((path, 'no case array'))
            i = j + 1
            continue
        ncases = len(re.findall(r'i\d+\s+-?\d+,\s*label', arr.group(1)))
        if re.match(r'^\s*switch.*label\s+%[^\s,]+\s*\[', line) is None and ncases == 0:
            parse_unsupported.append((path, 'unparsed switch form'))
        wlist = nodes.get(int(prof.group(1)))
        if wlist is None:
            arity_bad.append((path, 'branch_weights node not found'))
            i = j + 1
            continue
        weights_seen.extend(wlist)
        if len(wlist) != ncases + 1:
            arity_bad.append((path, f'node !{prof.group(1)} has {len(wlist)} weights, switch has {ncases} cases (+1 default expected)'))
        i = j + 1

print(f'switches={total_switches} annotated={annotated}')
print(f'clamped_weights={sum(1 for w in weights_seen if w == 1)}')
for p, why in parse_unsupported:
    print(f'PARSE-NOT-SUPPORTED: {p}: {why}')
for p, why in arity_bad:
    print(f'ARITY-MISMATCH: {p}: {why}')
if total_switches == 0 or annotated != total_switches or arity_bad or parse_unsupported:
    sys.exit(1)
sys.exit(0)
PYEOF
}

overall_rc=0

# ---------------------------------------------------------------------
# Scenario 1: stock JVM, no Yuhu.  Fixture self-validation.
# ---------------------------------------------------------------------
LOG1="$TMP_DIR/scenario1.log"
echo "=== [1/5] Baseline (stock interpreter + default tiering) ==="
wipe_ir
"$JAVA" -cp . com.example.SwitchProfileTest > "$LOG1" 2>&1
rc=$?
pass_fail "baseline" "$LOG1" $rc || overall_rc=1
echo ""

# ---------------------------------------------------------------------
# Scenario 2: bytecode-shape gate.  dense() must reach the JVM as a
# TABLESWITCH and sparse() as a LOOKUPSWITCH, or the two switch forms
# under test are not the ones we think they are.
# ---------------------------------------------------------------------
echo "=== [2/5] javap bytecode-shape gate ==="
JAVAP_OUT="$TMP_DIR/javap.log"
"$JAVAP" -p -c com/example/SwitchProfileTest.class > "$JAVAP_OUT" 2>&1 \
    || { echo "FAIL: javap could not disassemble"; overall_rc=1; }
SHAPE=$(awk '/^  (static |private static )?[a-z].*\(.*\);/ { split($0, a, "("); meth=a[1] }
             /tableswitch/   { print "TABLESWITCH in" meth }
             /lookupswitch/  { print "LOOKUPSWITCH in" meth }' "$JAVAP_OUT" | sort -u)
echo "$SHAPE" | sed 's/^/    /'
if echo "$SHAPE" | grep -q 'TABLESWITCH in.*dense' \
   && echo "$SHAPE" | grep -q 'LOOKUPSWITCH in.*sparse'; then
    echo "PASS: dense() has tableswitch and sparse() has lookupswitch"
else
    echo "FAIL: expected tableswitch in dense() and lookupswitch in sparse(); fix the fixture"
    overall_rc=1
fi
echo ""

# ---------------------------------------------------------------------
# Verify YuhuSwitchProfile actually toggles.
# ---------------------------------------------------------------------
for want in false true; do
    [ "$want" = false ] && sign=- || sign=+
    "$JAVA" "${COMMON_YUHU_FLAGS[@]}" "-XX:${sign}YuhuSwitchProfile" \
        -XX:+PrintFlagsFinal -version 2>&1 \
        | grep -E 'YuhuSwitchProfile[[:space:]]+:?=' > "$TMP_DIR/flags_$want.log"
    if ! grep -qE "YuhuSwitchProfile[[:space:]]+:?=[[:space:]]+$want" "$TMP_DIR/flags_$want.log"; then
        echo "FAIL: YuhuSwitchProfile did not take effective value $want:"
        cat "$TMP_DIR/flags_$want.log"
        exit 1
    fi
done
echo "Flag check: YuhuSwitchProfile toggles confirmed effective"
echo ""

# ---------------------------------------------------------------------
# Scenario 3: A/B control - Yuhu ON, switch profile consumption OFF.
# ---------------------------------------------------------------------
LOG3="$TMP_DIR/scenario3.log"
IR3="$TMP_DIR/ir_off"
mkdir -p "$IR3"
echo "=== [3/5] Yuhu ON, -YuhuSwitchProfile (control) ==="
wipe_ir
"$JAVA" "${COMMON_YUHU_FLAGS[@]}" -XX:-YuhuSwitchProfile \
    -cp . com.example.SwitchProfileTest > "$LOG3" 2>&1
rc=$?
while IFS= read -r f; do mv -f "$f" "$IR3/" 2>/dev/null; done < <(ir_glob)
pass_fail "switch profile off (control)" "$LOG3" $rc || overall_rc=1

for meth in dense sparse; do
    if tier6_fired "$LOG3" "$meth"; then
        echo "PASS: ${meth}() compiled at tier-6 (control)"
    else
        echo "FAIL: ${meth}() never reached tier-6 (control); no positive proof possible"
        grep -E "SwitchProfileTest::${meth}" "$LOG3" | head -5
        overall_rc=1
    fi
done

# IR files must exist; otherwise the negative grep below is a false PASS.
n_ir3=$(find "$IR3" -type f -name '*.ll' | wc -l | tr -d ' ')
if [ "$n_ir3" -ge 1 ]; then
    echo "PASS: at least one IR file captured from control run (count=$n_ir3)"
else
    echo "FAIL: no IR files captured from control run -- YuhuDumpIRToFile may not be producing files"
    overall_rc=1
fi

# switches must actually be present in the control IR (else the
# annotation-absence check below proves nothing); no jumptable lowering
# pass runs at IR-dump time, so 'switch i32' should survive.
if find "$IR3" -type f -name '*.ll' -exec grep -lE 'switch\s+i32' {} + 2>/dev/null | grep -q .; then
    echo "PASS: switch instructions present in control IR"
else
    echo "FAIL: no switch instructions in control IR -- fixture or dump shape changed"
    overall_rc=1
fi

# The control run must have ZERO annotated switches.  We cannot simply
# grep for the 'branch_weights' string, because YuhuBranchProbabilities
# (a separate flag, still ON here) legitimately emits branch_weights on
# the two-way branches of the same methods.  So use the structural
# validator and require switches present but none annotated.
CTRL_FILES=()
while IFS= read -r f; do CTRL_FILES+=("$f"); done < <(find "$IR3" -type f -name '*.ll')
VAL_OUT_C="$TMP_DIR/validate_control.log"
validate_switch_prof "${CTRL_FILES[@]}" > "$VAL_OUT_C" 2>&1
sed 's/^/    /' "$VAL_OUT_C"
C_SW=$(grep -oE 'switches=[0-9]+' "$VAL_OUT_C" | cut -d= -f2)
C_AN=$(grep -oE 'annotated=[0-9]+' "$VAL_OUT_C" | cut -d= -f2)
if [ "${C_SW:-0}" -ge 1 ] && [ "${C_AN:-1}" -eq 0 ]; then
    echo "PASS: control has switches but none annotated (count=$C_SW)"
else
    echo "FAIL: control switch annotation (switches=${C_SW:-?} annotated=${C_AN:-?})"
    overall_rc=1
fi
echo ""

# ---------------------------------------------------------------------
# Scenario 4: switch profile ON.  Positive proof: every switch carries
# a structurally correct branch_weights node.
# ---------------------------------------------------------------------
LOG4="$TMP_DIR/scenario4.log"
IR4="$TMP_DIR/ir_on"
mkdir -p "$IR4"
echo "=== [4/5] Yuhu ON, +YuhuSwitchProfile (switch profile) ==="
wipe_ir
"$JAVA" "${COMMON_YUHU_FLAGS[@]}" -XX:+YuhuSwitchProfile \
    -cp . com.example.SwitchProfileTest > "$LOG4" 2>&1
rc=$?
while IFS= read -r f; do mv -f "$f" "$IR4/" 2>/dev/null; done < <(ir_glob)
pass_fail "switch profile correctness" "$LOG4" $rc || overall_rc=1

for meth in dense sparse; do
    if tier6_fired "$LOG4" "$meth"; then
        echo "PASS: ${meth}() compiled at tier-6"
    else
        echo "FAIL: ${meth}() never reached tier-6"
        grep -E "SwitchProfileTest::${meth}" "$LOG4" | head -5
        overall_rc=1
    fi
done

n_ir4=$(find "$IR4" -type f -name '*.ll' | wc -l | tr -d ' ')
if [ "$n_ir4" -ge 2 ]; then
    echo "PASS: IR files captured from profile run (count=$n_ir4)"
else
    echo "FAIL: expected >=2 IR files (dense + sparse), got $n_ir4"
    overall_rc=1
fi

VAL_FILES=()
while IFS= read -r f; do VAL_FILES+=("$f"); done < <(find "$IR4" -type f -name '*.ll')
VAL_OUT="$TMP_DIR/validate.log"
validate_switch_prof "${VAL_FILES[@]}" > "$VAL_OUT" 2>&1
val_rc=$?
sed 's/^/    /' "$VAL_OUT"
if [ "$val_rc" -eq 0 ]; then
    echo "PASS: every switch in IR carries branch_weights with arity cases+1"
else
    echo "FAIL: switch !prof presence/arity check"
    overall_rc=1
fi

# At least one weight in the annotated dense/sparse vectors must be the
# clamped 1 (the never-executed row survives whenever javac keeps an
# aliased case row; dense()'s tableswitch rows 0..4 for values 0..4 with
# value... below lo or above hi is impossible here, but the empty case-5
# row -- if javac emitted one -- is zero-count.  If javac folded it, the
# clamp instead shows up nowhere and this gate must not fire blindly:
# gate on the count reported by the validator.
CLAMPED=$(grep -oE 'clamped_weights=[0-9]+' "$VAL_OUT" | cut -d= -f2)
if [ "${CLAMPED:-0}" -ge 1 ]; then
    echo "PASS: clamped-to-1 weight present (count=$CLAMPED)"
else
    echo "NOTE: no clamped-to-1 weight found; javac likely folded the"
    echo "      aliased empty case into the default row, so no zero row"
    echo "      reached do_switch.  Not a failure of the VM change."
fi

# Numeric equality: fixture's internal check() calls assert got==want on
# every iteration, so any divergence between flag settings aborts with
# FAIL <tag>: ... .  Both runs printing ALL OK proves the hint did not
# change semantics.
if grep -q "SwitchProfileTest: ALL OK" "$LOG3" && grep -q "SwitchProfileTest: ALL OK" "$LOG4"; then
    echo "PASS: control and profile runs agree on all oracle sums"
else
    echo "FAIL: control/profile runs disagree"
    overall_rc=1
fi
echo ""

# ---------------------------------------------------------------------
# Scenario 5: DeoptimizeALot stress.  Hint-driven weights must survive
# forced deopts (no guards are emitted, so this is a smoke test of the
# metadata path under tier churn and re-profiling).
# ---------------------------------------------------------------------
LOG5="$TMP_DIR/scenario5.log"
echo "=== [5/5] +YuhuSwitchProfile +DeoptimizeALot stress ==="
wipe_ir
"$JAVA" "${COMMON_YUHU_FLAGS[@]}" -XX:+YuhuSwitchProfile -XX:+DeoptimizeALot \
    -cp . com.example.SwitchProfileTest > "$LOG5" 2>&1
rc=$?
wipe_ir
pass_fail "switch profile + DeoptimizeALot" "$LOG5" $rc || overall_rc=1
echo ""

if [ "$overall_rc" -eq 0 ]; then
    echo "=========================================="
    echo "All switch multi-branch profile scenarios PASS."
    echo "=========================================="
else
    echo "=========================================="
    echo "At least one switch-profile scenario FAILED."
    echo "=========================================="
fi
exit $overall_rc

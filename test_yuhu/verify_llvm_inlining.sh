#!/bin/bash
#
# Yuhu Path B (LLVM-IR) inlining verification.
#
# Two independent signals are checked on every _tmp_yuhu_ir_*.ll file:
#
# 1. NEGATIVE check -- "Path B fully flattened" invariant.  Every Path B
#    inlinee llvm::Function is created with InternalLinkage + AlwaysInline
#    by YuhuFunction::build_inlinee; AlwaysInlinerPass + GlobalDCEPass in
#    YuhuCompiler::compile_method must remove every such define before IR
#    is dumped.  So no `define internal ... alwaysinline` block and no
#    `alwaysinline` attribute anywhere in the file should ever appear.
#    Any hit means either AlwaysInlinerPass failed to flatten (which Path
#    B is designed to make impossible) or the file was captured before
#    the pass ran.
#
# 2. POSITIVE check -- "which callees were Path B inlined".  After LLVM
#    flattens an alwaysinline function, the callee's llvm::Function is
#    gone, but LLVM preserves the inline origin through block-name and
#    alloca-name prefixes:
#
#        bci_5:  ; preds = %"com.example.LLVMInlineTest::lambdaApplyOrNeg_t...exit", %entry
#        %extended_sp.i44 = alloca i64, i64 11    <- LLVM-added .i<NN> uniquifier
#
#    So any `<class>::<method>_t<ms>` Java-method symbol appearing ANY-
#    WHERE in the file OTHER than the caller's own top-level define is
#    evidence that method was Path B-inlined.  A cross-method call that
#    was NOT Path B-inlined would show up as a `declare @"<class>::<method>..."`
#    line (stub-based compiled call), which we filter out separately.
#
# Report per file:
#   [INLINED(N)]  -- N distinct Path B inlinees successfully flattened.
#   [NO-PATH-B]   -- file has no evidence of Path B inlining (either the
#                    caller has no branchy small callees, or eligibility
#                    rejected them; not a bug, just no inlining happened).
#   [STUB-CALL(M)]-> M cross-method Yuhu declares; the caller invokes
#                    compiled nmethods via stubs rather than Path B.
#   [BAD]         -- unflattened Path B inlinees present (real bug).
#
# Usage:
#   ./verify_llvm_inlining.sh                 # scan all _tmp_yuhu_ir_*.ll
#   ./verify_llvm_inlining.sh '<glob>'        # subset, e.g. '*LLVMInlineTest*'
#   ./verify_llvm_inlining.sh -v              # verbose: list each callee
#   ./verify_llvm_inlining.sh -v '<glob>'     # both
#   ./verify_llvm_inlining.sh -i              # summary only for files with
#                                             # at least one Path B inlinee
#   ./verify_llvm_inlining.sh --help          # this header

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VERBOSE=0
ONLY_INLINED=0

# --- arg parsing --------------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    -v|--verbose)   VERBOSE=1; shift ;;
    -i|--inlined)   ONLY_INLINED=1; shift ;;
    -h|--help)      sed -n '1,45p' "$0"; exit 0 ;;
    *)              break ;;
  esac
done

PATTERN="${1:-_tmp_yuhu_ir_*.ll}"

# Yuhu-compiled Java-method symbol regex (matches <class>::<method>_t<ms>,
# with optional _v<seq> before _t, and method names that may include `$`,
# `<init>`, `<clinit>`, etc.).
YUHU_SYM_RE='[A-Za-z0-9_.$]+::[A-Za-z0-9<>_$]+(_v[0-9]+)?_t[0-9]+'

TOTAL=0
INLINED=0
NOPATHB=0
BAD=0
INLINED_LIST=()
BAD_LIST=()

echo "=================================================="
echo "Yuhu Path B (LLVM-IR) inlining verification"
echo "=================================================="
echo "Directory:     $SCRIPT_DIR"
echo "Pattern:       $PATTERN"
echo "Verbose:       $([ $VERBOSE -eq 1 ] && echo yes || echo no)"
echo "Only-inlined:  $([ $ONLY_INLINED -eq 1 ] && echo yes || echo no)"
echo ""

shopt -s nullglob
FILES=("$SCRIPT_DIR"/$PATTERN)
shopt -u nullglob

if [ ${#FILES[@]} -eq 0 ]; then
  echo "No IR files matched $SCRIPT_DIR/$PATTERN"
  exit 0
fi

for f in "${FILES[@]}"; do
  TOTAL=$((TOTAL + 1))
  name=$(basename "$f")

  # ---------- (1) Negative: any unflattened alwaysinline / internal define ----------
  defines=$(grep -c '^define ' "$f" 2>/dev/null)
  internal_defines=$(grep -cE '^define (internal|private) ' "$f" 2>/dev/null)
  alwaysinline_hits=$(grep -c 'alwaysinline' "$f" 2>/dev/null)

  # ---------- (2) Positive: extract caller, then remaining symbols ----------
  caller=$(grep -m1 '^define ' "$f" | grep -oE "$YUHU_SYM_RE" | head -1)
  [ -z "$caller" ] && caller="(no Yuhu define found)"

  # All distinct Yuhu symbols anywhere in the file (define line, block
  # labels, pred comments, .i-suffixed allocas that keep the original
  # inlinee function name as a prefix, etc.).
  all_syms=$(grep -oE "$YUHU_SYM_RE" "$f" 2>/dev/null | sort -u)

  # Non-caller Yuhu symbols = Path B inlinees that were flattened.
  if [ "$caller" = "(no Yuhu define found)" ]; then
    inlinees=$(printf '%s' "$all_syms")
  else
    inlinees=$(printf '%s\n' "$all_syms" | grep -vxF "$caller" || true)
  fi
  inline_count=0
  if [ -n "$inlinees" ]; then
    inline_count=$(printf '%s\n' "$inlinees" | grep -c . || true)
  fi

  # Stub-based compiled calls (declare lines with a Yuhu symbol) -- these
  # are the "did NOT go through Path B" siblings, useful context.
  stub_decl=$(grep -E '^declare .+ @"' "$f" 2>/dev/null | grep -cE "$YUHU_SYM_RE" || true)
  [ -z "$stub_decl" ] && stub_decl=0

  # ---------- classify ----------
  status=""
  if [ "$internal_defines" -gt 0 ] || [ "$alwaysinline_hits" -gt 0 ]; then
    BAD=$((BAD + 1))
    BAD_LIST+=("$name")
    status="BAD"
  elif [ "$inline_count" -gt 0 ]; then
    INLINED=$((INLINED + 1))
    INLINED_LIST+=("$name|$inline_count")
    status="INLINED($inline_count)"
  else
    NOPATHB=$((NOPATHB + 1))
    status="NO-PATH-B"
  fi

  # ---------- emit per-file line (honor -i) ----------
  show=1
  [ $ONLY_INLINED -eq 1 ] && [ "$status" = "NO-PATH-B" ] && show=0

  if [ $show -eq 1 ]; then
    printf "  [%-12s]  %-88s  defines=%s  internal=%s  alwaysinline=%s  stub-calls=%s\n" \
           "$status" "$name" "$defines" "$internal_defines" "$alwaysinline_hits" "$stub_decl"
  fi

  if [ "$status" = "BAD" ]; then
    grep -nE '^define (internal|private) ' "$f" | sed -E 's/^/             /' | head -10
    grep -n 'alwaysinline' "$f" | sed -E 's/^/             /' | head -10
  fi

  # ---------- verbose extras ----------
  if [ $VERBOSE -eq 1 ] && [ $show -eq 1 ]; then
    echo "             caller:  $caller"
    if [ "$inline_count" -gt 0 ]; then
      printf '%s\n' "$inlinees" | sed -E 's/^/             inlined: /'
    fi
    if [ "$stub_decl" -gt 0 ]; then
      grep -E '^declare .+ @"' "$f" | grep -E "$YUHU_SYM_RE" \
        | sed -nE 's|^declare [^(]*@"([^"]*)".*|             stub-call: \1|p' | sort -u
    fi
  fi
done

echo ""
echo "=================================================="
echo "Summary"
echo "=================================================="
echo "  Scanned                       : $TOTAL"
echo "  INLINED   (Path B flattened)  : $INLINED"
echo "  NO-PATH-B (nothing to inline) : $NOPATHB"
echo "  BAD       (unflattened left)  : $BAD"

if [ ${#INLINED_LIST[@]} -gt 0 ]; then
  echo ""
  echo "Files with Path B inlining:"
  for e in "${INLINED_LIST[@]}"; do
    n=${e%|*}; c=${e##*|}
    echo "  - $n  ($c inlinee(s))"
  done
fi

if [ $BAD -gt 0 ]; then
  echo ""
  echo "Bad files:"
  for n in "${BAD_LIST[@]}"; do echo "  - $n"; done
  exit 1
fi
exit 0

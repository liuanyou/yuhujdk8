# Activity 087: Tier-6 Codegen Started Consuming MDO Branch and Switch Profiles — `!prof branch_weights` on Conditional Branches and Switches, Restricted to the Optimized Tier

## Date
2026-11-25

## Status
**Implemented, runtime-verified, and locked down by tests.** Both consumption paths are
live behind product flags and covered by `test_yuhu/test_branch_probabilities.sh` (5 scenarios)
and `test_yuhu/test_switch_profile.sh` (6 scenarios), which pass on the
`macosx-aarch64-normal-server-slowdebug` image. The fast tier provably emits no profile
metadata: an intentionally broken copy of each suite exits 1, so the negative controls can fail.

## Problem Description

Yuhu's optimized tier produced IR that was blind to HotSpot's method data. Every Java
conditional branch became the same `CreateCondBr(cond, taken_bb, not_taken_bb)` shape with
the bytecode-order successor first, and every `tableswitch`/`lookupswitch` became a bare
`SwitchInst` with no weights — so LLVM's layout, block-placement and later BPI-driven passes
assumed uniform probabilities, and the tier ladder `0 -> 3 -> 5 -> 6` had no codegen meaning
beyond inlining.

Two increments were designed and approved:

* **Option B** (two-way): read `BranchData` for the branch bci, orient the condition so the
  hot edge is the `br`'s first successor, and attach `!prof branch_weights`.
* **Option R2** (multi-way): read `MultiBranchData` for the switch bci and attach
  `!prof branch_weights` with one weight per successor. Deliberately **metadata-only**: no
  case reordering, because `clusterifySwitch` re-sorts by value and clusters by probability,
  so `addCase` operand order influences nothing once the weights are present.

A third item emerged from measurement (see "The Tier Gate" below): the two consumption
helpers honoured only their product flags, so level 5 emitted byte-identical annotated IR
to level 6.

## Profile Data Path Analysis

### 1. The read path needs no CompileBroker work

`ciMethod::method_data_or_null()` reaches the **live** VM `MethodData` through
`VM_ENTRY_MARK`, and `ciMethodData::load_data()` copies the counter array into a ci-arena
buffer. Reads after that point are plain memory reads on the private copy, so they are safe
on a native-state compiler thread. Calling `load_data()` **directly** from that thread is
not: `translate_from` walks every entry and calls `ciEnv::get_klass`, which asserts
`IS_IN_VM`. Both helpers therefore stop at `method_data_or_null()`.

Slice access follows the fork's idiom: `bci_to_data(bci)` → generated `is_BranchData()` /
`is_MultiBranchData()` virtual → `as_...()`. `ProfileData::tag()` is not publicly readable in
this tree, so any tag comparison is a compile error; the `is_X()/as_X()` pair is the
discriminator, exactly as the devirtualization path uses `is_VirtualCallData()`.

### 2. Row-to-successor mapping for switches

`MultiBranchData` is allocated unconditionally for both switch forms (no truncation, unlike
`RetData`'s `BciProfileWidth` cap): `default_count_off_set` holds the default traffic, then
one cell pair per case index, so row *i* is `count_at(i)` where *i* is `value - lo` for
`tableswitch` and the pair index for `lookupswitch` — the same index `do_switch`'s
`switch_key(i)`/`switch_dest(i)` loop uses.

Two positional rules fall out of this and are the substance of the implementation:

* `do_switch` **skips** every case whose destination equals the default successor. Those
  executions' counts live in rows that are never emitted, so they must be folded into the
  default weight (`w[0] += count_at(skipped)`), otherwise BPI understates the default edge.
* The emitted vector length must equal `getNumCases() + 1`. If the MDO row count disagrees
  with the bytecode table, nothing is attached — a positional mismatch is worse than no hint.

### 3. LLVM acceptance was measured, not assumed

Probed against the exact linked toolchain (LLVM 20.1.5, `/opt/homebrew/opt/llvm@20/bin/opt`)
with `debug/probe_switch_prof.ll`: the verifier **accepts** `!prof !"branch_weights"` on a
`switch`, and `function(print<block-freq>)` with weights `[1000000, 100, 1]` puts ≈0.9999 of
the mass on the default block, pinning the operand order as
**[default, then cases in `addCase` order]**. Zero weights are rejected by the verifier, so
counts are clamped to 1 (same policy as the branch path) and capped at `0xFFFFFFFF` for the
`i32` operand type.

## Fix

### 1. Two-way branch orientation — `yuhuTopLevelBlock.cpp` (`do_if_helper`, ~line 1332)

`do_if_helper` is the single place a Java conditional becomes a terminator. When
`not_taken > taken`, the predicate becomes `ICmpInst::getInversePredicate(p)` and the hot
block is emitted first; `add_incoming()` is bound to the semantic `YuhuTopLevelBlock*`, not to
emission order, so the swap is semantics-preserving and needs no new deopt trap. Weights are
attached either way. Helper `branch_profile(int bci, uint& taken, uint& not_taken)`
(yuhuTopLevelBlock.cpp:1397) reads the slice.

### 2. Multi-way weights — `yuhuTopLevelBlock.cpp` (`do_switch`, ~line 1417)

A weight vector is accumulated alongside `addCase`: index 0 starts from `default_count()`,
each emitted case appends `count_at(i)`, and each case folded into the default adds its count
to slot 0. Attachment happens after the table is built, subject to the arity agreement above.
Helper `multi_branch_data(int bci)` (yuhuTopLevelBlock.cpp:1494) reads the slice.

### 3. Flags — `yuhu_globals.hpp:68` and `:74`

```cpp
product(bool, YuhuBranchProbabilities, true, "At the optimized tier, use the caller's ...")
product(bool, YuhuSwitchProfile, true, "At the optimized tier, attach LLVM !prof ...")
```

Both default true. Every failure mode degrades silently to IR identical to the pre-change
output: flag off, below optimized tier, NULL/immature MDO, NULL slot for the bci, wrong tag,
or all counters zero.

### 4. The tier gate — `yuhuTopLevelBlock.cpp:1400` and `:1496`

The flag descriptions promised "at the optimized tier", but nothing enforced it. With
`-XX:TieredStopAtLevel=5` the same fixture compiled `dense()`/`sparse()`/`run()` at
`CompLevel_yuhu_fast` and produced **8 annotated switches and 88 `branch_weights` nodes** —
indistinguishable from level 6. Both helpers now bail immediately after the flag check:

```cpp
if (env() == NULL || env()->comp_level() != CompLevel_yuhu_optimized) return false;  // or NULL
```

matching the pre-existing convention in `yuhuInlinePlanner.hpp:148,226`. The `env() == NULL`
test is deliberate: `env()` asserts non-NULL, but asserts are compiled out in product builds.
Note the LLVM pass pipeline is still shared between the two levels — what now differs is the
IR handed to it.

## The Profiling-Window Trap Found While Validating

Switch cells initially read as all zero, which looked like a consumer bug. It was a producer
timing property, isolated by config bisection and confirmed with `-XX:+PrintTieredEvents
-XX:+Verbose` (whose `mdo=i(b),b(b)` field shows whether an MDO existed at level 0):

* `MultiBranchData` has exactly **one** writer — the template interpreter
  (`profile_switch_case`, and Yuhu's `write_insts_profile_switch_case` twin). C1 contains zero
  references to `MultiBranchData`, so once a method leaves tier 0 the cells are frozen.
* `BranchData` has **two** writers — the interpreter *and* C1
  (`c1_LIRGenerator.cpp:927 profile_branch`), which is why branch weights look healthy in
  configurations where switch weights are empty.
* `AdvancedThresholdPolicy::should_create_mdo` builds the MDO only while still at tier 0 and
  only past `Tier0ProfilingStartPercentage` (default 200%) of the tier-3 thresholds, whereas
  promotion fires at 100%. With `-XX:-BackgroundCompilation` the compile is synchronous
  (`PrintCompilation` shows the blocking `b`), so the method is promoted **before the MDO
  exists**; every interpreter increment then short-circuits on `test_method_data_pointer`'s
  `cbz` and the cells stay zero forever.

Proof that the producer, not the consumer, was at fault: `-XX:CompileCommand=exclude,...::dense`
(leaving `dense()` interpreted) produced byte-identical cell values under `-BC` and `+BC`
(1818452, 1818432, 1618656, 1618656, 1618655). The suites therefore pin
`-XX:Tier0ProfilingStartPercentage=50`, which reopens a tier-0 window while keeping
synchronous, deterministic compilation — and it turned the `-/+YuhuSwitchProfile` control into
a real A/B: both arms collect identical data, only consumption differs.

Consequence for production, still open: switch hints are inherently sparse (≈25k switch
samples against ≈8.5M branch samples in one measured run), and a method whose promotion
outruns MDO creation silently gets no switch metadata. The durable fix is to make tier 3
profile switches; that is net-new C1 work, not a restore, since JDK8 C1 has no switch
profiling at all.

## Verification

`test_yuhu/test_switch_profile.sh` — 6 scenarios, all PASS:

| Scenario | Result |
|---|---|
| [1] stock baseline | oracle `ALL OK` |
| [2] javap shape gate | `TABLESWITCH` in `dense()`, `LOOKUPSWITCH` in `sparse()` |
| [3] `-YuhuSwitchProfile` control | `switches=8 annotated=0`, `clamped_weights=0` |
| [4] `+YuhuSwitchProfile` | `switches=8 annotated=8`, arity `cases+1` per switch, `clamped_weights=10`, oracles equal to control |
| [5] `+DeoptimizeALot` | correct |
| [6] tier-5 negative control | compiled at level 5, 8 IR files, switches present, `annotated=0` |

Representative emitted vectors:

```llvm
; tableswitch, 5 cases + never-taken default clamped 0 -> 1
  switch i32 %52, label %bci_121 [ i32 0, label %bci_76 ... ], !prof !5
!5 = !{!"branch_weights", i32 1, i32 448, i32 448, i32 384, i32 384, i32 384}

; lookupswitch: a compilation that keeps the zero-count row (clamps it) ...
!9 = !{!"branch_weights", i32 1, i32 512, i32 512, i32 512, i32 512, i32 1}
; ... and one that folds it into the default, giving 4 cases and 5 weights
!7 = !{!"branch_weights", i32 1, i32 512, i32 512, i32 512, i32 512}
```

Both defensive rules therefore fire on real data rather than only in code. The annotated IR
passes `opt -S -passes=verify` (rc=0) on LLVM 20.1.5.

`test_yuhu/test_branch_probabilities.sh` — 5 scenarios, all PASS: control `no branch_weights`,
profile arm `hits=40` (e.g. `!3 = {8160, 254}`), oracles identical, `+DeoptimizeALot` clean.

Negative controls were themselves tested by mutating each suite's tier-5 arm back to
`TieredStopAtLevel=6`. Both copies then exited 1, printing the level evidence
(`FAIL: run() never compiled at tier-5`) and the violation
(`FAIL: tier-5 IR carries branch_weights despite the level-6 gate`,
`FAIL: tier-5 must annotate nothing (switches=8 annotated=8)`), so the guards are not dead code.

## Known Boundaries

* **Path A inlinees.** `YuhuInlineBlock` overrides neither `do_if_helper` nor `do_switch`, so
  branches and switches inside Path-A inlinees emit un-annotated terminators. Fixing it means
  reading the *inlinee's* MDO in the inlinee's bci space, not the caller's.
* **OSR backedges.** Loop trip counts live in the backedge counter, not in a `BranchData`
  slice, so loop headers get no weight from this work.
* **Switch profile sparsity**, as described above.
* **Pipeline sharing.** Levels 5 and 6 still run the same LLVM passes; only the IR differs.

## Modified Files

* `hotspot/src/share/vm/yuhu/yuhu_globals.hpp` — `YuhuBranchProbabilities`,
  `YuhuSwitchProfile` (lines 68-78).
* `hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.hpp` — `branch_profile` (517),
  `multi_branch_data` (526) declarations and contract comments.
* `hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp` — `do_if_helper` (1332),
  `branch_profile` (1397), `do_switch` (1417), `multi_branch_data` (1494); tier gates at 1400
  and 1496.
* `test_yuhu/com/example/BranchProfileTest.java`, `test_yuhu/com/example/SwitchProfileTest.java`
  — fixtures; the switch fixture forces both `tableswitch` and `lookupswitch` with a skewed
  distribution plus a never-selected case row.
* `test_yuhu/test_branch_probabilities.sh`, `test_yuhu/test_switch_profile.sh` — suites,
  including the tier-5 negative-control scenarios.
* `debug/probe_switch_prof.ll` — LLVM verifier / block-frequency probe.

Related: Activity 082 (narrowOop encode/decode opacity), Activity 086 (jump-table interior
pointers — the same `tableswitch` lowering this work annotates).

## Impact

* Conditional-branch layout and switch probability now come from executed code rather than
  from bytecode order, at the tier where such hints are affordable.
* The 5-vs-6 ladder is meaningful for codegen content, and the fast tier no longer pays MDO
  snapshot reads for hints it must not use.
* Risk stays low: metadata-only, no new guards or traps, every path degrades to the previous
  IR, and semantics are checked numerically in both suites.

## Lessons Learned

1. **Prove the producer before debugging the consumer.** All-zero counters looked like a read
   bug; the read path was already faithful — the VM's own `-XX:+PrintMethodData` dump agreed
   cell-for-cell. The asymmetry between `BranchData` (interpreter + C1) and `MultiBranchData`
   (interpreter only) was the whole story. Any profile consumer whose data has a single-tier
   producer is timing-sensitive.
2. **A deterministic-test flag can silently disable the thing under test.**
   `-XX:-BackgroundCompilation` is required for reproducible tier-6 compiles, yet it starves
   tier-0 profiling. Split the two concerns: keep determinism, and open the window explicitly
   with `Tier0ProfilingStartPercentage`.
3. **A flag description is a contract.** Both flags said "At the optimized tier" while the
   code ignored `comp_level`. Grep for the tier convention that already exists
   (`yuhuInlinePlanner.hpp`) and reuse it instead of inventing a per-feature knob.
4. **Measure the backend, don't assume it.** Whether the verifier accepts `!prof` on a
   `SwitchInst`, and which position the default weight occupies, were settled by `opt` probes
   on the exact linked LLVM — documentation and intuition both disagree here.
5. **Negative controls need mutation tests.** An always-PASS absence check is decoration; the
   tier-5 scenarios were proven able to fail before they were trusted.

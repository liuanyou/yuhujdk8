# 084: `Math.log` Under Yuhu — Name-Based LLVM Intrinsic Callee Carries No Address (`assert(helper_address != 0)` in `yuhuIntrinsics.cpp:204`)

**Date:** 2026-10-31
**Status:** Root-caused and fixed. All 7 crashing Math intrinsics now build a baked-in helper address, compile to an installed nmethod, and produce results **bit-identical to C1/C2 and to the interpreter**.
**Severity:** High — a **deterministic VM abort on the Yuhu compile thread**, not a miscompile and not a GC bug. Any tier-6 compile of a method containing `Math.sin/cos/sqrt/log/log10/exp/pow` kills the process. Kafka (2.12-3.6.1) on this build died within seconds.
**Related commits:** `1db76d033d` (083), `c27857c18e` (`fix: grant write permission to GangWorker for g1` — the previous session's bug, unrelated to this one); the defect itself is original to the Yuhu tree, predating `872a085fcf` (oldest commit touching `yuhuIntrinsics.cpp`).
**Related docs:** [083](083_c2_membar_volatile_precedent_unschedulable_graph.md) (same "muting the assert is not a fix" discipline, §5 below), [082](082_narrow_oop_encode_decode_fold_stale_oop.md) (a case where the optimizer's rewriting was the real subject).

---

## 1. Problem Summary

Yuhu's intrinsic leaf calls are emitted in two steps that must agree on one thing: **the callee value
has to contain the helper's address as a constant.**

`YuhuIntrinsics::do_Math_1to1()` does not take the callee and emit a call — it first *extracts a
uint64 address out of the LLVM value* and registers that address as call-site metadata:

```cpp
// yuhuIntrinsics.cpp:186-208
void YuhuIntrinsics::do_Math_1to1(Value *function) {
  Value* callee = function;
  uint64_t statepoint_id = YuhuStatepointIDGenerator::next();
  uint64_t helper_address = 0;
  if (auto* CastInst = llvm::dyn_cast<llvm::ConstantExpr>(callee)) {          // :196
    if (CastInst->getOpcode() == llvm::Instruction::IntToPtr) {
      if (auto* IntConst = llvm::dyn_cast<llvm::ConstantInt>(CastInst->getOperand(0))) {
        helper_address = IntConst->getZExtValue();
      }
    }
  }
  assert(helper_address != 0, "helper_address should have a value");          // :204
  YuhuDebugInformationRecorder::get()->register_call_site(
      statepoint_id, inline_node_or_method(), helper_address,
      CallSiteType::leaf_call, bci());
  ...
```

Seven of the nine Math helpers are built by the **name-based** `make_function` overload, whose result
is an `llvm::Function*` — *not* an `inttoptr` `ConstantExpr` — so the three nested `dyn_cast`s match
nothing, `helper_address` stays `0`, and the assert aborts. The two helpers built by the
**address-based** overload (`tan`, `fabs`, plus `atan2`) work, which is exactly why the bug looked
function-specific instead of structural.

The invariant is not cosmetic. `register_call_site()` feeds `!yuhu.call_sites` named metadata, and
the JITLink pass reads that table back:

- `yuhuORCPlugins.cpp:241` asserts every resolved `$__GOT` symbol address satisfies
  `contains_call_target(addr) || get_symbol_by_range_addr(addr)`;
- `yuhuBuilder.cpp:1436-1498` (`scan_and_generate_all_relocations`) patches `adrp` sites and emits
  `runtime_call_type` relocations only for registered targets, otherwise
  `assert(symbol_entry != NULL, "Symbol should exist")`.

So a call site whose target is `0` is not "un-optimized", it is *unpatchable* (§5 proves it).

## 2. Symptom

Reported from the Kafka run (pid 89654, JRE `1.8.0-internal-debug-liuanyou_2025_12_21_08_54-b00`, VM
`25.0-b70-debug`):

```
#  Internal Error (/Users/liuanyou/CLionProjects/jdk8/hotspot/src/share/vm/yuhu/yuhuIntrinsics.cpp:204), pid=89654, tid=24067
#  assert(helper_address != 0) failed: helper_address should have a value
```

The `hs_err` and replay files for that run live under `~/Downloads/zipped/kafka_2.12-3.6.1/`, which
this session's shell could not read (macOS TCC denies file reads below `~/Downloads`; `ls` on the
directory succeeds, `cat`/`wc`/`cp` and the IDE's read tool all return *Operation not permitted*).
**Nothing in this document is quoted from that log.** Everything below comes from an independent
deterministic reproducer that reaches the same assert at the same line, with the same frame chain
(`report_vm_error` → `do_Math_1to1` → `do_intrinsic` → `do_call` → `YuhuBlock::process()` →
`YuhuBlock::interperate()` → `YuhuItem::process()` → `YuhuCompiler::compile_method()` →
`compile_cpu_thread_wrapper` ← `thread_entry`, on `JavaThread "Yuhu CompilerThread0"`).

## 3. The Two `make_function` Overloads — Where the Address Comes From

```cpp
// yuhuBuilder.cpp:266-282 — name form: no address anywhere in the result
Value* YuhuBuilder::make_function(const char* name, const char* params, const char* ret) {
  return YuhuContext::current().get_external(name, make_ftype(params, ret));
}
// yuhuBuilder.cpp:276-282 — address form: the address IS the constant
Value* YuhuBuilder::make_function(address func, const char* params, const char* ret) {
  return CreateIntToPtr(LLVMValue::intptr_constant((intptr_t) func),
                        PointerType::getUnqual(make_ftype(params, ret)));
}
```

and the name form's endpoint:

```cpp
// yuhuContext.hpp:81-92
llvm::Constant* get_external(const char* name, llvm::FunctionType* sig) {
  llvm::FunctionCallee callee = module()->getOrInsertFunction(name, sig);
  return llvm::cast<llvm::Constant>(callee.getCallee());   // → llvm::Function*
}
```

`Module::getOrInsertFunction` materialises a declaration named `llvm.log.f64`. A `Function` is a
`Constant`, but it is not a `ConstantExpr`, so `dyn_cast<ConstantExpr>` fails on it. The address form
round-trips an `intptr_constant` through `inttoptr`, which is precisely the shape `:196` matches.

Pre-fix assignment of the two forms over the intrinsics `do_intrinsic()` dispatches
(`yuhuIntrinsics.cpp:91-126`, `_min`/`_max` first, then `_dabs` … `_dexp`):

| helper | pre-fix form | shape |
|---|---|---|
| `sin` `cos` `sqrt` `log` `log10` `exp` | `make_function("llvm.*.f64", "d", "d")` | **name → no address → assert** |
| `pow` | `make_function("llvm.pow.f64", "dd", "d")` | **name → no address → assert (at `:267`, the 2-arg twin)** |
| `tan` `atan2` `fabs` | `make_function((address) (double(*)(double))::tan, …)` | address → ok |
| `f2i` `f2l` `d2i` `d2l`, `is_subtype_of`, `current_time_millis`, barrier runtimes | address | ok |

The same extraction idiom is duplicated at **12 sites** (all of them asserting on its failure), which
is the reason this class of bug is easy to introduce and easy to miss:
`yuhuIntrinsics.cpp:196/259/315`, `yuhuBlock.cpp:706/743/786/823`,
`yuhuBuilder.cpp:944/1169`, `yuhuTopLevelBlock.cpp:679/2191`, `yuhuTopLevelBlock.hpp:324`
(asserts at the corresponding `+8` lines; `yuhuBuilder.cpp:951` words it as
`"barrier runtime must be a baked-in address"`).

## 4. Reproduction Matrix (pre-fix)

Reproducer: `/tmp/wx_math/MathProbe.java` — eleven 9-byte static methods, each a single `Math` call,
dispatched by an `int` selector, 120 000 iterations. Each row is a **separate JVM** forcing exactly
one method to tier 6 via the fork's oracle directive, with synchronous compilation
(`-XX:-BackgroundCompilation`) so the compile task is attributable:

```
-XX:+UnlockDiagnosticVMOptions -XX:+UseYuhuInt -XX:+UseYuhuCompiler
-XX:TieredStopAtLevel=6 -XX:-BackgroundCompilation
-XX:CompileCommand=yuhuonly,MathProbe.<method>
```

| probe | result | assert site |
|---|---|---|
| `mLog` `mLog10` `mExp` `mSin` `mCos` | exit 134 (abort) | `yuhuIntrinsics.cpp:204` |
| `mSqrt` | exit 134 | `yuhuIntrinsics.cpp:204` |
| `mPow` | exit 134 | `yuhuIntrinsics.cpp:267` (`do_Math_2to1`) |
| `mTan` `mAbs` `mAtan2` | exit 0 | address-based → unaffected |
| `mControl` (no Math call) | exit 0 | baseline |

A useful fingerprint for reading future `hs_err` logs: the `do_intrinsic()+0x…` frame offset (pre-fix
build) identifies the intrinsic without any source access — `sin 0xa4`, `cos 0xc0`, `sqrt 0x114`,
`log 0x130`, `log10 0x14c`, `pow 0x168`, `exp 0x184`. Together with the line number (`:204` vs `:267`)
it pins both the function and the arity.

## 5. Muting the Assert Is Not a Fix (measured)

slowdebug has no product image, but `report_vm_error()` returns early when
`error_is_suppressed(file, line)` matches (`debug.cpp:223`, flag `SuppressErrorAt`,
`globals.hpp:2938`), i.e. `-XX:SuppressErrorAt=…` *continues execution along the exact
release-build path*. That turns "would product builds survive?" into a two-minute experiment.
Same matrix, both assert sites muzzled:

| probe | outcome with the assert muzzled | why |
|---|---|---|
| `mLog` `mLog10` `mExp` `mSin` `mCos` | exit 134, now at **`yuhuORCPlugins.cpp:241`** | LLVM lowers the named intrinsic to a real libm call; its `$__GOT` address was never registered because `helper_address` was 0 |
| `mSqrt` | exit 0, bit-identical | LLVM **does not emit a call** — `llvm.sqrt.f64` becomes the `fsqrt` instruction, so the bogus `0` registration is never consulted |
| `mPow` (constant exponent `2.0`) | exit 0, bit-identical | InstCombine folds `pow(x, 2.0)` to `x*x` — the call disappears before the GOT scan |
| `mTan` `mAbs` `mAtan2` `mControl` | exit 0 | already correct |

Two conclusions, both general:

1. **Relaxing the assert is not a fix.** It relocates the abort to the relocation pass, and where it
   does not abort (rows 2–3) it is only because the call — and with it the need for the address —
   was optimized away. The assert is the cheapest, most precise statement of the invariant.
2. **The invariant has an exception class worth naming**: a helper that LLVM lowers to *instructions*
   legitimately has no call target. `sqrt` belongs to that class; `sin/cos/log/log10/exp/pow` do not,
   because on AArch64 those transcendental values are computed by libm calls through the GOT.

## 6. The Fix

Option A, as approved: make every Math helper that can produce a call use the **address-based**
overload, and target the same runtime entry points C1, C2 and the interpreter use.

| helper | now | signature |
|---|---|---|
| `sin` `cos` `tan` `log` `log10` `exp` | `make_function((address) SharedRuntime::dsin/dcos/dtan/dlog/dlog10/dexp, "d", "d")` | `("d","d")` |
| `pow` | `make_function((address) SharedRuntime::dpow, "dd", "d")` | `("dd","d")` |
| `sqrt` | `make_function((address) (double (*)(double))::sqrt, "d", "d")` | `("d","d")` — see below |
| `atan2` `fabs` | unchanged (already address-based) | — |

Final form: `yuhuBuilder.cpp:474-525`, preceded by a comment recording the invariant so the pattern
cannot silently regress.

**Why `SharedRuntime::d*` rather than libm.** They are fdlibm ports (`sharedRuntimeTrig.cpp:839/897/954`,
`sharedRuntimeTrans.cpp:231/299/435/724`, none under an `__SOFTFP__` guard) and they are what every
other tier calls on this platform: C1 (`c1_LIRGenerator_aarch64.cpp:883-891`), C2
(`library_call.cpp:1923-1952`, `runtime_math(..., FN_PTR(SharedRuntime::dsin), "SIN")`), the
interpreter (`interpreter_aarch64.cpp:206-214`) and Yuhu's own interpreter
(`yuhu_interpreterGenerator_aarch64.cpp:1079-1087`). Same function ⇒ same result bits, so tier 6 can
no longer disagree with tier 0/3/4. It also removes 7 libm GOT dependencies in favour of in-DLL
`SharedRuntime` entries.

**The `sqrt` exception.** `SharedRuntime::dsqrt` is declared only under
`#if defined(__SOFTFP__) || defined(PPC)` (`sharedRuntime.hpp:144-146`; defined at
`sharedRuntime.cpp:413-417`), so it does not exist in this hard-float AArch64 build, and C1/C2 use
the `fsqrt` instruction (C2: `Matcher::has_match_rule(Op_SqrtD) ? inline_math(id) : false`,
`library_call.cpp:1943`). libm's `sqrt` is correctly rounded, so the result is identical; the price is
that tier 6 now pays a call where it used to pay one instruction. Getting the instruction back
requires §9's option B.

**Two things that are *not* gaps.** `Math.min`/`Math.max` (`do_Math_minmax`) emit no call at all
(`icmp` + `select`/`phi`) and therefore register nothing — correct as is. And `Math.atan2` has no
`SharedRuntime` counterpart because C2 refuses to intrinsic it (`library_call.cpp:1949-1950`,
`case vmIntrinsics::_datan2: return false;`), so all tiers end in libm `atan2` and already agreed.

## 7. Verification

| # | check | result |
|---|---|---|
| 1 | `make CONF=macosx-aarch64-normal-server-slowdebug hotspot` | `yuhuBuilder.cpp` recompiled, `Linking vm...`, 0 errors (21 s) |
| 2 | `make images` | 27 s; `jdk/lib/server/libjvm.dylib` refreshed; `images/j2sdk-bundle/…/bin/java -version` runs, so no AMFI re-sign was needed |
| 3 | crash matrix, all 11 probes forced to tier 6 | **11/11 exit 0** (was 7 aborts), incl. the `mControl` baseline |
| 4 | numerics: tier 6 vs `-Xint` vs `-XX:TieredStopAtLevel=4`, summed over 120 000 calls | **11/11 bit-identical**. Before the fix the same harness reported `mTan` as `-434734.76122529095` (tier 6, libm) vs `-434734.76122529083` (interpreter and tier 4) |
| 5 | `test_yuhu/test_matrix.sh` | rc=0; Yuhu `Register method … successfully` for normal and OSR entries; no FAIL, no `Killed: 9` |

Row 4 is the strongest available evidence: identical input, identical output, and the comparison is
meaningful because after the fix all tiers literally call the same function.

## 8. Cost

Seven helpers now emit a call where LLVM previously emitted a libm call of its own choosing — so for
`sin/cos/log/log10/exp/pow` the instruction count is unchanged and only the target moved.
The real deltas are two:

- `sqrt`: `fsqrt` (1 instruction) → a call to libm `sqrt`. Chosen deliberately (no `SharedRuntime`
  alternative exists) and acceptable for these call frequencies; reversible via option B.
- `tan`: was already a call; retargeted from libm to `SharedRuntime::dtan`, which *fixes* a measured
  last-ulp divergence from every other tier. This is one line beyond the crash fix and can be
  dropped independently if a strictly-minimal diff is required.

## 9. Options Considered and Rejected

| option | why not |
|---|---|
| relax / remove the `helper_address != 0` asserts | falsified by measurement — §5; the abort moves to `yuhuORCPlugins.cpp:241`, and surviving cases survive only because their call was optimized away |
| keep the `llvm.*.f64` names and register `0` as the target | `:241` and `yuhuBuilder.cpp:1480/1498` both key on the registered address; an unregistered GOT target is a hard failure, not a missed optimization |
| **B (hybrid)**: keep `llvm.sqrt.f64` for instruction-lowering intrinsics and *skip* call-site registration when no call was emitted; factor the 12 duplicated extraction sites into one shared helper | Correct and cheaper for `sqrt`, but it changes the registration contract at 12 sites and needs a "did this become a call?" predicate. Deferred as follow-up work, not dismissed — it is the path back to `fsqrt` |
| **C (de-intrify)**: drop `_dsin`…`_dexp` from `is_intrinsic()` and let them compile as ordinary calls | `java.lang.Math` transcendentals are **native** methods in JDK8 and Yuhu's `do_call` has no `is_native` handling anywhere in the tree (the only `is_native` check in Yuhu is `yuhuInliner.cpp:218`). Would need native-call frame construction first |
| `-XX:TieredStopAtLevel=4` | a workaround for the whole tier, not for this defect |
| `-XX:SuppressErrorAt=/yuhuIntrinsics.cpp:204,…` as a shipping mitigation | it is a diagnostic instrument (§5), not a fix: it deliberately continues down a path that then aborts |

## 10. Open Items

1. **`frame_address()` is still name-based** — `yuhuBuilder.cpp:597-605`,
   `make_function("llvm.frameaddress.p0", "i", "C")`. It has **zero callers** (declared at
   `yuhuBuilder.hpp:222`, defined, never used), so it cannot trip anything today, but it is the last
   helper that would reproduce this exact assert if routed through a leaf call.
2. **The 12-site duplication remains.** Nothing structural stops an 8th name-based helper from being
   added next to `sin()`. Option B's shared extraction helper is the durable guard.
3. **Product-build exposure is unmeasured in the general case.** This build is slowdebug; §5
   simulates the product path via `SuppressErrorAt`, which is evidence, not a product run.
4. **Kafka itself has not been re-run.** It is the original trigger and the realistic regression test;
   the reproducer covers the compile-time path, not the workload's use of the results.

## 11. Why This Is a Typical Issue — the Rules to Carry Forward

1. **In a JIT that reads its own IR back, the *shape* of a `Value` is an interface.** `ConstantExpr`
   `inttoptr` vs `llvm::Function*` is not an implementation detail — three call sites' metadata
   depends on which one `make_function` returns. Two overloads whose results are not interchangeable
   must not share a name, or must document the shape in the type (`YUHU_API`-style typedef, a
   `baked_callee()` accessor, or a `static_assert`-able wrapper).
2. **When a builder asserts "this value must contain a constant", check whether every producer of that
   value can supply one** — here 7 of 9 Math helpers could not. The asymmetry between the two
   `make_function` overloads is the whole bug; a passing `tan` proves nothing about a failing `log`.
3. **A debug assert that only looks like a metadata assert is usually a codegen-invariant assert.**
   `helper_address` is consumed by the relocation/GOT pass, so "just allow 0" converts a precise
   compile-time message into an imprecise link-time one. Muzzle it and observe (§5) before proposing
   any relaxation.
4. **"Which functions crash?" is a proxy for "which lowering emits a call?".** `sqrt` and constant-exponent
   `pow` survived muzzling because the optimizer removed the call. Tier-level and function-level
   pass patterns must be reproduced before a fix is declared, or you "fix" the assert and leave a
   different register-time failure behind.
5. **Route intrinsics to the runtime the other tiers use**, unless there is a measured reason not to.
   The `tan` divergence was pre-existing, invisible to every existing test, and only surfaced because
   the harness compared tier 6 against `-Xint` bit-for-bit.
6. **If a log is unreadable, reproduce instead of inferring.** `~/Downloads` is TCC-denied to this
   shell; the correct move was a 9-byte-per-method deterministic reproducer plus a per-JVM matrix
   (`-XX:CompileCommand=yuhuonly` to pin *which* method reached tier 6,
   `-XX:-BackgroundCompilation` to make the compile attributable), not narrative guessing about the
   Kafka frames.
7. **Watch your own harness.** Two early matrix rows were vacuous: an auto-incremented loop index was
   feeding a selector whose order did not match the probe's `switch`, so the method forced to tier 6
   was not the method being executed, and "exit=0" rows were false negatives. Enumerate
   `method:selector` pairs explicitly, and treat an unexpected PASS as suspect.

## 12. Files and Artifacts

Modified:

- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` — `sin/cos/tan/sqrt/log/log10/pow/exp` retargeted to the
  address-based `make_function` (`:474-525`), plus the invariant comment above the block.
  **Net +18/−9, one file, the only change in this activity.**

Unchanged and deliberately so: `yuhuIntrinsics.cpp` (the asserts are correct),
`yuhuDebugInformationRecorder.cpp:136-155` (`register_call_site`) and `:158`
(`embed_call_site_metadata`), `yuhuORCPlugins.cpp:236-241`, `yuhuBuilder.cpp:1436-1498`,
`atan2`/`fabs`/`f2i`…`d2l`/barrier helpers.

Evidence on disk (all under `/tmp/wx_math/`, nothing written into `test_yuhu/`):

- probe source and class: `MathProbe.java`, `MathProbe.class`
- drivers: `run.sh` (pre-fix crash matrix), `sup2.sh` (muzzled matrix), `num.sh` and `verify.sh`
  (numerics; `verify.sh` is the post-fix harness), `sup.sh`/`cmp.sh` (the flawed first versions,
  kept as the §11.7 counter-example)
- logs: `e_*.log` (7 pre-fix aborts), `s_824.log` / `s2_*.log` (muzzled runs, incl. the
  `yuhuORCPlugins.cpp:241` follow-up), `matrix.txt`, `matrix_sweep.log`,
  `build_hotspot.log` / `build_images.log` (first fix), `build2.log` / `build2_images.log` (the `tan`
  retarget). Post-fix there are deliberately **no** `v_*.log` files: the harness points
  `-XX:ErrorFile` at that prefix, and a run that never reaches the error reporter never creates one.

Repo pollution carried over from activity 083's investigation and **still uncommitted at the time of
writing**: `hs_err_pid6206.log`, `hs_err_pid6840.log` in the repository root (from `lldb` probes on
the GangWorker W^X bug). Not deleted pending the owner's word.

## 13. References

- `hotspot/src/share/vm/yuhu/yuhuIntrinsics.cpp:88-126` — `do_intrinsic()` dispatch of
  `_dabs/_dsin/_dcos/_dtan/_dsqrt/_dlog/_dlog10/_dpow/_dexp` → `do_Math_1to1`, `_datan2` → `do_Math_2to1`;
  `:186-243` / `:245-292` / `:308-361` — the three extraction-and-register sites
  (`do_Math_1to1`, `do_Math_2to1`, `do_System_currentTimeMillis`), asserts at `:204`, `:267`, `:323`;
  `:153` — `do_Math_minmax`, the call-free sibling that registers nothing.
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp:266-282` — the two `make_function` overloads;
  `yuhuContext.hpp:81-92` — `get_external` → `getOrInsertFunction` → `cast<Constant>(callee.getCallee())`.
- `hotspot/src/share/vm/yuhu/yuhuDebugInformationRecorder.{hpp,cpp}` — `CallSiteEntry`
  `{statepoint_id, inline_node_or_method, call_target, call_site_type, bci}`,
  `contains_call_target()`, `!yuhu.call_sites` emission.
- `hotspot/src/share/vm/yuhu/yuhuORCPlugins.cpp:236-241` and `yuhuBuilder.cpp:1436-1498` — the
  consumers that make a `0` call target fatal (`adrp` patching, `runtime_call_type` relocations).
- `hotspot/src/share/vm/runtime/sharedRuntime.hpp:131-146` — the `d*` declarations and the
  `__SOFTFP__`/`PPC`/`E500V2` guards around `dabs`/`dsqrt`; `sharedRuntimeTrig.cpp:839/897/954`,
  `sharedRuntimeTrans.cpp:231/299/435/724` — the fdlibm definitions.
- `hotspot/src/share/vm/opto/library_call.cpp:1922-1952` — `inline_math_native()`, the C2 side of the
  "same function" argument, including `_datan2 → return false`.
- `hotspot/src/share/vm/utilities/debug.cpp:125-227` — `error_is_suppressed()` / `report_vm_error()`;
  `runtime/globals.hpp:2938` — `notproduct(ccstrlist, SuppressErrorAt, …)`.
- `hotspot/src/share/vm/compiler/compilerOracle.cpp` (`yuhuonly`) with
  `simpleThresholdPolicy.cpp` / `advancedThresholdPolicy.cpp` (`call_event`/`loop_event`) — the lever
  that makes single-method tier-6 attribution possible.
- LLVM: `Module::getOrInsertFunction`, opaque pointers (why `llvm.*.f64` names carry no address), and
  the AArch64 backend's libm-call lowering for `sin/cos/log/log10/exp/pow` vs instruction lowering for
  `sqrt` — see activity 008 in this directory for the `.p0` intrinsic-naming change.

Note: the index in `activities/README.md` stops at activity 009 and has not been maintained for
010-083, so this document is not listed there.

# 081: LLVM StackMaps `parseRegisterLiveOutMask` Null-Deref on an Oop-Returning Patchpoint

## Problem Summary

While compiling `org.aspectj.weaver.reflect.ReflectionBasedReferenceTypeDelegateFactory::createGenericSignatureProvider`
at tier 6 (Yuhu), the JVM crashes with a `SIGSEGV` inside the AArch64 backend during
`AsmPrinter` code emission — **not** at runtime. The fault is a null-pointer dereference in
`llvm::StackMaps::parseRegisterLiveOutMask`, reached while encoding the live-out register
mask of an `llvm.experimental.patchpoint.p1` (an oop-returning VM-call site) inside a
Path-B inlinee.

This is a **code-generation-time** crash (compiler thread), independent of GC/OopMap
correctness, deopt, or the recent full-frame-state decache / entry-poll dominance work.

## Error Details

- **hs_err**: `test_yuhu/hs_err_pid67242.log`
- **Signal**: `SIGSEGV` / `EXC_BAD_ACCESS`, `si_addr = 0x0` (null deref)
- **Faulting symbol**: `llvm::StackMaps::parseRegisterLiveOutMask(unsigned int const*) + 364`
- **Compile task**: `createGenericSignatureProvider` (188 bytes), tier 6, Yuhu CompilerThread0
- **Trigger IR**: `test_yuhu/_tmp_yuhu_ir_RS4GC_...createGenericSignatureProvider_t1791851935906.ll`
- **Offending instruction (in the IR)**: line 60, an `llvm.experimental.patchpoint.p1` with
  intrinsic id `10004`, attribute `"statepoint-id"="10003"`, `patchBytes = 16`, and an
  `addrspace(1)` (oop) result used at line 61-62 as a base pointer for a `getelementptr` + `load double`.

```llvm
%14 = call ptr addrspace(1) (i64, i32, ptr, i32, ...)
      @llvm.experimental.patchpoint.p1(i64 10004, i32 16,
      ptr nonnull inttoptr (i64 10004 to ptr), i32 0) #6   ; #6 = "statepoint-id"="10003"
```

## Why It Looked Daunting (but is not)

The IR is well-formed: `opt --passes=verify` passes and RS4GC succeeded (the dumped file is the
post-RS4GC `.ll`). This crash is **not** the SSA dominance bug (fixed in 080-era work), **not** a
GC/OopMap correctness issue, and **not** caused by Yuhu's reserved physical registers.

## Fault Instruction (from lldb on stock `llc`)

```
->  ldr  x8, [x8]              ; x8 = 0   -> EXC_BAD_ACCESS @ 0x0
    ldrh w8, [x8, #0x18]       ; read uint16 field of the (null) register class
    madd w8, w10, w9, w8       ; w9 = 0x213 (531), w10 = 0
    add  x8, x11, w8, uxtw #4  ; index RegClassInfos[] (16-byte entries)
```

Resident registers at the fault: `x11 = &llvm::RegClassInfos`, `x14 = &AArch64::MPR8RegClass`,
`x15 = AArch64MCRegisterClasses + 16960`. So `StackMaps` is enumerating a live-out physical
register taken from a call's register mask and dereferences a **null** register-class /
`RegClassInfo` pointer for it.

## Investigation Timeline

### Stage 1: Reproduce offline, isolate from the JVM

The crash reproduces on a plain toolchain, which proves it is not a Yuhu-fork/JVM-only problem:

```
/opt/homebrew/opt/llvm/bin/llc -mtriple=aarch64-apple-darwin --filetype=null <the .ll>
```

| Configuration | Result |
|---|---|
| Homebrew **LLVM 20.1.5 `llc`**, no `-mattr` | crashes |
| Your command with `-mattr=+reserve-x20..x28` | crashes identically |
| `-O0` | crashes |
| `-O3` | crashes |

**Conclusion**: reserved registers are **not required** to trigger the fault; the crash is
optimization-level independent (structural, not a fragile regalloc coincidence).

### Stage 2: Rule out peripheral constructs

Each construct was removed from the full IR; the fault **persisted** every time, so none is the cause:

| Construct removed from the IR | Still crashes? |
|---|---|
| Fixed-register inline asm reads (`mov $0, x0/x12/x28/x29`) | yes |
| `llvm.read_register("sp")` | yes |
| `llvm.experimental.stackmap` calls | yes |
| `gc.statepoint` with empty `"deopt"()` bundle (isolated) | no crash |
| `patchpoint.void` with `ptr null` target (isolated) | no crash |
| poll `gc.statepoint` + inline asm + read_register (isolated) | no crash |
| a single `patchpoint.p1` result reused as `gc-live` (isolated, short live range) | no crash |

### Stage 3: Attribute to the call family (patchpoint `.p0` vs `.p1`)

`llvm.experimental.patchpoint` returns via different address spaces. Both families exist in this
function; they were neutralized independently (replaced by a plain extern call, preserving every
downstream use so the module still verifies and reaches codegen):

| Transformation | Result |
|---|---|
| Replace **`.p1`** patchpoint calls (keep all `.p0` + all `gc.statepoint`) | **fixed** — verifies + compiles |
| Replace **`.p0`** patchpoint calls (keep `.p1`) | still crashes |

So the offending live-out is carried by an oop-returning **`patchpoint.p1`** site, not by `.p0`
nor by any `gc.statepoint`.

### Stage 4: Pin the exact site

The function contains exactly two `patchpoint.p1` calls: intrinsic ids `10004` (line 60, in the
inlinee) and `10011` (line 118). Neutralizing each one at a time:

| Neutralized `patchpoint.p1` | Result | Interpretation |
|---|---|---|
| id `10004` only (keep `10011`) | clean | **10004 is the culprit** |
| id `10011` only (keep `10004`) | crashes | 10011 alone is fine |

## Confirmed Trigger (introduced by a Yuhu code change)

The offending `patchpoint.p1` (id 10004) is produced by `YuhuBuilder::CreateInlineOopForStaticField`
(`hotspot/src/share/vm/yuhu/yuhuBuilder.cpp:875`). The site was changed from an **inline-asm**
constant-oop load to an **`llvm.experimental.patchpoint.p1`** (numBytes 16, id/statepoint-id split,
`CallSiteType::oop_call` + `register_patch_point`). Mapping IR line 60-62 to the code:

| IR (line 60-62) | Code |
|---|---|
| `patchpoint.p1(i64 10004, i32 16, ptr inttoptr(i64 10004), i32 0)` | `pp_args={statepoint_id_for_patchpoint, 16, callee=inttoptr(temp_placeholder), 0}` |
| `#6 = "statepoint-id"="10003"` | `AB.addAttribute("statepoint-id", statepoint_id)` |
| `%14` → `getelementptr i8, ptr addrspace(1) %14, i64 104` → `load double` | `CreateGEP(jbyte, mirror_oop, field_offset=104)` → primitive `load double` |

Mechanism: the inline-asm version returned an addrspace(1) `mirror_oop` via `inttoptr(inlineasm i64)`
and was a plain call that **never enters `StackMaps`**. A `patchpoint` is a stackmap-bearing
instruction, so the change routed this site through `parseRegisterLiveOutMask` (live-out register
mask), which has the null-`RegClassInfo` bug. Note the result **is already** `ptr addrspace(1)`
(`patchpoint.p1` overload) — the address-space tag is not the problem; the patchpoint itself is.
Because the `java_mirror` here is a compile-time constant oop (patched via `oop_call` relocation),
it does not need to be a GC-forwarded relocatable base; the inline-asm + `oop_Relocation` form
avoided the crash while keeping identical GEP derived-pointer addressing.

## Root Cause

A bug in **LLVM 20.1.5's `StackMaps`/AArch64 live-out encoding**: `parseRegisterLiveOutMask`
assumes every live-out physical register of a call has a valid register class / `RegClassInfo`
entry, and dereferences that pointer **without a null check**. At the `patchpoint.p1` id `10004`
site (an oop-returning VM call inside the Path-B inlinee), the register allocator leaves a
physical register live-out whose class resolves to null, producing the null deref.

Why this specific site: a lone oop-returning patchpoint with a short live range does **not** crash
(verified via a truncated module). The fault only manifests with this function's full, overlapping
oop live ranges, which force a specific physical register into the patchpoint's live-out set.
The result is register-allocation / live-range sensitive, which is why `llvm-reduce` cannot
minimize it automatically — its IR round-trip perturbs the very live ranges that trigger the bug.

## Scope Boundaries (what this is NOT)

- **Not** Yuhu's IR: the module verifies and survives RS4GC.
- **Not** reserved registers (`+reserve-xNN`): crashes with them off.
- **Not** the fixed-register inline asm, `read_register`, or the frame-prologue stackmaps.
- **Not** the recent deopt/decache / entry-poll dominance / OopMap correctness work. The
  full-frame-state decache could at most shift which oops stay live in registers across VM calls,
  making this latent LLVM bug fire more often — it is not the cause (crash survives removing every
  spill/helper construct).

## Resolution (implemented — Yuhu IR-gen, no LLVM fork change)

The crash only fires on the **oop-returning `patchpoint.p1`** overload. `CreateInlineMetadata`
already returns an addrspace(0) `Metadata_type` pointer and never crashes, proving the problem is
tied to the addrspace(1)-typed result, not to patchpoints in general. So `CreateInlineOop` and
`CreateInlineOopForStaticField` (yuhuBuilder.cpp) were switched from
`getDeclaration(mod, experimental_patchpoint, { oop_addrspace1_type() })` (the `.p1` overload) to
`{ jlong_type() }` (an **i64** result) and the addrspace(1) pointer is recovered at the last use via
`CreateIntToPtr(call, oop_addrspace1_type())`.

Why this is sufficient and safe:
- With `isPointerLike == false`, RS4GC cannot place the constant in a `gc-live` bundle, so it stops
  being a forced live-out register value → the `parseRegisterLiveOutMask` path is never exercised at
  these sites (matches the Stage-3 finding that `.p0`/i64 patchpoints compile cleanly).
- The machine code is **unchanged**: the reserved 16-byte `movz/movk + blr` window and the patchpoint
  stackmap offset are independent of the IR result type, so `scan_and_generate_all_relocations`'
  `oop_call` branch still locates the window and rewrites the movz/movk immediates to the oop pool
  index, then emits an `oop_Relocation`. Nothing in the recorder, ORC plugin, or relocation phase changed.
- Correctness holds because a `java_mirror` / constant oop is patched via `oop_Relocation` and does not
  need to be a GC-forwarded relocatable base.

This also restores the project's own documented design decision ("return i64 from the patchpoint and
`CreateIntToPtr` at the last use"), which the `.p1` migration had inadvertently reversed.

## ⚠️ STATUS UPDATE — the `.p1` / Path-A conclusion was WRONG; crash NOT resolved

After implementing Path A (switch the two oop sites from `patchpoint.p1` to an i64 return +
`inttoptr`), the regenerated IR confirms `patchpoint.p1` count = **0**, but the SAME SIGSEGV in
`StackMaps::parseRegisterLiveOutMask` still fires — now on a `patchpoint.p0` produced by an
RS4GC-lowered `gc.statepoint`. So the earlier bisect ("neutralizing `.p1` id 10004 fixed the module")
was a **live-range artifact**: relieving pressure at one site happened to move the bad register out of
every mask in that snapshot; it was never the cause. `.p1` vs `.p0` vs `oop` is irrelevant.

**True root cause (verified against llvm/llvm-project `main`, `llvm/lib/CodeGen/StackMaps.cpp`):**
```cpp
StackMaps::createLiveOutReg(unsigned Reg, const TargetRegisterInfo *TRI) const {
  unsigned DwarfRegNum = getDwarfRegNum(Reg, TRI);
  unsigned Size = TRI->getSpillSize(*TRI->getMinimalPhysRegClass(Reg)); // null deref
  return LiveOutReg(Reg, DwarfRegNum, Size);
}
```
`parseRegisterLiveOutMask` iterates every set bit of the live-out mask over ALL physical registers
(`Reg != TRI->getNumRegs()`) and calls `createLiveOutReg`. For the offending register (number 531, in
the **floating-point** domain) `getMinimalPhysRegClass` returns **nullptr**, and `getSpillSize(*nullptr)`
dereferences it (`ldr x8,[x8]`, `x8=0`). There is no null guard, and LLVM is an optimized build (no
assert), so the compiler thread dies during AsmPrinter emission.

**Proof it is general and not Yuhu/IR-specific:** reproduces on stock Homebrew LLVM 20.1.5 `llc
-mtriple=aarch64-apple-darwin --filetype=null` with **no** reserve flags; the only feature toggle that
avoids it is `-mattr=-fp-armv8` (removes the FP registers from the mask) — `-sve`/`-neon`/`-fmv` do not.
Any method with a live `double` across a `gc.statepoint`/stackmap can hit it.

**The real fix is in the LLVM backend** (guard the null class), not in Yuhu's IR generation:
```cpp
const TargetRegisterClass *RC = TRI->getMinimalPhysRegClass(Reg);
unsigned Size = RC ? TRI->getSpillSize(*RC) : 0;         // or: if (!RC) continue; in the loop
```
Because `libjvm` links a **stock** Homebrew LLVM (`--with-llvm=/opt/homebrew/Cellar/llvm/20.1.5`),
applying this requires building a patched LLVM and reconfiguring. Path A is retained only because it
matches the documented i64+inttoptr design; it does NOT address this crash.

## ✅ Interim Workaround — Yuhu tier-6 gate (implemented)

Bug submitted upstream: **llvm/llvm-project#231361** (https://github.com/llvm/llvm-project/issues/231361),
together with the 27-line reproducer in `debug/minirepro/repro.ll`. While waiting for a fix, Yuhu skips
tier-6 compilation for methods whose IR satisfies the crash predicate, so the tiered framework falls
back to C1/C2 for exactly those methods.

**Predicate** (two-way, in `yuhu_triggers_stackmaps_fp_bug`, `yuhuCompiler.cpp`):

```
bailed = has_authored_patchpoint(F)         // any call to llvm.experimental.patchpoint.* (void / .p0 / .p1)
      && function_uses_fp(F)                // any float/double arg, ret, instruction result, or operand
```

**No `gc.statepoint` term.** Earlier reasoning said the crash needed all three (patchpoint +
gc.statepoint + FP) -- that was wrong, an artifact of how the reduction test `q.ll` happened to be
shaped. Direct reductions prove patchpoint + FP alone is sufficient:

| file | shape | result |
|---|---|---|
| `repro.ll` | patchpoint + statepoint + FP | CRASH |
| `t_pp_fp_no_sp.ll` | patchpoint + FP, **no** gc.statepoint, **no** plain call | **CRASH** |
| `t_pp_void_fp_from_arg.ll` | void patchpoint (`ptr null` target) + FP loaded from arg (independent of pp), **no** gc.statepoint, **no** plain call | **CRASH** |
| `t_fp_but_no_pp_at_all.ll` | plain call + FP, **no** authored patchpoint | ok |
| `t_nofp.ll` | patchpoint + statepoint, integer only | ok |
| `t_stackmap_fp.ll` | stackmap + statepoint + FP, no authored patchpoint | ok |
| `t_patchonly.ll` | patchpoint, no FP | ok |

So the load-bearing pair is exactly **`authored patchpoint + FP`**; anything else LLVM sees in the
function is irrelevant to whether the crash fires. This also means the pre/post-RS4GC timing of
the check is not a concern: we're scanning for the authored patchpoints Yuhu itself emits and FP
types, both present already in the pre-RS4GC IR. (If we later discover methods where FP is only
introduced by RS4GC's own lowering, revisit — no evidence of that so far.)

**Timing**: the check runs on the pre-RS4GC IR (right after `embed_call_site_metadata()`, before
`createResourceTracker()`). This is intentional — `runGCPasses()` fires inside the ORC IR-transform
callback where there is no `ciEnv`, so a clean abort isn't possible from there. Since the predicate
is now independent of RS4GC output, this placement is straightforwardly correct.

Since a `record_method_not_compilable` on a deterministic IR-based predicate will hit every time,
the gate is permanent (per Yuhu tier) rather than a retry. The call passes **`all_tiers=false`**
to get `MethodCompilable_not_at_tier` — the method stays compilable at C1/C2. (`ciEnv::
record_method_not_compilable` defaults `all_tiers` to `true`, which would set
`MethodCompilable_never` and also disable C2 — a bug I caught during review. Note the one other
Yuhu call site, `yuhuFunction.cpp:329`, currently uses the default and is likely wrong for the
same reason, but is out of scope here.)

**Kill switch**: `-XX:+YuhuAllowFPCodegen` (default `false`). Once an upstream LLVM fix is picked up
in the linked LLVM, flip this on to restore tier-6 for FP-using methods without touching code.

**Files touched**:
- `hotspot/src/share/vm/yuhu/yuhu_globals.hpp` — new `product(bool, YuhuAllowFPCodegen, false, …)`.
- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` — `yuhu_triggers_stackmaps_fp_bug()` helper in the anon
  namespace, plus the gate inserted in `compile_method` right after
  `recorder->embed_call_site_metadata()` and before `createResourceTracker()` (so nothing on the
  JIT side needs unwinding).

**Coverage note**: this is strictly narrower than an "any FP" gate; the difference is FP-using
methods with **no** authored `llvm.experimental.patchpoint` (pure-arithmetic leaves with no
inline-const oop/metadata site, no VM-call, and no unwind path). Those are rare in the JDK but
valuable to keep on tier-6, and empirically are safe to compile.

**Removal criteria**: when an LLVM release containing the
`StackMaps::parseRegisterLiveOutMask` null guard is linked into `libjvm`, set
`-XX:+YuhuAllowFPCodegen` in the default flag values, then delete the helper + gate + flag
(`grep -rn YuhuAllowFPCodegen` finds them all).

## Fix Directions (analysis recorded before the resolution above landed)

1. **Backend hardening (cleanest).** In the LLVM fork linked into `libjvm`, guard
   `StackMaps::parseRegisterLiveOutMask` against a null register class / `RegClassInfo` before
   dereferencing (skip the live-out or `llvm_unreachable` with the register id). This file is not
   in the jdk8 tree; the patch belongs in the external LLVM fork.
2. **Yuhu IR-gen workaround.** Avoid leaving GC oops as *physical-register live-outs* across
   `patchpoint.p1` VM calls — e.g. ensure the surviving oop is held in a spill slot recorded via
   `gc-live` (relocate-based) rather than a live-out register, or reduce the number of
   simultaneously-live oops across inlinee VM-call patchpoints so the RA does not park one in the
   offending class.
3. **Upstream report.** Provide the reproducer and the pinned site (`patchpoint.p1` id `10004`) to
   the LLVM version Yuhu links.

## Reproduction / Verification Commands

```sh
L=/opt/homebrew/opt/llvm/bin
F=_tmp_yuhu_ir_RS4GC_org.aspectj.weaver.reflect.ReflectionBasedReferenceTypeDelegateFactory__createGenericSignatureProvider_t1791851935906.ll

# crashes (backend null deref):
"$L/llc" -mtriple=aarch64-apple-darwin --filetype=null "$F" -o /dev/null
# also crashes with reserved regs / at -O0 and -O3 (proves independence from them)

# confirming the trigger: replace the patchpoint.p1 id 10004 call with a plain
# `call ptr addrspace(1) @yuhu_fake_pp1()` -> module verifies AND compiles clean.
```

## References

- Faulting routine: `llvm/lib/CodeGen/StackMaps.cpp` — `StackMaps::parseRegisterLiveOutMask`
- Faulting IR site: `createGenericSignatureProvider` RS4GC `.ll`, line 60 (`patchpoint.p1` id 10004, `"statepoint-id"="10003"`)
- hs_err: `test_yuhu/hs_err_pid67242.log`
- Related reserved-register context: `llvm_bug_report.md` (its reserved-register hypothesis is
  **disproved** by this issue — the crash reproduces without any `+reserve-*`)
- Yuhu VM-call / patchpoint emission: `hotspot/src/share/vm/yuhu/` (patchpoint overload guidance in
  memory: "patchpoint intrinsic overload types", "Yuhu VM Call Stub Implementation Pattern")

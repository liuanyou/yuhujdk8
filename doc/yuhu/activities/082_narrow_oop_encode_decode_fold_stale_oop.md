# 082: InstCombine Collapses the Narrow-Oop Encode/Decode Pair — Tier-6 Stale-Oop SIGSEGV in `Resolve.signatureMoreSpecific`

**Date:** 2026-10-28
**Status:** Root-caused and fixed. The fix is in `YuhuBuilder::CreateOpaqueOopShift` / `CreateOpaqueBaseSub`; the scoped reproducer ran green across five consecutive executions, where before the fix it crashed on the very first one.
**Severity:** Highest — silent heap corruption produced by an LLVM **middle-end pass**, not by anything the Yuhu front end emitted wrong.
**Related activities:** [065](065_RS4GC_InlineAsm_Incompatibility.md) (RS4GC vs inline asm), [076](076_hashmap_resize_live_oop_integer_escape.md) (a live oop escaping as an integer), 072/073 (safepoint-poll and monitor liveness), 081 (stackmap live-out).

---

## 1. Problem Summary

At tier 6, `com.sun.tools.javac.comp.Resolve.signatureMoreSpecific` crashed with a `SIGSEGV`
under GC pressure. The IR Yuhu emitted was **correct**: every reference read was decoded and
every reference write was encoded, and RS4GC produced well-formed statepoints with the right
live sets. Then InstCombine ran, noticed that with `narrow_oop_base == NULL` and
`narrow_oop_shift == 3` the composition `encode(decode(n))` is the identity on `n`, and folded
the whole chain away. What survived in the register/spill slot across the safepoints was the
**encoded `i32` narrowOop**, read before the GC.

A `narrowOop` is an integer. RS4GC only tracks `isHandledGCPointerType` values, so an `i32`
never enters a live set and never gets a `gc.relocate`; the spill slot it lands in is, quite
correctly, absent from every OopMap. When `finally { currentResolutionContext = prevResolutionContext; }`
later stored that value into field `this+128` of a **live** object, it wrote a pre-GC address
into the heap. The next read of that field produced a pointer into reclaimed memory, and the
crash surfaced in interface dispatch.

Turning `-XX:+UseYuhuCompiler` off makes the crash disappear, which is the signature of this
class of bug: nothing in the lowering is wrong, the optimizer is simply allowed to do
arithmetic that is only valid when the value is not a GC-relative reference.

## 2. Symptom

`test_yuhu/hs_err_pid28999.log` (2026-10-28 12:27, pre-fix build):

```
#  SIGSEGV (0xb) at pc=0x000000010d9b1ba0, pid=28999, tid=5123
# Problematic frame:
# v  ~RuntimeStub::yuhu_interface_call_stub
...
siginfo:si_signo=SIGSEGV: si_errno=0, si_code=2 (SEGV_ACCERR), si_addr=0x00000005d56dd5f8
 x0=0x0000000000000000  x1=0x00000005d56dd5f0  ...  x14=0x00000000baadbabe
J 2563 yuhu com.sun.tools.javac.comp.Resolve.signatureMoreSpecific(...)Z
   @ 0x000000010dabd364 [0x000000010dabcd40+0x624]
```

Register facts worth recording:

- The stub dereferences the receiver's klass at `[x1, #8]`; `si_addr == x1 + 8`, so **x1 is the
  bad pointer**, and the fault is the klass load, not a field access.
- `x1 == 0xbaadbabe << 3`, and `x14` holds `0xbaadbabe` = `badHeapWordVal`
  (`globalDefinitions.hpp`) — the slowdebug zap fill for dead heap words. The arithmetic identity
  is exact; the natural reading is that the narrow word being decoded *was* the zap fill, i.e.
  the load came from memory GC had already reclaimed and zapped. (This run predates the
  trace-column caveats listed in §3 item 2, so treat the reading as inference, not as measured
  fact.)
- `SEGV_ACCERR` rather than `SEGV_MAPERR` — a permission fault, not a completely unmapped
  address. Consistent with a decoded address inside the reserved region that is no longer
  usable; the exact page state was not pinned down.

Same shape in the tree: `test_yuhu/hs_err_pid60568.log` and `hs_err_pid73471.log` (both
2026-10-27) — `yuhu_interface_call_stub` with `signatureMoreSpecific` as the last Yuhu frame.
`test_yuhu/hs_err_pid53809.log` is from the same hunt but faults in a **C2** frame
(`Resolve$MethodResolutionContext.deferredAttrContext`), i.e. the same underlying corruption
surfacing after the value had flowed back through non-Yuhu code.

## 3. How It Was Localized (and what misled us)

The path was long, and most of it is worth writing down because each stage produced a
conclusion that had to be retracted.

1. **Native VerifyOops instrumentation.** `verify_oop_wrapper` was emitted as a leaf call at
   every `CreateDecodeHeapOop` result, every decache slot, and (briefly) every reference putfield.
   Signature: `YuhuRuntime::verify_oop_wrapper(oop, msg, holder, field_off, site_id)`. Gated by
   the develop flag `VerifyOops`, traced to a file by `-XX:YuhuVerifyOopFile=<path>`.
2. **The trace columns lied.** `holder=` at a decode site is built from a different spill slot
   than the one the `getfield` actually dereferences, and `off=` means a frame-slot offset on
   `decache` lines but a field byte offset on `decode` lines. Grepping by `holder=<addr>` also
   silently drops every line where that object is the *value* being verified. The correct query
   is the bare hex address across all columns. (Recorded in 076-era notes; do not trust those
   columns literally again.)
3. **A shallow field scan was added, crashed the diagnostic itself, was hardened, then deleted.**
   `is_oop(true)` validates only the header, so a zapped corpse passes it; the 1-level
   reference-field walk faulted inside `Klass::layout_helper` on a bogus klass word
   (`hs_err_pid71701.log`, no longer in the tree). Hardening = `Metaspace::contains(klass)`
   before *any* klass dereference. The scan and its `YuhuVerifyOopFields` flag were later removed
   by decision (§10).
4. **Two wrong verdicts.** First "GC-invisible stale root", then "reference field never
   initialized" (because the value was `badHeapWordVal`-shifted). The second was overturned: the
   same address had verified OK dozens of times earlier in the same run, so it was a genuinely
   live object that was collected while something still pointed at it. Fingerprint alone does not
   distinguish *never written* from *collected*; the discriminator is whether the bare address
   ever appeared valid in the trace.
5. **The step that actually worked: stop reading the trace, read the post-RS4GC IR.** Dumping
   `-XX:+YuhuDumpIRToFile` for the method and following the provenance of the offending store
   was decisive in minutes, where the trace had been inconclusive for days.

## 4. Root Cause

### 4.1 The smoking gun in the IR

`test_yuhu/_tmp_yuhu_ir_RS4GC_com.sun.tools.javac.comp.Resolve__signatureMoreSpecific_t1793159087926.ll`
(post-RS4GC, pre-fix build):

```llvm
795:   %307 = load i32, ptr addrspace(1) %addr127, align 4     ; narrowOop read by getfield
1168:  %addr179 = getelementptr i8, ptr addrspace(1) %.188, i64 128
1169:  store i32 %307, ptr addrspace(1) %addr179, align 4      ; finally-restore, NO encode
2052:  %addr310 = getelementptr i8, ptr addrspace(1) %.289, i64 128
2053:  store i32 %307, ptr addrspace(1) %addr310, align 4      ; second finally path, NO encode
```

The value stored into a live `Resolve`'s field 128 is the *load result itself* — the decode and
the matching encode are simply not in the function any more. The file contains no shift asm at
all; its only three `asm` calls are the frame/register readers from the prologue helpers
(`mov $0, x0`, `mov $0, x28`, `mov $0, x29`).

### 4.2 The fold, reproduced in two minutes

No JVM needed. With the project's exact triple and datalayout:

```llvm
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32-ni:1"
target triple = "aarch64-apple-darwin"

define i32 @old_roundtrip(i32 %n) {          ; base == NULL, shift == 3, plain IR
  %z = zext i32 %n to i64
  %s = shl i64 %z, 3
  %p = inttoptr i64 %s to ptr addrspace(1)
  %i = ptrtoint ptr addrspace(1) %p to i64
  %r = lshr i64 %i, 3
  %out = trunc i64 %r to i32
  ret i32 %out
}
```

```
$ opt -passes=instcombine -S in.ll
define i32 @old_roundtrip(i32 %n) {
  ret i32 %n          ; <- the entire round trip is gone
}
```

Three separate identities do the damage, and each one is individually valid IR:

| Identity | Why InstCombine thinks it is legal |
|---|---|
| `lshr(shl(x, 3), 3) → x` | pure arithmetic on `i64` |
| `ptrtoint(inttoptr(y)) → y` | **even with `ni:1` in the datalayout** — verified empirically, do not rely on the non-integral annotation to protect you |
| `sub(add(x, C), C) → x` | the base-relative mode is folded just as well (§6.3) |

`n` is the narrow value as it existed **before** any intervening safepoint. Nothing downstream
of the fold is pointer-typed, so RS4GC has no way to help.

### 4.3 Three independent vectors, one bug

Fixing only the fold would have left the bug half-open. The same stale narrow can be produced by:

1. **Folding** — the identity above. Defence: opacity (hide the shift in inline asm).
2. **Scheduling** — even an unfolded encode can be *hoisted above* or *sunk below* a safepoint by
   the optimizer, at which point it computes from a value that GC has already moved. Defence:
   `hasSideEffects = true`, which makes the call non-speculatable and pins it to its position.
3. **Integer escape** — if the encode is fed by an IR `ptrtoint(oop)`, LLVM may CSE that `i64` and
   keep *it* live across safepoints; an integer is invisible to RS4GC, which is exactly the 076
   `HashMap.resize` failure mode in a new costume. Defence: pass the `ptr addrspace(1)` **directly**
   to the asm, so no `ptrtoint` of a heap oop is ever created.

## 5. Why This Is a Typical Issue (the generalizable rule)

HotSpot's compressed-oop arithmetic is an **isometry**: `encode(decode(n)) = n` holds for every
value in a *fixed* heap layout. It stops holding the moment the value it describes may be
**relocated**, which is precisely the situation a GC-aware compiler is in. Any front end that
lowers JVM reference arithmetic into LLVM IR and then runs LLVM's middle-end inherits this trap,
because the middle-end has no idea that some integers are GC-relative names for objects. C1/C2
never expose it: the arithmetic lives in the backend, emitted by
`MacroAssembler::encode_heap_oop` / `decode_heap_oop` (`cpu/aarch64/vm/macroAssembler_aarch64.hpp:817-824`,
reached from `c1_LIRAssembler_aarch64.cpp` and C2's instruction selection), i.e. after no
optimizing pass can reinterpret it any more. Yuhu is the first compiler in this VM that puts the
same arithmetic into an optimizing IR — and note that its two helpers are a *reimplementation* of
those macro-assembler routines (`CreateDecodeHeapOop`'s own comment says it "mimics the AArch64
macro assembler approach"), so they must be kept in sync by hand.

The rules to carry forward:

- **Anything whose correctness depends on *when* it executes relative to a safepoint must be
  opaque and pinned in IR.** Inline asm with `sideeffect` is the cheapest way to get both.
- **RS4GC correctness must be judged on the IR the machine backend actually sees**, i.e. after
  the middle-end. Dumping pre-optimization IR and declaring victory is how this bug survived.
- **A pointer's integer form must never exist as an IR node** in a GC-managed address space.
- **Datalayout address-space annotations are not a safety mechanism here** (`ni:1` did not stop
  `ptrtoint(inttoptr(x))`).
- Mode constants must be read from the source that establishes their reachability
  (`Universe::initialize_heap`), not from what the current machine happens to use.

## 6. The Fix

### 6.1 Opaque shift helper — `yuhuBuilder.cpp:1356`

```cpp
Value* YuhuBuilder::CreateOpaqueOopShift(Value* value, bool left, int shift) {
  std::string asm_string = std::string(left ? "lsl $0, $1, #" : "lsr $0, $1, #") +
                           std::to_string(shift);

  FunctionType* asm_type = FunctionType::get(YuhuType::intptr_type(),
                                             {value->getType()}, false);
  llvm::InlineAsm* asm_func = llvm::InlineAsm::get(
    asm_type, asm_string, "=r,r",
    true,           // Has side effects: pin the shift here — never hoisted above
                    // nor sunk below an intervening safepoint
    false, llvm::InlineAsm::AD_ATT);

  return CreateCall(asm_type, asm_func, std::vector<Value*>{value},
                    left ? "decoded_oop_addr" : "compressed_oop");
}
```

Both operands are 64-bit registers, which is why the decode side is fed the zero-extended
narrowOop and the encode side the pointer itself. `$0`/`$1` positional substitution is already
the established idiom in this codebase (`CreateReadFramePointer` `"mov $0, x29"`,
`CreateReadCurrentPC` `"adr $0, ."` in `yuhuBuilder.cpp:607-676`, with the same
"prevents optimization" sideeffect rationale).

### 6.2 The ordering insight: hide the **shift**, leave the **base** in IR

This is what makes *one* instruction per side sufficient for every compressed-oop mode. Once the
shift is inside the asm, the base arithmetic has nothing left to cancel against — `add`/`sub` of
a constant around an opaque, pinned value gives the optimizer no identity. And the decode-side
`add` is safe in IR because its only user is the same-block `inttoptr`, so no integer form of the
pointer escapes the site.

`CreateDecodeHeapOop` (`:1438`) and `CreateEncodeHeapOop` (`:1472-1495`):

```cpp
// decode: opaque shift, optional base add in IR
Value* decoded_int = CreateOpaqueOopShift(compressed_as_int, /*left=*/true, shift);
if (base != NULL) {
  decoded_int = CreateAdd(LLVMValue::intptr_constant((intptr_t)base), decoded_int);
}
result = CreateIntToPtr(decoded_int, YuhuType::oop_addrspace1_type());

// encode: base == NULL — the ptr addrspace(1) goes straight into the shift
Value* shifted = CreateOpaqueOopShift(oop, /*left=*/false, shift);
return CreateIntCast(shifted, YuhuType::jint_type(), false);

// encode: base != NULL — the sub must also be inside the asm, otherwise a
// ptrtoint of the oop has to exist, which is vector 3 above
Value* delta = CreateOpaqueBaseSub(oop, (intptr_t)base);
if (shift > 0) { delta = CreateLShr(delta, LLVMValue::intptr_constant(shift)); }
return CreateIntCast(delta, YuhuType::jint_type(), false);
```

`CreateOpaqueBaseSub` (`:1382`) emits `"sub $0, $1, $2"` with constraint `"=r,r,r"`. The base
travels as a **register**, not an immediate: a 4Gb-aligned base is not encodable in a single
`sub` immediate field.

Net effect: **no code path in the builder produces a `ptrtoint` of a heap oop any more.**

### 6.3 All four modes, and a hole that was shipped and then closed

`Universe::initialize_heap` (`memory/universe.cpp:838-863`) establishes which combinations are
reachable:

| `base` | `shift` | selected when | first attempt | final |
|---|---|---|---|---|
| `NULL` | 3 | zero-based, heap end > 4Gb | opaque `lsr/lsl #3` | OK |
| `NULL` | 0 | `UnscaledNarrowOop`, heap end ≤ 4Gb (`:863`) | `lsr/lsl #0` (a legal move — verified with `llc`) | OK |
| `!= NULL` | 3 | heap reserved end > 32Gb (`:838-841`) | **left as plain IR — still folded** | fixed |
| `!= NULL` | 0 | unreachable: `:848` zeroes the base in the same `else` that could pick shift 0 | n/a | handled anyway |

The third row is the important one. The first cut of this fix papered over `base == NULL` and
excused the base mode as "would need two instructions in the asm". That was wrong twice over:
`opt -passes=instcombine` on the base-mode round trip collapses it to `ret i32 %n` exactly like
the zero-based one, and `base != NULL` in the field is *always* `shift == 3`, i.e. the branch
left unprotected was the live one. The user found this by reading the decode helper (lines
1418-1422 as it stood before this second round of edits, so the numbers have since moved) and
asking a single question: *"no need inline asm here?"* Lesson: an unverified "this mode is
exotic" claim is a defect waiting to ship.

## 7. Why Inline Asm Does Not Break RS4GC (see 065)

065 recorded that upstream RS4GC dies on inline asm — *"Cannot take the address of an inline
asm!"* — because it tries to wrap every call in a `gc.statepoint`. Yuhu took **Option 2** there
and reimplemented the pass (`yuhu/yuhuRewriteStatepointsForGC.cpp`, ~3400 lines), which contains
the exemption at `:3082`:

```cpp
auto NeedsRewrite = [&TLI](Instruction &I) {
    if (const auto *Call = dyn_cast<CallBase>(&I)) {
        // make sure inline asm being used doesn't need GC support, otherwise it is a big miss for GC
        if (isa<InlineAsm>(Call->getCalledOperand()))
            return false;
```

Two consequences, both required by this fix:

- The asm call **does not become a statepoint**, so no OopMap is emitted per shift. That is what
  we want — a pure ALU instruction needs no safe point.
- Liveness is still use-driven: a pointer passed as an asm operand is a *use*, so it stays in the
  live set and still gets relocated. Opacity does not cost us GC visibility.

The fork's own comment ("otherwise it is a big miss for GC") is the caveat to remember: inline
asm is a blind spot for RS4GC **in general**. It is safe here precisely because the asm performs
no call and no memory access, and its result is immediately re-materialized from a relocated
pointer. Do not extend this pattern to anything that could be a safepoint.

## 8. Verification

Offline, using the project's triple/datalayout:

| Check | Result |
|---|---|
| `opt -passes=instcombine` on the zero-based round trip, old shape | `ret i32 %n` — the fold, reproduced |
| same, new shape | full chain retained (2 asm calls + zext/trunc) |
| `opt -passes=instcombine` on the base-mode round trip, old shape | `ret i32 %n` — proves the §6.3 hole was real |
| same, new shape | full chain retained, `lsl`/`add`/`inttoptr`/`sub`/`lshr` all survive |
| `llc -filetype=obj` on every new shape, incl. `#0` shifts | assembles |

In the tree:

- `-fsyntax-only` with the real command from `compile_commands.json` for `yuhuBuilder.cpp` and
  `yuhuRuntime.cpp`: clean.
- `make CONF=macosx-aarch64-normal-server-slowdebug hotspot`, republish
  `hotspot/dist/jre/lib/server/libjvm.dylib` → `jdk/lib/server/libjvm.dylib`, `codesign --force --sign -`.
- Post-RS4GC IR of the crashing method after the fix
  (`test_yuhu/scoped_repro/fixcheck/ircheck/_tmp_yuhu_ir_RS4GC_...t1793194677751.ll`):
  **12 `lsl` + 5 `lsr` opaque sites, 0 `sub` (this machine runs `base == NULL`)**; every encode is
  fed by a `ptr addrspace(1)`, one of them literally by a relocation result:

  ```llvm
  %object.relocated241 = call coldcc ptr addrspace(1) @llvm.experimental.gc.relocate.p1(...)
  %compressed_oop = call i64 asm sideeffect "lsr $0, $1, #3", "=r,r"(ptr addrspace(1) %object.relocated241)
  %1337 = trunc i64 %compressed_oop to i32
  store i32 %1337, ptr addrspace(1) %addr161, align 4     ; this+128, now from a relocated pointer
  ```

  and every decoded pointer is still GC-tracked — all **12 of 12** `inttoptr` results of an
  opaque decode appear in a `"gc-live"` bundle somewhere in the function (516 statepoints total),
  e.g. at `:64-67` the decode result `%31` is both a statepoint argument and a bundle member:

  ```llvm
  %decoded_oop_addr = call i64 asm sideeffect "lsl $0, $1, #3", "=r,r"(i64 %30) #5
  %31 = inttoptr i64 %decoded_oop_addr to ptr addrspace(1)
  %statepoint_token26 = call token(...) @llvm.experimental.gc.statepoint.p0(i64 10003, ...,
        ptr addrspace(1) %31, ...) [ "gc-live"(..., ptr addrspace(1) %28, ptr addrspace(1) %31) ]
  ```

  so opacity did not cost GC visibility.
- Runtime: **five separate executions** of the scoped javac reproducer (each completing its full
  internal loop, `round 280 done` → `ALL_ROUNDS_DONE`), scoped with
  `-XX:YuhuCompileOnlyOf` to `Resolve.signatureMoreSpecific`, `resolveMethod`,
  `Scope.computeOverload`, `Scope$SymbolEntry.<init>`, under
  `-Xbatch -Xms256m -Xmx512m -XX:+UseParallelGC -XX:+VerifyOops` — all `rc=0`, no `hs_err`,
  where the pre-fix build crashed in the first execution.
  VerifyOops totals for three of them: 972,007,864 / 971,999,676 / 972,148,169 — same amount of
  work before and after, so no verify site was lost.
- Control with `-XX:+YuhuTraceInstalls` to prove Yuhu really compiled and installed the method
  (otherwise a green run only proves the interpreter worked).

## 9. The Invariant Check Is Now Implemented (was open item 1)

### 9.1 Why review cannot supply this guarantee

Everything the builder emits is correct *by construction*, and the dumped IR of the buggy build
looked correct: the encode was present in the builder's output and only disappeared after
InstCombine. A reviewer would have to mentally run LLVM's scalar optimizer over every method in
every compressed-oop mode to see the fold coming. Decidability lives on the **post-middle-end**
IR, which only the compiler itself has. So the check is a machine check, wired into the pipeline,
default-on.

### 9.2 The three ingredients

1. **The builder hands its type knowledge to the IR as named metadata.** After optimization,
   `store i32` into a reference field and a plain `int`-field copy are byte-identical, so no IR
   pattern can tell them apart; only `field->type()` knew. Two marks, both emitted
   *unconditionally* (the dumped IR is identical whether the net is on or off — only the checking
   is flag-gated):
   - `!yuhu.narrow_oop_store` on the putfield/putstatic `StoreInst`
     (`yuhuBlock.cpp:1348-1362`, marked when `!field->type()->is_primitive_type() && UseCompressedOops`)
     — and *only* there; the `aastore` element store is not marked, see §11 item 3;
   - `!yuhu.narrow_oop_encode` on the encode's inline-asm call (`yuhuBuilder.cpp:1510`, `:1523`).

   Marks — not pattern-matching on the asm string — because changing the asm string must not be
   able to silently disable the net. Marker lookup follows the existing idiom in
   `yuhuRewriteStatepointsForGC.cpp:1158` (`setMetadata("is_base_value", ...)`).
2. **A decisive def-chain walk** (`yuhuInvariants.cpp:142`, `reaches_undecoded_narrow_load`): from
   the value a marked store writes, violation iff it reaches a `load i32` whose pointer is
   `ptr addrspace(1)` before crossing a marked encode. What stops the walk is what makes the check
   free of false positives: marked encode / `Constant` / `Argument` / any other `LoadInst` /
   any other `CallBase` (opaque) / `AllocaInst` / `StoreInst` end a branch as safe; only casts,
   binary ops, GEPs, `select` and `phi` incoming values are transparent, because those are exactly
   the instructions the middle-end uses to cancel an encode. An unmarked store is never
   considered, which is why an int-field copy cannot be reported.
3. **The scan runs at the right point in the pipeline.** Measured on dumped IR:
   `InstCombine`+`SimplifyCFG` preserve both marks, but `YuhuRewriteStatepointsForGC` **drops the
   store mark** off the stores it rewrites (a pre-RS4GC dump has 1 store mark, its post-RS4GC twin
   has 0, while all 3 encode marks survive). So `runGCPasses` was split into two
   `ModulePassManager`s and the scan sits between them (`yuhuIRTransformer.cpp:188-219`). Scanning
   after RS4GC would have found no marked store at all and printed that as a clean result — a
   green light from a blind check, which is the failure mode to distrust most.

### 9.3 Where the failure is decided, and why not inside `runGCPasses`

`runGCPasses` is the ORC IR-transform callback: the `ciEnv` is gone there
(`yuhuCompiler.cpp:1415-1419` says so) and errors end in `fatal(err_msg(...))` — a JVM abort, not
a fallback. Failing after `generate_native_code` would skip `RT->remove()` (`yuhuCompiler.cpp:1757`)
and `release_last_code_blob_unlocked()`. So `yuhuCompiler.cpp:1481-1500` clones the module, runs
the *identical* function-level prefix (InstCombine + SimplifyCFG, no RS4GC — RS4GC must not touch
the thread-local `YuhuDebugInformationRecorder`), scans the clone and on a violation does
`env->record_failure("Yuhu narrowOop encode invariant violated")` +
`YuhuDebugInformationRecorder::release()` + `return` — the same clean-failure path the
flatten/AlwaysInliner check already uses (`yuhuCompiler.cpp:1464-1467`). The callback keeps a
report-only observer at the same point of the real pipeline (`yuhuIRTransformer.cpp:202-215`), which
is a *witness*, not a second decider: a refused compilation never materializes, so on the poison run
the observer saw 0 marked stores (the poison emits no marks) while the deciding check reported 280
violations, and on the clean run both agree store-for-store. Its value is that a clone/observer
mismatch would mean cloning or the pass split changed the answer.

### 9.4 The check cannot go blind silently — and a count is not an answer

A mark that vanishes *inside* the middle-end says something about the check, not about the bug. The
first version of this reporting only counted the losses (`1 of 15 reference-field store mark(s)
vanished while module normal was being optimized. ... find which pass dropped the mark.`) and it
**did** fire on a real workload — but it could not answer the question it told the reader to go and
ask, because a vanished mark has two causes with opposite meaning and a count cannot tell them
apart:

- **deleted** — the `StoreInst` was destroyed. It writes nothing, so it cannot write a stale
  narrowOop; this invariant is not the issue. A live-looking heap field write disappearing is a bug
  of its own, but the IR dump answers it, not this net.
- **rewritten** — the store is still there and still writes, but no longer carries the mark, so
  `scan()` never looked at it. **This is the only way the check can report clean without having
  seen the store**, and a warning that merely says "go find out which" leaves a real fold installed.

So `check_after_middle_end` now follows every marked site into its own clone. In LLVM 20,
`CloneModule(M, vmap)` (`Transforms/Utils/Cloning.h:51`) fills a `ValueToValueMapTy`, which is
`ValueMap<const Value*, WeakTrackingVH>` (`Transforms/Utils/ValueMapper.h:37`): the entries null
themselves out when the cloned instruction is destroyed, which is precisely the deletion signal,
and no dangling pointer is ever read. Each site is then classified (`yuhuInvariants.cpp:285-303`,
`MarkFate`): missing from the map or a nulled handle → *deleted*; present with the metadata →
*kept*; present without it → *rewritten*.

Report shape, printed only when a mark was actually lost (`:474-504`):

```
Yuhu narrowOop invariant: reference-field store marks in module normal after the middle-end: 14 kept, 1 deleted, 0 rewritten without a mark
    deleted in <mangled fn name> block <label>: store i32 ... , !yuhu.narrow_oop_store
    deleted stores write nothing, so they cannot store a stale narrowOop; but if the field write
    is missing from the installed code that is a separate bug -- check this method in the IR dump (-XX:+YuhuDumpIRToFile)
```

The encodes get the same three-way accounting (`:496-504`). They are reported and *not* acted on: an
encode is sideeffect asm, so only a fold or the deletion of a block that proved unreachable can
remove it, the two look identical in the value map, and the def-chain walk — which stops at *any*
call, marked or not — is what decides.

A *rewritten* store is the interesting one and is handled there and then: its def chain is walked
without the mark (`:342-351`) and anything it finds is added to the deciding total, so it fails the
compilation exactly like a marked violation. The report says how many were re-checked:
`rewritten stores were re-checked by def chain without the mark: N violation(s)`.

None of this is paid for on the common path: a site holds one `llvm::Instruction*` into the original
module (which outlives the clone) and all naming/printing happens at report time (`print_lost_site`,
`:263`), capped at 8 lines per category, so a pathological method cannot flood the log.

With `-XX:+YuhuTraceIRCompilation` the summary line always prints both totals and how many kept
their mark, which proves the check ran — a silent pass cannot:
`... 15 reference-field store(s) (14 still marked) and 15 encode(s) (14 still marked) checked after
InstCombine+SimplifyCFG in module normal, 0 violation(s)`.

**Reporting is trace-gated; the deciding is not.** A mark lost to an unreachable block is ordinary
optimizer behaviour, and on a real application it prints several times per start-up, so the kept /
deleted / rewritten lines and their per-site detail appear only under `-XX:+YuhuTraceIRCompilation`
(`yuhuInvariants.cpp:338`, `:356`, `:481`, `:506`). What runs regardless: the classification itself
and the def-chain re-scan of every `rewritten` store, whose violations still count into the deciding
total and still print and still refuse the compilation with no flag set. So the blind spot is still
closed by default; what is quiet by default is the announcement that a mark vanished — see §9.10 for
when that quiet costs something.

### 9.5 It closes the §6.3 hole, which is the argument for having it

The check keys on the store mark and on reachability of a narrow heap load. It does not know or
 care about zero-based vs base-relative mode, the shift value, or the asm text. The base-mode
fold that was shipped and then closed in §6.3 is therefore exactly what it would have caught on
the first run, in either mode — a reviewer looking at a base-mode round trip in an x86 dump
would plausibly not have.

### 9.6 Flags

`yuhu_globals.hpp:114-121` (both `develop`, i.e. compiled out of product builds — the net is a
slowdebug/debug guarantee, not a shipping one; that is worth deciding about deliberately):

- `-XX:+YuhuCheckNarrowOopInvariant` (default **true**): run the check, `record_failure` on a
  violation.
- `-XX:+YuhuBreakNarrowOopEncode` (default **false**): self-test. Re-emits the pre-082
  `ptrtoint`/IR-shift shape on **both** encode and decode and emits no marks, so the check has
  something to catch. Poisoning only the encode does not reproduce the fold — the opaque `lsl`
  decode still blocks it. Never enable outside an experiment.

Neither flag gates what a *violation* does, and violations print unconditionally. The loss reporting
of §9.4 is gated on the pre-existing `-XX:+YuhuTraceIRCompilation`, which is a `product` diagnostic
flag, so it exists in product builds too.

### 9.7 Verification

One purpose-built workload (a method that reads a narrowOop field into a local, allocates between
the read and the write, then stores the local into another object's reference field; driven under
ParallelGC so a collection and relocation can land in between), compiled only into Yuhu via
`-XX:CompileCommand=yuhuonly,<class>.<method>` with `-XX:+UseYuhuCompiler -XX:+UseYuhuInt
-XX:TieredStopAtLevel=6 -XX:YuhuComplexityThreshold=1`. Nothing was written into `test_yuhu/`; the
workload lived in `/tmp`.

| Run | Configuration | Observed |
|---|---|---|
| A | real builder, check on, `-XX:+VerifyOops -XX:+YuhuTraceIRCompilation` | exit 0, `NI_DONE rounds=4000`; `1 reference-field store(s) (1 still marked) and 3 encode(s) (3 still marked) ... 0 violation(s)`; the clone and the in-pipeline observer report the same state; method installed (`Register method NI::transfer successfully`) |
| B | A + `YuhuBreakNarrowOopEncode` | **280 violation reports = 280 refusals, 0 installs**, program still correct (`NI_DONE`, same sum) → the C1/C2 fallback works. Every refusal names `store i32 %10, ptr addrspace(1) %addr19, align 4, !yuhu.narrow_oop_store` fed by `%10 = load i32, ptr addrspace(1) %addr, align 4` — the §4.1 signature. 280 rather than 4 because the test forces recompilation (`ComplexityThreshold=1`) and the method stays hot for the whole 4000-round loop |
| C | B + `-XX:-YuhuCheckNarrowOopInvariant` | **did not crash in 8 runs** (5 plain, 3 with `VerifyOops`): all exit 0 with the poisoned code installed. An earlier run of this configuration *did* SIGSEGV at `# j NI.main(...)+79`, and that crash is not reproducible now — withdrawn as evidence. What the poisoned-store IR actually shows is in §9.8 |
| D | javac in-process over `test_yuhu/com/example/*.java`, `-J-XX:YuhuCompileOnlyOf='com.sun.tools.javac.*'`, threshold 1 | 250 invariant lines over 125 installs, largest method 5 stores / 5 encodes, **0 violations, 0 refusals, 0 warnings** → no false positive found. The run then aborted with `SEGV_ACCERR` in `JavacParser.typetag` inside `ThreadLocalStorage::thread()`; **it aborts identically with `-XX:-YuhuCheckNarrowOopInvariant`**, so that is a pre-existing, unrelated defect worth its own activity, not caused by the check |
| perf | A vs C alternating | inside noise at this scale; not a meaningful measurement |
| E | A and B re-run after §9.4's classification landed, both with `-XX:+YuhuLLVMInlining` | A: exit 0, `NI_DONE`, 0 violations, **no loss line**, and the trace line reports every site `still marked` — so the value-map walk executes and resolves correctly on every site. B: exit 0, 300 violation reports / 290 refusals / 0 installs, no loss line (the poison emits no marks, so there is nothing to lose) |

### 9.8 What the poison run says, and the check's real coverage limit

With the poison on, the *deciding* check fires on 280 compilations — but the compilations that
survive to materialization tell a different story, and it is worth writing down because it bounds
what the net guarantees. Dumped IR for a poisoned `NI.transfer` that ran the **real** pipeline:

- pre-RS4GC artifact: `store i32 %32, ptr addrspace(1) %addr, align 4, !yuhu.narrow_oop_store`
  with `%32 = trunc(lshr(ptrtoint %local_1_, 3))` — the foldable shape, unfused;
- post-RS4GC artifact: `store i32 %24, ...` with `%24 = trunc(lshr(ptrtoint %.0, 3))` — the encode
  still present as a plain IR `lshr`, and **the store's mark is gone** (RS4GC rebuilt the store);
- replaying `opt -passes=instcombine,simplifycfg` on the pre-RS4GC artifact reproduces the real
  pipeline exactly: the store survives with its mark, unfused.

So in that shape the fold needs the *whole* round trip in one region and did not happen; the
builder's IR that the deciding check refused *was* foldable. Two consequences:

1. **The scan is anchored, not final.** The last IR the compiler produces — post-RS4GC — is never
   machine-checked. It is safe today only because nothing optimizes after `runGCPasses` (the
   pipeline is InstCombine → SimplifyCFG → RS4GC → `verifyModule` → codegen). Any pass added after
   RS4GC, or any change to materialization-time optimization, moves the fold past the net and the
   scan must be re-anchored there. Recorded as open item 7.
2. **The store mark is not durable.** Because RS4GC drops it, a post-RS4GC scan would find *no*
   marked store in any method and report every module clean — the most convincing possible false
   green. That is why §9.4 classifies every lost mark instead of counting them, and it is the single
   most important thing this session learned about the file pair.

`make CONF=macosx-aarch64-normal-server-slowdebug hotspot` clean (15 s incremental); the rebuilt
`libjvm.dylib` was republished to `build/macosx-aarch64-normal-server-slowdebug/jdk/lib/server/`
and re-signed. **Note:** `build/.../images/j2sdk-bundle/jdk1.8.0.jdk/Contents/Home/jre/lib/server/libjvm.dylib`,
which `test_yuhu/*.sh` scripts load, was *not* touched at that point and was stale relative to this
build. It has since been republished and re-signed as well — a run from the `j2sdk-bundle` path (as in
§9.9) loads that copy, so both trees have to be refreshed for a rebuild to be observable.

Build gotcha found on the way (worth recording for every future LLVM-side file): a raw LLVM header
included *after* `llvmHeaders.hpp` pulls `<cassert>`, which replaces the two-argument HotSpot
`assert(p, msg)` macro that `llvmHeaders.hpp` takes care to restore, and then `ci/*.hpp` fails to
compile with *too many arguments provided to function-like macro invocation*. HotSpot headers
first, raw LLVM headers last (`yuhuInvariants.cpp:26-46`).

### 9.9 It fired on a real application, which is what forced §9.4

The first chance the anti-blindness reporting got outside the purpose-built workload was an
application run, not a test:

```
<images>/j2sdk-bundle/jdk1.8.0.jdk/Contents/Home/bin/java \
  -XX:+UseYuhuInt -XX:+UseYuhuCompiler -XX:TieredStopAtLevel=6 -XX:+YuhuLLVMInlining -jar mhdcrm-app.jar

Yuhu narrowOop invariant: 1 of 15 reference-field store mark(s) vanished while module normal was being optimized. ...
Yuhu narrowOop invariant: 1 of 15 compressed-oop encode(s) are gone from module normal after the middle-end. ...
```

What that says, and what it does not: one module of the application reached the check with 15 marked
reference-field stores and 15 marked encodes, and exactly one of each did not survive
InstCombine+SimplifyCFG. No violation line was printed, so the check found no stale narrow feeding a
store it could see. Nothing in those two lines distinguishes the two readings of the disappearance,
which is why the count-only version was replaced by the classification in §9.4. The likely benign
reading is one block that proved unreachable taking its store and its encode with it — the pair going
down together is what suggests a shared region rather than two unrelated events — but *likely* is not
a guarantee, and the rewritten-store case is precisely the one a count cannot see. Two other notes
from the same exchange: the run used `-XX:+YuhuLLVMInlining`, which is what the earlier local
validations had not been exercising; and the copy of `libjvm.dylib` under
`images/j2sdk-bundle/.../Home/jre/lib/server/` is what such a run actually loads, so it has to be
republished along with `jdk/lib/server/` or the observation is being made on an older build.

### 9.10 What a `deleted` line means: the regex case, resolved

Re-running the application on the classifying build produced:

```
    deleted    in java.util.regex.Pattern::compile_t1793244976078 block not_zero136:
      store i32 %252, ptr addrspace(1) %addr140, align 4, !yuhu.narrow_oop_store
deleted in <the encode of the same value, same block>  !yuhu.narrow_oop_encode
... 14 kept, 1 deleted, 0 rewritten without a mark ... 0 violation(s)
```

`0 rewritten` + `0 violation(s)` is what makes this an easy case for the invariant: the net never
lost sight of a live store. The remaining question belonged to §9.4's *deleted* reading — was that
arm genuinely unreachable? Reproduced locally (no application needed: `Pattern` internals alone, with
`-XX:CompileCommand=yuhuonly,java.util.regex.Pattern.compile -XX:YuhuComplexityThreshold=1
-XX:+YuhuDumpIRToFile`), and replaying the two passes separately with stock `opt` gives the mechanism:

- **`Pattern::expr`, block `not_zero`** — the arm is guarded by `%39 = icmp ne ptr addrspace(1)
  %local_5_, null`, and the SSA for that local is `%local_5_ = phi [null, %no_exception],
  [%local_5_12, %no_exception72]` with `%local_5_12 = phi [null, %no_exception], [%local_5_12,
  %no_exception72]`: every non-self incoming is `null`, so the value is *provably* null and the
  icmp folds to `false`. The putfield the builder emitted for a null-valued local is dead code.
  InstCombine alone is enough (with `instcombine` only, the store marks in that module go 2 → 0;
  `simplifycfg` alone changes nothing).
- **`Pattern::group0`, block `bci_792`** — the arm is guarded by `%result == 1` where `%result` flows
  from `%nonnull_result = phi i32 [1, %is_instance], [2, %not_instance]`, i.e. an **inlined
  `instanceof`** whose operand's type is statically known; it folds to the other constant and the
  putfield's region becomes unreachable.

Fingerprint to remember: **a store and its encode disappearing together, from the same block, is
arm deletion** — a fold would consume the encode and leave the store (that is §9.8's shape), and a
rebuild would show up as `rewritten`. Both cases above were also behavior-neutral: a regex battery
(7 patterns × 300 rounds, groups and `matches()`) produced byte-identical output against the same
JVM at `-XX:TieredStopAtLevel=4 -XX:-UseYuhuCompiler` and against `-Xint`.

What `deleted` still does **not** prove, and the two-minute check on any new line of this shape: a
count of erased dead code is reassuring only if the unreachability was established by a *constant*.
This repository has a documented mechanism (see the `ni:1` non-integral-address-space note: a
constant-derived `ptr addrspace(1)` reaching a branch condition) by which an `icmp ne <oop>, null`
folds to **poison** instead of to `false`, `br i1 poison` makes *both* arms unreachable, and
reachable code — putfields, statepoints, deopt bundles, whole inlined bodies — is physically erased.
In the `expr` replay above, poison is indeed present (`phi ptr addrspace(1) [poison, %no_exception]`,
`br i1 poison`), but only *inside* the subtree that the already-sound `icmp ne null, null → false`
fold had cut off: the deciding proof was the null-only phi, not the poison. So the procedure for
every new `deleted` report is: `-XX:+YuhuDumpIRToFile`, replay `opt -passes=instcombine` on the
pre-opt module, and look at the *guard of the named block*. Guard folds on a constant (null local,
known klass, inlined instanceof) → dead code, correct. Guard folds to `poison`, or the block still
had a live predecessor → that is the `ni:1` erasure, a lost field write, and a different activity.
The classifier cannot make this call by itself: it knows a store vanished, not why.

Verified after the reporting became trace-gated, on this same `Pattern` reproduction: without
`-XX:+YuhuTraceIRCompilation` a full run prints **no** loss line at all and still finishes correct
(`PD_DONE acc=500000`); with it, the `0 kept, 2 deleted` report and the per-site lines return;
with the poison on and **no** trace flag, 41 refusals still print and `Pattern::compile` is never
installed while the program still computes the right answer through the C1/C2 fallback.

## 10. Cleanup Done Alongside

The diagnostics built during the hunt were stripped once they had served their purpose, per
instruction:

- Removed: `YuhuVerifyOopFields` flag, `yuhu_verify_shallow_fields` (1-level field scan),
  `yuhu_dump_holder_layout`, `yuhu_dump_holder_card`.
- Remaining in `verify_oop_wrapper` (`yuhuRuntime.cpp:558`): `VerifyOops` early-out, null
  early-out, `is_oop(true)`, one tty line naming `site/msg/holder/off/oop`, then
  `ShouldNotReachHere()`. The reasoning is now written into the code: *a broken oop can arrive
  that way for many reasons, and nothing printed here can distinguish them* — the hs_err return
  PC is what localizes the site.
- `-XX:YuhuVerifyOopFile` (the per-visit trace) is kept but is default-off; when enabled it is
  enormously verbose (one historical run produced a 2.5 Gb log).

## 11. Open Items

1. ~~The invariant check is not implemented.~~ **Implemented — see §9.** It is still worth
   extending: the check covers reference-field *stores* only, so the §4.3 integer-escape vector
   (a live pointer leaving the oop representation without a store to catch) remains a
   construction-time discipline, not a verified one.
   The original wording, kept for the record: no `store i32` into a reference-typed heap field may
   be fed by a def chain that reaches a `load i32` without passing an encode node; on violation
   `record_failure` the compilation and fall back to C1/C2. The §6.3 hole is exactly what this
   would have caught, which is the argument for having it instead of relying on review.
2. **`test_yuhu/scoped_repro/run_scoped.sh:28`** still passes the deleted
   `-XX:+YuhuVerifyOopFields`, so the script aborts with *Unrecognized VM option*.
3. **Array element paths** (`aastore` / `aaload`) are covered by the *fix* — they go through the same
   two helpers — but **not by the check**: `mark_store` is called only from the field-write path
   (`yuhuBlock.cpp:1361`), while `YuhuTopLevelBlock::do_astore` encodes at `yuhuTopLevelBlock.cpp:1225`
   and stores at `:1241` without marking, so `scan()` never inspects an array element store and a
   folded encode there would be reported as clean. The encode *is* marked (the mark lives in the
   encode helper), so the def-chain walk would still stop correctly at it — the gap is only that the
   store is not a scan starting point. Closing it is two lines at `:1241`.
4. **Machine-code re-verification is a manual step.** Dump the disassembly of the fixed build and
   confirm the old `[sp, #24]` reference spill slot is gone and the `finally` restore now reads
   `ldr` from the oop slot → `lsr #3` → `str w?, [x?, #128]`. See
   `doc/yuhu/HOW_TO_DISASSEMBLE_YUHU_CODE.md`.
5. Narrow-**klass** encode/decode was deliberately *not* converted: the builder's klass path
   (`yuhuBuilder.cpp:319-332`) still emits plain IR `lshr`/`shl`, and `Klass::decode_klass_not_null`
   (`oops/klass.hpp:731`) yields a metaspace `Klass*`, which JDK8 GC never relocates, so folding it
   cannot create a stale root.
6. **Duplicated algorithm.** The heap-oop encode/decode in the builder mirrors
   `MacroAssembler::encode_heap_oop_not_null` / `decode_heap_oop_not_null` rather than sharing code
   with it. The klass path at least asserts the shift matches
   `LogKlassAlignmentInBytes`; the heap-oop path has no equivalent cross-check against
   `Universe::narrow_oop_mode()`.
7. **The scan is anchored before RS4GC, so the final IR is unchecked.** See §9.8: post-RS4GC there
   is no marked store left to scan (RS4GC rebuilds stores and drops the mark), and the net's
   deciding check runs on a clone of the pre-pipeline module. Today nothing optimizes after RS4GC,
   so the scanned point and the installed IR agree. If a pass is added after
   `YuhuRewriteStatepointsForGC`, or materialization starts optimizing, the check must move with
   it — and RS4GC must first be made to preserve `yuhu.narrow_oop_store`, otherwise the moved
   check would be silently blind.
8. **`develop` flags only.** Both flags (§9.6) are compiled out of product builds, so the net
   protects slowdebug/debug testing and not shipping `libjvm`. Either accept that or promote
   `YuhuCheckNarrowOopInvariant` to `product`.
9. **Unrelated crash surfaced while testing:** in-process javac over `test_yuhu/com/example/*.java`
   with threshold 1 aborts (`SEGV_ACCERR` in `ThreadLocalStorage::thread()` called from
   `JavacParser.typetag`) identically with and without the new check — needs its own activity.
10. ~~**The two loss-reporting branches are untested locally.**~~ **Exercised** — a `Pattern`
    compilation deleted a marked store and its encode, both branches printed, the classification was
    right, and the mechanism was traced to a provably-dead arm (§9.10). What is still not exercised
    locally is a `rewritten` site (a live store that lost its mark), which is the only case the net
    cannot see without the re-scan.
11. **A `deleted` reference-field store is only benign once its unreachability has been checked** —
    see §9.10 for the procedure and for the `ni:1` poison mechanism that can erase *reachable*
    regions through the same code path. If a `deleted` report ever turns out to sit under a guard
    that folded to poison rather than to a constant, that is a lost heap assignment and needs its
    own activity.
12. **The loss reporting is silent by default (§9.4), and the cost of that is specifically the
    poison class.** A violation is never silent — it prints and refuses with no flag. But a *live*
    region erased by a `poison`-folded branch (§9.10) is reported only as a lost mark, and those now
    need `-XX:+YuhuTraceIRCompilation` to appear. So the workflow that catches that class is
    opt-in: when regex/collector/framework code behaves as if an assignment did not happen, re-run
    once with the trace flag and grep `after the middle-end`. If that class ever needs to be caught
    automatically, the check to add is not a mark count — it is an assertion that no branch condition
    in the module derives from a constant `inttoptr` into `ptr addrspace(1)`.

## 12. Files and Artifacts

Modified:
- `hotspot/src/share/vm/yuhu/yuhuBuilder.hpp` — `CreateOpaqueOopShift` (`:286`),
  `CreateOpaqueBaseSub` (`:296`) and the rationale comment that documents the fold.
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` — helpers (`:1356`, `:1382`), decode (`:1438`),
  encode (`:1479`, `:1491`).
- `hotspot/src/share/vm/yuhu/yuhuRuntime.cpp` — `verify_oop_wrapper` simplified (154 lines
  removed, incl. `memory/metaspace.hpp`, `memory/barrierSet.hpp`, `memory/cardTableModRefBS.hpp`
  includes that existed only for the dumps).

Modified for the invariant check (§9), later the same day:
- `hotspot/src/share/vm/yuhu/yuhuInvariants.hpp` — `class YuhuNarrowOopInvariant : public AllStatic`
  (`:83`) plus the rationale comment. The two pre-existing classes in this file pair are
  Shark-lineage value tracking (`YuhuCompileInvariants` / `YuhuTargetInvariants`) and were left
  untouched — open item 1's "contains no check" was about the file, not about them.
- `hotspot/src/share/vm/yuhu/yuhuInvariants.cpp` — mark helpers (`:80`, `:88`), the def-chain walk
  (`:142`), `scan` (`:370`), the clone-based deciding check (`:400`) and the mark-retention
  classification that replaced its two count-only warnings (`MarkedSite`/`collect_marked`/`classify`/
  `classify_sites`, `:239-358`).
- `hotspot/src/share/vm/yuhu/yuhu_globals.hpp:114-121` — `YuhuCheckNarrowOopInvariant` (true),
  `YuhuBreakNarrowOopEncode` (false), both `develop`.
- `hotspot/src/share/vm/yuhu/yuhuBlock.cpp:1348-1362` — capture the putfield/putstatic `StoreInst`
  and mark it when the field is a reference under compressed oops.
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp:1440`, `:1481`, `:1510`, `:1523` — `mark_encode` on
  both encode shapes; the `YuhuBreakNarrowOopEncode` poison paths in `CreateEncodeHeapOop` and
  `CreateDecodeHeapOop`.
- `hotspot/src/share/vm/yuhu/yuhuIRTransformer.cpp:186-219` — pipeline split (prefix, then scan,
  then RS4GC) and the report-only observer.
- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp:1481-1500` — the deciding check, `record_failure`,
  recorder release, return before `generate_native_code`.

Evidence still on disk (pre-fix vs post-fix IR of the crashing method):
- `test_yuhu/_tmp_yuhu_ir_RS4GC_com.sun.tools.javac.comp.Resolve__signatureMoreSpecific_t1793159087926.ll`
- `test_yuhu/scoped_repro/fixcheck/ircheck/_tmp_yuhu_ir_RS4GC_com.sun.tools.javac.comp.Resolve__signatureMoreSpecific_t1793194677751.ll`
- `test_yuhu/hs_err_pid28999.log`, `test_yuhu/hs_err_pid44925.log`, `test_yuhu/hs_err_pid53809.log`,
  `test_yuhu/hs_err_pid60568.log`, `hs_err_pid73471.log`
- `test_yuhu/scoped_repro/fixcheck/{run_r1..r5.log,progress.log}`, `test_yuhu/scoped_repro/verify.log` (2.5 Gb, historical)

## 13. References

- [Activity 065: RS4GC Inline Asm Incompatibility](065_RS4GC_InlineAsm_Incompatibility.md) — why
  inline asm is exempt from statepoint rewriting, and the Option 1 / Option 2 decision.
- [Activity 076: HashMap.resize — a live oop escaping as an integer](076_hashmap_resize_live_oop_integer_escape.md)
  — vector 3 of §4.3, the same failure family reached from the other direction.
- `doc/yuhu/HOW_TO_DISASSEMBLE_YUHU_CODE.md` — for the machine-code re-check in §11 item 4.
- HotSpot: `memory/universe.cpp` `initialize_heap()` (compressed-oop mode selection) and the mode
  accessors `Universe::narrow_oop_base()` / `narrow_oop_shift()` (`memory/universe.hpp:356-358`);
  `oopDesc::encode_heap_oop` / `decode_heap_oop` (`oops/oop.hpp:147-156`) are the C++ forms of the
  same arithmetic that the AArch64 macro assembler and this builder mirror.
- LLVM: `llvm::InlineAsm::get` (constraint strings, `hasSideEffects`),
  `RewriteStatepointsForGC` (statepoint lowering, `gc-live` bundles).

Note: the index in `activities/README.md` stops at activity 009 and has not been maintained for
010-082, so this document is not listed there.

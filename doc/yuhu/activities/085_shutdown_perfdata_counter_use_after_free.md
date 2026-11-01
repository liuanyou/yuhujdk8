# 085: SIGSEGV in `PerfLongVariant::inc()` at VM Shutdown — `ObjectMonitor::_sync_FutileWakeups` Dereferenced After `PerfDataManager::destroy()`

**Date:** 2026-10-31
**Status:** Root-caused from a truncated `hs_err` log. **No code change applied**, per the reviewer's instruction to leave the tree as it is. Fix options are recorded in §12 for later decision.
**Severity:** Medium — a **use-after-free at VM exit**. Not a Yuhu/JIT bug, not a GC bug, not a miscompile. Requires a specific shutdown race (see §11) and does not affect steady-state execution. Once triggered, it kills the process with a truncated crash report, so it can silently swallow whatever the true primary error was.
**Related commits:** `9779bc4c10` (the 084 fix; unrelated to this crash — no Yuhu file touches `exit_globals` / `perfMemory_exit` / `PerfDataManager`).
**Related docs:** [084](084_math_intrinsic_helper_address_zero_assert.md) (same "diagnose from artifacts, do not muzzle the symptom" discipline; §14), [083](083_c2_membar_volatile_precedent_unschedulable_graph.md), [082](082_narrow_oop_encode_decode_fold_stale_oop.md).

---

## 1. Problem Summary

At VM shutdown, the last surviving VM thread — `JavaThread "DestroyJavaVM"` — was executing `Threads::destroy_vm() → thread->exit(true) → ensure_join(this) → ObjectLocker(threadObj)`. The `java.lang.Thread` object's monitor was contended (someone else held or was joining on the main thread's object), so the main thread fell into the inflated-contention loop in `ObjectMonitor::EnterI` and, after a futile wakeup, executed

```cpp
if (ObjectMonitor::_sync_FutileWakeups != NULL) {
  ObjectMonitor::_sync_FutileWakeups->inc();
}
```

The static pointer was non-NULL, so the guard passed. But the `PerfLongVariant` object it points to had **already been `delete`d** by another thread running `PerfDataManager::destroy()` (reached via `exit_globals()` → `perfMemory_exit()`). The freed heap block had been recycled by the OS allocator and reused as another zeroed allocation, so the member `_valuep` at offset `0x28` read back as `0x0`. `PerfLongVariant::inc()` is the un-guarded one-liner

```cpp
inline void inc() { (*(jlong*)_valuep)++; }        // perfData.hpp:425
```

so dereferencing `_valuep == NULL` produced `SIGSEGV (SEGV_ACCERR) @ 0x0`.

Nothing in the codebase NULLs the 19 `ObjectMonitor::_sync_*` statics when the corresponding `PerfData` objects are destroyed, so the existing `!= NULL` check cannot catch a dangling pointer. The bug is structural: the shutdown sequence has two paths that both call `exit_globals()`, and they are not mutually exclusive with the main thread's own shutdown work.

---

## 2. Symptom

Artifact: `test_yuhu/hs_err_pid43945.log` (4122 bytes, 81 lines, mtime 2026-10-31 14:08). **The log is truncated mid-line at 81** — the last emitted content is `x5=` under "Register to memory mapping:", with no `Native frames`, `VM info`, `GC history`, or `Dynamic libraries` sections. Why the writer died is not proven here and is listed as an open item in §13.

Header block (verbatim from the log):

```
# SIGSEGV (0xb) at pc=0x000000010bbd88b0, pid=43945, tid=4099
# Problematic frame:
# V  [libjvm.dylib+0x3b48b0]  PerfLongVariant::inc()+0x10

siginfo:si_signo=SIGSEGV: si_errno=0, si_code=2 (SEGV_ACCERR), si_addr=0x0000000000000000

Current thread (0x0000000122782000):  JavaThread "DestroyJavaVM" [_thread_blocked, id=4099, ...]
```

Register snapshot:

```
 x0=0x0000600000210a28   x8=0x0000600000210a28    x9=0x0000000000000000
x28=0x0000000122782000   fp=0x000000016b3de300    lr=0x000000010c0c7358
pc=0x000000010bbd88b0
```

`x0`/`x8` (the `this` pointer) is in the `0x6000…` range — macOS C-heap / `malloc` small-object region, not the mmap'd `PerfMemory` region. `x9 = 0` is the faulting load target.

No Yuhu/JIT frames appear anywhere in the visible stack; no code-cache write fault; no `Killed: 9`.

---

## 3. Reconstructing the Stack From a Truncated Log

The image dylib is `build/macosx-aarch64-normal-server-slowdebug/jdk/lib/server/libjvm.dylib` (mtime 2026-10-31 11:22, i.e. the one produced by the 084 `make images` build). There is **no DWARF** in this dylib, so `llvm-dwarfdump --lookup` returns nothing; symbolisation uses `llvm-nm -n --defined-only` (a sorted, 184 440-row table) plus a `bisect` helper.

**dylib base** = `pc − file_offset_of_pc` = `0x10bbd88b0 − 0x3b48b0` = **`0x10b824000`**.

Every runtime address is reduced to a file offset by subtracting that base, then looked up in the `nm` table.

| runtime | file offset | symbol | Δ into symbol |
|---|---|---|---|
| `0x10bbd88b0` (pc) | `0x3b48b0` | `__ZN15PerfLongVariant3incEv` | `+0x10` |
| `0x10c0c7358` (lr) | `0x8a3358` | `__ZN13ObjectMonitor6EnterIEP6Thread` | `+0x704` |
| `0x10c0c6074` (saved lr at `fp`) | `0x8a2074` | `__ZN13ObjectMonitor5enterEP6Thread` | `+0x63c` |
| `0x10c21ef78` (saved lr at next fp) | `0x9fbf78` | `__ZN12ObjectLockerC2E6HandleP6Threadb` | `+0x10` |

Frame 4 lies past the truncation point of the stack dump and is deliberately left unsymbolised. Two other offsets I probed in the "Top of Stack" region — `ThreadLocalStorage::thread+0x10`, `nmethod::get_deopt_original_pc+0x3c` — are almost certainly stale slot contents rather than live return addresses, so I do not report them as frames.

The three visible frames give the whole call chain from `ObjectLocker::ObjectLocker` down to the faulting `inc()` — no Yuhu frames, no compiled Java frames.

---

## 4. Pinning the Exact Source Line Inside `EnterI`

`EnterI` contains **two** sites that increment `_sync_FutileWakeups`:

| site | source line | context |
|---|---|---|
| A | `objectMonitor.cpp:621-623` | "Inflated enter - Futile wakeup" — first contention loop; guard appears **before** `++nWakeups`; `Knob_SpinAfterFutile` read follows |
| B | `objectMonitor.cpp:795-797` | "Wait Reentry - futile wakeup" — wait-set re-entry loop; guard appears **after** `++nWakeups`; no `Knob_SpinAfterFutile` follows |

The disassembly around `EnterI+0x704` matches **site A** exactly (see the annotated listing in §5 of the previous conversation):

```
  8a331c:  bl   __ZN13ObjectMonitor7TryLockEP6Thread
  8a3320:  subs w8, w0, #0 ; cset w8, le ; tbnz → 8a3334         ; if TryLock ≤ 0 → guard
  8a3334:  adrp x8, 0x4f9a000 ; ldr x8,[x8,#0xf08]
  8a333c:  subs x8, x8, #0 ; cset w8, eq ; tbnz → 8a335c         ; if (_sync_FutileWakeups != NULL)
  8a3350:  ldr  x0, [x8, #0xf08]                                 ; 0x4f9af08 = _sync_FutileWakeups
  8a3354:  bl   0x3b48a0 <__ZN15PerfLongVariant3incEv>           ; → inc()
  8a3358:  ← lr here (EnterI+0x704)
  8a335c:  ldr  w8,[sp,#0x1c] ; add w8,w8,#1 ; str w8,[sp,#0x1c] ; ++ nWakeups
  8a3368:  adrp x8, 0x4f3c000 ; ldr w8,[x8,#0xf18]              ; Knob_SpinAfterFutile
```

`0x4f9af08` resolves exactly to `__ZN13ObjectMonitor19_sync_FutileWakeupsE` and `0x4f3cf18` exactly to `Knob_SpinAfterFutile` — the ordering "guard → `inc()` → `++nWakeups` → `Knob_SpinAfterFutile`" is only consistent with **site A**, [objectMonitor.cpp:620-630](../../hotspot/src/share/vm/runtime/objectMonitor.cpp).

The `os::PlatformEvent::park(jlong 1000)` and `os::PlatformEvent::park()` calls just before the `TryLock` in the same loop are the timed/untimed parks at `objectMonitor.cpp:602-611`, i.e. the thread had been parked on a contended inflated monitor and had just received a futile wakeup. That matches `[_thread_blocked]` in the thread header.

---

## 5. Falsifying the Layout / ODR Theory

A first hypothesis was that the compiler read a different member offset than the writer (ODR / class-layout mismatch between two TUs). Falsified by three independent checks against the binary:

1. Only one copy of each accessor symbol exists in the image dylib: `PerfLongVariant::inc()`, `PerfLongVariant::inc(jlong)`, and `PerfLong::get_value()` each appear exactly once.
2. `PerfData::PerfData`'s own constructor at `0x90ab78` initialises `_valuep` to NULL with `str xzr, [x8, #0x28]` — same `#0x28` the `inc()` load uses. No layout disagreement.
3. The header layout derived from `perfData.hpp` matches the machine code exactly: vptr `0x0`, `_name` `0x8`, `_v` `0x10`, `_u` `0x14`, `_on_c_heap` `0x18`, `_flags` `0x1c`, `_pdep` `0x20`, **`_valuep` `0x28`**.

---

## 6. Why a Live Counter Cannot Have `_valuep == NULL`

Grepping `_valuep =` in `perfData.cpp` yields exactly two writers:

- The constructor initialiser list: `PerfData(...) : _name(NULL), _u(u), _v(v), _valuep(NULL), _on_c_heap(false)` — sets NULL **before** `create_entry` runs.
- `PerfData::create_entry()` at `perfData.cpp:121-186`, which unconditionally assigns `_valuep = valuep` at line **182**. If `PerfMemory::alloc` returns NULL, the fallback is a `NEW_C_HEAP_ARRAY` (line 140-142) and `_on_c_heap = true`; either way `_valuep` is a valid non-NULL pointer.

Since every `PerfCounter` is constructed through `create_counter → create_entry` (`objectMonitor.cpp:2379` `NEWPERFCOUNTER` macro, `thread.cpp:3425` `ObjectMonitor::Initialize()`), **a properly constructed, live counter cannot have `_valuep == NULL`**. Reading NULL therefore implies the object is no longer the counter — the memory was freed and reused as some other zeroed allocation.

Corollary: this is **use-after-free with memory recycling**, not a "half-initialised object" and not a "counter never created". `_sync_FutileWakeups` cannot be the "never-created" case either, because `UsePerfData = true` on this build (verified by `-XX:+PrintFlagsFinal`) and the guard at 621 would then have skipped.

An interesting asymmetry in `perfData.hpp` reinforces this: the constructors guard their initial write with `if (is_valid()) *(jlong*)_valuep = initial_value;` (`:392`, `:413`), where `is_valid()` is precisely `_valuep != NULL` (`:307`). But the mutators `inc()` / `add()` / `set_value()` / `get_value()` (`:425-427`, `:487`, `:369`) **do not** invoke `is_valid()`. So the codebase does acknowledge "a `PerfData` may exist whose `_valuep` is not safe to touch"; it just doesn't apply that acknowledgement at the `EnterI` site.

---

## 7. Who Deletes the Counter

```
exit_globals()                        init.cpp:150-165
  └ perfMemory_exit()                 perfMemory.cpp:59-78
      ├ if (!UsePerfData) return;
      ├ if (!PerfMemory::is_initialized()) return;
      ├ if (!StatSampler::is_active())
      │     PerfDataManager::destroy()      perfData.cpp:277-295
      │       └ for (auto* p : _all) delete p;   ← deletes every wrapper
      │         _all = _sampled = _constants = NULL;
      └ PerfMemory::destroy()
```

Two things about `PerfDataManager::destroy()` matter:

- `PerfData::~PerfData()` at `perfData.cpp:112-119` frees `_name` (and the C-heap entry when `_on_c_heap`) but **never touches `_valuep`**. There is no "invalidation" pass.
- Nothing anywhere in the codebase NULLs the 19 `ObjectMonitor::_sync_*` statics at destroy time. They are defined to `NULL` once at `objectMonitor.cpp:2349-2367`, populated in `ObjectMonitor::Initialize()` at `:2373-2404`, and never written again — **except** `_sync_FailedSpins = NULL` at `:2509`, which is a knob-driven (`Knob_LogSpins == 0`) side-effect during `initialize_knobs()`, not part of shutdown.

So after `PerfDataManager::destroy()` the pointer is *dangling*, not null. The `if (_sync_FutileWakeups != NULL)` guard at `objectMonitor.cpp:621` and the identical one at `:795` cannot help.

`before_exit()` runs at `thread.cpp:4000` (destroy_vm step ③) and calls `StatSampler::disengage(); StatSampler::destroy();` at `java.cpp:503-504`, which sets `_task = NULL`. So by the time the concurrent `exit_globals()` executes, `StatSampler::is_active()` is `false` and `PerfDataManager::destroy()` **does** run. The "keep the counters alive because the sampler might still read them" safety net is disarmed on this path.

---

## 8. Who Is the Victim

The name `"DestroyJavaVM"` is written in exactly one place in the tree — `jni.cpp:5281`, inside `jni_DestroyJavaVM`'s `JavaVMAttachArgs`:

```cpp
destroyargs.name = (char *)"DestroyJavaVM";
res = vm->AttachCurrentThread((void **)&env, (void *)&destroyargs);
```

So the crash occurred on the thread running `jni_DestroyJavaVM`'s body, which calls `Threads::destroy_vm()` at `jni.cpp:5291`. The victim sequence in `Threads::destroy_vm()` is:

```
thread.cpp:3961   bool Threads::destroy_vm()
thread.cpp:3968     wait until only this thread is non-daemon
thread.cpp:3997     thread->invoke_shutdown_hooks()
thread.cpp:4000     before_exit(thread)
thread.cpp:4002     thread->exit(true)                ← ExitType=destroy_vm
thread.cpp:1874       JavaThread::exit → ensure_join(this)
thread.cpp:1717         ObjectLocker lock(threadObj, thread)   ← frame 3
objectMonitor.cpp       ObjectMonitor::enter  → EnterI         ← frames 2, 1
                        _sync_FutileWakeups->inc()             ← frame 0 (fault)
```

The `ObjectLocker` is on the main thread's **own** `java.lang.Thread` object (its `threadObj` handle), used to set `TERMINATED` and `notify_all` any waiters before the thread disappears. The `_thread_blocked` state and the presence of the futile-wakeup path both confirm real contention on that monitor.

---

## 9. Ruling Out Alternative Destructors

`exit_globals()` has four call sites in the tree; only one of them can run concurrently with `Threads::destroy_vm()` on the main thread:

| call site | reachable during shutdown of this shape? |
|---|---|
| `thread.cpp:4051` inside `Threads::destroy_vm()` | **No** — happens *after* `thread->exit(true)` returns, so it cannot race this thread's own step ④. |
| `vm_operations.cpp:450` inside `VM_Exit::doit()` on the **VMThread** | **Yes** — reached via `vm_exit()` (`java.cpp:574-595`) which is called from `JVM_Exit`/`JVM_Halt` (`jvm.cpp:396-409`) whenever a Java thread calls `System.exit()` / `Runtime.exit()` / `Runtime.halt()`, or from init-failure paths. Runs on a different thread than `destroy_vm()`. |
| `compileBroker.cpp:2104` | No — inside `#ifndef PRODUCT if (CompileTheWorld \|\| ExitOnFullCodeCache)`, both false on this run. Preceded by `before_exit(...)` + `vm_direct_exit(...)`. |
| `os::shutdown() → perfMemory_exit()` (`os_bsd.cpp:1075`) | No — `os::shutdown()` is only reached through `vm_shutdown()`, which is only called via `vm_shutdown_during_initialization` (initialization-failure path, `java.cpp:697-700`). |

The BSD **`atexit(perfMemory_exit_helper)`** route (`os_bsd.cpp:3610-3612` + `:3707-3719`) is also ruled out: `PerfAllowAtExitRegistration` defaults to `false` (`globals.hpp:3706`, verified on the built VM by `-XX:+PrintFlagsFinal`), so `atexit` was never registered on this run.

Cross-check: `grep -R "exit_globals\|perfMemory_exit\|PerfDataManager" hotspot/src/share/vm/yuhu/` returns zero matches. Yuhu's tier-6 pipeline does not participate in this crash.

Conclusion: the deleter is `VM_Exit::doit() → exit_globals() → perfMemory_exit() → PerfDataManager::destroy()`, running on the VMThread while the main thread was still executing `thread->exit(true)`.

---

## 10. The Two Concurrency Exit Paths

The design assumes these two shutdown paths are mutually exclusive:

| Path | Who runs `exit_globals()` | Assumption |
|---|---|---|
| Normal `DestroyJavaVM` | Main thread at `thread.cpp:4051`, *after* step ④ completes | Nothing else touches perf counters after step ④ |
| `System.exit()` / `Runtime.halt()` | VMThread inside `VM_Exit::doit()` | No `DestroyJavaVM` is concurrently active |

The gap: **nothing enforces that only one path runs**. If a shutdown hook (which is a daemon Java thread), or another still-live daemon, calls `System.exit(0)` (or `Runtime.getRuntime().halt(...)`) while the main thread is inside step ④, then:

- `before_exit()` at `java.cpp:447` short-circuits on the second entry (`_before_exit_status == BEFORE_EXIT_DONE`), so nothing there orders the two paths;
- `vm_exit()` at `java.cpp:574` posts `VM_Exit` and blocks the caller;
- the VMThread runs `VM_Exit::doit()`, which calls `exit_globals()` (`vm_operations.cpp:450`), which deletes every `PerfData`;
- the main thread is still parked in `ObjectMonitor::EnterI` on the contended `java.lang.Thread` monitor; when it wakes and executes `_sync_FutileWakeups->inc()`, it dereferences a freed pointer.

`exit_globals()` itself has a `static bool destructorsCalled` guard (`init.cpp:151`) which only prevents double-destroy; it does **not** stop a *user* of the perf data from dereferencing freed objects. `vm_direct_exit(_exit_code)` in `VM_Exit::doit()` at `java.cpp:607-611` uses `::exit(code)`, not `_exit`, so `atexit` handlers and stdio flushing run concurrently with the still-live main thread — the window between `exit_globals()` and actual process termination is not microscopic.

---

## 11. Frequency and Trigger Conditions

Three conditions must all hold. None is exotic, but the intersection is uncommon:

1. **`java.lang.Thread`-object contention at exit** — some thread must be `join()`-ing, `synchronized`-ed on, or JVMTI-inspecting the main thread's `Thread` object, forcing `ensure_join`'s `ObjectLocker` into `EnterI`'s slow path with at least one futile wakeup. Fast-path `TryLock` on an uncontended monitor never reaches the faulting line.
2. **A concurrent `System.exit()`/`Runtime.halt()`** from a daemon thread or a shutdown hook, i.e. something that runs `VM_Exit::doit()` on the VMThread *while* the main thread is still in step ④.
3. **The tiny timing window** between the main thread waking up and executing `bl PerfLongVariant::inc()` — a handful of instructions. Widened considerably in this `slowdebug` build (`assert`, `guarantee`, `OrderAccess::fence`, heavier bookkeeping) and on macOS with its preemptive scheduler.

Rough practical read: **rare on `-Xint` or a fast `product` build; noticeably more likely on `slowdebug` with a JVM that spawns daemon threads (Kafka-family workloads, thread-pool frameworks with `ExecutorService` shutdown hooks that call `System.exit` on failure, test-harness drivers with `System.exit(1)` on assertion failure — the `test_yuhu/com/example` files `LLVMInlineTest.java`, `C1LogicOpReproDriver.java`, `MixedIntFloatTest.java`, `DecimalTest.java` and `NineParameterTest.java` are examples of the latter).**

---

## 12. Fix Options (Recorded, Not Applied)

Ranking reflects the diagnostic-only outcome and the "fix at creation, not at consumer" preference from prior activities.

1. **Do not delete `PerfData` objects at exit — keep them alive.** Remove or gate the `delete p` loop in `PerfDataManager::destroy()` (`perfData.cpp:283-286`). Rationale: the process is about to `_exit` and the OS reclaims everything anyway; keeping the objects alive eliminates the entire UAF class for every static `PerfData*` holder in the VM (there are far more than the 19 `ObjectMonitor::_sync_*`), not just the one we hit. Cost: some retained memory until the process dies; no behavioural change for anything that still reads counters, because their `_valuep` remains valid. Lowest risk.
2. **Keep the deletes, but NULL every global that holds a `PerfData*` in the same critical section.** Local to `ObjectMonitor` this means writing `NULL` into all 19 statics at end of `PerfDataManager::destroy()`. Correct but requires a full audit of every VM-subsystem static counter holder (`SafepointSynchronize`, `ObjectSynchronizer`, `Biasing`, `PerfMemoryManager`, JVMTI, G1, `ciEnv`, etc.); each missed holder stays dangling. Higher effort, narrower coverage.
3. **Backport instead of inventing.** Upstream JDK 8u has had several shutdown-race fixes in this area; JDK 9+ moved most cleanup to single-threaded process teardown. Confirming which specific upstream change (if any) closes this crash should precede writing a fork-local patch. Network access was unavailable during this diagnosis, so this was not verified against the upstream tree.
4. **Zero-rebuild A/B to confirm the theory.** Rerun the exact failing command with `-XX:-UsePerfData`. Prediction: the crash disappears because `ObjectMonitor::Initialize()` never allocates the counters (`objectMonitor.cpp:2377` `if (UsePerfData)`), so `_sync_FutileWakeups` stays `NULL` and the guard at `:621` correctly skips. This does not fix anything; it validates the causal chain and, if the process now crashes somewhere else at exit, proves that the concurrent-`exit_globals()` race is broader than just perf data.

Rejected variants (recorded so the space stays closed):

- Adding an `is_valid()` or `_valuep != NULL` check inside `PerfLongVariant::inc()`. Masks the UAF and leaves the freed block dereferenced on adjacent code paths (`add`, `set_value`, `get_value`). Same anti-pattern as "muzzling the assert", which 084 §5 already rules out as a discipline.
- Removing `exit_globals()` from `VM_Exit::doit()`. Would break the `Runtime.halt()` path's guarantee that PerfMemory's external resources are released before the process exits; not the right lever.
- Serialising `PerfDataManager::destroy()` against every VM exit user with a lock. Enormistic surface area; option 1 gets the same effect at zero cost.

---

## 13. Open Items

1. **The truncated log itself.** 4122 bytes, cut mid-line at "Register to memory mapping: x5=". The thread header, siginfo, registers, instructions, and the start of the register mapping are all present; the very next section (`Native frames`, `VM info`, `GC history`, `Dynamic libraries`) is missing. Consistent with either (a) the report writer re-faulted inside `VMError::print_more` while walking the (already partly destroyed) VM state, or (b) `::exit` reached the process-death stage before the writer flushed the file. Not established; needs `strace`/`dtruss` or a rerun with `-XX:ErrorFile` pointing at a scratch path.
2. **Which application-level code path called `System.exit()` concurrently.** Not visible from this log; the next reproducer run should capture stdout and the exit code and correlate with the test harness (see §11 last paragraph for candidates in `test_yuhu/com/example/`).
3. **Whether anything synchronises on the main `java.lang.Thread` object.** Same source of evidence as item 2.
4. **The upstream JDK 8u position.** Once network is available, compare `vm_operations.cpp` / `perfData.cpp` / `thread.cpp` shutdown blocks against latest jdk8u to decide whether option 3 ("backport") is preferable to a fork-local option 1.
5. **Repo hygiene, unrelated to the crash:** `hotspot/src/share/vm/runtime/thread.cpp.txt` (172 KB, mtime 2025-06-27) sits inside a compiled source directory and is a stale duplicate of `thread.cpp`. `diff` of the two `Threads::destroy_vm` regions shows they are byte-identical today, so this file is not the cause of anything, but it does pollute grep results and should probably be removed.

---

## 14. Why This Is a Typical Issue — Rules to Carry Forward

1. **Never hand-decode AArch64 crash bytes.** The prior 084-session hand-decode read `[x8, #0x20]` where `llvm-objdump` shows `#0x28`. Both point at "some member offset", but only the correct one lines up with `perfData.hpp`'s layout; the wrong one silently supports a plausible-but-false layout theory. Use `llvm-objdump -d --start-address --stop-address`, always.
2. **`llvm-dwarfdump` is a dead end on this build** (no DWARF in the image dylib). Keep `/tmp/wx_math/nm_libjvm.txt` (`llvm-nm -n --defined-only`, 184 440 rows) plus the bisect helper at `/tmp/wx_math/sym.py` in the toolchain. Compute `base = pc − file_offset` from the crash header's `libjvm.dylib+0x…` token, then subtract base from every runtime address to convert to a file offset.
3. **Frame-pointer chain is trustworthy in `slowdebug`.** Rebuild the stack from `lr` (the immediate caller) plus saved `(fp, lr)` pairs at `fp+0` / `fp+8`. Stop at the first pair whose `fp` is outside the thread's declared stack range, and treat the remaining "Top of Stack" values as slot residue, not frames.
4. **Resolve the global in the disassembly, do not guess the field.** The two `adrp x8, 0x4f9a000 ; ldr x8,[x8,#0xf08]` operands resolve to a specific data-section address (`0x4f9af08`) which the `nm` table maps **exactly** to `ObjectMonitor::_sync_FutileWakeups`. Same for `Knob_SpinAfterFutile`. Pinning the counter's identity via the resolved global is what disambiguated site A from site B.
5. **Prove UAF, don't assume it.** Here, the proof was three facts that together are inescapable: (i) `_valuep` is written in only two places in the whole tree; (ii) one of them is unconditionally non-NULL after construction; (iii) the runtime value read back is NULL. If the counter were still alive, `_valuep` could not be NULL. Enumerating the writers via grep is what turns "looks like UAF" into "must be UAF".
6. **Enumerate the possible destructors before naming one.** Four call sites of `exit_globals()` in the tree; one was post-step-④ by construction, one was `#ifndef PRODUCT` test-only, one was init-failure-only, one was `atexit`-gated by a flag that reads `false` on this build. Only the VMThread `VM_Exit::doit()` path survived elimination. `-XX:+PrintFlagsFinal` on the built VM is cheap and falsifies whole branches.
7. **Guard `!= NULL` is not a lifecycle guarantee.** If a static pointer can dangle (its object is deleted but the pointer is not nulled), any `if (ptr != NULL)` check passes and still faults. Structural fixes must either *never* delete the pointee or *always* NULL the pointer at the same moment; neither is achievable by adding more null checks at consumers.
8. **Cross-check "no Yuhu involvement".** `grep -R "exit_globals\|perfMemory_exit\|PerfDataManager" share/vm/yuhu/` returning zero is the shortest way to say "not our JIT" — worth recording explicitly rather than leaving implicit.

---

## 15. Files and Artifacts

**Read for this diagnosis (no edit):**

- `test_yuhu/hs_err_pid43945.log` — 81 lines, 4122 bytes, the crash log.
- `hotspot/src/share/vm/runtime/objectMonitor.cpp` — `_sync_*` statics `:2349-2367`, `Initialize()` `:2373-2404`, site A `:620-630`, site B `:788-800`, the knob-only `_sync_FailedSpins = NULL` `:2509`.
- `hotspot/src/share/vm/runtime/perfData.hpp` — member layout `:288`-area, `is_valid()` `:307`, `get_value()` `:369`, guarded ctor writes `:392`, `:413`, `inc()` `:425`, `inc(jlong)` `:426`, `add()` `:427`, `set_value()` `:487`.
- `hotspot/src/share/vm/runtime/perfData.cpp` — ctor `:78-110` (`_valuep(NULL)`), `~PerfData` `:112-119`, `create_entry` `:121-186` (assigns `_valuep` unconditionally at `:182`), `PerfDataManager::destroy` `:277-295`.
- `hotspot/src/share/vm/runtime/perfMemory.cpp` — `perfMemory_exit` `:59-78`, `PerfMemory::destroy` `:156-…` (does not unmap; irrelevant to `si_addr=0`).
- `hotspot/src/share/vm/runtime/init.cpp` — `exit_globals()` `:150-165`, with the `static bool destructorsCalled` guard.
- `hotspot/src/share/vm/runtime/thread.cpp` — `create_vm` → `ObjectMonitor::Initialize()` `:3425`; `ensure_join` `:1713-1728`; `JavaThread::exit` → `ensure_join` `:1874`; `Threads::destroy_vm` `:3961-4054`, exit sequence and the trailing `exit_globals()` at `:4051`.
- `hotspot/src/share/vm/runtime/vm_operations.cpp` — `VM_Exit::doit` `:436-462`, `exit_globals()` at `:450`, `vm_direct_exit` at `:458`/`:460`.
- `hotspot/src/share/vm/runtime/java.cpp` — `before_exit` `:447-572` (single-run status machine, `StatSampler::disengage/destroy` at `:503-504`), `vm_exit` `:574-595`, `vm_direct_exit` `:607-611`, `vm_perform_shutdown_actions` `:613-631` (comment warns against calling `exit_globals()` there), `vm_shutdown_during_initialization` `:697-700`.
- `hotspot/src/share/vm/prims/jni.cpp` — `jni_DestroyJavaVM` `:5262-5301`, the sole writer of the `"DestroyJavaVM"` thread name at `:5281`.
- `hotspot/src/share/vm/prims/jvm.cpp` — `JVM_Exit` `:396-403`, `JVM_Halt` `:406-409` — the Java-level `System.exit` / `Runtime.halt` entry points.
- `hotspot/src/share/vm/compiler/compileBroker.cpp` — `#ifndef PRODUCT if (CompileTheWorld || ExitOnFullCodeCache) … exit_globals()` `:2100-2106` (ruled out).
- `hotspot/src/os/bsd/vm/os_bsd.cpp` — `perfMemory_exit_helper` `:3610-3612`, `atexit(...)` `:3707-3719` (ruled out; flag is `false`), `os::shutdown` `:1075-…` (init-failure only).
- `hotspot/src/share/vm/runtime/globals.hpp` — `PerfAllowAtExitRegistration` `:3706` (product default `false`).
- `hotspot/src/share/vm/runtime/synchronizer.hpp` — `class ObjectLocker` `:145-162`, ctor signature `ObjectLocker(Handle, Thread*, bool doLock = true)` matching the mangled frame-3 symbol.
- `hotspot/src/share/vm/runtime/statSampler.hpp` — `is_active()` `:62`.

**Investigation byproducts under `/tmp/wx_math/` (nothing under the repo):**

- `nm_libjvm.txt` — 184 440-line sorted `llvm-nm -n --defined-only` table of the image dylib.
- `pairs.txt` — `awk`-projected `(address, name)` two-column form of the table, consumed by `sym.py`.
- `sym.py` — `bisect`-based address→symbol resolver; takes file offsets and prints `<offset> -> <mangled_symbol>+<delta>`.
- `verify.sh`, `build_hotspot.log`, `build_images.log`, `build2.log`, `build2_images.log`, `matrix_sweep.log` — inherited from the 084 session; unchanged this session.
- `probe.cpp` — empty placeholder from an aborted repro attempt.

**Deliberately not created this session:** any `.log` in the repo root; any patched source; any `.d`/`.svg` graph dump.

---

## 16. References

- `hs_err` log: `test_yuhu/hs_err_pid43945.log` (the only artifact that contains the faulting registers, instruction bytes and partial stack).
- Binary under analysis: `build/macosx-aarch64-normal-server-slowdebug/jdk/lib/server/libjvm.dylib`, mtime `2026-10-31 11:22` (produced by the 084 `make images` step). Runtime and file-offset mapping confirmed by agreement on `PerfLongVariant::inc()` (`0x3b48a0` = the `nm` value = the crash's `pc − 0x10`).
- Flags observed on the built VM: `UsePerfData = true`, `PerfAllowAtExitRegistration = false`.
- Prior activity docs: [084](084_math_intrinsic_helper_address_zero_assert.md) (the immediately preceding fix; establishes the "muting the assert is not a fix" discipline this doc extends to "null-checking the mutator is not a fix" and to the "diagnosis-only until approval" behaviour), [083](083_c2_membar_volatile_precedent_unschedulable_graph.md), [082](082_narrow_oop_encode_decode_fold_stale_oop.md).
- README index: this doc is `085`; `doc/yuhu/README.md` lists only `001-009`, and no index references the recent activities (same note as 083 and 084).

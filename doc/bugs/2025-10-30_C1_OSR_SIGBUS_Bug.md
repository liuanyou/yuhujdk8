# C1 On-Stack Replacement (OSR) SIGBUS Crashes on ARM64

## Bug Summary
**Issue**: JVM crashed with `SIGBUS` when executing C1 OSR-compiled loops on ARM64  
**Root Cause**: C1's AArch64 OSR entry skipped the compiled-frame prologue (no `build_frame`), so OSR spill stores clobbered the caller frame and restored a garbage LR  
**Resolution**: Reinstate the frame build sequence in `c1_LIRAssembler_aarch64.cpp::osr_entry` so OSR entry mirrors the normal prologue/epilogue  
**Workaround**: Disable OSR compilation with `-XX:-UseOnStackReplacement` (not required after fix)  
**Status**: ✅ **FIXED** (October 31, 2025)  
**Date**: October 30, 2025 (updated October 31, 2025)

### Detailed Artifacts
Documentation, CFG, crash analysis, and regression assets live under `doc/bugs/C1_OSR_SIGBUS/`:
- [Fix roadmap](C1_OSR_SIGBUS/C1_OSR_FIX_ROADMAP.md)
- [hotLoop control-flow diagram](C1_OSR_SIGBUS/HOTLOOP_CFG.md)
- [Crash analysis](C1_OSR_SIGBUS/OSR_CRASH_ANALYSIS.md)
- [Minimal repro source](C1_OSR_SIGBUS/OSRCrashTest.java) and [test runner script](C1_OSR_SIGBUS/run_osr_test.sh)

## Problem Description

### Symptoms
- `SIGBUS`/`pc == lr` crashes at non-CodeCache addresses (heap, stack, metadata)
- Only reproduced with C1 OSR execution; interpreter-only or OSR-disabled runs stable
- Minimal Java test (`OSRCrashTest`) reproduced crash within seconds

### Example Crash Reports
```
SIGBUS (0xa) at pc=0x000000011b840430, pid=…
  lr=0x000000011b840430, sp=0x…  ← lr restored from corrupted stack slot
Instructions: executing metadata/heap (not CodeCache)
```

## Investigation Recap
- Disabling OSR (`-XX:-UseOnStackReplacement`) eliminated crashes but preserved OSR compilation → issue in OSR execution path
- Crash site analysis showed stack slots ([sp+64]) holding LR overwritten with metadata addresses → epilogue `ret` jumped into garbage
- Disassembly of OSR entry (B6) showed `str w?, [sp,#32/#36/#40]` without any preceding `sub sp,…` — OSR path reused caller frame directly
- Normal entry had prologue `sub sp,#0x50; stp x29,x30,[sp,#64]`; OSR entry did not
- Enabling the commented-out `build_frame(initial_frame_size_in_bytes(), bang_size_in_bytes());` in `c1_LIRAssembler_aarch64.cpp::osr_entry` reintroduced the prologue, matching the normal path
- After fix: OSR entry emits `stp x29, x30, [sp,#-frame_size]!` etc. before touching spill slots; no more LR corruption

## Root Cause (Final)
On AArch64, C1's `LIR_Assembler::osr_entry` skipped the compiled-frame prologue. The generated OSR entry immediately stored locals to `[sp+offset]` assuming the frame already existed. Because the interpreter frame had just been popped, `sp` pointed into the caller’s activation, so these stores overwrote the caller’s saved LR/x29. The epilogue later loaded that corrupted LR and executed `ret`, branching into arbitrary memory.

## Resolution
1. Restore frame construction inside `LIR_Assembler::osr_entry`:
   ```cpp
   // c1_LIRAssembler_aarch64.cpp
   void LIR_Assembler::osr_entry() {
       build_frame(initial_frame_size_in_bytes(), bang_size_in_bytes());
       // … existing OSR buffer copy logic …
   }
   ```
2. Verified generated code now matches normal entry:
   - Prologue: `stp x29, x30, [sp,#-frame_size]!`, `mov x29, sp`
   - Spill slots reserved before `str w?, [sp,#offset]`
   - Epilogue and backedge unchanged
3. Added targeted assertions/logging while validating (optional safeguard):
   ```cpp
   assert(CodeCache::contains(osr_entry), "OSR entry must be in CodeCache");
   ```

## Validation
- Minimal `OSRCrashTest` run 100 iterations with OSR enabled → **no crashes**
- Spring Boot workload: 1 hour soak with OSR enabled → **no crashes**
- LR watchpoint on `[sp+64]` confirmed no unintended writes post-fix
- lldb single-step at B6 shows prologue before spill stores

## Updated Workaround Guidance
- Workaround (`-XX:-UseOnStackReplacement`) no longer necessary once the fix is applied
- For older builds lacking the fix, workaround remains effective (documented for reference)

## Lessons Learned
- Always ensure OSR entry mirrors the standard prologue/epilogue on every architecture
- Watchpoints on saved LR slots quickly reveal frame-construction bugs
- Minimal deterministic reproduction (simple hot loop) was critical for fast iteration

---
**Resolution Date**: October 31, 2025  
**Status**: ✅ Fixed  
**Impact**: Critical (prior to fix)  
**Workaround**: Optional (legacy)  
**Recommendation**: Upgrade to build with OSR frame fix or backport patch to `c1_LIRAssembler_aarch64.cpp`


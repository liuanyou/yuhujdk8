# C1 OSR SIGBUS Crash Analysis (Resolved)

## Final Summary
- **Crash Type**: `SIGBUS (BUS_ADRALN)` with `pc == lr` pointing outside the CodeCache
- **Root Cause**: AArch64 C1 OSR entry skipped building the compiled frame (missing `build_frame(...)`). OSR spill stores wrote to the caller frame, corrupting the saved LR. The epilogue then executed `ret` with this bogus LR, jumping to arbitrary addresses.
- **Fix**: Re-enable the frame build sequence in `c1_LIRAssembler_aarch64.cpp::osr_entry` so the OSR entry mirrors the standard prologue/epilogue before touching any stack slots.

## Key Evidence
### 1. Saved LR was overwritten
- Original OSR entry emitted stores like:
  ```assembly
  str w2, [sp,#40]
  str w3, [sp,#36]
  str w4, [sp,#32]
  ```
  without any preceding `sub sp, #frame_size`. Because the interpreter frame had just been removed, `sp` pointed into the caller’s frame. These stores overwrote `[sp+64]` (the saved LR).
- Later, the epilogue (`ldp x29,x30,[sp,#64]; add sp,#0x50; ret`) reloaded the corrupted LR and branched into `0x11b84xxxx`, causing SIGBUS.

### 2. Watchpoint confirmation
- Setting a watchpoint on `[sp+64]` (saved LR slot) triggered immediately on the OSR path before the fix, confirming that OSR spill stores touched the caller frame.
- After the fix, the watchpoint never triggers outside the legitimate prologue/epilogue.

### 3. Code comparison
- Normal entry prologue: `sub sp,#0x50; stp x29,x30,[sp,#64]`
- OSR entry (B6) before fix: no prologue → it assumed the frame already existed.
- After the fix: OSR entry now starts with something like `stp x29, x30, [sp,#-0x50]!; mov x29, sp`, matching the normal path.

## Fix Verification
1. Minimal reproducer (`OSRCrashTest`): 100 runs with OSR enabled → **pass**
2. Spring Boot workload: 1-hour soak → **no SIGBUS**
3. `[sp+64]` watchpoint → **no unexpected writes**
4. Manual disassembly: OSR entry emits `build_frame` before copying locals

## Patch Snapshot
```diff
 void LIR_Assembler::osr_entry(...) {
-  // previously: copy OSR buffer without reserving the frame
+  build_frame(initial_frame_size_in_bytes(), bang_size_in_bytes());
   // then restore interpreter state into the compiled frame
   ...
 }
```

## Follow-up Notes
- Keep `OSRCrashTest` for regression testing.
- When touching OSR paths, always compare normal vs OSR prologue/epilogue on every architecture.
- For branches that do not yet have the fix, `-XX:-UseOnStackReplacement` remains a viable workaround.

---
**Fix Date**: 2025-10-31  
**Status**: ✅ Resolved

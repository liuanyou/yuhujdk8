# C1 OSR SIGBUS Bug - Fix Roadmap (Updated)

## Overview
Issue resolved: C1’s AArch64 OSR entry skipped building the compiled frame, so OSR spill stores overwrote the caller frame and corrupted LR. Reinstating the prologue (`build_frame(...)`) in `c1_LIRAssembler_aarch64.cpp::osr_entry` fixes the crashes.

## What We Did

### ✅ Minimal Reproduction
- Created `test/OSRCrashTest.java` to trigger OSR within seconds
- Used `-XX:CompileThreshold=100 -XX:+TraceOnStackReplacement` to confirm OSR execution

### ✅ Logging & Diagnostics
- Added temporary logging/assertions in `SharedRuntime::OSR_migration_begin` and interpreter OSR jump to confirm valid targets
- Set LR watchpoints on `[sp+saved_lr_offset]` to catch stack corruption

### ✅ Root Cause Fix
- `c1_LIRAssembler_aarch64.cpp` (`LIR_Assembler::osr_entry`): re-enable `build_frame(initial_frame_size_in_bytes(), bang_size_in_bytes());`
- Verified generated OSR entry now matches normal prologue/epilogue before touching spill slots

### ✅ Validation
- Minimal test: 100 successful runs with OSR enabled
- Spring Boot workload: 1-hour soak, no SIGBUS
- Confirmed `[sp+64]` (saved LR) unchanged except by prologue/epilogue
- CodeCache integrity checks (`CodeCache::contains(osr_entry)`) pass

## Notes for Future Work
- Keep `OSRCrashTest` for regression
- When modifying OSR paths, compare normal vs OSR prologue on each architecture
- For older builds lacking the fix, fallback flag `-XX:-UseOnStackReplacement` remains a reliable workaround

## Quick Command Reference
```bash
# Minimal repro with OSR enabled
./build/.../bin/java \
  -XX:+UseOnStackReplacement \
  -XX:+TraceOnStackReplacement \
  -XX:CompileThreshold=100 \
  OSRCrashTest
```

## Status
- 🟢 Fix merged locally (October 31, 2025)
- 🟢 Regression suite: PASS
- ⚠️ Backport as needed if shipping to other branches

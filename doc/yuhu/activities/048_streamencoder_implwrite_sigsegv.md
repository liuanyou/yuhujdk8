# Issue 048: SIGSEGV in StreamEncoder.implWrite After Return Value Fix

## Problem Summary

After fixing the return value propagation bug (Issue 047), a new **SIGSEGV** crash occurs in `sun.nio.cs.StreamEncoder.implWrite`:

```
SIGSEGV (0xb) at pc=0x000000010a9c62dc, pid=98734
J 122 yuhu sun.nio.cs.StreamEncoder.implWrite([CII)V (156 bytes)
si_addr=0x0000000100000208
```

**Status**: IllegalArgumentException is FIXED ✅, but now we have a hard crash (SIGSEGV)

---

## Crash Details

### Error Information
- **Signal**: SIGSEGV (0xb) - Segmentation Fault
- **Faulting Address**: `0x0000000100000208`
- **Location**: `implWrite` at offset `0x15c` from nmethod start
- **Thread State**: `_thread_in_Java` (crashed while executing Java code)

### Register State (Critical Evidence)

From crash log:
```
x0 =0x0000000000000001  (unknown)
x1 =0x0000000000000b86  (unknown)
...
x16=0x000000076abc003e  (suspicious - char array with low bits set)
x17=0x000000076ab4efee  (unknown)
x21=0x0000000100000000  ← CRITICAL: Invalid pointer!
x22=0x000000076abc0000  [C char array (VALID)
x23=0x000000076ab8b9c0  StreamEncoder (VALID)
x24=0x000000076ab4eff0  HeapCharBuffer (VALID, position=23, limit=23)
x25=0x000000076abf12f8  CoderResult (VALID)
x28=0x000000012e00c800  Thread pointer (CORRECT)
```

**Key Finding**: 
- Faulting address: `0x0000000100000208 = x21 + 0x208`
- `x21 = 0x0000000100000000` is NOT a valid pointer
- Should be using `x28` (thread pointer), but code is using corrupted `x21`

---

## Root Cause Analysis

### LLVM Register Spill Conflict with Yuhu Frame

#### 1. The Real Problem: Stack Pointer Aliasing

After fixing the return value bug (Issue 047), the Yuhu compiler now correctly stores method call results. However, this exposed a deeper issue: **LLVM backend's register spill instructions corrupt Yuhu's frame layout**.

**Execution Flow**:

```asm
; Phase 1: LLVM Prologue (allocates spill space)
sub sp, sp, #0x70        ; SP = initial_sp -112 bytes
stp x28, x27, [sp, #16]  ; Save callee-saved registers

; Phase 2: Yuhu Frame Allocation
sub x26, sp, #0xA0       ; x26 = SP -160 = initial_sp -272
mov sp, x26              ; SP = x26 (Yuhu frame bottom)
                         ; Now SP points 160 bytes below LLVM's SP

; Phase 3: LLVM Runtime Spill (THE BUG)
str x21, [sp, #8]        ; ← CRASH! Accesses initial_sp -264
```

**The Problem**: 
- LLVM thinks it's accessing `[llvm_sp - 112 + 8]`
- Actually accessing `[yuhu_sp + 8] = [llvm_sp -160 + 8]`
- **Offset mismatch**: 160 -112 = 48 bytes off!
- Falls into Yuhu's locals/stack region instead of spill area

#### 2. Why SIGSEGV Occurs

From crash log:
```
x21 = 0x0000000100000000  ← Invalid pointer!
si_addr = 0x0000000100000208 = x21 + 0x208
```

**What Happens**:
1. `x21` is a callee-saved register that LLVM decides to spill
2. Spill instruction: `str x21, [sp, #8]`
3. But `sp` now points to Yuhu frame bottom (not LLVM's expected location)
4. Address `0x100000208` is NOT a valid memory location → SIGSEGV

#### 3. Why This Happened After Issue 047 Fix

**Before**: Return values were discarded (bug), so fewer live ranges, less register pressure
**After**: Return values properly stored, more live variables, LLVM needs to spill MORE registers

The increased spill demand exposed the fundamental incompatibility:
- **LLVM prologue** allocates based on its own analysis
- **Yuhu frame** moves SP without adjusting LLVM's spill offsets
- **Runtime**: LLVM's hardcoded `[sp, #imm]` instructions access wrong addresses

---

### Why Heap Base Decoding Was a Red Herring

#### Initial Misdiagnosis

The faulting address `0x0000000100000208` looked like:
```
0x100000000 + 0x208
↑               ↑
heap base?      Thread::last_Java_fp offset
```

This suggested compressed oop decoding with wrong heap base. But:

**Critical Evidence Against Heap Base Theory**:
1. Method parameters are CORRECT during `encoder.encode()` call:
   ```
   x1 = 0x000000076ab8bad8  (cb - CharBuffer) ✅
   x2 = 0x000000076ab4eff0  (bb - ByteBuffer) ✅
   x28 = 0x000000012e00c800  (Thread pointer) ✅
   ```
2. LLVM IR shows correct heap base usage in other places
3. Crash happens in **safepoint polling / frame management code**, not field access

#### The Real x21 Story

`x21 = 0x0000000100000000` is NOT a decoded oop - it's:
- An **uninitialized callee-saved register**
- LLVM allocated x21 for some virtual register
- At runtime, x21 was never initialized before use
- When spilled, it contains garbage → crash

**Why x21 Instead of x28?**
- LLVM ran out of callee-saved registers (x19-x28 = only 10 regs)
- Register allocator assigned different virtual regs to x21 at different points
- Some code paths didn't initialize x21 properly

---

### Solution: Static Spill Space Reservation

The fix reserves space for ALL possible LLVM spills within the Yuhu frame:

```cpp
// yuhu_globals.hpp
const int yuhu_llvm_spill_slots = 10;  // 80 bytes (x19-x28)

// yuhuFunction.cpp - Force LLVM prologue to allocate 80 bytes
llvm::AllocaInst* spill_reservation = builder()->CreateAlloca(
  llvm::ArrayType::get(YuhuType::intptr_type(), yuhu_llvm_spill_slots),
  nullptr, "llvm_spill_reservation");

// yuhuStack.cpp - Yuhu frame also reserves 80 bytes
_extended_frame_size = frame_words + locals_words + yuhu_llvm_spill_slots;
```

**How This Fixes It**:
1. LLVM sees 80-byte alloca → allocates ~80-112 bytes in prologue
2. Yuhu frame includes same 80-byte reservation
3. When `mov sp, x26`, LLVM's `[sp, #offset]` accesses fall within reserved area
4. No corruption of locals/stack → no SIGSEGV

---

## Files Modified

- **hotspot/src/share/vm/yuhu/yuhu_globals.hpp** - Added `yuhu_llvm_spill_slots` constant (80 bytes)
- **hotspot/src/share/vm/yuhu/yuhuFunction.cpp** - Create 80-byte alloca to force LLVM prologue allocation
- **hotspot/src/share/vm/yuhu/yuhuStack.cpp** - Extended frame size by 80 bytes for spill reservation
- **hotspot/src/share/vm/yuhu/yuhuCompiler.cpp** - Include spill space in frame_size calculation
- **test_yuhu/hs_err_pid98734.log** - SIGSEGV crash log
- **test_yuhu/_tmp_yuhu_ir_sun.nio.cs.StreamEncoder__implWrite.ll** - LLVM IR file

---

## Related Issues

- **Issue #047**: Return value propagation bug (FIXED ✅ - exposed this spill issue)
- **Issue #048**: SIGSEGV due to LLVM register spill conflict (FIXED by static spill reservation)

---

*Updated: 2026-02-28 PM - Corrected root cause analysis from heap base decoding to LLVM spill conflict*

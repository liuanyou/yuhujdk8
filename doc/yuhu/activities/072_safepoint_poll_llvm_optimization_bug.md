# Activity 072: Safepoint Poll LLVM Optimization Bug

## Problem Description

When the Yuhu compiler generates multiple safepoint polls in the same method, LLVM's optimizer can incorrectly optimize the second (and subsequent) safepoint poll calls, causing runtime crashes.

## Root Cause

### LLVM Optimization Behavior

LLVM's Common Subexpression Elimination (CSE) pass identifies that multiple `call void @gc.safepoint_poll()` calls target the same function address and optimizes away redundant address computations.

**Example - Math.min method with two safepoint polls:**

```llvm
; LLVM IR
entry:
  call void @gc.safepoint_poll()  ; First poll
  ...
  call void @gc.safepoint_poll()  ; Second poll (same target)
```

**What LLVM generates (optimized):**

```assembly
; First safepoint poll - full sequence
adrp  x19, 0x0000000104640000  ; Load polling page address
ldr   wzr, [x19]               ; Trigger safepoint check

; Second safepoint poll - LLVM REUSES x19
mov   x10, x19                 ; BUG: Reuses polling page address
blr   x10                      ; BUG: Jumps to polling page, not a function!
```

### Why This Is Wrong

**Safepoint poll is NOT a function call!** It's a memory read from the polling page that:
- Triggers a SIGSEGV if a safepoint is pending
- The signal handler recognizes the fault address as the polling page
- The thread then enters the safepoint

The `gc.safepoint_poll()` declaration in LLVM IR is just a placeholder - it should generate `adrp + ldr` instructions, **NOT** an actual function call via `blr`.

## The Bug Chain

1. **Yuhu generates** multiple `call void @gc.safepoint_poll()` in LLVM IR
2. **LLVM's CSE pass** sees both calls target the same address
3. **LLVM optimizes** the second call by reusing the register from the first call
4. **Generated code** becomes `mov x10, x19` + `blr x10` instead of `adrp + ldr`
5. **YuhuVirtualAddressScanner** in `yuhuORCPlugins.cpp` looks for pattern:
   ```
   adrp + ldr/movk + blr
   ```
   But the second safepoint poll is just `mov + blr`, **doesn't match the pattern**
6. **`clean_eliminated_call_sites()`** removes the call site record because it wasn't recognized
7. **At runtime**: `blr x10` jumps to polling page (`0x0000000104640000`), which is not executable
8. **SIGSEGV occurs** with corrupted LR (e.g., `0x4c4d8001118ae1e4`)
9. **Signal handler** calls `SharedRuntime::get_poll_stub(corrupted_pc)`
10. **Assertion fails**: `assert(cb && cb->is_nmethod()) failed: safepoint polling: pc must refer to an nmethod`

## Crash Example

```
# Internal Error (sharedRuntime.cpp:545)
# assert(cb && cb->is_nmethod()) failed: safepoint polling: pc must refer to an nmethod

Current thread: 0x000000013380e000

Native frames:
  C  0x4c4d8001118ae1e4    <- Corrupted LR (should be 0x00000001118ae1e4)
  v  ~RuntimeStub::yuhu_static_call_stub
  J 10 yuhu java.lang.AbstractStringBuilder.ensureCapacityInternal(I)V

Polling page: 0x0000000104640000

Faulting instruction sequence:
  0x00000001118ae1b8: adrp  x19, 0x0000000104640000
  0x00000001118ae1bc: ldr   wzr, [x19]
  0x00000001118ae1c0: nop
  0x00000001118ae1c4: mov   x10, x19           <- x10 = polling page address!
  0x00000001118ae1cc: cmp   w9, w8
  0x00000001118ae1d0: csel  w8, w8, w9, gt
  0x00000001118ae1d4: str   w8, [sp, #8]
  0x00000001118ae1d8: mov   w19, #0x24
  0x00000001118ae1dc: movk  w19, #0xdead, lsl #16
  0x00000001118ae1e0: blr   x10                <- Jumps to polling page! BUG!
```

## Why Other Solutions Don't Work

### Option A: Add dummy parameter to gc.safepoint_poll(i32)
**Rejected**: HotSpot's `gc_safepoint_poll()` runtime function has a fixed signature `void gc_safepoint_poll()`. Cannot be changed.

### Option B: Use volatile inline asm
**Rejected**: RS4GC (Return Statepoints for GC) won't recognize inline asm as a safepoint poll location.

### Option C: Insert optimization barrier between calls
**Problematic**: Requires inserting dummy volatile operations between every pair of safepoint polls. Fragile and impacts code quality.

### Option D: Fix YuhuVirtualAddressScanner (RECOMMENDED)
**Best solution**: Handle LLVM's optimization by tracking register dependencies.

## Recommended Solution: Option D

### Approach

Update `YuhuVirtualAddressScanner::scan_forwards_for_call_targets()` to recognize safepoint polls even when LLVM optimizes away the `adrp` instruction.

### Implementation Strategy

1. **Track register assignments backwards** when `blr xN` is encountered
2. **Check if xN was set by**:
   - `adrp xN, ...` (current pattern - already handled)
   - `mov xN, xM` where xM was previously set by `adrp` (new case)
   - `movk xN, ...` sequence (may also need handling)
3. **Trace back through register copies** to find the original `adrp` that loaded the polling page address
4. **Verify the address** matches `os::get_polling_page()` to confirm it's a safepoint poll
5. **Create proper call site record** even for optimized sequences

### Pattern Matching Logic

```cpp
// Current pattern (already works):
// adrp xN, polling_page
// ldr/movk xN, offset
// blr xN

// New pattern to handle:
// adrp xM, polling_page     ; Original load
// ldr wzr, [xM]             ; First poll
// ...
// mov xN, xM                ; LLVM optimization - copy register
// blr xN                    ; Second poll (needs to be recognized)
```

### Key Considerations

- **Register tracking scope**: Need to track register assignments within a reasonable instruction window (e.g., 20-30 instructions)
- **Register clobbering**: Stop tracking if the register is overwritten by another instruction
- **Multiple polls**: May need to handle chains like `mov xN, xM` where `xM` was set by `mov xM, xL` from `adrp xL`
- **Polling page verification**: Confirm the address in the original `adrp` matches the actual polling page

## Impact

This bug affects **any method with multiple safepoint polls**, which includes:
- Methods with entry safepoint + exit safepoint
- Methods with loops (safepoint polls in loop back edges)
- Long methods with multiple safepoint poll insertion points

## References

- Crash log: `test_yuhu/hs_err_pid87385.log`
- IR file: `test_yuhu/_tmp_yuhu_ir_java.lang.Math__min.ll`
- Scanner code: `hotspot/src/share/vm/yuhu/yuhuVirtualAddressPatcher.cpp`
- Safepoint poll generation: `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp`

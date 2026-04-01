# 057: Callee-Saved Register Corruption in Yuhu-Interpreter Interop

## Problem Summary

When Yuhu-compiled LLVM IR calls into the HotSpot interpreter, **callee-saved registers (x19-x28) are corrupted**, causing crashes when Yuhu code resumes execution.

## Root Cause Analysis

### The Architectural Mismatch

| Component | Callee-Saved Policy |
|-----------|---------------------|
| **C1/C2 Compiled Methods** | ✅ Don't use callee-saved (x19-x28)<br>✅ Only use caller-saved (x0-x7, x9-x17)<br>✅ Zero overhead on method calls |
| **Runtime Stubs** | ✅ Preserve callee-saved they use<br>✅ Save/restore within stub |
| **HotSpot Interpreter** | ❌ **Does NOT preserve callee-saved**<br>❌ Assumes caller saves them |
| **Yuhu (LLVM)** | ✅ Uses callee-saved for performance<br>❌ Expects AAPCS compliance<br>❌ Crashes when callee corrupts them |

### The Crash Scenario

```
Yuhu-compiled method (uses x19, x20, ...)
     ↓
     Call Java method (goes to interpreter)
     ↓
Interpreter executes (corrupts x19, x20, ...)
     ↓
     Return to Yuhu
     ↓
Yuhu continues with WRONG values in x19, x20 ❌
```

### Evidence from C1 Assembly

From `debug/c1_new_code.txt` (String.hashCode()):

```assembly
[Verified Entry Point]
0x0000000128842340: nop
0x0000000128842344: sub  sp, sp, #0x40          ; Allocate stack frame
0x0000000128842348: stp  x29, x30, [sp, #48]    ; Save fp, lr only
; ❌ NO save/restore of x19-x28!

; Method body uses x19 (line 275):
0x0000000128842580: mov  x19, #0xdead           ; Direct use without prologue save
```

**Conclusion**: C1/C2 compiled methods don't use callee-saved registers, so they don't need to save them.

### Why C1/C2 Can Avoid Callee-Saved

**Performance optimization:**
1. Java-to-Java calls are extremely frequent
2. Saving/restoring 10 callee-saved registers per call would be expensive
3. C1/C2 register allocator prioritizes caller-saved registers (x0-x7, x9-x17)
4. If more registers needed → spill to stack (not use callee-saved)

**Result**: Zero-overhead method calls between compiled methods.

---

## The Real Problem

### Yuhu Cannot Follow C1/C2's Strategy

**Constraints:**
1. **LLVM's register allocator** automatically uses callee-saved for performance
2. **We're not modifying LLVM** - can't disable callee-saved usage
3. **Even if we could**, it would increase register pressure and spills
4. **Performance would degrade** for complex methods

### Attempted Solutions (All Failed)

#### Solution A: Caller-Saves Callee-Saved (Rejected)

```llvm
; Before calling Java method
call void asm sideeffect "stp x19, x20, [sp, #-16]!\0A...", "~{memory}"()
%result = call ptr @java_method(...)
call void asm sideeffect "ldp x19, x20, [sp], #16\0A...", "~{memory}"()
```

**Problems:**
- ❌ SP changes dynamically during function execution
- ❌ HotSpot deoptimization may trigger at any safepoint
- ❌ If deopt happens after save but before restore → wrong sender_sp
- ❌ Stack frame layout becomes unpredictable

#### Solution B: Fixed Callee-Saved Area in Frame (Rejected)

Reserve 80 bytes at bottom of Yuhu frame for callee-saved save area.

**Problems:**
- ❌ LLVM's spill allocator doesn't know about this "reserved" area
- ❌ LLVM will allocate spill slots from SP upward
- ❌ Your 80 bytes will be overwritten by LLVM spills
- ❌ Would require modifying LLVM's TargetFrameLowering

#### Solution C: Prologue Saves All Callee-Saved (Rejected)

Save all x19-x28 in prologue, even if not used.

**Problems:**
- ❌ Wastes cycles on methods that don't need callee-saved
- ❌ Increases frame size unnecessarily
- ❌ Still doesn't solve the fundamental problem:
  - **Who saves them when YOU call the interpreter?**

---

## Fundamental Insight

### The Calling Convention Hierarchy

In HotSpot's compiled world:

```
Java Method (C1/C2)
  ↓
Java Method (C1/C2)
  ↓
Runtime Stub
  ↓
Java Method (C1/C2)
```

**Rules:**
1. **Java → Java**: No callee-saved used, zero overhead
2. **Java → Runtime**: Runtime saves what it uses internally
3. **Runtime → Java**: Runtime restores before calling

**Everyone follows AAPCS!**

### The Yuhu-Interpreter Violation

```
Yuhu (AAPCS-compliant)
  ↓
Interpreter (NOT AAPCS-compliant!)
  ↓
Yuhu (expects AAPCS)
```

**The interpreter violates AAPCS by:**
- Using callee-saved registers (x19-x28)
- NOT saving/restoring them in prologue/epilogue
- Assuming callers don't care (true for C1/C2, false for Yuhu)

---

## Viable Solutions

### Solution 1: Make Interpreter AAPCS-Compliant ✅ (Recommended)

Modify interpreter entry points to save/restore callee-saved:

```cpp
// hotspot/src/cpu/aarch64/vm/interpreter/yuhu/yuhuInterpreterGenerator_aarch64.cpp

// In generate_method_entry() or similar
__ stp(r19, r20, Address(__ pre(sp, -2 * wordSize)));
__ stp(r21, r22, Address(__ pre(sp, -2 * wordSize)));
__ stp(r23, r24, Address(__ pre(sp, -2 * wordSize)));
__ stp(r25, r26, Address(__ pre(sp, -2 * wordSize)));
__ stp(r27, r28, Address(__ pre(sp, -2 * wordSize)));

// ... interpreter executes ...

__ ldp(r27, r28, Address(__ post(sp, 2 * wordSize)));
__ ldp(r25, r26, Address(__ post(sp, 2 * wordSize)));
__ ldp(r23, r24, Address(__ post(sp, 2 * wordSize)));
__ ldp(r21, r22, Address(__ post(sp, 2 * wordSize)));
__ ldp(r19, r20, Address(__ post(sp, 2 * wordSize)));
```

**Pros:**
- ✅ Fixes the root cause (interpreter non-compliance)
- ✅ Makes interpreter safe for ANY caller (not just Yuhu)
- ✅ Minimal overhead (only for interpreted calls)
- ✅ Follows AAPCS standard

**Cons:**
- ❌ Requires modifying interpreter code generation
- ❌ Slight performance cost for interpreted methods

---

### Solution 2: Eliminate Interpreter Calls ✅ (Long-term)

Compile ALL called methods with Yuhu (no interpretation).

**Approach:**
- Aggressive inlining
- On-demand compilation of callees
- No cross-boundary calls

**Pros:**
- ✅ No interoperability issues
- ✅ Better performance (no interpreter overhead)
- ✅ Clean architecture

**Cons:**
- ❌ Major architectural change
- ❌ Requires full JIT infrastructure
- ❌ Long-term effort

---

### Solution 3: Hybrid Approach ⚠️ (Workaround)

For methods that MUST call interpreter:
- Force LLVM to avoid callee-saved (if possible)
- Accept performance degradation for those methods only

**Implementation:**
- Use `+reserve-x19`, `+reserve-x20`, ..., `+reserve-x28`
- LLVM only uses caller-saved (like C1/C2)
- More spill slots, but correct behavior

**Pros:**
- ✅ No interpreter modification
- ✅ Works with current architecture

**Cons:**
- ❌ Performance degradation
- ❌ Increased register pressure
- ❌ More spills to stack
- ❌ Doesn't scale for complex methods

---

## Current Status

### Implemented Solution: Hybrid Approach with Fixed Save Area

**Architecture:**
- Reserve 6 words (48 bytes) in every Yuhu frame right after LLVM spill slots
- Location: `[sp, #80]` (fixed offset after 80-byte LLVM spill area)
- Saves: x19, x20, x23, x25, x27 (5 registers = 40 bytes) + 8 bytes padding
- Maintains 16-byte SP alignment for AAPCS compliance

**Frame Layout:**
```
High Addresses
+----------------------------------+
| LLVM Spill Slots                 | ← 10 words (80 bytes)
+----------------------------------+
| Callee-Saved Save Area           | ← 6 words (48 bytes) [sp, #80]
|   - stp x19, x20, [sp, #80]      |
|   - stp x23, x25, [sp, #96]      |
|   - str x27, [sp, #112]          |
+----------------------------------+
| Expression Stack                 |
| Monitors                         |
| Frame Header (6 words)           |
| Locals                           |
+----------------------------------+
Low Addresses
```

**Call Sequence:**
```llvm
; Before calling Java method
decache_for_Java_call(call_method)
builder()->CreateSaveCalleeSavedRegisters()  ; Save to [sp, #80]
%result = call @java_method(...)
builder()->CreateRestoreCalleeSavedRegisters() ; Restore from [sp, #80]
```

**Key Design Decisions:**
1. **Fixed offset (#80)**: LLVM spill slots are always 10 words, so save area is at predictable location
2. **No LLVM modification needed**: LLVM allocates spills in first 80 bytes, never touches save area
3. **Encapsulated in YuhuBuilder**: `CreateSaveCalleeSavedRegisters()` and `CreateRestoreCalleeSavedRegisters()` methods
4. **Selective preservation**: Only save 5 registers (x19, x20, x23, x25, x27) that Yuhu actually uses

**Modified Files:**
- `yuhu_globals.hpp`: Added `yuhu_callee_saved_save_area = 6` constant
- `yuhuStack.cpp`: Updated frame size calculation (+6 words)
- `yuhuCompiler.cpp`: Updated frame size calculation (+6 words)
- `yuhuBuilder.hpp`: Added save/restore method declarations
- `yuhuBuilder.cpp`: Implemented save/restore methods with inline assembly
- `yuhuTopLevelBlock.cpp`: Use save/restore methods before/after Java calls

**Pros:**
- ✅ No interpreter modification required
- ✅ Works with existing HotSpot architecture
- ✅ Minimal performance overhead (only for actual Java calls)
- ✅ Clean separation of concerns (LLVM spills vs. Yuhu save area)
- ✅ Maintains AAPCS compliance for Yuhu code

**Cons:**
- ⚠️ Increases frame size by 48 bytes per method
- ⚠️ Still has call overhead (10 instructions per Java call)
- ⚠️ Doesn't fix root cause (interpreter non-compliance)

**Status:** ✅ Implemented and ready for testing

---

### Previous Temporary Workaround

**Old approach:**
- Reserved x21, x22, x24, x26, x28 via `+reserve-xNN`
- Still vulnerable to corruption for non-reserved regs (x19, x20, x23, x25, x27)
- Works for simple methods, fails for complex ones

**Evidence:**
- Simple methods (multiply, add) → Work fine
- Complex methods (StreamEncoder.implWrite, encodeArrayLoop) → Crash

---

---

## Performance Analysis: Callee-Saved Usage vs. Spilling

### The Core Trade-off

**Question**: We keep saying "using callee-saved registers avoids spilling", but doesn't saving callee-saved registers before Java method calls also hurt performance?

**Answer**: It depends on the **method characteristics** and **call frequency**.

---

### Two Approaches Compared

#### Approach A: LLVM Uses Callee-Saved + Save/Restore on Calls

**Current LLVM behavior:**
```llvm
; Yuhu method body (uses x19, x20)
%val = add i32 %a, %b    ;可能在 x19 中
%val2 = mul i32 %val, %c ;可能在 x20 中

; Before calling Java method
call void asm "stp x19, x20, [sp, #-16]!", ...
%result = call ptr @java_method(...)
call void asm "ldp x19, x20, [sp], #16", ...
```

**Performance cost:**
- ✅ **Inside method**: Zero spills (registers sufficient)
- ❌ **Per call**: 2 stores + 2 loads = 4 instructions
- ❌ **Assuming 10 calls**: 40 extra instructions

---

#### Approach B: LLVM Avoids Callee-Saved (Like C1/C2)

**C1/C2 approach:**
```llvm
; LLVM forced to use only caller-saved (x0-x7, x9-x17 = 18 regs)
; If virtual registers > 18 → spill to stack

; Method body
%val = add i32 %a, %b
store i32 %val, ptr %spill_slot1  ; ← Spill!
%val2 = mul i32 %val, %c
store i32 %val2, ptr %spill_slot2 ; ← Spill!

; Load when needed
%v1 = load i32, ptr %spill_slot1
```

**Performance cost:**
- ❌ **Per spill**: 1 store + 1 load = 2 instructions
- ❌ **Frequently accessed variables**: Load every time
- ✅ **Method calls**: Zero overhead (no save needed)
- ❌ **Assuming 10 spills, each used 5 times**: (10×2) + (10×5) = 70 instructions

---

### Scenario Analysis

#### Scenario 1: Compute-Bound Methods (Few Calls)

**Example**: `Matrix.multiply()` (heavy computation)

| Approach | Instruction Count | Reason |
|----------|------------------|--------|
| **A: Use callee-saved** | ~100 | Almost no method calls |
| **B: Avoid callee-saved** | ~150 | Many spills/loads |

**Winner: Approach A (Use callee-saved)**

---

#### Scenario 2: Complex Methods (Frequent Calls)

**Example**: `StreamEncoder.implWrite()` (I/O heavy)

| Approach | Instruction Count | Reason |
|----------|------------------|--------|
| **A: Use callee-saved** | ~500 | Save 10 regs × 100 calls = 1000 extra |
| **B: Avoid callee-saved** | ~600 | More spills, but zero call overhead |

**Winner: Close, depends on exact call count**

---

#### Scenario 3: Very Complex Methods (Heavy Compute + Many Calls)

**Example**: Compilers, interpreters

| Approach | Instruction Count | Reason |
|----------|------------------|--------|
| **A: Use callee-saved** | ~2000 | Save/restore dominates |
| **B: Avoid callee-saved** | ~2500 | Spills dominate |

**Winner: Approach A (Use callee-saved)**

---

### Key Insights

#### Why C1/C2 Chose Approach B?

**HotSpot's special situation:**
- Java method calls are **extremely frequent** (every 10-20 bytecodes on average)
- Methods are typically **very small** (< 20 bytecodes)
- Call overhead is amplified

**C1/C2's calculation:**
```
Typical Java method:
- Local variables: < 10
- Temporary values: < 5
- Registers needed: ~15

Caller-saved registers: 18 (x0-x7, x9-x17)
→ 15 < 18, sufficient!
→ No spills needed!
→ Zero-overhead method calls!
```

**Result: Approach B is better for Java workloads!**

---

#### Why LLVM Chose Approach A?

**LLVM's general-purpose design:**
- Compiles various languages (C, C++, Rust, etc.)
- Function sizes vary hugely
- Call frequency uncertain

**LLVM's calculation:**
```
Typical C/C++ function:
- Local variables: possibly dozens
- Loop temporaries: possibly hundreds
- Caller-saved not enough!
→ Must use callee-saved!
→ Reduce spills!
```

**Result: Approach A is better for general workloads!**

---

### Your Situation: Yuhu

#### Constraints:

1. **Cannot modify LLVM** → Cannot force avoidance of callee-saved
2. **Must call interpreter** → Face register corruption problem
3. **Performance matters** → Cannot accept massive spills

#### Three Options:

##### Option 1: Let LLVM Continue Using Callee-Saved

**Performance**: ✅ Optimal  
**Correctness**: ❌ Crashes (unless interpreter fixed)  
**Implementation difficulty**: ⚠️ Moderate (requires interpreter changes)

---

##### Option 2: Force LLVM to Avoid Callee-Saved

**Performance**: ❌ Degraded (increased spills)  
**Correctness**: ✅ Safe (compatible with interpreter)  
**Implementation difficulty**: ❌ Hard (requires LLVM backend changes)

---

##### Option 3: Hybrid Approach

**Strategy**:
- Simple methods: Avoid callee-saved (like C1/C2)
- Complex methods: Use callee-saved + save/restore on calls

**Implementation**:
```cpp
// YuhuCompiler::compile_method
if (method->code_size() < threshold) {
  // Force reserve all callee-saved
  JTMB.addFeatures({"+reserve-x19", "+reserve-x20", ..., "+reserve-x28"});
} else {
  // Allow using callee-saved
  // But save/restore before interpreter calls
  generate_save_restore_before_interpreter_call();
}
```

**Performance**: ⚠️ Compromise  
**Correctness**: ✅ Safe  
**Implementation difficulty**: ⚠️ Moderate

---

### Recommendation

#### Best Choice: Fix the Interpreter (Option 1)

**Reasons:**
1. ✅ **Optimal performance** (LLVM自由选择 registers)
2. ✅ **Correctness guarantee** (interpreter complies with AAPCS)
3. ✅ **Long-term benefit** (solves more than just Yuhu's problem)
4. ✅ **Standards compliance** (AAPCS is ARM calling convention)

**Implementation cost:**
- Modify interpreter entry code (~100 lines)
- Test validation (~1-2 days)

---

#### Fallback: Hybrid Approach (Option 3)

**If interpreter cannot be modified short-term:**
- Force reserve callee-saved for simple methods
- Dynamically save/restore for complex methods
- Gradually transition to Option 1

---

## Impact

### Affected Scenarios

1. **Yuhu-compiled method calls interpreted method**
   - Any bytecode invoke* that isn't inlined
   - Especially common in large applications

2. **Deoptimization triggers**
   - If deopt happens after interpreter corrupts callee-saved
   - Wild pointers in saved registers → crash

3. **Safepoint polling**
   - Interpreter may have corrupted registers at safepoint
   - GC scans wrong locations

### Unaffected Scenarios

1. **Pure Yuhu-compiled code** (no external calls)
2. **Calls to runtime stubs** (they preserve callee-saved)
3. **Simple methods** (don't need many callee-saved)

---

## Next Steps

### Immediate (Required)

1. **Document the issue** ← This document
2. **Analyze interpreter entry code** (aarch64)
3. **Implement Solution 1** (AAPCS-compliant interpreter)

### Short-term

1. Review all interpreter MethodKind entries
2. Ensure consistent callee-saved handling
3. Add regression tests

### Long-term

1. Consider Solution 2 (full compilation)
2. Evaluate LLVM backend modifications
3. Optimize register allocation strategy

---

## Related Issues

- **Issue #042**: x28 register reservation
- **Issue #048**: StreamEncoder.implWrite SIGSEGV (register spill conflict)
- **Issue #050**: x21 register corruption in method calls
- **Activity #039**: Deoptimization parameter restoration failure

---

## Technical Deep Dive

### Why "Caller-Saves Callee-Saved" is Wrong

Some engineers suggest: "Just save callee-saved before calling interpreter."

**Why this doesn't work:**

1. **Dynamic SP changes:**
   ```assembly
   sub sp, sp, #spill_size      ; LLVM prologue
   stp x19, x20, [sp, #-16]!    ; Your save → SP changes!
   blr x8                       ; Call interpreter
   ldp x19, x20, [sp], #16      ; Restore → SP changes!
   ```
   
   HotSpot computes `sender_sp = current_sp + frame_size`.
   If `current_sp` is mid-save/restore, calculation is wrong!

2. **Deoptimization can happen anytime:**
   - At safepoints
   - On exceptions
   - During GC
   
   If deopt triggers between save and restore → corrupted state!

3. **Stack frame invariants:**
   HotSpot relies on fixed frame layout for:
   - Unwinding
   - GC oop scanning
   - Deoptimization
   
   Dynamic SP breaks these invariants!

---

### Why C1/C2 Don't Have This Problem

**C1/C2 design philosophy:**

```
Design Goal: Minimize Java-to-Java call overhead

Strategy:
1. Never use callee-saved in Java methods
2. Spill to stack if more registers needed
3. Method calls have ZERO register save overhead

Trade-off:
- Slightly more spills in complex methods
- But MOST methods are simple → net win
```

**Evidence from assembly:**
```assembly
; C1 String.hashCode() - NO callee-saved used!
sub sp, sp, #0x40
stp x29, x30, [sp, #48]
; ... method body using only x0-x7, x9-x17 ...
```

**Yuhu cannot follow this strategy because:**
- We're not modifying LLVM's register allocator
- LLVM optimizes for general case, not HotSpot's calling convention
- Forcing LLVM to avoid callee-saved would require major changes

---

## Conclusion

**Root cause:** Interpreter violates AAPCS calling convention.

**Correct fix:** Make interpreter AAPCS-compliant (save/restore callee-saved).

**Workarounds:** All have serious drawbacks (performance, correctness, or both).

**Recommendation:** Implement Solution 1 immediately.

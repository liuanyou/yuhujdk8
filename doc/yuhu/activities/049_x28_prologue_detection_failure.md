# Issue 049: x28 Prologue Detection Failure in Yuhu Compiler

## Problem Summary

During Yuhu compilation, the compiler attempts to find x28 (callee-saved register) in the LLVM prologue to patch stubs. However, **x28 is not always saved in the prologue**, causing the assertion to fail:

```
assert(x28_offset != -1) failed: x28 not found in prologue - unable to patch x28 restoration stubs
```

**Crash Location**: `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp:671`

**Error Type**: Internal VM Error (assertion failure)

---

## Root Cause Analysis

### Why x28 Is Not Always in Prologue

LLVM's register allocator decides which callee-saved registers to preserve based on **actual usage** in the function. If x28 is never used in a particular method, LLVM will **not save it in the prologue**.

**Example from crash log**:
```
Current CompileTask:
yuhu:  467    9       6     com.example.YuhuTest::helperAdd (4 bytes)
```

The method `helperAdd` is very simple (only 4 bytes), likely just returning a constant or doing simple arithmetic. Such methods may not need x28 at all!

### Current Implementation Flow

1. **Analyze prologue** to find x28 offset:
   ```cpp
  int x28_offset = YuhuPrologueAnalyzer::find_x28_offset_from_x29(llvm_code_start);
   ```

2. **Assert that x28 was found**:
   ```cpp
  assert(x28_offset != -1, "x28 not found in prologue - unable to patch x28 restoration stubs");
   ```

3. **Patch stubs** to use correct x28 offset:
   ```cpp
  patch_stubs_for_method(target, x28_offset);
   ```

**Problem**: Step 2 assumes x28 is ALWAYS present, which is incorrect.

---

## Why This Approach Is Flawed

### 1. x28 Usage Is Method-Dependent

LLVM only saves registers that are actually needed:
- **Simple methods** (e.g., `helperAdd`) → May not use x28→ No prologue save
- **Complex methods** (e.g., `implWrite`) → Uses many registers → x28 saved

### 2. x28 Is Already Reserved - No Need for Patching

**Critical**: In Issue #042, we already implemented **x28 register reservation** at the LLVM JIT level:

```cpp
// hotspot/src/share/vm/yuhu/yuhuCompiler.cpp
JTMB.addFeatures({"+reserve-x28"});  // Reserve x28 as callee-saved
```

This means:
- ✅ **x28 is preserved throughout the entire function execution**
- ✅ **x28 will NOT be corrupted by method calls**
- ✅ **No runtime patching of x28 offsets is needed**

### 3. The Original Problem Was Different

Issue #042's problem was:
- **Before**: x28 was modified by adapters during method calls → calculation errors
- **Fix**: `+reserve-x28` feature ensures x28 is never used/corrupted
- **Result**: x28 remains stable across method calls

**But the current code still tries to patch x28 offsets**, which is:
- ❌ Unnecessary (x28 is already protected)
- ❌ Error-prone (fails when x28 is not in prologue)
- ❌ Redundant work (contradicts Issue #042's fix)

---

## Solution Options

### Option A: Remove x28 Detection and Patching (Recommended) ✅

**Rationale**: 

1. **Issue #042 already solved the real problem**: The `+reserve-x28` feature ensures x28 is never used or corrupted by LLVM
2. **x28 patching is redundant**: Since x28 is reserved, it doesn't need runtime protection
3. **Prologue detection fails for simple methods**: Methods that don't use x28 won't have it in prologue, causing assertion failures

**Changes**:
1. Remove `find_x28_offset_from_x29()` call
2. Remove `patch_stubs_for_method()` call  
3. Remove the assertion
4. Document that x28 is already protected by Issue #042's `+reserve-x28` feature

**Pros**:
- ✅ Simpler code
- ✅ No more false assertions
- ✅ Works with both simple and complex methods
- ✅ Consistent with Issue #042's fix
- ✅ Eliminates redundant work

**Cons**:
- ⚠️ Need to verify no other code depends on x28 patching

---

### Option B: Make x28 Detection Optional

Keep the detection but make it non-fatal if x28 is not found:

```cpp
int x28_offset = YuhuPrologueAnalyzer::find_x28_offset_from_x29(llvm_code_start);
if (x28_offset != -1) {
  patch_stubs_for_method(target, x28_offset);
} else {
  // x28 not used in this method, no patching needed
  tty->print_cr("Yuhu: x28 not found in prologue, skipping patch for %s", func_name);
}
```

**Pros**:
- ✅ Backward compatible with existing patch logic
- ✅ Handles both cases gracefully

**Cons**:
- ⚠️ Still maintains unnecessary patch complexity
- ⚠️ Requires keeping `YuhuPrologueAnalyzer::find_x28_offset_from_x29()`

---

### Option C: Force x28 Usage in All Methods (Rejected) ❌

Force LLVM to always use x28 by creating a dummy reference:

```cpp
// Force x28 to be considered "used"
llvm::Value* dummy = builder()->CreateReadRegister(28);
```

**Why Rejected**:
- ❌ Artificially increases register pressure
- ❌ Wastes a callee-saved register
- ❌ Unnecessary complexity
- ❌ Goes against LLVM's optimization goals

---

## Recommended Fix

**Implement Option A**: Remove x28 detection and patching entirely.

### Code Changes Required

#### 1. `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp`

**Remove** (around line 669-675):
```cpp
// Step 2: Find x28's offset relative to x29 and patch all stubs
int x28_offset = YuhuPrologueAnalyzer::find_x28_offset_from_x29(llvm_code_start);
assert(x28_offset != -1, "x28 not found in prologue - unable to patch x28 restoration stubs");
tty->print_cr("Yuhu: Found x28 at offset %d from x29, patching stubs for %s",
              x28_offset, func_name);
patch_stubs_for_method(target, x28_offset);
```

**Replace with**:
```cpp
// Step 2: No x28 patching needed
// With static spill reservation (Issue #048), all callee-saved registers (x19-x28)
// have reserved space in the Yuhu frame. LLVM's spill instructions automatically
// access the correct locations without manual patching.
```

#### 2. Update Frame Size Calculation

Since we're no longer patching x28, ensure frame size calculation doesn't depend on it:

```cpp
// Step 3: Calculate final frame_size using actual prologue size
int frame_size = frame_words + locals_words + actual_prologue_words;
// Note: actual_prologue_words already includes space for any callee-saved regs
// that LLVM decided to use. Our static spill reservation ensures safety.
frame_size = align_size_up(frame_size, 2);
```

#### 3. Documentation Update

Update comments in `yuhu_globals.hpp`:

```cpp
// LLVM register spill reservation (AArch64)
// Reserve space for ALL callee-saved registers (x19-x28 = 10 regs = 80 bytes)
// This covers the MAXIMUM possible spills. Individual methods may use fewer registers.
// Since we statically reserve this space, NO runtime patching of register offsets is needed.
const int yuhu_llvm_spill_slots = 10;  // 80 bytes - absolute maximum for AArch64
```

---

## Testing Strategy

### Test Case 1: Simple Method (No x28 Usage)
```java
public class SimpleTest {
    public static int helperAdd(int a, int b) {
        return a + b;
    }
    
    public static void main(String[] args) {
        System.out.println(helperAdd(1, 2));
    }
}
```

**Expected**: Compiles successfully without assertion failure.

### Test Case 2: Complex Method (x28 Usage)
```java
public class ComplexTest {
    public static void implWrite(byte[] buf, int off, int len) {
        // Complex logic that uses many registers
        // Likely to trigger x28 usage
    }
}
```

**Expected**: Compiles successfully, x28 properly handled by spill reservation.

### Test Case 3: Mixed Workload
Run with multiple compilation tiers:
```bash
java -XX:+UseYuhuCompiler -XX:TieredStopAtLevel=6 \
     -XX:CompileCommand=yuhuonly,com/example/* \
    com.example.YuhuTest
```

**Expected**: No crashes, stable execution.

---

## Impact Analysis

### Before Fix
- ❌ Simple methods crash during compilation
- ❌ Assertion failure at line 671
- ❌ Inconsistent behavior (works for some methods, fails for others)

### After Fix
- ✅ All methods compile successfully
- ✅ Unified handling regardless of register usage
- ✅ Simpler codebase (remove ~10-20 lines)
- ✅ Better alignment with static spill reservation strategy

---

## Files Modified

1. **hotspot/src/share/vm/yuhu/yuhuCompiler.cpp**
   - Remove x28 detection logic
   - Remove x28 patching logic
   - Update comments

2. **hotspot/src/share/vm/yuhu/yuhu_globals.hpp**
   - Update documentation for spill reservation

3. **Optional**: `hotspot/src/share/vm/yuhu/yuhuPrologueAnalyzer.cpp`
   - May remove `find_x28_offset_from_x29()` if no other callers exist

---

## Related Issues

- **Issue #042**: Yuhu x28 register corruption in method calls (FIXED by `+reserve-x28` feature)
  - Implemented LLVM JIT-level x28 reservation
  - Ensures x28 is never used or corrupted throughout function execution
  - Makes runtime x28 patching unnecessary
  
- **Issue #048**: SIGSEGV due to LLVM register spill conflict (FIXED by static spill reservation)
  - Reserves 80 bytes for all callee-saved registers (x19-x28)
  - Prevents LLVM spill instructions from corrupting Yuhu frame
  
- **Issue #047**: Return value propagation bug (FIXED - exposed Issue #048)

---

## Timeline

- **Discovered**: 2026-02-28 (during Issue #048 testing)
- **Root Cause Identified**: x28 not always saved in prologue
- **Solution Designed**: Remove x28 patching, rely on static spill reservation
- **Status**: Pending implementation

---

*Created: 2026-02-28 PM*

# 050: x21 Register Corruption in Method Calls

## Problem

After calling `CharBuffer.array()` (interpreted execution), the `x21` register is corrupted, causing incorrect thread field access.

### Root Cause

Yuhu-generated code uses `x21` to hold `Thread*` pointer, but after calling interpreted methods, `x21` is overwritten by the interpreter (e.g., with `TemplateInterpreter::_active_table`).

According to ARM64 JVM calling convention, `x21` is callee-saved, but the template interpreter doesn't preserve it when used for dispatch table.

### Execution Trace

```asm
; Before src.array() call
x21 = 0x0000000123024800 (Thread*)
x28 = 0x0000000123024800 (Thread*)

; After src.array() call (interpreted)
x21 = 0x000000010b6bd4b0 (TemplateInterpreter::_active_table) ← CORRUPTED
x28 = 0x0000000123024800 (Thread*) ← Still correct

; Subsequent exception check
ldr  x8, [x21, #8]  ; ❌ Wrong! Accesses 0x10b6bd4b8 instead of Thread->_pending_exception
```

### IR Pattern

The LLVM IR reads `x28` once at function entry:
```llvm
%rthread = call i64 asm "mov $0, x28", "=r"()
%thread_ptr = inttoptr i64 %rthread to ptr
```

LLVM backend allocates `%thread_ptr` to a callee-saved register (e.g., `x21`), assuming it will be preserved across calls. However, the interpreter violates this assumption.

## Solution

Mark `x21` as a reserved register in LLVM target configuration, identical to the fix for `x28` in issue #042.

### Fix

In `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp`:

```cpp
JTMB.addFeatures({"+reserve-x28", "+reserve-x21"});
```

This prevents LLVM from using `x21` for general purposes and ensures `Thread*` remains stable across method calls.

### Why This Works

- `+reserve-x21` tells LLVM that `x21` has special semantics and must be treated as preserved
- LLVM will either:
  1. Avoid using `x21` entirely, or
  2. Reload from the source (`x28`) after any potential clobber
- The interpreter can freely use `x21` for dispatch without breaking Yuhu's assumptions

## Verification

After applying the fix, verify:
1. No more `x21` corruption after interpreted calls
2. Exception checks correctly access `Thread->_pending_exception`
3. No performance regression (register pressure should be manageable)

## Related Issues

- **042**: `x28` register corruption - same root cause, same solution
- Both issues stem from interpreter violating callee-saved register conventions

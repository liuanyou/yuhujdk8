# AArch64 CAS trailing_membar Assertion Failure

**Date**: November 3, 2025  
**Status**: ✅ RESOLVED  
**Severity**: HIGH  
**Platform**: AArch64 (ARM64) only  
**JDK Version**: OpenJDK 8 custom build  
**Compiler**: C2 (Server Compiler)

---

## Summary

An assertion failure in the AArch64 architecture description file (`aarch64.ad`) occurred when compiling non-volatile CAS (Compare-And-Swap) operations. The `needs_acquiring_load_exclusive()` predicate function incorrectly assumed all CAS operations must have a `trailing_membar`, causing an assertion failure for non-volatile CAS operations like those in `ConcurrentHashMap::casTabAt()`.

**Root Cause**: The predicate function `needs_acquiring_load_exclusive()` had an overly strict assertion that all CAS operations must have a `trailing_membar`, but non-volatile CAS operations (e.g., `Unsafe.compareAndSwapObject()`) do not have memory barriers.

**Resolution**: Modified the predicate to return `false` (use normal CAS instruction) when `trailing_membar()` is `NULL`, instead of asserting failure.

---

## Symptoms

### Crash Signature
```
# A fatal error has been detected by the Java Runtime Environment:
#
#  Internal Error (/Users/.../hotspot/src/cpu/aarch64/vm/aarch64.ad:1349), pid=74720, tid=20739
#  assert(ldst->trailing_membar() != NULL) failed: expected trailing membar
#
# JRE version: OpenJDK Runtime Environment (8.0)
# Java VM: OpenJDK 64-Bit Server VM (25.0-b70-debug mixed mode bsd- compressed oops)
```

### When It Occurs
- During C2 compilation on AArch64 platform
- When compiling methods containing non-volatile CAS operations
- Specifically when compiling `java.util.concurrent.ConcurrentHashMap::casTabAt()`
- Only occurs when `UseBarriersForVolatile` is `false` (default)

### Example Method
```java
// java.util.concurrent.ConcurrentHashMap
static final <K,V> boolean casTabAt(Node<K,V>[] tab, int i,
                                    Node<K,V> c, Node<K,V> v) {
    return U.compareAndSwapObject(tab, ((long)i << ASHIFT) + ABASE, c, v);
}
```

This method uses `Unsafe.compareAndSwapObject()`, which is **not** a volatile operation, so it does not have memory barriers.

---

## Root Cause Analysis

### The Problem: Overly Strict Assertion

#### Original Code
```cpp
// hotspot/src/cpu/aarch64/vm/aarch64.ad
bool needs_acquiring_load_exclusive(const Node *n)
{
  assert(is_CAS(n->Opcode()), "expecting a compare and swap");
  if (UseBarriersForVolatile) {
    return false;
  }

  LoadStoreNode* ldst = n->as_LoadStore();
  assert(ldst->trailing_membar() != NULL, "expected trailing membar");  // ← FAILS HERE

  // so we can just return true here
  return true;
}
```

#### Why It Fails

1. **Volatile CAS Operations**:
   - Have `trailing_membar()` → Use acquiring load (`ldaxr`/`stlxr`) → Return `true`
   - Example: `AtomicReference.compareAndSet()` (volatile)

2. **Non-Volatile CAS Operations**:
   - Do **NOT** have `trailing_membar()` → Should use normal CAS (`ldxr`/`stxr`) → Should return `false`
   - Example: `Unsafe.compareAndSwapObject()` (non-volatile)

3. **The Bug**:
   - The original code assumed **all** CAS operations are volatile
   - When a non-volatile CAS is encountered, `trailing_membar()` returns `NULL`
   - The assertion fails, crashing the JVM

### Instruction Matching in ADLC

The AArch64 architecture description file defines two sets of CAS instructions:

1. **Normal CAS Instructions** (no predicate):
   ```cpp
   instruct compareAndSwapP(iRegINoSp res, indirect mem, iRegP oldval, iRegP newval, rFlagsReg cr) %{
     match(Set res (CompareAndSwapP mem (Binary oldval newval)));
     // Uses: cmpxchg (normal CAS, no acquire semantics)
   %}
   ```

2. **Acquiring CAS Instructions** (with predicate):
   ```cpp
   instruct compareAndSwapPAcq(iRegINoSp res, indirect mem, iRegP oldval, iRegP newval, rFlagsReg cr) %{
     predicate(needs_acquiring_load_exclusive(n));  // ← Only matches if returns true
     match(Set res (CompareAndSwapP mem (Binary oldval newval)));
     // Uses: cmpxchg_acq (acquiring CAS with acquire semantics)
   %}
   ```

**ADLC Matching Rule**: Instructions with predicates are tried first. If the predicate returns `true`, that instruction is selected. Otherwise, the matcher falls back to the instruction without a predicate.

### Memory Ordering Semantics

- **Volatile CAS**: Requires acquire semantics on load and release semantics on store
  - Uses `ldaxr` (load-acquire-exclusive) and `stlxr` (store-release-exclusive)
  - Has `trailing_membar()` (MemBarAcquire) to ensure visibility

- **Non-Volatile CAS**: Only requires atomicity, not ordering
  - Uses `ldxr` (load-exclusive) and `stxr` (store-exclusive)
  - No memory barriers needed

---

## Resolution

### The Fix

```cpp
// hotspot/src/cpu/aarch64/vm/aarch64.ad
bool needs_acquiring_load_exclusive(const Node *n)
{
  assert(is_CAS(n->Opcode()), "expecting a compare and swap");
  if (UseBarriersForVolatile) {
    return false;
  }

  LoadStoreNode* ldst = n->as_LoadStore();
  // Only volatile CAS operations have trailing_membar.
  // Non-volatile CAS operations should not use acquiring load.
  if (ldst->trailing_membar() == NULL) {
    // Non-volatile CAS: return false to use normal CAS instruction
    return false;
  }

  // Volatile CAS with trailing_membar: use acquiring load
  return true;
}
```

### What Changed

1. **Removed the assertion**: No longer asserts that `trailing_membar()` must be non-NULL
2. **Added NULL check**: If `trailing_membar()` is `NULL`, return `false` to use normal CAS
3. **Preserved volatile behavior**: If `trailing_membar()` exists, return `true` to use acquiring CAS

### Result

- **Non-volatile CAS** (`trailing_membar() == NULL`):
  - Returns `false` → Matches `compareAndSwapP` → Uses `cmpxchg` (normal CAS)
  - ✅ Correct behavior

- **Volatile CAS** (`trailing_membar() != NULL`):
  - Returns `true` → Matches `compareAndSwapPAcq` → Uses `cmpxchg_acq` (acquiring CAS)
  - ✅ Correct behavior (unchanged)

---

## Technical Details

### Related Code Locations

1. **Predicate Function**:
   - `hotspot/src/cpu/aarch64/vm/aarch64.ad:1341-1358`

2. **CAS Instruction Definitions**:
   - Normal: `hotspot/src/cpu/aarch64/vm/aarch64.ad:8191-8207` (`compareAndSwapP`)
   - Acquiring: `hotspot/src/cpu/aarch64/vm/aarch64.ad:8268-8287` (`compareAndSwapPAcq`)

3. **Similar Predicates** (for reference):
   - `needs_releasing_store()`: `hotspot/src/cpu/aarch64/vm/aarch64.ad:1324-1335`
     - Also checks `trailing_membar() != NULL` but returns the result, doesn't assert

4. **Java Method**:
   - `jdk/src/share/classes/java/util/concurrent/ConcurrentHashMap.java:758-761`

### Debugging Process

1. **Initial Error**: Assertion failure with no method information
2. **Added Debug Print**: Modified `needs_acquiring_load_exclusive()` to print method name before assertion
3. **Identified Method**: `java/util/concurrent/ConcurrentHashMap::casTabAt`
4. **Analyzed CAS Type**: Confirmed it's non-volatile (`Unsafe.compareAndSwapObject()`)
5. **Fixed Logic**: Changed assertion to conditional return

---

## Verification

### Test Case
```bash
javac /path/to/Main.java
# Previously crashed with assertion failure
# Now compiles successfully
```

### Expected Behavior
- Non-volatile CAS operations compile without errors
- Volatile CAS operations still use acquiring load (unchanged behavior)
- No performance regression

---

## Related Issues

This bug is related to the broader topic of memory ordering in concurrent operations:

- **Volatile Operations**: Require memory barriers for visibility guarantees
- **Non-Volatile Atomic Operations**: Only require atomicity, not ordering
- **AArch64 Instructions**: Provide both normal and acquiring/releasing variants

---

## Lessons Learned

1. **Don't assume all operations of a type have the same semantics**: Not all CAS operations are volatile
2. **Use predicates to select instructions, not to validate assumptions**: Predicates should return `true`/`false`, not assert
3. **Check similar predicates for patterns**: `needs_releasing_store()` correctly handles NULL `trailing_membar()` by returning `false`

---

## References

- AArch64 Architecture Reference Manual: Load-Exclusive and Store-Exclusive instructions
- OpenJDK HotSpot C2 Compiler: Architecture Description Language (ADLC)
- Java Memory Model: Volatile semantics vs. atomic operations


# C2 MemBarAcquire DecodeN Precedent Bug

**Date**: August 21, 2026  
**Status**: ✅ RESOLVED  
**Severity**: HIGH  
**Platform**: AArch64 (ARM64) with compressed oops enabled  
**JDK Version**: OpenJDK 8 custom build  
**Compiler**: C2 (Server Compiler)

---

## Summary

An assertion failure in the AArch64 architecture description file (`aarch64.ad`) occurred during C2 compilation when the `unnecessary_acquire()` predicate encountered a `DecodeN` node as the `MemBarAcquire` precedent instead of the expected `LoadStore` node (e.g., `GetAndSetP`/`GetAndSetN`).

**Root Cause**: In `LibraryCallKit::inline_unsafe_load_store()`, when intrinsifying `AtomicReference.getAndSet()` with compressed oops enabled, the `load_store` variable was reassigned from `GetAndSetNNode` to a wrapping `DecodeNNode`. The `DecodeNNode` was then incorrectly passed as the precedent to `insert_mem_bar_trailing_load_store(Op_MemBarAcquire, ...)`.

**Resolution**: Save the original `LoadStore` node before the `DecodeN` reassignment, and pass the saved node as the `MemBarAcquire` precedent.

---

## Symptoms

### Crash Signature
```
# A fatal error has been detected by the Java Runtime Environment:
#
#  Internal Error (aarch64.ad:1242), pid=60480, tid=17923
#  assert(load_store->is_LoadStore()) failed: unexpected graph shape
#
# JRE version: OpenJDK Runtime Environment (8.0)
# Java VM: OpenJDK 64-Bit Server VM (25.0-b70-debug mixed mode bsd- compressed oops)
```

### Stack Trace
```
V  unnecessary_acquire(Node const*)
V  State::_sub_Op_MemBarAcquire(Node const*)
V  State::DFA(int, Node const*)
V  Matcher::Label_Root(Node const*, State*, Node*, Node const*)
V  Matcher::match_tree(Node const*)
V  Matcher::xform(Node*, int)
V  Matcher::match()
V  Compile::Code_Gen()
V  Compile::Compile(ciEnv*, C2Compiler*, ciMethod*, int, bool, bool, bool)
```

### When It Occurs
- During C2 compilation on AArch64 with compressed oops enabled
- When compiling methods containing `AtomicReference.getAndSet()` (or any `Unsafe.getAndSetObject()`)
- The method must be hot enough to trigger C2 compilation (compile level 4)
- Reproducible — occurs deterministically every time the method is C2-compiled

### Example Method
```java
// org.apache.kafka.clients.consumer.internals.Fetcher
public void validateOffsetsIfNeeded() {
    RuntimeException exception = cachedOffsetForLeaderException.getAndSet(null);
    if (exception != null) {
        throw exception;
    }
    // ...
}
```

The `AtomicReference.getAndSet(null)` call triggers the intrinsic path.

---

## Root Cause Analysis

### The Buggy Code

In `hotspot/src/share/vm/opto/library_call.cpp`, function `inline_unsafe_load_store()`:

```cpp
// Step 1: Create the atomic LoadStore node
Node* load_store;
// ...
// With compressed oops on 64-bit:
load_store = _gvn.transform(new (C) GetAndSetNNode(control(), mem, adr,
                                                      newval_enc, adr_type, ...));

// Step 2: SCMemProjNode to prevent LoadStore from being optimized away
Node* proj = _gvn.transform(new (C) SCMemProjNode(load_store));
set_memory(proj, alias_idx);

// Step 3: BUG — reassigns load_store to DecodeN
if (type == T_OBJECT && kind == LS_xchg) {
#ifdef _LP64
    if (adr->bottom_type()->is_ptr_to_narrowoop()) {
      load_store = _gvn.transform(new (C) DecodeNNode(load_store,
                                                       load_store->get_ptr_type()));
    }
#endif
    // ...
}

// Step 4: Pass the DecodeN (not GetAndSetN) as MemBarAcquire precedent
insert_mem_bar_trailing_load_store(Op_MemBarAcquire, load_store);  // ← BUG!
```

### The Graph Before and After

**Buggy graph**:
```
GetAndSetNNode (LoadStore ✓)
    ↓
DecodeNNode (NOT a LoadStore ✗)
    ↓
MemBarAcquire.Precedent → DecodeNNode  ← assertion fails!
```

**Correct graph**:
```
GetAndSetNNode (LoadStore ✓)
    ↓                ↓
DecodeNNode     MemBarAcquire.Precedent → GetAndSetNNode  ✓
(result value)  (ordering dependency)
```

### Why Only Compressed Oops

The bug only triggers when:
1. `_LP64` is defined (64-bit platform)
2. `adr->bottom_type()->is_ptr_to_narrowoop()` is true (compressed oops enabled)

Without compressed oops, the code takes the `GetAndSetPNode` path (regular pointer), and no `DecodeNNode` is created. The `load_store` variable remains pointing to the `GetAndSetPNode`, which IS a `LoadStore`.

### The Assertion

In `hotspot/src/cpu/aarch64/vm/aarch64.ad`:
```cpp
bool unnecessary_acquire(const Node *barrier)
{
  // ...
  if (mb->trailing_load_store()) {
    Node* load_store = mb->in(MemBarNode::Precedent);
    assert(load_store->is_LoadStore(), "unexpected graph shape");  // ← CRASH
    return is_CAS(load_store->Opcode());
  }
  return false;
}
```

`MemBarNode::Precedent` is `TypeFunc::Parms` (index 5). The function expects this edge to point to a `LoadStore` node, but it points to a `DecodeN` node instead.

---

## Resolution

### The Fix

In `hotspot/src/share/vm/opto/library_call.cpp`:

```cpp
  // SCMemProjNodes represent the memory state of a LoadStore. Their
  // main role is to prevent LoadStore nodes from being optimized away
  // when their results aren't used.
  Node* proj = _gvn.transform(new (C) SCMemProjNode(load_store));
  set_memory(proj, alias_idx);

+ // Save the original LoadStore node (GetAndSetP/GetAndSetN) before any DecodeN
+ // wrapping. This is needed as the MemBarAcquire precedent must be a LoadStore.
+ Node* load_store_for_barrier = load_store;

  if (type == T_OBJECT && kind == LS_xchg) {
#ifdef _LP64
    if (adr->bottom_type()->is_ptr_to_narrowoop()) {
      load_store = _gvn.transform(new (C) DecodeNNode(load_store, load_store->get_ptr_type()));
    }
#endif
    // ...
  }

  // Add the trailing membar surrounding the access
  insert_mem_bar(Op_MemBarCPUOrder);
- // Pass load_store as precedent to ensure membar follows the LoadStore node
- insert_mem_bar_trailing_load_store(Op_MemBarAcquire, load_store);
+ // Pass the original LoadStore node as precedent to ensure membar follows it.
+ // Do NOT pass the DecodeN wrapper as it is not a LoadStore node.
+ insert_mem_bar_trailing_load_store(Op_MemBarAcquire, load_store_for_barrier);
```

### Why This Fix Is Correct

1. **`load_store_for_barrier`** holds the original `GetAndSetNNode`/`GetAndSetPNode` — a `LoadStore` subclass
2. **`load_store`** is still reassigned to `DecodeNNode` for the method return value — this is correct, Java code needs a regular oop
3. The `MemBarAcquire` precedent edge is a **pure ordering constraint** — it doesn't consume the value, it only ensures the barrier is scheduled after the atomic operation
4. The `unnecessary_acquire()` predicate can now correctly check `is_LoadStore()` and `is_CAS()` on the precedent

---

## Debugging Process

### Step 1: Identify the crash location
The assertion `load_store->is_LoadStore()` fails in `unnecessary_acquire()` at `aarch64.ad:1242`.

### Step 2: Dump C2 Ideal Graph
Used `-XX:+PrintIdealGraphFile` to dump the IR graph to XML files at different compilation phases.

### Step 3: Analyze graph structure
Searched the XML for `MemBarAcquire` nodes and traced their precedent edges:
```bash
grep -n "MemBarAcquire" fetcher_ideal.xml
grep "to='<node_id>'" fetcher_ideal.xml  # find edges
```

### Step 4: Compare phases
- **Early phase** (`fetcher_ideal_bak.xml`): `CallStaticJava` for `getAndSet` — no intrinsic, no bug
- **Late phase** (`fetcher_ideal.xml`): `GetAndSetN` + `DecodeN` + `MemBarAcquire` — bug present

### Step 5: Trace the code path
Found the bug in `inline_unsafe_load_store()` in `library_call.cpp` where `load_store` is reassigned to `DecodeNNode` before being passed as the MemBarAcquire precedent.

---

## Technical Details

### Related Code Locations

1. **Bug location**:
   - `hotspot/src/share/vm/opto/library_call.cpp:3053-3080` — `inline_unsafe_load_store()`

2. **Assertion that catches the bug**:
   - `hotspot/src/cpu/aarch64/vm/aarch64.ad:1225-1247` — `unnecessary_acquire()`

3. **MemBarNode precedent definition**:
   - `hotspot/src/share/vm/opto/memnode.hpp:970` — `Precedent = TypeFunc::Parms`

4. **Affected Java methods**:
   - `java.util.concurrent.atomic.AtomicReference.getAndSet()`
   - `sun.misc.Unsafe.getAndSetObject()`

### Key Insight: Precedent Edge Semantics

The `MemBarNode::Precedent` edge (index 5) is a **pure ordering constraint**:
- It tells the scheduler: "do not schedule this MemBar before the precedent node completes"
- It does NOT consume the value produced by the precedent
- The precedent must be a `LoadStore` node so the predicate can determine the barrier's semantics

The `DecodeNNode` is just a type conversion (narrow oop → regular oop) with no memory ordering semantics. Using it as the precedent is semantically incorrect.

---

## Lessons Learned

1. **Don't reuse variables for different purposes**: The `load_store` variable was used for both the atomic operation result and the method return value. When the `DecodeN` wrapper was added, the original `LoadStore` reference was lost.

2. **Compressed oops create additional graph nodes**: The `EncodeP`/`DecodeN` nodes for narrow oop conversion can introduce subtle bugs if the original node references are not preserved.

3. **Ideal graph dumps are invaluable**: The `-XX:+PrintIdealGraphFile` flag and IGV (Ideal Graph Visualizer) are essential for debugging C2 compiler bugs.

4. **Compare early vs. late phases**: Dumping the graph at different compilation phases helps pinpoint exactly when a bug is introduced.

---

## References

- OpenJDK Bug: C2 compiler assertion failure in `unnecessary_acquire()` on AArch64
- Related: `2025-11-03_AARCH64_CAS_TrailingMembar_Assert_Bug.md` — similar assertion in `needs_acquiring_load_exclusive()`
- HotSpot C2 Compiler: `LibraryCallKit::inline_unsafe_load_store()` intrinsic implementation

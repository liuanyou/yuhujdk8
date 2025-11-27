# MemBarNode::_kind Field Not Set - Volatile CAS Uses Wrong Instruction

**Date**: November 3, 2025  
**Status**: ✅ RESOLVED  
**Severity**: MEDIUM  
**Platform**: All platforms (AArch64 most affected)  
**JDK Version**: OpenJDK 8  
**Compiler**: C2 (Server Compiler)

---

## Summary

The `MemBarNode::_kind` field, which describes the relationship between a memory barrier and nearby memory access operations, is never explicitly set during graph construction. This causes all membar nodes to default to `Standalone`, preventing the C2 compiler from correctly identifying volatile CAS operations and selecting the optimal instruction (`casal` instead of `casl` on AArch64).

**Root Cause**: The `MemBarNode` constructor does not initialize the `_kind` field, and there is no mechanism to set it when creating membar nodes for volatile operations or CAS operations.

**Resolution**: 
1. Modified the `MemBarNode` constructor to initialize `_kind` to `Standalone`
2. Added setter methods (`set_trailing_load()`, `set_leading_store()`, etc.) to set `_kind` after creation
3. Kept `insert_mem_bar()` unchanged for backward compatibility
4. Added specialized methods (`insert_mem_bar_trailing_load()`, `insert_mem_bar_leading_store()`, etc.) that internally call the setters
5. Updated all membar creation sites for volatile operations to use the specialized methods

---

## Symptoms

### Performance Impact

- **Volatile CAS operations** (e.g., `AtomicReference.compareAndSet()`) use `casl` instead of `casal` on AArch64
- This results in slightly less efficient code generation, though still semantically correct
- The difference is only visible in C2 compiler output (C1 always uses acquire/release semantics)

### Assembly Code Comparison

**Expected** (with correct `_kind`):
```assembly
dmb ish        ; Release barrier
casal w8, w11, [x10]  ; Acquiring CAS (acquire+release)
dmb ishld      ; Acquire barrier
```

**Actual** (with `_kind = Standalone`):
```assembly
dmb ish        ; Release barrier
casl w8, w11, [x10]   ; Normal CAS (release only)
dmb ishld      ; Acquire barrier
```

### When It Occurs

- During C2 compilation of methods containing volatile CAS operations
- Affects all platforms, but most noticeable on AArch64 where `casal` vs `casl` makes a difference
- Does not cause crashes or incorrect behavior, only suboptimal code generation

---

## Root Cause Analysis

### The Problem: `_kind` Field Never Initialized

#### Original Code

```cpp
// hotspot/src/share/vm/opto/memnode.cpp
MemBarNode::MemBarNode(Compile* C, int alias_idx, Node* precedent)
  : MultiNode(TypeFunc::Parms + (precedent == NULL? 0: 1)),
    _adr_type(C->get_adr_type(alias_idx))
    // _kind is NOT initialized here! (defaults to Standalone = 0)
{
  init_class_id(Class_MemBar);
  // ...
}
```

#### Why It Fails

1. **`_kind` Field Defaults to `Standalone`**:
   - The enum `_kind` field defaults to the first enum value (`Standalone = 0`)
   - This happens because the field is never explicitly initialized

2. **`trailing_membar()` Cannot Find Volatile CAS Membars**:
   ```cpp
   // hotspot/src/share/vm/opto/memnode.cpp
   MemBarNode* LoadStoreNode::trailing_membar() const {
     MemBarNode* trailing = NULL;
     for (DUIterator_Fast imax, i = fast_outs(imax); i < imax; i++) {
       Node* u = fast_out(i);
       if (u->is_MemBar()) {
         if (u->as_MemBar()->trailing_load_store()) {  // ← Checks _kind == TrailingLoadStore
           trailing = u->as_MemBar();
         }
       }
     }
     return trailing;  // Returns NULL if _kind != TrailingLoadStore
   }
   ```

3. **`needs_acquiring_load_exclusive()` Returns `false`**:
   ```cpp
   // hotspot/src/cpu/aarch64/vm/aarch64.ad
   bool needs_acquiring_load_exclusive(const Node *n) {
     LoadStoreNode* ldst = n->as_LoadStore();
     if (ldst->trailing_membar() == NULL) {  // ← Returns NULL for volatile CAS!
       return false;  // Use casl (no acquire)
     }
     return true;  // Use casal (acquire+release)
   }
   ```

4. **Result: Wrong Instruction Selected**:
   - `needs_acquiring_load_exclusive()` returns `false` for volatile CAS
   - Matcher selects `compareAndSwapP` (no predicate) instead of `compareAndSwapPAcq` (with predicate)
   - `compareAndSwapP` uses `casl` (no acquire semantics)

### Expected vs. Actual Behavior

**Expected** (based on design):
- `AtomicReference.compareAndSet()` should have `_kind = TrailingLoadStore` for `MemBarAcquire`
- `trailing_membar()` should return the `MemBarAcquire` node
- `needs_acquiring_load_exclusive()` should return `true`
- Should use `casal` (acquire+release)

**Actual** (what happens):
- All membar nodes have `_kind = Standalone` (default)
- `trailing_membar()` returns `NULL` for volatile CAS
- `needs_acquiring_load_exclusive()` returns `false` for volatile CAS
- Uses `casl` (release only, no acquire in instruction)

---

## Resolution

### The Fix

**Adopted Approach**: Keep `insert_mem_bar()` unchanged for backward compatibility, add specialized `insert_mem_bar_xxx()` methods that internally set `_kind` using setter methods.

**Advantages**:
- ✅ Backward compatible: All existing `insert_mem_bar()` calls remain unchanged
- ✅ Minimal changes: Only volatile operation sites use new methods
- ✅ Clear intent: Method names express the purpose
- ✅ No constructor changes: Don't need to modify all subclasses

#### 1. Modified Constructor to Initialize `_kind`

**File**: `hotspot/src/share/vm/opto/memnode.cpp`

```cpp
MemBarNode::MemBarNode(Compile* C, int alias_idx, Node* precedent)
  : MultiNode(TypeFunc::Parms + (precedent == NULL? 0: 1)),
    _adr_type(C->get_adr_type(alias_idx)),
    // _kind defaults to Standalone (first enum value)
    _kind(Standalone)
{
  init_class_id(Class_MemBar);
  // ...
}
```

#### 2. Added Setter Methods

**File**: `hotspot/src/share/vm/opto/memnode.hpp`

```cpp
class MemBarNode: public MultiNode {
  // ... existing code ...
  
public:
  // Setter methods for _kind field
  // Note: _kind is initialized to Standalone in constructor, so no set_standalone() needed
  void set_trailing_load() { _kind = TrailingLoad; }
  void set_trailing_store() { _kind = TrailingStore; }
  void set_leading_store() { _kind = LeadingStore; }
  void set_trailing_load_store() { _kind = TrailingLoadStore; }
  void set_leading_load_store() { _kind = LeadingLoadStore; }
  
  // ... existing code ...
};
```

#### 3. Added Specialized Methods

**File**: `hotspot/src/share/vm/opto/graphKit.hpp`

```cpp
// Helper functions to build synchronizations
int next_monitor();
Node* insert_mem_bar(int opcode, Node* precedent = NULL);
// Specialized methods for membar with specific _kind
Node* insert_mem_bar_trailing_load(int opcode, Node* precedent = NULL);
Node* insert_mem_bar_leading_store(int opcode, Node* precedent = NULL);
Node* insert_mem_bar_trailing_store(int opcode, Node* precedent = NULL);
Node* insert_mem_bar_leading_load_store(int opcode, Node* precedent = NULL);
Node* insert_mem_bar_trailing_load_store(int opcode, Node* precedent = NULL);
```

**File**: `hotspot/src/share/vm/opto/graphKit.cpp`

```cpp
// Keep original insert_mem_bar unchanged
Node* GraphKit::insert_mem_bar(int opcode, Node* precedent) {
  MemBarNode* mb = MemBarNode::make(C, opcode, Compile::AliasIdxBot, precedent);
  mb->init_req(TypeFunc::Control, control());
  mb->init_req(TypeFunc::Memory,  reset_memory());
  Node* membar = _gvn.transform(mb);
  set_control(_gvn.transform(new (C) ProjNode(membar, TypeFunc::Control)));
  set_all_memory_call(membar);
  return membar;
}

// Specialized method for trailing load
Node* GraphKit::insert_mem_bar_trailing_load(int opcode, Node* precedent) {
  MemBarNode* mb = MemBarNode::make(C, opcode, Compile::AliasIdxBot, precedent);
  mb->set_trailing_load();  // Set _kind
  mb->init_req(TypeFunc::Control, control());
  mb->init_req(TypeFunc::Memory,  reset_memory());
  Node* membar = _gvn.transform(mb);
  set_control(_gvn.transform(new (C) ProjNode(membar, TypeFunc::Control)));
  set_all_memory_call(membar);
  return membar;
}

// Similar implementations for other insert_mem_bar_xxx() methods...
```

#### 4. Updated MemBar Creation Sites

**Volatile Field Load** (`Parse::do_get_xxx`):
```cpp
if (field->is_volatile()) {
  // Memory barrier includes bogus read of value to force load BEFORE membar
  // Set _kind = TrailingLoad for volatile field load
  insert_mem_bar_trailing_load(Op_MemBarAcquire, ld);
}
```

**Volatile Field Store** (`Parse::do_put_xxx`):
```cpp
if (is_vol) {
  // Set _kind = LeadingStore for volatile field store (before store)
  insert_mem_bar_leading_store(Op_MemBarRelease, NULL);
}
// ... store code (store node created) ...
if (is_vol) {
  // Set _kind = TrailingStore for volatile field store (after store)
  // Pass store node as precedent to ensure membar follows the store
  insert_mem_bar_trailing_store(Op_MemBarVolatile, store);
}
```

**CAS Operation** (`LibraryCallKit::inline_unsafe_load_store`):
```cpp
// CAS 前：使用专门的方法设置 _kind = LeadingLoadStore
insert_mem_bar_leading_load_store(Op_MemBarRelease, NULL);
insert_mem_bar(Op_MemBarCPUOrder);  // 普通 membar，不需要设置 _kind

// ... CAS operation (load_store node created) ...

// CAS 后：使用专门的方法设置 _kind = TrailingLoadStore
// Pass load_store as precedent to ensure membar follows the LoadStore node
insert_mem_bar(Op_MemBarCPUOrder);  // 普通 membar，不需要设置 _kind
insert_mem_bar_trailing_load_store(Op_MemBarAcquire, load_store);
```

**Unsafe Volatile Access** (`LibraryCallKit::inline_unsafe_access`):
```cpp
if (is_volatile) {
  if (is_store)
    insert_mem_bar_leading_store(Op_MemBarRelease);  // Before store
  // ... load or store operation ...
  if (!is_store)
    // Pass load node as precedent to ensure membar follows the load
    insert_mem_bar_trailing_load(Op_MemBarAcquire, p);  // After load
  else
    // Pass store node as precedent to ensure membar follows the store
    insert_mem_bar_trailing_store(Op_MemBarVolatile, store);  // After store
}
```

**load_field_from_object** (`LibraryCallKit::load_field_from_object`):
```cpp
// Build the load.
Node* loadedField = make_load(NULL, adr, type, bt, adr_type, is_vol);

// If reference is volatile, prevent following memory ops from
// floating up past the volatile read.  Also prevents commoning
// another volatile read.
// This matches the behavior in Parse::do_get_xxx()
if (is_vol) {
  // Memory barrier includes bogus read of value to force load BEFORE membar
  // Set _kind = TrailingLoad for volatile field load
  // Pass load node as precedent to ensure membar follows the load
  insert_mem_bar_trailing_load(Op_MemBarAcquire, loadedField);
}

return loadedField;
```

**Note**: `putOrdered` operations use `insert_mem_bar()` (Standalone) because they are not volatile accesses.

### Precedent Edge Fix

During implementation, we discovered that trailing membar nodes need to have the `Precedent` edge set to the operation node (load/store/load_store) to prevent array bounds errors and enable proper optimization.

**Issue**: When `precedent == NULL`, `MemBarNode` constructor doesn't allocate the `Precedent` input slot (`_max = 5`, valid indices 0-4). However, some code (e.g., `aarch64.ad`'s `unnecessary_acquire`) accesses `mb->in(MemBarNode::Precedent)` (index 5), causing an out-of-bounds error.

**Solution**: For trailing membar nodes (after operations), pass the operation node as `precedent`:
- `insert_mem_bar_trailing_load_store`: Pass `load_store` node
- `insert_mem_bar_trailing_load`: Pass load node (`p` or `ld`)
- `insert_mem_bar_trailing_store`: Pass store node

For leading membar nodes (before operations), `precedent` remains `NULL` because the operation node doesn't exist yet.

**Principle**:
- **Trailing membar** (after operation): Must pass operation node as `precedent`
- **Leading membar** (before operation): Keep `precedent = NULL`

### _pair_idx Field Fix

**Problem**: In ASSERT mode, `MemBarNode` has a `_pair_idx` field used to verify paired membar nodes. The `leading_membar()` and `trailing_membar()` methods assert that `_pair_idx` values match:

```cpp
// hotspot/src/share/vm/opto/memnode.cpp:3140
assert(mb->_pair_idx == _pair_idx, "bad leading membar");
```

If `_pair_idx` is not set correctly, this assert fails.

**Solution**:
1. Initialize `_pair_idx` to 0 in the `MemBarNode` constructor
2. Add `set_pair_idx()` and `pair_idx()` methods to `MemBarNode`
3. In `insert_mem_bar_leading_store` and `insert_mem_bar_leading_load_store`, set `_pair_idx = mb->_idx` after transform
4. In `insert_mem_bar_trailing_store` and `insert_mem_bar_trailing_load_store`, search through control flow to find the corresponding leading membar and set the same `_pair_idx`

**Files Modified**:
- `hotspot/src/share/vm/opto/memnode.cpp`: Initialize `_pair_idx` in constructor
- `hotspot/src/share/vm/opto/memnode.hpp`: Add setter/getter methods
- `hotspot/src/share/vm/opto/graphKit.cpp`: Set `_pair_idx` in leading and trailing membar creation methods

**Key Implementation Details**:
1. **Leading membar**: After `transform()`, set `_pair_idx = mb->_idx` to use the node's index as the pair identifier
2. **Trailing membar**: Before `transform()`, search through control flow (handling Region nodes) to find the corresponding leading membar, then set the same `_pair_idx`
3. **Control flow search**: For Region nodes, take the first path (`in(1)`) to find the leading membar

**Why This Is Needed**:
- ASSERT mode verification: `leading_membar()` and `trailing_membar()` verify paired membar correctness
- Debugging support: `_pair_idx` helps developers verify membar pairing relationships
- Consistency: Ensures paired membars have explicit association identifiers

**Note**: `_pair_idx` is only used in `#ifdef ASSERT` mode and does not affect production code performance.

### Store Node Release Memory Order Fix

During implementation, we discovered that `aarch64.ad`'s `unnecessary_volatile` and `unnecessary_release` functions expect the `Precedent` Store node to be marked as `is_release()` for volatile stores.

**Issue**: `store_to_memory()` always creates Store nodes with `MemOrd::unordered`, even for volatile accesses. This causes `aarch64.ad`'s assert to fail:
```
assert(!release || (mbvol->in(MemBarNode::Precedent)->is_Store() && 
       mbvol->in(MemBarNode::Precedent)->as_Store()->is_release())) failed
```

**Solution**: For volatile stores, create Store nodes with `MemOrd::release`. Additionally, we need to:
1. Handle compressed pointers correctly (`StoreNNode` and `StoreNKlassNode`)
2. Get the Store node from memory state after `final_sync(ideal)` in IdealKit branches

**Implementation**:
- Added `store_to_memory_release()` method for non-T_OBJECT types
- Added `store_oop_release()` method for T_OBJECT types with GC barriers
- Added `store_oop_to_unknown_release()` method for T_OBJECT types to unknown locations
- Modified `inline_unsafe_access()` to use these methods for volatile stores

**Files Modified**:
- `hotspot/src/share/vm/opto/graphKit.hpp`: Added method declarations (`store_to_memory_release`, `store_oop_release`, `store_oop_to_unknown_release`, `store_oop_to_object_release`)
- `hotspot/src/share/vm/opto/graphKit.cpp`: Implemented release Store creation methods with compressed pointer support
- `hotspot/src/share/vm/opto/library_call.cpp`: Use release methods for volatile stores, handle IdealKit branch Store node retrieval
- `hotspot/src/share/vm/opto/parse3.cpp`: Use release methods for volatile field stores
- `hotspot/src/share/vm/opto/idealKit.hpp`: Added `store_release` method declaration
- `hotspot/src/share/vm/opto/idealKit.cpp`: Implemented `store_release` method with compressed pointer support

**Key Implementation Details**:
1. **Compressed Pointer Handling**: Both `store_to_memory_release` and `IdealKit::store_release` handle compressed pointers (`StoreNNode` for compressed OOPs, `StoreNKlassNode` for compressed class pointers)
2. **IdealKit Store Node Retrieval**: After `final_sync(ideal)`, the Store node is retrieved from memory state to use as `precedent` in `insert_mem_bar_trailing_store`
3. **Memory Order**: All volatile stores use `MemOrd::release` to satisfy `aarch64.ad`'s optimization predicates

**Principle**:
- **Volatile store**: Must create `MemOrd::release` Store nodes to satisfy `aarch64.ad`'s assert and enable optimizations (e.g., using `stlr` instruction)
- **Non-volatile store**: Continue using `MemOrd::unordered` Store nodes

### What Changed

1. **Modified constructor**: Initialize `_kind` to `Standalone` explicitly
2. **Added setter methods**: `set_trailing_load()`, `set_leading_store()`, etc.
3. **Added specialized methods**: `insert_mem_bar_trailing_load()`, `insert_mem_bar_leading_store()`, etc.
4. **Kept `insert_mem_bar()` unchanged**: All existing calls remain valid (use `Standalone`)
5. **Updated volatile operation sites**: Use specialized methods that set `_kind` correctly
6. **Fixed Precedent edges**: Pass operation nodes as `precedent` for trailing membar nodes
7. **Fixed load_field_from_object**: Added membar handling for volatile fields to match `Parse::do_get_xxx` behavior
8. **Fixed _pair_idx field**: Initialize and set `_pair_idx` for paired membar nodes (LeadingStore/TrailingStore and LeadingLoadStore/TrailingLoadStore) to satisfy ASSERT mode verification
9. **Fixed Store node memory order**: Create `MemOrd::release` Store nodes for volatile stores to satisfy `aarch64.ad`'s assert requirements

### Result

- **Volatile Field Load**: `MemBarAcquire` has `_kind = TrailingLoad` ✅
- **Volatile Field Store**: `MemBarRelease` has `_kind = LeadingStore`, `MemBarVolatile` has `_kind = TrailingStore` ✅
- **CAS Operation**: `MemBarRelease` has `_kind = LeadingLoadStore`, `MemBarAcquire` has `_kind = TrailingLoadStore` ✅
- `trailing_membar()` now correctly finds `TrailingLoadStore` membar for volatile CAS ✅
- `needs_acquiring_load_exclusive()` returns `true` for volatile CAS ✅
- Volatile CAS now uses `casal` instead of `casl` ✅

---

## Technical Details

### Related Code Locations

1. **MemBarNode Definition**:
   - `hotspot/src/share/vm/opto/memnode.hpp:946-997`
   - `hotspot/src/share/vm/opto/memnode.cpp:3013-3024`

2. **Membar Creation Sites**:
   - Volatile load: `hotspot/src/share/vm/opto/parse3.cpp:268-272`, `library_call.cpp:2722`
   - Volatile store: `hotspot/src/share/vm/opto/parse3.cpp:280-314`, `library_call.cpp:2632, 2725`
   - CAS operation: `hotspot/src/share/vm/opto/library_call.cpp:2914-3036`
   - Unsafe volatile access: `hotspot/src/share/vm/opto/library_call.cpp:2630-2725`
   - load_field_from_object: `hotspot/src/share/vm/opto/library_call.cpp:5872-5903`
   - Specialized methods: `hotspot/src/share/vm/opto/graphKit.cpp:3084-3147`
   - Release Store creation: `hotspot/src/share/vm/opto/graphKit.cpp:1529-1572, 1688-1720, 1748-1775`

3. **Instruction Selection**:
   - Predicate: `hotspot/src/cpu/aarch64/vm/aarch64.ad:1341-1358` (`needs_acquiring_load_exclusive`)
   - Instruction definitions: `hotspot/src/cpu/aarch64/vm/aarch64.ad:8191-8287`

4. **Trailing Membar Lookup**:
   - `hotspot/src/share/vm/opto/memnode.cpp:2757-2779` (`LoadStoreNode::trailing_membar()`)

### Why This Is a Design Defect

The `_kind` field was designed to:
- Identify the relationship between membar and memory access
- Enable `trailing_membar()` to find the correct membar for CAS operations
- Allow instruction selection predicates to differentiate volatile vs non-volatile operations

However, the field was **never actually set** during graph construction, making it effectively useless. This is a design defect where:
- The infrastructure exists (enum, field, methods)
- The infrastructure is used (checked in predicates)
- But the infrastructure is never populated (field never set)

---

## Verification

### Test Case

```java
import java.util.concurrent.atomic.AtomicReference;

public class TestVolatileCAS {
    public static void main(String[] args) {
        AtomicReference<Object> ref = new AtomicReference<>();
        Object old = new Object();
        Object newObj = new Object();
        
        // This should use casal (acquiring CAS) on AArch64
        ref.compareAndSet(old, newObj);
    }
}
```

### Expected Behavior After Fix

1. **Compile with C2**:
   ```bash
   javac TestVolatileCAS.java
   java -XX:+PrintAssembly -XX:CompileCommand=print,TestVolatileCAS::main TestVolatileCAS
   ```

2. **Assembly Output** (AArch64):
   ```assembly
   casal w8, w11, [x10]  ; Should see casal (not casl)
   ```

3. **Verification**:
   - `MemBarAcquire` after CAS has `_kind = TrailingLoadStore`
   - `trailing_membar()` returns non-NULL for CAS operations
   - `needs_acquiring_load_exclusive()` returns `true` for volatile CAS

---

## Impact Assessment

### Severity: MEDIUM

**Why not HIGH**:
- Does not cause crashes or incorrect behavior
- Code is still semantically correct (memory barriers provide ordering)
- Performance impact is minor (slightly less efficient instruction)

**Why not LOW**:
- Affects all volatile CAS operations
- Prevents optimal code generation
- Could be more significant on certain architectures or workloads

### Affected Operations

- ✅ Volatile CAS operations (`AtomicReference.compareAndSet()`, etc.)
- ✅ Volatile field stores (though less critical)
- ✅ Volatile field loads (though less critical)

### Platforms Affected

- **AArch64**: Most affected (can use `casal` vs `casl`)
- **x86_64**: Less affected (different instruction set)
- **Other platforms**: Varies by architecture

---

## Related Issues

This bug is related to:

1. **[AArch64 CAS trailing_membar Assertion Failure](./2025-11-03_AARCH64_CAS_TrailingMembar_Assert_Bug.md)**:
   - The `trailing_membar` fix handles non-volatile CAS (which never have membar)
   - This fix handles volatile CAS (which should have `TrailingLoadStore` membar)
   - Both fixes are necessary and independent

2. **[Why Both CAS Methods Use `casl` Instead of `casal` in C2](./WHY_BOTH_CASL_NOT_CASAL_IN_C2.md)**:
   - This document explains the root cause (`_kind` never set)
   - The current bug document describes the fix

3. **[MemBarNode::_kind 枚举值详解](./MEMBAR_KIND_ENUM_EXPLANATION.md)**:
   - Detailed explanation of `_kind` enum values
   - How they relate to memory access operations

4. **[MemBarNode Opcode 详解](./MEMBAR_OPCODE_EXPLANATION.md)**:
   - Explanation of different membar opcodes
   - How they relate to `_kind` values

---

## Lessons Learned

1. **Initialize all fields**: Even if a field has a default value, explicitly initialize it if it's part of the design
2. **Design vs. Implementation gap**: The design included `_kind` for a purpose, but the implementation never populated it
3. **Test infrastructure**: The infrastructure (enum, field, methods) existed and was used, but never tested with actual values
4. **Code review**: This could have been caught if code reviews checked that all designed fields are actually set
5. **Backward compatibility**: When fixing bugs, prefer adding new methods over changing existing ones to maintain compatibility
6. **Clear intent**: Method names that express intent (e.g., `insert_mem_bar_trailing_load()`) are clearer than generic methods with parameters

---

## References

- AArch64 Architecture Reference Manual: Load-Exclusive and Store-Exclusive instructions
- OpenJDK HotSpot C2 Compiler: Architecture Description Language (ADLC)
- Java Memory Model: Volatile semantics vs. atomic operations
- C2 Compiler: Ideal Graph representation and optimization phases

---

## Related Documentation

- **[How to Fix MemBarNode::_kind](./HOW_TO_FIX_MEMBAR_KIND.md)**: Detailed fix implementation guide with all code changes
- **[MemBarNode::_kind Fix Implemented](./MEMBAR_KIND_FIX_IMPLEMENTED.md)**: Implementation summary (may be outdated, see HOW_TO_FIX_MEMBAR_KIND.md for current implementation)
- **[Should We Revert trailing_membar Fix?](./SHOULD_WE_REVERT_TRAILING_MEMBAR_FIX.md)**: Relationship between two fixes
- **[Volatile vs Non-Volatile CAS Assembly Examples](./VOLATILE_VS_NONVOLATILE_CAS_ASSEMBLY_EXAMPLES.md)**: Assembly code comparison
- **[MemBarNode::_kind 枚举值详解](./MEMBAR_KIND_ENUM_EXPLANATION.md)**: Detailed explanation of `_kind` enum values
- **[MemBarNode Opcode 详解](./MEMBAR_OPCODE_EXPLANATION.md)**: Explanation of different membar opcodes



# 067: describe_scope Missing Between add_safepoint and end_safepoint

## Problem Summary

The Yuhu compiler crashes during GC with an assertion failure when reading scope descriptors from nmethod debug info:

```
assert(index > 0 && index <= metadata_size(), "must be a valid non-zero index")
```

The crash occurs with `index=50918348` (garbage value) while `metadata_size()=8` (valid range [1, 1]).

## Root Cause

In `yuhuDebugInformationRecorder.cpp`, the `convert_and_add_to_real_recorder()` method registers safepoints but **does not call `describe_scope()`** between `add_safepoint()` and `end_safepoint()`:

```cpp
// Line 287-288 in yuhuDebugInformationRecorder.cpp
real_recorder->add_safepoint(pc_offset, oopmap);
real_recorder->end_safepoint(pc_offset);
// MISSING: describe_scope() call!
```

## Technical Analysis

### nmethod Debug Info Layout

An nmethod contains several debug info sections:

```
[oops] → [metadata] → [scopes_data] → [scopes_pcs] → [dependencies] → ...
```

- **`_scopes_data_offset`**: Stream containing scope descriptors (method, bci, locals, expressions)
- **`_scopes_pcs_offset`**: Array of PcDesc entries, each containing `scope_decode_offset`
- **`scope_decode_offset`**: Byte offset into `scopes_data` where this PC's scope info begins

### The Missing Link

**`add_safepoint()`** creates a PcDesc entry with `scope_decode_offset` initialized to `serialized_null`.

**`describe_scope()`** populates the `scopes_data` stream with:
- Sender stream offset
- Method metadata index (via `oop_recorder()->find_index(method_enc)`)
- Bytecode index (bci)
- Locals, expressions, monitors
- Updates `last_pd->set_scope_decode_offset(stream_offset)`

**Without `describe_scope()`:**
1. PcDesc entries are created (via `add_safepoint`)
2. `scopes_data` stream remains **empty** (size = 0)
3. `scope_decode_offset` fields remain invalid
4. When GC reads scope info, it gets garbage → crash

### Correct Sequence

HotSpot compilers (C1/C2) use this pattern:

```cpp
// For safepoints (GC points with OopMap):
recorder->add_safepoint(pc_offset, oopmap);
recorder->describe_scope(pc_offset, method, bci, reexecute, 
                         is_method_handle_invoke, return_oop,
                         locals, expressions, monitors);
recorder->end_safepoint(pc_offset);

// For non-safepoints (deopt points without OopMap):
recorder->add_non_safepoint(pc_offset);
recorder->describe_scope(pc_offset, method, bci, reexecute,
                         is_method_handle_invoke, return_oop,
                         locals, expressions, monitors);
recorder->end_non_safepoint(pc_offset);
```

### describe_scope() Parameters

For Yuhu's use case:

1. **`pc_offset`**: PC offset in code (same as passed to `add_safepoint`)
2. **`method`**: The Java method being compiled (the caller)
3. **`bci`**: Bytecode index at this PC
   - For safepoint polls: the poll location BCI
   - For call sites: the invoke* bytecode BCI (available via `bci()`)
4. **`reexecute`**: Whether to re-execute bytecode after deopt (usually `false` for call return points)
5. **`is_method_handle_invoke`**: Whether this is a MethodHandle invoke
6. **`return_oop`**: Whether the return value is an oop (check `method->return_type()->is_oop()`)
7. **`locals`**: DebugToken for local variables (can be NULL/empty for now)
8. **`expressions`**: DebugToken for expression stack (can be NULL/empty for now)
9. **`monitors`**: DebugToken for synchronized monitors (can be NULL/empty for now)

### Why Yuhu Doesn't Call describe_scope()

Yuhu's architecture differs from C1/C2:
- LLVM performs register allocation (Yuhu doesn't control it)
- LLVM optimizes, reorders, eliminates variables
- After code generation, Yuhu doesn't know where Java locals are stored
- Locals/expressions/monitors tracking would require:
  - LLVM IR metadata marking each Java local
  - Custom LLVM pass to preserve through optimization
  - Post-codegen extraction of variable locations
  - Mapping from LLVM registers/stack slots to Java locals

## Impact

**GC will work** if `describe_scope()` is called with minimal info:
- `method` and `bci` are sufficient for GC to identify the stack frame
- Locals/expressions/monitors can be NULL/empty

**Deoptimization won't work** without proper locals/expressions tracking, but this can be added later.

## Location

- **File**: `hotspot/src/share/vm/yuhu/yuhuDebugInformationRecorder.cpp`
- **Method**: `YuhuDebugInformationRecorder::convert_and_add_to_real_recorder()`
- **Lines**: 287-288

## Fix Strategy

Add `describe_scope()` call between `add_safepoint()` and `end_safepoint()` for each call site:

```cpp
real_recorder->add_safepoint(pc_offset, oopmap);

// Describe the scope at this safepoint
real_recorder->describe_scope(pc_offset,
                              target_method,  // Method being compiled
                              bci,            // Bytecode index
                              false,          // reexecute
                              false,          // is_method_handle_invoke
                              false,          // return_oop
                              NULL,           // locals
                              NULL,           // expressions
                              NULL);          // monitors

real_recorder->end_safepoint(pc_offset);
```

## Related Documents

- **035**: Deoptimization scope_desc_missing
- **037**: Yuhu deoptimization root cause
- **063**: OopMap redesign exact offset registration

## Status

**Documented**: 2025
**Fixed**: Pending

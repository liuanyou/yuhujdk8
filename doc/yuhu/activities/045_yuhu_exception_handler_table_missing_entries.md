# Yuhu Compiler Exception Handler Table Missing Entries Issue

## Problem Description

The Yuhu compiler generates exception and deoptimization handlers correctly, but the JVM reports "missing exception handler" error during execution. The error occurs in `SharedRuntime::compute_compiled_exc_handler` at line 717 in `sharedRuntime.cpp`.

## Root Cause Analysis

1. **Exception Handler Generation**: Yuhu correctly generates exception and deopt handlers using `start_a_stub` and `end_a_stub` mechanisms, placing them in the stubs section of the CodeBuffer.

2. **ExceptionHandlerTable Issue**: The problem lies in how the `ExceptionHandlerTable` is handled during method registration:
   - Yuhu creates an empty `ExceptionHandlerTable handler_table;`
   - No call to populate the table with exception handler information
   - The table remains empty during `env->register_method()` call

3. **Lookup Failure**: When the JVM needs to find an exception handler:
   - `ExceptionHandlerTable::subtable_for(int catch_pco)` is called
   - The function iterates through an empty table
   - Returns NULL since no matching entries exist
   - `SharedRuntime::compute_compiled_exc_handler` detects NULL return and triggers "missing exception handler" error

4. **Comparison with C1**: C1 compiler calls `generate_exception_handler_table()` before registration, which populates the table with exception handling metadata collected during compilation.

## Technical Details

The current Yuhu implementation:
```cpp
ExceptionHandlerTable handler_table;  // Empty table
ImplicitExceptionTable inc_table;

env->register_method(target,
                     entry_bci,
                     &offsets,           // Contains handler offsets
                     0,                  // orig_pc_offset
                     &combined_cb,
                     frame_size,
                     &oopmaps,
                     &handler_table,     // Empty!
                     &inc_table,
                     this,
                     env->comp_level(),
                     false,
                     false);
```

## Impact

- Exception handling fails in Yuhu-compiled methods
- JVM crashes with internal error when exceptions occur
- Exception handlers are present in memory but unreachable via standard JVM mechanism

## Solution Approach

The Yuhu compiler needs to either:
1. Properly populate the ExceptionHandlerTable with exception handler metadata, or
2. Implement an alternative exception handling registration mechanism that accounts for the generated handlers

The generated handlers themselves are correct (as evidenced by the disassembly showing proper handler code), but the metadata linking them to the JVM's exception handling system is missing.
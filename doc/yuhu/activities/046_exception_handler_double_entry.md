# Issue 046: Exception Handler Double Entry - Root Cause and Solution

## Problem Summary

Yuhu compiler's exception handler was being entered twice, causing the assertion `assert(false) failed: DEBUG MESSAGE: exception oop already set` in `Runtime1::handle_exception_from_callee_id` stub.

**Root Cause**: The Yuhu compiler was incorrectly generating an exception handler for ALL methods, including those without try-catch blocks. When a method without try-catch threw an exception, the JVM would:
1. Call the exception handler (first entry)
2. Fail to find a matching catch block
3. Jump back to the same exception handler (second entry) → assertion failure

## Solution: Separate Unwind Handler from Exception Handler

### Key Insight

**Exception handlers should only be generated for methods with try-catch blocks.** Methods without try-catch should use an **unwind handler** to propagate exceptions to their callers.

This matches the JVM's exception propagation model:
- **Exception handler**: Catches and handles exceptions within the current method
- **Unwind handler**: Propagates exceptions upward when no handler exists in the current method

### Implementation Strategy

#### For methods WITHOUT try-catch:
```cpp
// Only generate unwind handler
generate_unwind_handler(combined_cb, frame_size * wordSize);
offsets.set_value(CodeOffsets::UnwindHandler, adapter_size + llvm_code_size);
// NO exception handler generated
```

#### For methods WITH try-catch:
```cpp
// Generate both handlers
generate_exception_handler(combined_cb, exc_handler_size);
generate_unwind_handler(combined_cb, frame_size * wordSize);
offsets.set_value(CodeOffsets::Exceptions, 0);
offsets.set_value(CodeOffsets::UnwindHandler, adapter_size + llvm_code_size + exc_handler_size);
```

### Why This Fixes the Double Entry

Before the fix:
1. Method without try-catch throws exception
2. JVM calls exception handler (expects to find catch block)
3. No catch block found, JVM tries to propagate upward
4. Jumps back to exception handler entry → double entry

After the fix:
1. Method without try-catch throws exception  
2. JVM calls unwind handler directly (no expectation of catch block)
3. Unwind handler propagates to caller immediately
4. No double entry because we never expected to handle locally

## Unwind Handler Design

### Section Placement

**Unwind handler is placed in the insts (code) section**, NOT the stubs section. This is different from the exception handler.

**Evidence from nmethod constructor** (`nmethod.cpp`):
```cpp
// Exception handler and deopt handler are in the stub section
if (offsets->value(CodeOffsets::Exceptions) != -1) {
  _exception_offset = _stub_offset + offsets->value(CodeOffsets::Exceptions);
}
// Unwind handler is in the code section
if (offsets->value(CodeOffsets::UnwindHandler) != -1) {
  _unwind_handler_offset = code_offset() + offsets->value(CodeOffsets::UnwindHandler);
}
```

### Frame Restoration

The unwind handler must restore the caller's frame before jumping to `Runtime1::unwind_exception_id`. 

**Key insight**: After executing `ldp x29, x30, [sp], #16`, the `x30 (lr)` register will contain the **caller's return address** (saved by the `bl` instruction that called the current method).

**Correct sequence**:
1. Clear JavaThread exception fields (set by exception dispatch)
2. Execute `sub sp, x29, #frame_size_in_bytes` to restore SP to the position where x29/x30 were saved
3. Execute `ldp x29, x30, [sp], #16` to restore caller's frame pointer and return address
4. Jump to `Runtime1::unwind_exception_id` — at this point `lr` contains the caller's return address

**Why this works**:
- Method prologue: `stp x29, x30, [sp, #-16]!` saves the return address (in x30) to stack
- Exception occurs, JVM dispatches to unwind handler
- Unwind handler restores the frame: after `ldp`, `x30` = original return address from caller
- `Runtime1::unwind_exception_id` uses `lr` to call `SharedRuntime::exception_handler_for_return_address(rthread, lr)`
- This finds the caller's exception handler based on the return address

### Implementation

**Measure phase** (knowing frame_size_in_bytes):
```cpp
int YuhuCompiler::measure_unwind_handler_size(int frame_size_in_bytes) {
  // Use write_insts_remove_frame to generate the correct sequence
  masm.write_insts_remove_frame(frame_size_in_bytes);
  // Returns total size including TLS clear + frame restore + jump
}
```

**Generate phase** (placed in insts section after LLVM code):
```cpp
// Set insts end to cover adapter + LLVM code
combined_cb.insts()->set_end(combined_base + adapter_size + llvm_code_size);

// Generate unwind handler directly into insts section (NOT using start_a_stub)
generate_unwind_handler(combined_cb, frame_size * wordSize);

// Record offset relative to insts_begin
offsets.set_value(CodeOffsets::UnwindHandler, adapter_size + llvm_code_size);
```

### Critical Differences from Exception Handler

| Aspect | Exception Handler | Unwind Handler |
|--------|------------------|----------------|
| **Section** | Stubs section | Insts (code) section |
| **Purpose** | Handle exceptions in current method (has try-catch) | Propagate exception to caller (no try-catch) |
| **Offset calculation** | `_stub_offset + offset` | `code_offset() + offset` |
| **Frame handling** | May catch at any point | Must fully restore caller's frame |
| **Entry** | Via `exception_handler_for_pc` lookup | Direct jump from exception dispatch |
| **Generated when** | Method has try-catch blocks | Method has NO try-catch blocks |

## Known Limitations and Future Work

### 1. Synchronized Methods Not Handled

The current unwind handler implementation does not release monitors for synchronized methods. This will cause issues when a synchronized method throws an exception.

**Required fix**: Before restoring the frame, the unwind handler must:
1. Check if the method is synchronized
2. If yes, call `unlock_object` to release the monitor
3. Then proceed with frame restoration

This requires implementing a `MonitorExitStub` similar to C1's approach in `c1_LIRAssembler_aarch64.cpp:emit_unwind_handler()`.

### 2. Exception Handler for Try-Catch Not Implemented

While the unwind handler is now correctly implemented, the exception handler generation for methods with try-catch blocks is still pending.

**Required work**:
- Analyze LLVM IR to detect landing pads (catch blocks)
- Generate appropriate exception handler stubs for each landing pad
- Register exception table entries mapping PC ranges to handlers
- Handle exception type matching and filter clauses

### 3. Conditional Handler Registration

The current implementation should check whether a method actually needs an exception handler before generating one. This can be done by:
- Checking `exc_handler_size > 0` (indicates try-catch present)
- Analyzing LLVM IR for `invoke` instructions with exception destinations
- Using LLVM's exception handling intrinsics to detect landing pads

## Debug Evidence

From hs_err_pid49248.log:
```
V  [libjvm.dylib+0xa9d6ec]  VMError::report_and_die()+0x498
V  [libjvm.dylib+0x3d0d80]  report_vm_error(char const*, int, char const*, char const*)+0x78
V  [libjvm.dylib+0x7c8dbc]  MacroAssembler::debug64(char*, long long, long long*)+0x628
v  ~RuntimeStub::handle_exception_from_callee Runtime1 stub
```

Assertion location:
```
Internal Error (/Users/liuanyou/CLionProjects/jdk8/hotspot/src/cpu/aarch64/vm/macroAssembler_aarch64.cpp:2366)
assert(false) failed: DEBUG MESSAGE: exception oop already set
```

## Test Case

Running `test_yuhu/test_yuhu_simple.sh` triggers this issue consistently for methods without try-catch.

## Related Files

- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - Yuhu exception/unwind handler generation
- `hotspot/src/cpu/aarch64/vm/c1_Runtime1_aarch64.cpp` - Runtime1 stub implementation
- `hotspot/src/share/vm/c1/c1_Runtime1.hpp` - Runtime1 interface
- `hotspot/src/share/vm/c1/c1_LIRAssembler_aarch64.cpp` - C1 exception/unwind handler reference
- `hotspot/src/share/vm/code/nmethod.cpp` - nmethod constructor showing section offsets
- `hotspot/src/share/vm/c1/c1_ExceptionHandler.hpp` - Exception table management

## Next Steps

1. Implement monitor exit for synchronized methods in unwind handler
2. Add LLVM IR analysis to detect try-catch blocks
3. Implement exception handler generation for methods with landing pads
4. Test with complex exception scenarios (nested try-catch, finally blocks)
5. Verify exception propagation across mixed-mode boundaries (interpreted ↔ compiled)

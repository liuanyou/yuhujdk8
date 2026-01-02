# deoptimized_entry_point Returns NULL Issue

## Problem Summary

LLVM IR verification fails with "null operand" errors during compilation of `encodeArrayLoop`:

```
/opt/homebrew/Cellar/llvm/20.1.5/bin/opt: error: expected number in address space
  %56 = call <cannot get addrspace!> i32 <null operand!>(i32 %55, ptr %thread_ptr)
              ^
Call parameter type does not match function signature!
Operand is null
```

This error occurs **12 times** in the IR for `encodeArrayLoop`, suggesting it affects multiple method calls.

## Root Cause Analysis

### 1. Call Flow

When a Java method is invoked in Yuhu-compiled code:

```cpp
// YuhuTopLevelBlock::do_call() - Line 1485
Value *deoptimized_frames = builder()->CreateCall(
    deopt_func_type, entry_point, deopt_args);

// If the callee got deoptimized, reexecute in interpreter
builder()->CreateCondBr(
    builder()->CreateICmpNE(deoptimized_frames, LLVMValue::jint_constant(0)),
    reexecute, call_completed);

builder()->SetInsertPoint(reexecute);
Value* deopt_callee = builder()->deoptimized_entry_point();  // ← Returns NULL!
builder()->CreateCall(func_type, deopt_callee, args);  // ← NULL pointer used here!
```

### 2. The Problem: NULL Return Value

```cpp
// yuhuBuilder.cpp:394-409
Value* YuhuBuilder::deoptimized_entry_point() {
  // For AArch64, we don't use CppInterpreter, so we need a different approach
  // TemplateInterpreter uses a different deoptimization mechanism
  // The signature is "iT" -> "i": int main_loop(int recurse, Thread* thread)
  // This is used when a callee gets deoptimized and we need to reexecute in the interpreter
  // 
  // For AArch64 with TemplateInterpreter, deoptimization is handled differently:
  // - Deoptimization::unpack_frames handles frame creation
  // - The interpreter entry point is determined by the method kind
  // - We may need to create a stub function or use a different mechanism
  //
  // TODO: Implement proper deoptimized entry point for AArch64/TemplateInterpreter
  // For now, we return NULL which will cause a runtime error if called
  return NULL;  // ← THIS IS THE PROBLEM!
}
```

### 3. Comparison with Shark (x86_64)

Shark compiler uses CppInterpreter and has a working implementation:

```cpp
// sharkBuilder.cpp:345
Value* SharkBuilder::deoptimized_entry_point() {
  return make_function((address) CppInterpreter::main_loop, "iT", "v");
}
```

**Key difference:**
- **Shark (x86_64)**: Uses `CppInterpreter::main_loop` 
- **Yuhu (AArch64)**: Uses `TemplateInterpreter`, which has a different deoptimization mechanism

### 4. Why This Affects `encodeArrayLoop`

The method has **12 method calls** that could potentially be deoptimized:
- Multiple calls to `CharBuffer` and `ByteBuffer` methods
- Each call generates a `reexecute` block with a null function pointer
- All 12 calls fail IR verification

## Deoptimization in TemplateInterpreter

### How TemplateInterpreter Handles Deoptimization

1. **Deoptimization::unpack_frames()** - Creates new interpreter frames
2. **Method entry points** - Determined by method kind (accessor, native, etc.)
3. **YuhuInterpreter::_deopt_entry[]** - Array of deoptimization entry points

### Key Data Structures

```cpp
// yuhu_interpreter.hpp:106
static YuhuEntryPoint _deopt_entry[number_of_deopt_entries];

// yuhu_interpreter.cpp:12
YuhuEntryPoint YuhuInterpreter::_deopt_entry[number_of_deopt_entries];
```

### Entry Point Structure

```cpp
class YuhuEntryPoint {
private:
  address _entry[number_of_states];  // Entry points for different TosState
  
public:
  address entry(TosState state) const {
    return _entry[state];
  }
};
```

## Solution Options

### Option 1: Use YuhuInterpreter Deopt Entry (Recommended)

**Implementation:**

```cpp
Value* YuhuBuilder::deoptimized_entry_point() {
  // Use YuhuInterpreter's deoptimization entry point
  // The signature should match: (int recurse, Thread* thread) -> void
  // 
  // YuhuInterpreter::_deopt_entry[0] provides the deoptimization entry point
  // We need to get the address at runtime since it's initialized during interpreter setup
  
  // Create a function that returns the deopt entry address
  // Signature: "iT" -> "v" (int, Thread* -> void)
  return make_function(
    (address) YuhuInterpreter::deopt_entry(vtos),  // Use vtos (void) entry
    "iT", "v");
}
```

**Required changes:**

1. Add accessor method in `YuhuInterpreter`:
```cpp
// yuhu_interpreter.hpp
static address deopt_entry(TosState state) {
  return _deopt_entry[Interpreter::TosState_as_index(state)].entry(state);
}
```

2. Ensure deopt entries are initialized during interpreter setup

### Option 2: Create Stub Function

**Implementation:**

```cpp
Value* YuhuBuilder::deoptimized_entry_point() {
  // Create a stub that properly transitions to interpreter
  // This stub would:
  // 1. Set up interpreter frame
  // 2. Call Deoptimization::unpack_frames()
  // 3. Jump to interpreter entry point
  
  return make_function(
    (address) YuhuRuntime::deoptimize_caller,  // New runtime function
    "iT", "v");
}
```

**Required changes:**

1. Implement `YuhuRuntime::deoptimize_caller()`:
```cpp
JRT_ENTRY(void, YuhuRuntime::deoptimize_caller(int recurse, JavaThread* thread))
  // 1. Get current frame
  frame stub_frame = thread->last_frame();
  
  // 2. Find compiled frame that called us
  frame caller_frame = stub_frame.sender(&reg_map);
  
  // 3. Trigger deoptimization
  Deoptimization::deoptimize_frame(thread, caller_frame.id());
  
  // 4. Return to interpreter (Deoptimization::unpack_frames handles this)
JRT_END
```

### Option 3: Disable Deoptimization Check (Temporary Workaround)

**Not recommended for production**, but can be used for testing:

```cpp
// YuhuTopLevelBlock::do_call()
Value *deoptimized_frames = builder()->CreateCall(
    deopt_func_type, entry_point, deopt_args);

// TEMPORARY: Skip deoptimization check
// BasicBlock *reexecute      = function()->CreateBlock("reexecute");
BasicBlock *call_completed = function()->CreateBlock("call_completed");
// builder()->CreateCondBr(...);  // Commented out
builder()->CreateBr(call_completed);  // Always go to call_completed

// builder()->SetInsertPoint(reexecute);  // Skip reexecute block entirely
// ... reexecute code commented out ...

builder()->SetInsertPoint(call_completed);
```

**Drawbacks:**
- No deoptimization support
- May crash if a method actually gets deoptimized
- Only for testing/debugging

## Recommended Solution

**Use SharedRuntime's deoptimization blob** - This approach follows the same pattern used by C1 and C2 compilers.

### Step-by-Step Implementation

#### 1. Add required includes to yuhuBuilder.cpp

```cpp
#include "runtime/deoptimization.hpp"
#include "runtime/sharedRuntime.hpp"
```

#### 2. Update YuhuBuilder::deoptimized_entry_point()

```cpp
Value* YuhuBuilder::deoptimized_entry_point() {
  // For AArch64, we don't use CppInterpreter, so we need a different approach
  // TemplateInterpreter uses a different deoptimization mechanism
  // The signature is "iT" -> "v": void unpack_with_reexecution(int recurse, Thread* thread)
  // This is used when a callee gets deoptimized and we need to reexecute in the interpreter
  // 
  // For AArch64 with TemplateInterpreter, deoptimization is handled by the same
  // SharedRuntime deoptimization blob used by C1 and C2 compilers.
  // 
  // The deoptimization blob provides multiple entry points:
  // - unpack_with_reexecution(): for normal deoptimization with re-execution
  // - unpack_with_exception_in_tls(): when there's an exception in TLS
  // 
  // Use the same deoptimization blob infrastructure as C1/C2
  DeoptimizationBlob* deopt_blob = SharedRuntime::deopt_blob();
  assert(deopt_blob != NULL, "deoptimization blob must have been created");
  
  // Return the reexecution entry point with signature "iT" -> "v"
  // (int recurse, Thread* thread) -> void
  return make_function(
    (address) deopt_blob->unpack_with_reexecution(),
    "iT", "v");
}
```

## Testing

After implementation, verify:

1. **IR Generation**: Check that calls now have valid function pointers
```bash
/opt/homebrew/Cellar/llvm/20.1.5/bin/opt -verify _tmp_yuhu_ir_*.ll
```

2. **Runtime Behavior**: Test with a method that triggers deoptimization
```bash
java -XX:+UseYuhuCompiler \
     -XX:CompileCommand=yuhuonly,com/example/Matrix.multiply \
     -XX:+PrintCompilation \
     com.example.Matrix
```

3. **Deoptimization Events**: Check that deoptimization works correctly
```bash
java -XX:+UseYuhuCompiler \
     -XX:+TraceDeoptimization \
     -XX:+LogCompilation \
     com.example.Matrix
```

## Related Issues

- **027**: LLVM function parameter mismatch (separate issue)
- **028**: CodeBuffer overflow due to alignment bug (fixed)
- **030**: C1 and C2 deoptimization comparison (analysis document)

All four issues prevented successful compilation of `encodeArrayLoop`.

## References

- `yuhuBuilder.cpp:394` - Fixed NULL implementation
- `yuhuTopLevelBlock.cpp:1495-1503` - Usage site
- `c1_Runtime1_aarch64.cpp` - C1 deoptimization implementation
- `sharedRuntime.cpp` - Shared deoptimization blob infrastructure
- `sharkBuilder.cpp:345` - Working implementation for x86_64

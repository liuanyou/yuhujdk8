# Activity 068: Yuhu VM Call GC Crash - Missing RuntimeStub Wrapper

## Issue Summary

**Crash:** `assert(is_valid()) failed: check invoke` in `Bytecode_invoke::verify()`

**Location:** `hotspot/src/share/vm/code/nmethod.cpp:nmethod::preserve_callee_argument_oops()`

**Trigger:** GC safepoint stops thread inside VM call (e.g., `YuhuRuntime::new_instance`) when compiling methods with `new` bytecode

## Root Cause Analysis

### The Crash Sequence

1. Yuhu-compiled method executes `new #5 // class java/lang/StringBuilder` at BCI 36
2. Fast path TLAB allocation fails → slow path calls `YuhuRuntime::new_instance` (via `JRT_ENTRY`)
3. `JRT_ENTRY` executes `ThreadInVMfromJava __tiv(thread)` → triggers safepoint check
4. GC safepoint stops thread **inside the VM function**
5. GC thread scans frames starting from youngest frame
6. Youngest frame is the **nmethod frame** (at VM call return point)
7. GC creates `RegisterMap` with `include_argument_oops = true` (default initialization)
8. GC calls `nmethod::preserve_callee_argument_oops()`
9. This function creates `SimpleScopeDesc` to read BCI from scope descriptor
10. BCI = 36 (points to `new` instruction)
11. Constructs `Bytecode_invoke(method, bci=36)`
12. `Bytecode_invoke::verify()` calls `is_valid()` which checks if bytecode at BCI 36 is an invoke instruction
13. **Assertion fails** because BCI 36 is `new`, not an invoke bytecode

### Why C1/C2 Don't Have This Issue

**C1/C2 Architecture:**
```
Java method (nmethod) → RuntimeStub → VM function (e.g., Runtime1::new_instance)
```

**Yuhu Architecture:**
```
Java method (nmethod) → VM function directly (e.g., YuhuRuntime::new_instance)
```

**Critical Difference:**

When C1/C2 call VM functions:
1. They go through a **RuntimeStub** (separate CodeBlob)
2. When GC stops inside the VM function, the youngest frame is the **RuntimeStub frame**
3. GC calls `RuntimeStub::preserve_callee_argument_oops()` which **does nothing** (line 307 in codeBlob.hpp)
4. GC walks to sender frame (nmethod)
5. `frame::sender()` sets `include_argument_oops = false` initially
6. `sender_for_compiled_frame()` sets it based on `RuntimeStub::caller_must_gc_arguments()` → `false` (for callers from compiled code)
7. When scanning nmethod frame, `preserve_callee_argument_oops()` is **NOT called** because `include_argument_oops = false`
8. GC relies on OopMap to find live oops → **no assertion**

When Yuhu calls VM functions directly:
1. No RuntimeStub wrapper
2. When GC stops inside the VM function, the youngest frame is the **nmethod frame**
3. GC calls `nmethod::preserve_callee_argument_oops()` **directly**
4. This tries to create `Bytecode_invoke` with BCI pointing to `new` instruction
5. **Assertion fails**

### Key Code Paths

**RegisterMap initialization** (frame.cpp:101-111):
```cpp
void RegisterMap::clear() {
  set_include_argument_oops(true);  // ← DEFAULT IS TRUE
  // ...
}
```

**Frame sender walk** (frame_aarch64.cpp:540, 521):
```cpp
frame frame::sender(RegisterMap* map) const {
  map->set_include_argument_oops(false);  // ← Start with false
  // ...
}

frame frame::sender_for_compiled_frame(RegisterMap* map) const {
  if (map->update_map()) {
    map->set_include_argument_oops(_cb->caller_must_gc_arguments(map->thread()));
    // ← For RuntimeStub called from nmethod: false
    // ...
  }
}
```

**RuntimeStub's no-op** (codeBlob.hpp:307):
```cpp
void preserve_callee_argument_oops(frame fr, const RegisterMap *reg_map, OopClosure* f)  { 
  /* nothing to do */ 
}
```

**nmethod's implementation** (nmethod.cpp:1985-1998):
```cpp
void nmethod::preserve_callee_argument_oops(frame fr, const RegisterMap *reg_map, OopClosure* f) {
  if (!method()->is_native()) {
    SimpleScopeDesc ssd(this, fr.pc());
    Bytecode_invoke call(ssd.method(), ssd.bci());  // ← Assertion fires here if bci not invoke
    // ...
  }
}
```

### preserve_callee_argument_oops() Purpose

This function preserves outgoing oop arguments at **Java-to-Java call sites** during GC:
- When caller passes oop arguments to callee (another Java method)
- GC needs to update these oop locations if objects move
- Uses `Bytecode_invoke` to read callee's method signature to know which arguments are oops

**Why it's called for youngest frame:**
- RegisterMap initializes `include_argument_oops = true`
- For safety, GC tries to preserve callee arguments even at first frame
- For compiled code, OopMap already marks live oops, so this is technically redundant
- But the mechanism still runs and expects valid invoke bytecodes

### Why Yuhu Can't Use C1/C2's RuntimeStubs

C1/C2's RuntimeStubs (e.g., `Runtime1::new_instance_id`) are tightly coupled to C1/C2:
1. **Calling convention mismatch**: Expect specific register layouts
2. **Frame layout assumptions**: Built with C1/C2's frame size, FP/LR positions
3. **OopMap expectations**: Assume C1/C2's decached register state
4. **Argument passing**: Use specific argument registers that Yuhu may not match

## Solution: Create Yuhu-Specific RuntimeStub Wrappers

### Approach

Yuhu must generate its own RuntimeStub wrappers for VM calls:

1. **Generate RuntimeStub at compilation time**
   - Create a stub that matches Yuhu's calling convention
   - Save Yuhu's live registers
   - Set up proper frame layout
   - Handle thread state transition (ThreadInVMfromJava)
   - Call the VM function
   - Restore registers and return

2. **Register as RuntimeBlob**
   - Use `RuntimeStub::new_runtime_stub()` or similar
   - Set `caller_must_gc_arguments = false`
   - RuntimeStub's `preserve_callee_argument_oops()` will be no-op

3. **Call the stub from nmethod**
   - Instead of calling VM function directly, call the RuntimeStub
   - Register OopMap at the stub call site
   - Register call site metadata for JITLink correlation

### Expected Flow After Fix

```
Yuhu nmethod → Yuhu RuntimeStub → VM function (YuhuRuntime::new_instance)
```

When GC stops during VM call:
1. Youngest frame = **Yuhu RuntimeStub frame**
2. GC calls `RuntimeStub::preserve_callee_argument_oops()` → **does nothing**
3. GC walks to sender (nmethod frame)
4. `include_argument_oops = false` (from `caller_must_gc_arguments()`)
5. nmethod's `preserve_callee_argument_oops()` **not called**
6. GC uses OopMap to find live oops → **no crash**

### Implementation Considerations

1. **Stub generation mechanism**
   - Similar to C1's `Runtime1::generate_stub_call()`
   - Need to generate assembly for: frame setup, register save, call, restore, return
   - Must handle both leaf and non-leaf VM calls differently

2. **OopMap registration**
   - At stub call site in nmethod: mark live oops before call
   - Stub itself may need internal OopMap if it has safepoints

3. **last_Java_pc handling**
   - Non-leaf VM calls need last_Java_pc set before call
   - Leaf VM calls don't need it
   - Must match C1/C2's pattern for VM calls

4. **Thread state transition**
   - Use `ThreadInVMfromJava` or `ThreadInVMfromNative` appropriately
   - Ensure safepoint checks work correctly
   - Handle async exceptions

5. **Stub reuse**
   - Same VM function can share one stub across multiple compilation units
   - Stubs can be generated once at JVM startup or lazily

### Files to Modify

- `hotspot/src/share/vm/yuhu/yuhuRuntimeStub.cpp` (new) - RuntimeStub generation
- `hotspot/src/share/vm/yuhu/yuhuRuntimeStub.hpp` (new) - RuntimeStub interface
- `hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp` - Use stubs instead of direct VM calls
- `hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.hpp` - Update call_vm template
- `hotspot/src/share/vm/yuhu/yuhuDebugInformationRecorder.cpp` - Register stub call sites

### Verification

Test cases that should pass after fix:
- `new` bytecode with slow path (allocation failure)
- Methods with multiple `new` bytecodes
- GC during VM call (force with `-XX:+AlwaysPreTouch -XX:+ExplicitGCInvokesConcurrent`)
- Deoptimization at VM call sites
- Stack walking through VM call frames
- Methods with various VM calls (allocation, monitors, etc.)

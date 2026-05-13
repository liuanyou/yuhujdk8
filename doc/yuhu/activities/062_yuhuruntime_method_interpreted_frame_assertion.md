# Activity 062: YuhuRuntime::method() Assumes Interpreted Frame But Called From Yuhu-Compiled Code

**Date**: 2026-03-19  
**Author**: AI Assistant  
**Status**: Analysis Complete - Pending Fix  
**Related Files**: 
- `hotspot/src/share/vm/yuhu/yuhuRuntime.hpp` (method() function)
- `hotspot/src/share/vm/yuhu/yuhuRuntime.cpp` (new_instance, find_exception_handler, etc.)
- `hotspot/src/share/vm/runtime/frame.cpp` (interpreter_frame_method assertion)

**Related Issue**: Assertion failure when Yuhl runtime functions called from Yuhu-compiled code

---

## Executive Summary

`YuhuRuntime::method()` calls `interpreter_frame_method()` which asserts that the frame is an interpreted frame. However, all Yuhu runtime functions are only called from **Yuhu-compiled code**, which creates **compiled frames**, not interpreted frames. This causes an assertion failure.

**Root Cause**: Legacy code from Shark/Zero architecture that only had interpreted frames, not updated for AArch64 compiled frames.

**Evidence**: 
```
assert(is_interpreted_frame()) failed: interpreted frame expected
  at frame.cpp:400 in frame::interpreter_frame_method()
```

---

## Problem Description

### Symptoms

**Assertion Failure** (frame.cpp:400):
```
assert(is_interpreted_frame()) failed: interpreted frame expected
```

**Crash Location**: `frame::interpreter_frame_method()`

### Call Chain

**From error log** (hs_err_pid94442.log, lines 20-26):
```
V  frame::interpreter_frame_method() const+0x4c         ← Line 23: Assertion fails
V  YuhuRuntime::method(JavaThread*)+0x28                ← Line 24: Calling interpreter_frame_method
V  YuhuRuntime::new_instance(JavaThread*, int)+0x17c    ← Line 25: Slow path allocation
J 65 yuhu com.example.NineParameterTest.testNineParameters(...)  ← Line 26: Yuhu-compiled
```

### Execution Flow

1. `testNineParameters()` (Yuhu-compiled) executes `new` bytecode
2. Fast TLAB path fails or slow path taken
3. Calls `YuhuRuntime::new_instance(thread, index)` runtime function
4. Runtime function calls `method(thread)` to get current method
5. `method()` calls `last_frame(thread).interpreter_frame_method()`
6. **Assertion fails**: Frame is Yuhu-compiled, not interpreted

---

## Root Cause Analysis

### The Problematic Code

**yuhuRuntime.hpp:77-79**:
```cpp
static Method* method(JavaThread *thread) {
  return last_frame(thread).interpreter_frame_method();  // ← Assumes interpreted frame!
}
```

### Why It Fails

**Stack frame hierarchy**:
```
[Top of stack]
1. RuntimeStub frame (new_instance JRT_ENTRY)
2. Yuhu-compiled frame (testNineParameters)  ← NOT interpreted!
3. ...
```

**When `method(thread)` is called**:
1. `last_frame(thread)` gets the top Java frame
2. This is the **Yuhu-compiled frame** (not a runtime stub)
3. `interpreter_frame_method()` checks `is_interpreted_frame()`
4. **Assertion fails** because it's a compiled frame

### All Call Sites of `method(thread)`

**yuhuRuntime.cpp** (5 locations):

1. **Line 46** - `find_exception_handler()`:
   ```cpp
   constantPoolHandle pool(thread, method(thread)->constants());
   ```

2. **Line 90** - `new_instance()`:
   ```cpp
   Klass* k_oop = method(thread)->constants()->klass_at(index, CHECK);
   ```

3. **Line 127** - `anewarray()`:
   ```cpp
   Klass* klass = method(thread)->constants()->klass_at(index, CHECK);
   ```

4. **Line 136** - `multianewarray()`:
   ```cpp
   Klass* klass = method(thread)->constants()->klass_at(index, CHECK);
   ```

5. **Line 81** - `bcp()` helper:
   ```cpp
   return method(thread)->code_base() + bci;
   ```

### Call Site Analysis

**All callers are YuhuRuntime functions** that are only invoked from **Yuhu-compiled code**:

- `find_exception_handler()` - Called from `YuhuTopLevelBlock::marshal_exception_fast()` (yuhuTopLevelBlock.cpp:627)
- `new_instance()` - Called from object allocation slow path (yuhuTopLevelBlock.cpp:2035)
- `anewarray()` - Called from `do_anewarray()` (yuhuTopLevelBlock.cpp:2084)
- `multianewarray()` - Called from `do_multianewarray()` (yuhuTopLevelBlock.cpp:2114)

**Key Invariant**: These functions are **never called from interpreted code**, only from Yuhu-compiled code.

---

## Historical Context

### Origin of the Bug

**Yuhu is based on Shark** (LLVM backend for Zero architecture):
- Zero architecture **only has interpreted frames**
- Shark compiled code still used interpreted-style frames
- `interpreter_frame_method()` was correct for Zero/Shark

**AArch64 Yuhu is different**:
- Yuhu generates **real compiled frames** (not interpreted)
- Frame layout matches C1/C2 compiled frames
- `interpreter_frame_method()` is **incorrect** for compiled frames

### Why It Wasn't Caught Earlier

**Fast path dominance**:
- TLAB allocation fast path doesn't call runtime
- Most object allocations succeed without calling `new_instance()`
- Only slow path or edge cases trigger the runtime call

**Activity 061 impact**:
- After fixing klass pointer encoding, more allocations may hit slow path
- Exposed this pre-existing bug

---

## The Fix Required

### Correct Implementation

Since `method(thread)` is **only called from Yuhu-compiled code**, it should handle **compiled frames**:

```cpp
static Method* method(JavaThread *thread) {
  frame f = last_frame(thread);
  
  // Yuhu runtime is only called from Yuhu-compiled code
  // The caller frame is always a compiled frame, never interpreted
  assert(f.is_compiled_frame() || f.is_runtime_frame(), 
         "Yuhu runtime called from unexpected frame type");
  
  // Get method from the compiled frame's CodeBlob
  CodeBlob* cb = f.cb();
  assert(cb->is_nmethod() || cb->is_runtime_stub(), "expected compiled code");
  return cb->as_nmethod_or_shared_runtime()->method();
}
```

**Alternative** (simpler, if we trust the invariant):
```cpp
static Method* method(JavaThread *thread) {
  // Called only from Yuhu-compiled code via JRT_ENTRY
  // The frame below the runtime stub is the Yuhu-compiled frame
  frame f = last_frame(thread);
  return f.cb()->as_nmethod()->method();
}
```

### Key Differences

**Old (incorrect)**:
```cpp
return last_frame(thread).interpreter_frame_method();  // Assumes interpreted
```

**New (correct)**:
```cpp
return last_frame(thread).cb()->as_nmethod()->method();  // Assumes compiled
```

---

## Frame Type Investigation

### How to Check Frame Type

**frame API methods**:
- `is_interpreted_frame()` - True for interpreted frames
- `is_compiled_frame()` - True for C1/C2/Yuhu compiled frames  
- `is_runtime_frame()` - True for runtime stubs
- `is_entry_frame()` - True for JNI/JavaCalls entry frames
- `cb()` - Returns CodeBlob (contains method for compiled frames)

### For Compiled Frames

**Get Method from CodeBlob**:
```cpp
CodeBlob* cb = frame.cb();
if (cb->is_nmethod()) {
  Method* method = cb->as_nmethod()->method();
}
```

**Yuhu frames should be nmethods**:
- Yuhu generates CodeBlob similar to C1/C2
- Should be registered as nmethod
- `cb()->as_nmethod()->method()` should work

---

## Evidence

### Error Log (hs_err_pid94442.log)

**Line 5**: Assertion message
```
assert(is_interpreted_frame()) failed: interpreted frame expected
```

**Line 17**: Current thread
```
Current thread (0x000000015b809800):  JavaThread "main" [_thread_in_vm, id=6403, stack(...)]
```

**Lines 20-26**: Native frames
```
V  [libjvm.dylib+0xa9ce8c]  VMError::report_and_die()+0x498
V  [libjvm.dylib+0x3d1440]  report_vm_error(...)+0x78
V  [libjvm.dylib+0x48d274]  frame::interpreter_frame_method() const+0x4c
V  [libjvm.dylib+0xb5e2fc]  YuhuRuntime::method(JavaThread*)+0x28
V  [libjvm.dylib+0xb5eaa0]  YuhuRuntime::new_instance(JavaThread*, int)+0x17c
J 65 yuhu com.example.NineParameterTest.testNineParameters(IIIIIIIII)I
```

**Lines 35-38**: Java frames
```
J 65 yuhu com.example.NineParameterTest.testNineParameters(IIIIIIIII)I
j  com.example.NineParameterTest.main([Ljava/lang/String;)V+30
v  ~StubRoutines::call_stub
```

### Yuhu Code Invocation

**Object allocation slow path** (yuhuTopLevelBlock.cpp:2035):
```cpp
builder()->new_instance(),  // Calls YuhuRuntime::new_instance()
```

**YuhuRuntime::new_instance** (yuhuRuntime.cpp:89-115):
```cpp
JRT_ENTRY(void, YuhuRuntime::new_instance(JavaThread* thread, int index))
  Klass* k_oop = method(thread)->constants()->klass_at(index, CHECK);  // ← Calls method()
  instanceKlassHandle klass(THREAD, k_oop);
  klass->check_valid_for_instantiation(true, CHECK);
  klass->initialize(CHECK);
  oop obj = klass->allocate_instance(CHECK);
  thread->set_vm_result(obj);
JRT_END
```

---

## Key Takeaways

### 1. Yuhl Runtime Only Called From Compiled Code

All `YuhuRuntime::*` functions are only invoked from Yuhu-compiled code, never from interpreted code. The frame accessing logic should reflect this invariant.

### 2. Frame Type Matters

- **Interpreted frames**: Method stored in frame's `_interpreter_frame_method` field
- **Compiled frames**: Method stored in CodeBlob's `_method` field
- Different APIs required to access method from different frame types

### 3. Legacy Code From Zero Architecture

Yuhu inherited Shark's Zero-based implementation that assumed interpreted frames. AArch64 Yuhu generates real compiled frames, requiring different frame access patterns.

### 4. Fast Path Masked The Bug

Most allocations use TLAB fast path (no runtime call). The bug only manifests when slow path is taken, which is rare under normal conditions.

---

## Related Issues

- **Activity 061**: Klass pointer encoding fix (may have increased slow path frequency)
- **Shark/Zero**: Original source of interpreted frame assumption
- **C1/C2**: Both handle compiled frames correctly in runtime functions

---

**Next Steps**: Update `YuhuRuntime::method()` to handle compiled frames instead of interpreted frames

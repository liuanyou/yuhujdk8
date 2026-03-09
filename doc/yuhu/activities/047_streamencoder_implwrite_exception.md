# Issue 047: IllegalArgumentException from UncaughtExceptionHandler After Yuhu Compilation

## Problem Summary

After successfully compiling `com.example.Matrix.multiply`, Yuhu compiler encounters a critical error when executing `sun.nio.cs.StreamEncoder.implWrite([CII)V`:

```
Exception: java.lang.IllegalArgumentException thrown from the UncaughtExceptionHandler in thread "main"
```

**Key characteristic**: Full stack trace is suppressed - only this single line is visible, making diagnosis extremely difficult.

## Timeline

1. **Compilation succeeds**: `com.example.Matrix.multiply` (method without try-catch) compiles and runs correctly
2. **Second compilation**: `sun.nio.cs.StreamEncoder.implWrite([CII)V` gets compiled by Yuhu
3. **Runtime failure**: During execution of implWrite, exception occurs with no detailed trace

## Root Cause Analysis (Updated)

### CRITICAL BUG: Method Call Return Value Not Propagated to Local Variable

**Discovery Date**: 2026-02-28

**Problem**: Yuhu compiler fails to store the return value of method calls into local variables, causing subsequent uses of that variable to read incorrect values from the stack frame.

#### Evidence from `StreamEncoder.implWrite`

**Java Source:**
```java
CharBuffer cb = CharBuffer.wrap(cbuf, off, len);  // Returns CharBuffer in x0
while (cb.hasRemaining()) {  // Should use the returned CharBuffer
    // ...
}
```

**Generated LLVM IR:**
```llvm
; Line 54: Call to CharBuffer.wrap()
%19 = call ptr inttoptr (i64 4999711360 to ptr)(i64 0, ptr %2, i32 %3, i32 %4)
; ↑ Returns CharBuffer object in %19 (x0 register)

; ❌ MISSING: No store instruction to save %19 to local variable slot!
; Should be: store ptr %19, ptr %cb_slot, align 8

; Later at line 114-115: Phi node loads from WRONG stack slot
%local_0_ = phi ptr [ %31, %no_exception ], ...
; %31 = load ptr, ptr %frame[16]  ← Wrong! This is old value, not wrap() result

; When calling hasRemaining(), passes wrong object
%hasRemaining = call ... hasRemaining(%local_0_)  
; ↑ Passes incorrect CharBuffer (or worse, passes cbuf array instead!)
```

#### Impact on Runtime Behavior

1. **Wrong Object Type**: `cb.hasRemaining()` receives wrong object:
   - Expected: CharBuffer object (returned from `wrap()`)
   - Actual: Old value from stack frame (possibly the original `cbuf` char array)

2. **Field Access Errors**: When `hasRemaining()` tries to access CharBuffer fields:
   - Reads from wrong memory layout (array instead of Buffer object)
   - Gets invalid values for `count`, `pos`, `limit` fields
   - May trigger `IllegalArgumentException` or other runtime exceptions

3. **Cascade Failures**: Subsequent operations on `cb` all use corrupted state:
   - `encoder.encode(cb, bb, false)` - passes wrong buffer
   - Loop condition checks - operate on garbage data
   - Exception throwing - based on invalid state

#### Technical Root Cause

**Location**: `hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp`

**Function**: `cache_after_Java_call()` at line 305-323

**Bug**: Line 320 creates YuhuValue with NULL LLVM value:
```cpp
push(YuhuValue::create_generic(type, NULL, false));
//                               ↑^^^ 
//                               Should be: call_result from CreateCall
```

**Correct Implementation Should Be**:
```cpp
Value* call_result = builder()->CreateCall(...);  // Line 1615
// ...
push(YuhuValue::create_generic(type, call_result, false));
//                               ↑^^^^^^^^^^^
//                               Pass actual call result
```

#### Why This Causes IllegalArgumentException

When `hasRemaining()` is called with wrong object:
1. Receives char array instead of CharBuffer
2. Tries to access Buffer field at offset 23 (`isReadOnly` flag)
3. Reads garbage data from array memory
4. Validation logic detects invalid state
5. Throws `IllegalArgumentException`

The exception appears from `UncaughtExceptionHandler` because:
- No try-catch in implWrite for this type of internal error
- Exception propagates up through native call frames
- JVM's default handler reports it as "from UncaughtExceptionHandler"

#### Diagnostic Evidence

**LLVM IR Analysis** (`test_yuhu/_tmp_yuhu_ir_sun.nio.cs.StreamEncoder__implWrite.ll`):

1. **Line 54**: `%19 = call ... wrap(...)` - Return value assigned to `%19`
2. **Lines 63-66**: Store instructions for parameters, but NO store for `%19`
3. **Line 114**: `%local_0_ = phi ptr [ %31, ... ]` - Loads from wrong slot
4. **Search result**: `%19` only appears once (at definition), never used!

This confirms: Yuhu compiler discards the return value entirely.

---

## Suspected Root Causes (Original Hypotheses - Now Secondary)

### Hypothesis A: Missing Exception Handler Registration

**StreamEncoder.implWrite source analysis:**
```java
void implWrite(char cbuf[], int off, int len) throws IOException {
    CharBuffer cb = CharBuffer.wrap(cbuf, off, len);
    
    if (haveLeftoverChar)
        flushLeftoverChar(cb, false);
    
    while (cb.hasRemaining()) {
        CoderResult cr = encoder.encode(cb, bb, false);
        if (cr.isUnderflow()) {
            // ...
        }
        if (cr.isOverflow()) {
            // ...
            writeBytes();
            continue;
        }
        cr.throwException();  // ← CRITICAL: This can throw exceptions!
    }
}
```

**Problem**: The method calls `cr.throwException()`, which can throw exceptions. However, Yuhu compiler's current logic may not detect this as requiring an exception handler because:
- No explicit try-catch in Java source
- LLVM IR might have exception edges from `throwException()` call
- Current detection only checks for landing pads, not potential exception-throwing calls

**Expected behavior**: Methods that can throw exceptions (even without try-catch) need proper exception handling - either local handler or unwind handler.

### Hypothesis B: Synchronized Method Monitor Not Released

If `implWrite` or any called method is synchronized, the unwind handler must release monitors before propagating exceptions upward. Current implementation does NOT handle this.

### Hypothesis C: Stack Frame Corruption

Yuhu-generated code might corrupt the stack frame, causing:
- Wrong return addresses
- Corrupted exception oop
- Invalid PC values during exception dispatch

This would explain why the stack trace is truncated.

## Technical Details

### Method Signature
```
sun/nio/cs/StreamEncoder implWrite ([CII)V
```

### Execution Context
- Called during Matrix multiplication test
- Part of `System.out.println("loop " + i + " c[0][0] " + c[0][0])` 
- println → print → write → implWrite call chain

### Error Characteristics
- Error type: `IllegalArgumentException` (NOT the original exception!)
- Location: `UncaughtExceptionHandler` in thread "main"
- Visibility: Single line only, full trace suppressed
- Timing: Occurs AFTER Matrix.multiply compilation succeeds

## Debugging Challenges

### Why No Stack Trace?

The fact that we only see one line suggests:
1. **Exception happened inside exception handler** - JVM cannot recursively handle exceptions
2. **Corrupted thread state** - Stack walking fails due to invalid frame pointers
3. **Native code exception** - Exception thrown from JNI/native code without proper Java frame

### Diagnostic Protocol (from memory)

According to "Yuhu Compiler Exception Truncation Diagnostic Protocol":
> Diagnosis must start from native/runtime state: inspect exception entry point registers (e.g., x0–x30, pc, sp), nmethod metadata, stub linkage, and frame layout — not source code lines or try-catch structure.

## Investigation Status

### ✅ COMPLETED: Root Cause Identified

**Date**: 2026-02-28

**Method**: LLVM IR analysis and code review of Yuhu compiler

**Finding**: Method call return value not propagated to local variables

**Evidence**: 
- `test_yuhu/_tmp_yuhu_ir_sun.nio.cs.StreamEncoder__implWrite.ll` shows `%19` (wrap() return) never used
- `cache_after_Java_call()` passes NULL instead of `call_result`
- Runtime symptom: IllegalArgumentException from wrong object type

**Status**: Root cause confirmed, fix identified, awaiting code change

---

### Original Investigation Plan (Now Historical)

### Step 1: Confirm Which Methods Are Compiled

Add logging to YuhuCompiler::compile_method:
```cpp
tty->print_cr("Yuhu: Compiling %s.%s %s", 
              target->holder()->name()->as_utf8(),
              target->name()->as_utf8(),
              target->signature()->as_utf8());
```

**Expected output**: See if `StreamEncoder.implWrite` is actually compiled by Yuhu.

### Step 2: Check Exception Handler Size

Add logging to measure/generate functions:
```cpp
tty->print_cr("Yuhu: exc_handler_size=%d for %s", 
              exc_handler_size, func_name);
```

**What to look for**: 
- If `exc_handler_size > 0` but handler not registered → registration bug
- If `exc_handler_size == 0` but method can throw → detection bug

### Step 3: Temporarily Exclude implWrite from Yuhu Compilation

Test if excluding this method fixes the issue:
```bash
java -XX:+UseYuhuCompiler -XX:TieredStopAtLevel=6 \
     -XX:CompileCommand=yuhuonly,com/example/Matrix.multiply \
     -XX:CompileCommand=-yuhuonly,sun.nio.cs.StreamEncoder.implWrite \
     com.example.Matrix
```

### Step 4: Analyze LLVM IR

If implWrite is compiled, examine its LLVM IR:
```bash
# Dump LLVM IR for implWrite
# Look for: invoke instructions, landing pads, exception edges
```

**What to look for**:
- `invoke` vs `call` instructions (invoke has exception path)
- Landing pad blocks
- Exception handling metadata

### Step 5: Native-Level Debugging

When breakpoint hits at exception entry:
1. **Registers**: x0-x30, pc, sp
2. **Stack**: Backtrace from sp
3. **nmethod metadata**: Find blob for current PC
4. **Exception oop**: What exception was thrown?
5. **Handler lookup**: What PC was used to search?

## Related Files

- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - Handler generation logic
- `hotspot/src/cpu/aarch64/vm/c1_Runtime1_aarch64.cpp` - Exception dispatch
- `hotspot/src/share/vm/code/nmethod.cpp` - Exception table registration
- `hotspot/src/share/vm/c1/c1_LIRAssembler_aarch64.cpp` - C1 reference implementation

## Known Limitations (from Issue 046)

Current Yuhu exception handling gaps:
1. ❌ Synchronized methods: monitor exit not implemented
2. ❌ Try-catch detection: Only checks for landing pads, misses exception-throwing calls
3. ❌ Exception handler registration: Skipped to avoid double entry (temporary workaround)
4. ❌ Mixed-mode boundaries: Interpreter ↔ Compiled exception propagation untested

## Initial Evidence

**Test command:**
```bash
java -XX:+UseYuhuCompiler -XX:TieredStopAtLevel=6 \
     -XX:CompileCommand=yuhuonly,com/example/Matrix.multiply \
     com.example.Matrix
```

**Error output:**
```
开始测试矩阵乘法...
loop 0 c[0][0] 0
loop 1 c[0][0] 0
...
[Compilation: sun.nio.cs.StreamEncoder.implWrite]
Exception: java.lang.IllegalArgumentException thrown from the UncaughtExceptionHandler in thread "main"
```

**Missing information:**
- No hs_err file generated
- No Java stack trace
- No indication of which line/method threw original exception

### Next Steps (Updated)

#### Immediate Actions

1. **✅ COMPLETED (2026-03-09)**: Root cause analysis and fix implementation
   - Bug #1: `cache_after_Java_call()` passing NULL instead of `call_result`
   - Bug #2: `YuhuCacher::process_*_slot()` overwriting live values with stale frame data
   
2. **✅ COMPLETED (2026-03-09)**: Two-part fix implemented
   - Fix #1: Updated `yuhuTopLevelBlock.cpp` + `.hpp` to pass `call_result` parameter
   - Fix #2: Updated `yuhuCacheDecache.cpp` to skip reload if value already has LLVM value
   - Files modified:
     - `hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.hpp` line 220
     - `hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp` lines 305-323, 1632
     - `hotspot/src/share/vm/yuhu/yuhuCacheDecache.cpp` lines 368-376 (process_local_slot)
     - `hotspot/src/share/vm/yuhu/yuhuCacheDecache.cpp` lines ~300 (process_stack_slot)
   
3. **✅ VERIFIED (2026-03-09)**: IR confirms fix working correctly
   - `%19` (wrap result) now flows directly to phi node at line 112
   - `%19` stored to correct local variable slot `frame[12]` at lines 1375, 1390
   - `hasRemaining()` receives correct CharBuffer at line 198
   - No more loading from wrong `frame[4]` slot
   
4. **TODO**: Run test to verify runtime behavior fixed
   - Command: `java -XX:+UseYuhuCompiler -XX:TieredStopAtLevel=6 com.example.Matrix`
   - Expected: No more IllegalArgumentException from UncaughtExceptionHandler
   - Expected: Correct output showing matrix multiplication results

5. **TODO**: Check for similar issues in other methods
   - Search for other invoke+astore patterns that might have same bug
   - Verify all method call return values are properly propagated

### Original Priority List (Historical)

---

*Created: 2026-02-28*
*Status: Investigating*
*Severity: Critical (blocks all real-world workloads)*

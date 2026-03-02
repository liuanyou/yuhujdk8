# Yuhu Compiler Exception Handler Generation Bug

## Problem Summary

Yuhu compiler incorrectly generates Exception Handler and Deopt Handler addresses, causing both to point to the same invalid `udf #0` trap instruction address, leading to SIGILL crashes when exceptions occur.

## Key Evidence

From `test_yuhu/hs_err_pid26982.log`:
```
Registers:
 x0=0x000000076ab512f0  x1=0x00000001219f5244  x2=0x0000000000000001  x3=0x00000001219f35d8
 x4=0x0000000180490a4f  x5=0x000000000000dead  x6=0x0000000000000029  x7=0x000000011f81c800
 x8=0x0000000000000000  x9=0x0000600001650788 x10=0x000000016d3d62a8 x11=0x00000000fffffffd
x12=0x0000000000000000 x13=0x0000000000000000 x14=0x0000000000000000 x15=0x0000000000000000
x16=0x000000018057ceac x17=0x00000001dfdd3238 x18=0x0000000000000000 x19=0x000000000000dead
x20=0x000000016d3d61e0 x21=0x000000010bd394b0 x22=0x0000000104c388e6 x23=0x000000076ab90fe8
x24=0x000000016d3d61d0 x25=0x00000001219f8300 x26=0x0000000104c3c268 x27=0x0000000000000000
x28=0x000000011f81c800  fp=0x000000016d3d63f0  lr=0x00000001219f35d8  sp=0x000000016d3d62f0
pc=0x00000001219f5244 cpsr=0x0000000060001000

# x1 == pc == 0x00000001219f5244 (deopt handler trap instruction)
# lr = 0x00000001219f35d8 (normal return address)
# x0 = IllegalArgumentException (exception object)
```

From `debug/yuhu_new_code.txt`:
```
[Exception Handler]
[Stub Code]
[Deopt Handler Code]
  0x00000001219f5244: udf       #0
```

## Root Cause

In `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp:626-627`:
```cpp
CodeOffsets offsets;
offsets.set_value(CodeOffsets::Deopt, 0);
offsets.set_value(CodeOffsets::Exceptions, 0);
```

**Yuhu compiler incorrectly sets both Deopt and Exception handler offsets to 0**, causing them to point to the same invalid address (trap instruction area).

## JVM Exception Handling Flow

### Normal Exception Flow:
1. Exception occurs in compiled code
2. Exception object stored in thread's `pending_exception` field
3. Control returns to caller normally
4. Caller calls `check_pending_exception()`
5. JVM looks up exception handler in method's exception table
6. Jump to local exception handler (catch block) or propagate up

### Current Broken Flow:
1. Exception occurs in compiled code
2. Exception object stored in `pending_exception`
3. JVM tries to jump to Exception Handler
4. **Handler address points to `udf #0` trap instruction**
5. **SIGILL crash occurs**

## Impact

- All exceptions in Yuhu-compiled methods cause JVM crashes
- Cannot handle any Java exceptions (NullPointerException, etc.)
- Makes Yuhu compiler unusable for production

## Solution Required

Fix Yuhu compiler to properly generate Exception Handler and Deopt Handler code:
1. Generate proper exception handler stub code
2. Set correct offsets in CodeOffsets
3. Ensure handlers point to valid code, not trap instructions

## Files Affected
- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` - Exception handler generation
- `debug/yuhu_new_code.txt` - Generated assembly showing trap addresses
- `test_yuhu/hs_err_pid26982.log` - Crash evidence
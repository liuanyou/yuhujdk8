# YuhuInterpreter Double Array Load Bug (daload)

**Date**: 2025-10-18  
**Status**: **RESOLVED**  
**Severity**: Critical

## Summary

YuhuInterpreter crashes with `NullPointerException` when accessing double array elements, even though the array itself is successfully created and is not null. This bug affects Spring Boot Actuator and any application that uses double arrays.

## Symptom

```
Caused by: java.lang.NullPointerException: null
	at sun.management.BaseOperatingSystemImpl.getSystemLoadAverage(BaseOperatingSystemImpl.java:71)
	at io.micrometer.core.instrument.binder.system.ProcessorMetrics.bindTo(ProcessorMetrics.java:93)
```

### Confusing Behavior

The error appears to be a null array, but debugging shows:
```java
private double[] loadavg = new double[1];  // ✅ This succeeds
System.out.println("loadavg = " + loadavg);  // ✅ Prints: [D@6d06d69c (not null!)
System.out.println("loadavg[0] = " + loadavg[0]);  // ❌ NullPointerException!
```

The array **IS** created successfully, but accessing its elements crashes.

---

## Root Cause

**File:** `hotspot/src/cpu/aarch64/vm/yuhu/yuhu_templateTable_aarch64.cpp`  
**Function:** `YuhuTemplateTable::daload()`  
**Lines:** 501-502

### The Bug

The `daload` bytecode instruction loads a double value from an array. The implementation had **TWO critical errors**:

```cpp
void YuhuTemplateTable::daload()
{
    transition(itos, dtos);
    __ write_inst("mov x1, x0");
    __ write_inst_pop_ptr(__ x0);
    // r0: array
    // r1: index
    index_check(__ x0, __ x1); // leaves index in r1, kills rscratch1
    
    // Calculate element address: array + (index * 8)
    __ write_insts_lea(__ x0, YuhuAddress(__ x0, __ w1, YuhuAddress::uxtw(3)));
    
    // BUG 1: Loading into s0 (32-bit float) instead of d0 (64-bit double)
    // BUG 2: Loading from x1 (index) instead of x0 (calculated address)
    __ write_inst("ldr s0, [x1, #%d]", arrayOopDesc::base_offset_in_bytes(T_DOUBLE));
}
```

### The Problems

1. **Wrong register size**: `s0` is a 32-bit float register (holds only 4 bytes)
   - Should use `d0` (64-bit double register, holds 8 bytes)
   - Loading a double into `s0` only reads 4 bytes, corrupting the value

2. **Wrong base register**: After `lea`, the element address is in `x0`
   - But the code loads from `x1` (which contains the index, not an address!)
   - Loading from an invalid address causes memory access violation → NullPointerException

### Correct Implementation

From standard AArch64 interpreter (`templateTable_aarch64.cpp:722-723`):

```cpp
__ lea(r1,  Address(r0, r1, Address::uxtw(3)));
__ ldrd(v0, Address(r1,  arrayOopDesc::base_offset_in_bytes(T_DOUBLE)));
```

Notice:
- Stores calculated address in `r1` (not `r0`)
- Loads into `v0`/`d0` (double register, not float)
- Loads from `r1` (the calculated address)

---

## The Fix

**Changed Lines 501-502:**

```cpp
// BEFORE (BROKEN):
__ write_insts_lea(__ x0, YuhuAddress(__ x0, __ w1, YuhuAddress::uxtw(3)));
__ write_inst("ldr s0, [x1, #%d]", arrayOopDesc::base_offset_in_bytes(T_DOUBLE));

// AFTER (FIXED):
__ write_insts_lea(__ x1, YuhuAddress(__ x0, __ w1, YuhuAddress::uxtw(3)));
__ write_inst("ldr d0, [x1, #%d]", arrayOopDesc::base_offset_in_bytes(T_DOUBLE));
```

**Changes:**
1. Store calculated address in `x1` instead of `x0`
2. Load into `d0` (double) instead of `s0` (float)

---

## Impact

### Affected Applications

- **Spring Boot with Actuator**: Crashes during startup when `ProcessorMetrics` tries to get system load average
- **Any application using double arrays**: Crashes when accessing `doubleArray[index]`
- **Scientific computing**: Any numeric calculations using double arrays
- **Financial applications**: Any code working with double arrays

### Specific Failure: BaseOperatingSystemImpl

```java
public class BaseOperatingSystemImpl {
    private double[] loadavg = new double[1];
    
    public double getSystemLoadAverage() {
        unsafe.getLoadAverage(loadavg, 1);  // ✅ This works
        return loadavg[0];  // ❌ NullPointerException! (daload bytecode)
    }
}
```

The method:
1. ✅ Creates the array successfully (`new double[1]`)
2. ✅ Calls native method to populate it
3. ❌ Crashes when reading `loadavg[0]` due to broken `daload`

---

## Test Case

### Minimal Reproduction

```java
public class TestDaload {
    public static void main(String[] args) {
        double[] arr = new double[1];  // ✅ Creates array
        arr[0] = 1.5;                  // ✅ dastore works (if fixed)
        double value = arr[0];         // ❌ daload crashes!
        System.out.println(value);
    }
}
```

### Comprehensive Test

```java
public class TestFieldInitialization {
    static class SimpleArrayInit {
        private double[] data = new double[1];
        
        public void test() {
            System.out.println("data = " + data);      // ✅ Shows array exists
            System.out.println("data[0] = " + data[0]); // ❌ NullPointerException!
        }
    }
}
```

---

## Bytecode Involved

When accessing `doubleArray[index]`:

```
aload_0          // Load 'this' or local var
getfield data    // Get array reference
iconst_0         // Load index
daload           // ← THIS BYTECODE WAS BROKEN!
```

The `daload` bytecode:
- **Stack before**: `[array_ref, index]`
- **Stack after**: `[double_value]`
- **Operation**: Load double from `array[index]` and push onto stack

---

## Why This Went Undetected

1. **JIT compiler not affected**: Only interpreter mode (`-Xint`) with YuhuInt (`-XUseYuhuInt`) triggers this
2. **Double arrays are less common**: Most code uses primitive variables or object arrays
3. **Crash looks like null array**: The NullPointerException made it appear like a null reference issue, not a bytecode bug
4. **Spring Boot masked it**: Only appeared when Actuator was enabled (which uses `getSystemLoadAverage()`)

---

## Related Bugs

This is **NOT** related to the AspectJ floating-point bugs (see `2025-10-08_AspectJ_Pointcut_Recognition_Issue.md`):

| Bug | Component | Symptom |
|-----|-----------|---------|
| AspectJ Bug #1 | `wide_iinc` | Integer corruption |
| AspectJ Bug #2 | `dcmpg`/`dcmpl` | Comparison failures |
| **This Bug** | `daload` | Array access crashes |

While the AspectJ bugs affected floating-point comparisons, this bug affects array access - a completely different operation.

---

## Test Results

### Before Fix

```
Test 1: Simple array field initialization
[Test1] SimpleArrayInit.test()
  this = TestFieldInitialization$SimpleArrayInit@15db9742
  data = [D@6d06d69c  ← Array exists!
  data.length = 1     ← Can get length
Test 1 FAILED with exception: java.lang.NullPointerException  ← Crashes on data[0]
```

### After Fix

```
Test 1: Simple array field initialization
[Test1] SimpleArrayInit.test()
  this = TestFieldInitialization$SimpleArrayInit@15db9742
  data = [D@6d06d69c
  data.length = 1
  data[0] = 0.0  ← Works!

Test 4: Different array sizes
  arr1 = [D@55f96302
  arr2 = [D@3d4eac69
  arr3 = [D@42a57993
  intArr = [I@75b84c92
  All arrays initialized correctly!
```

---

## Technical Details

### AArch64 Register Usage

- **Integer registers**: `x0-x30` (64-bit), `w0-w30` (32-bit lower half)
- **Floating-point registers**: 
  - `v0-v31` (128-bit SIMD)
  - `d0-d31` (64-bit double precision)
  - `s0-s31` (32-bit single precision)

### Array Element Access Formula

For `doubleArray[index]`:
```
address = array_base + (index * 8) + header_offset
```

Where:
- `array_base` = pointer to array object
- `index * 8` = double is 8 bytes (left shift by 3)
- `header_offset` = `arrayOopDesc::base_offset_in_bytes(T_DOUBLE)`

### The Bug in Detail

**Line 501** calculated address correctly but stored in wrong register:
```cpp
// Calculates: x0 = array + (index << 3)
__ write_insts_lea(__ x0, YuhuAddress(__ x0, __ w1, YuhuAddress::uxtw(3)));
// Now x0 has element address, but x1 still has index!
```

**Line 502** then loaded from wrong register with wrong size:
```cpp
// Tries to load from x1 (index value, not address!)
// And loads only 4 bytes into s0 instead of 8 bytes into d0
__ write_inst("ldr s0, [x1, #%d]", arrayOopDesc::base_offset_in_bytes(T_DOUBLE));
```

This caused:
- Reading from invalid memory address (`x1` = index, not a pointer)
- Only reading 4 bytes when 8 bytes needed
- Both errors combined to cause memory access violation

---

## Verification

Run test to verify fix:

```bash
cd /Users/liuanyou/CLionProjects/jdk8

# Compile test
./build/macosx-aarch64-normal-server-slowdebug/jdk/bin/javac TestFieldInitialization.java

# Run with YuhuInterpreter
./build/macosx-aarch64-normal-server-slowdebug/jdk/bin/java \
  -Xint -XUseYuhuInt TestFieldInitialization
```

Expected: All tests pass, no NullPointerException

---

## Other Array Load Bytecodes (Status Check)

| Bytecode | Type | Status |
|----------|------|--------|
| `iaload` | int[] | ✅ Works |
| `laload` | long[] | ✅ Works |
| `faload` | float[] | ✅ Works |
| **`daload`** | **double[]** | ❌ **Was broken, now fixed** |
| `aaload` | Object[] | ✅ Works |
| `baload` | byte[]/boolean[] | ✅ Works |
| `caload` | char[] | ✅ Works |
| `saload` | short[] | ✅ Works |

Only `daload` (double array load) was affected.

---

## Status: CLOSED - Bug Fixed ✅

**Fixed by:** Liu Anyou  
**Date:** October 18, 2025  
**Verification:** Spring Boot Actuator now works, all double array tests pass


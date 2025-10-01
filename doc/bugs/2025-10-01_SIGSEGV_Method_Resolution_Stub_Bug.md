# SIGSEGV in Method Resolution Stub - Invalid Cache Entry Access

## Issue Summary
- **Date**: 2025-10-01
- **Signal**: `SIGSEGV (0xb)` - Segmentation fault
- **Location**: `~BufferBlob::yuhInterpreter`
- **Root Cause**: Invalid memory access in method resolution stub

## Crash Details

### Signal Information
```
SIGSEGV (0xb) at pc=0x0000000105be2c90, pid=23803, tid=4867
si_addr=0x0000000a0275e2f0 (invalid memory address)
```

### Faulting Instruction
```assembly
0x105be2c90: ldr    d0, [x2, #0x58]
```

### Call Stack Context
```
V  [libjvm.dylib+0x6e1d84]  InterpreterRuntime::resolve_invoke(JavaThread*, Bytecodes::Code)+0x4e4
v  ~BufferBlob::yuhInterpreter
v  ~BufferBlob::yuhInterpreter
v  ~BufferBlob::yuhInterpreter
v  ~BufferBlob::yuhInterpreter
v  ~BufferBlob::yuhInterpreter
```

## Root Cause Analysis

### Instruction Decode
- **Instruction**: `ldr d0, [x2, #0x58]`
- **Type**: Load double-precision floating point from memory
- **Address Calculation**: `x2 + 0x58 = 0x0000000a0275e2f0` (invalid address)
- **Register x2**: `0x0000000a0275e298` (invalid pointer)

### Offset Calculation
The offset `0x58` (88 decimal) is correct for:
- **`ConstantPoolCache::base_offset()`** = sizeof(ConstantPoolCache) = 72 bytes (0x48)
- **`+ ConstantPoolCacheEntry::f2_offset()`** = 16 bytes (0x10)
- **Total = 72 + 16 = 88 bytes = 0x58**

### The Real Problem
1. **Wrong Register**: The instruction uses `x2` but should use `x0` (from previous lea instruction)
2. **Wrong Data Type**: Loading `d0` (double float) instead of `x0` (pointer)
3. **Invalid Cache Entry**: The constant pool cache entry contains garbage data

### Code Location
**File**: `hotspot/src/cpu/aarch64/vm/yuhu/yuhu_templateTable_aarch64.cpp`  
**Method**: `load_invoke_cp_cache_entry()`  
**Line**: 3642

```cpp
__ write_inst_ldr(method, YuhuAddress(cache, method_offset));
```

The issue is that this line generates the wrong instruction pattern.

## Impact
- **Severity**: Critical - JVM crashes during method resolution
- **Affected Operations**: All method calls that require resolution
- **Symptoms**: SIGSEGV with invalid memory access during invoke operations

## Fix Required
The method resolution stub needs to:
1. **Use correct register** (`x0` instead of `x2`)
2. **Load correct data type** (pointer instead of double float)
3. **Ensure cache entry is properly initialized** before access

## Related Issues
- This is the third SIGSEGV in the YuhuInterpreter debugging session
- Previous issues: ADRP encoding, register misuse, bitfield operations
- All issues stem from incorrect instruction generation in YuhuInterpreter

## Status
- **Identified**: ✅ Root cause found
- **Fixed**: ✅ Fix implemented
- **Tested**: ❌ Not yet tested

## Fix Applied
**File**: `hotspot/src/cpu/aarch64/vm/yuhu/yuhu_templateTable_aarch64.cpp`  
**Method**: `YuhuTemplateTable::ldc2_w()`  
**Line**: 268

**Before (Buggy)**:
```cpp
__ write_insts_lea(__ x2, YuhuAddress(__ x1, __ x1, YuhuAddress::lsl(3)));
```

**After (Fixed)**:
```cpp
__ write_insts_lea(__ x2, YuhuAddress(__ x1, __ x0, YuhuAddress::lsl(3)));
```

**Explanation**: The bug was using `__ x1` twice instead of `__ x1` and `__ x0`, causing incorrect address calculation (`x2 = x1*9` instead of `x2 = x1 + x0*8`).

## Register Analysis

### What Each Register Holds in `ldc2_w()`:

**Line 256**: `__ write_insts_get_unsigned_2_byte_index_at_bcp(__ w0, 1);`
- **`x0`** = **Constant Pool Index** (2-byte index from bytecode)
- Loads the index of the constant we want to access from the constant pool

**Line 258**: `__ write_insts_get_cpool_and_tags(__ x1, __ x2);`
- **`x1`** = **Constant Pool Pointer** (address of the constant pool)
- **`x2`** = **Tags Array Pointer** (address of the tags array)

### Why We Need `x0` (Index) Instead of `x1` (Constant Pool):

The address calculation for accessing a constant pool entry is:
```
Address = ConstantPool + (index * sizeof(entry))
```

Since each constant pool entry is 8 bytes (64-bit), we multiply the index by 8:
```cpp
__ write_insts_lea(__ x2, YuhuAddress(__ x1, __ x0, YuhuAddress::lsl(3)));
```

This generates: `add x2, x1, x0, lsl #3` → `x2 = x1 + x0*8`

**Where:**
- `x1` = Constant Pool base address
- `x0` = Index of the entry we want (0, 1, 2, 3, ...)
- `lsl #3` = Multiply by 8 (since each constant pool entry is 8 bytes)

### Example Calculation:

If we want constant pool entry #3:
- `x1` = 0x1000 (constant pool base)
- `x0` = 3 (index)
- **Correct calculation**: `x2 = 0x1000 + 3*8 = 0x1018`
- **Buggy calculation**: `x2 = 0x1000 + 0x1000*8 = 0x9000` (completely wrong address!)

The bug used the constant pool base address twice instead of using the base address and the index, resulting in a massive address miscalculation that pointed to invalid memory.

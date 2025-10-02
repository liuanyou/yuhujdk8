# SIGSEGV in LinearSwitch - Invalid Address Calculation

**Date**: 2025-10-01  
**Issue**: Fourth SIGSEGV crash in YuhuInterpreter debugging session  
**Signal**: SIGSEGV (0xb)  
**PC**: 0x00000001061c3ba0  
**Fault Address**: 0x000000091d1fb6c4  

## Crash Details

### Signal Information
```
SIGSEGV (0xb) at pc=0x00000001061c3ba0, pid=62939, tid=6659
si_addr=0x000000091d1fb6c4
```

### Faulting Instruction
```assembly
0x1061c3ba0: ldr    w8, [x8, #0x8]     ; CRASH HERE
0x1061c3ba4: cmp    w0, w8
0x1061c3ba8: b.eq   0x1061c3bbc
0x1061c3bac: subs   x1, x1, #0x1
0x1061c3bb0: b.pl   0x1061c3b9c
```

### Register State
- **x8**: 0x000000091d1fb6bc (invalid base address)
- **x19**: 0x000000011d1fb6b4 (bytecode pointer - aligned)
- **x1**: 0x0000000000000055 (counter/index)
- **x21**: 0x0000000105818ed8 (dispatch table - valid)
- **x22**: 0x000000011d1fb6b3 (bytecode pointer)

## Root Cause Analysis

### Location
**File**: `hotspot/src/cpu/aarch64/vm/yuhu/yuhu_templateTable_aarch64.cpp`  
**Method**: `YuhuTemplateTable::fast_linearswitch()`  
**Line**: 3187

### The Bug
```cpp
// Line 3187: Load switch table counter - ROOT CAUSE
__ write_inst_ldr(__ x1, YuhuAddress(__ x19, BytesPerInt));  // WRONG: loads 8 bytes
```

### Register Width Issue
The bug was loading **8 bytes (64-bit)** into `x1` instead of **4 bytes (32-bit)** into `w1`:

**Generated Instruction**: `ldr x1, [x19, #4]` (8-byte load)
**Should be**: `ldr w1, [x19, #4]` (4-byte load)

### Memory Analysis
**Switch Table Memory Layout**:
```
[x19 + 0]: 0x2b000000 (default offset - 43 decimal)
[x19 + 4]: 0x02000000 (num pairs - 2 decimal) ← Counter should be 2
[x19 + 8]: 0x01000000 (first key - 1 decimal)
[x19 + 12]: 0x19000000 (first offset - 25 decimal)
```

**What Actually Happened**:
- **Correct counter**: `0x02000000` (2 decimal)
- **Wrong load**: `0x0100000002000000` (8 bytes: counter + next value)
- **Invalid calculation**: `x8 = x19 + 0x0100000002000000 * 8` = invalid address

## Call Chain
```
InterpreterRuntime::resolve_invoke
├── LinkResolver::resolve_invoke
├── LinkResolver::resolve_invokestatic  
├── LinkResolver::resolve_static_call
├── InstanceKlass::initialize
└── YuhuInterpreter::fast_linearswitch()
    ├── Calculate aligned bytecode pointer (x19)
    ├── Load switch table counter (x1)
    ├── Search loop:
    │   ├── Calculate table entry address: x8 = x19 + x1 * 8
    │   ├── Load value: ldr w8, [x8, #8]  ← CRASH HERE
    │   ├── Compare values
    │   └── Decrement counter and loop
    └── Handle found/default cases
```

## Debugging Process

### Memory Analysis Results
**lldb Commands Used**:
```bash
(lldb) memory read --format x --size 4 $x19
0x11b34b6b4: 0x2b000000 0x02000000 0x01000000 0x19000000
0x11b34b6c4: 0x08000000 0x22000000 0xb30074b2 0x00a70057

(lldb) memory read --format x --size 4 $x22  
0x11b34b6b3: 0x000000e3 0x0000002b 0x00000002 0x00000001
```

### Register Analysis Results
- **x22**: 0x000000011b34b6b3 (bytecode pointer - valid)
- **x19**: 0x000000011b34b6b4 (aligned pointer - valid)
- **x1**: 0x0100000002000000 (corrupted counter - WRONG!)
- **x0**: 0x0000000008000000 (switch value - valid)

### Key Discovery
The switch table counter should be **2** (0x02000000), but loading 8 bytes instead of 4 bytes resulted in `0x0100000002000000`, combining the counter with the next memory value.

## Status
- **Identified**: ✅ Root cause location found
- **Analyzed**: ✅ Register width bug identified  
- **Fixed**: ✅ Fix implemented
- **Tested**: ❌ Not yet tested

## Fix Applied
**File**: `hotspot/src/cpu/aarch64/vm/yuhu/yuhu_templateTable_aarch64.cpp`  
**Method**: `YuhuTemplateTable::fast_linearswitch()`  
**Line**: 3187

**Before (Buggy)**:
```cpp
__ write_inst_ldr(__ x1, YuhuAddress(__ x19, BytesPerInt));
```

**After (Fixed)**:
```cpp
__ write_inst_ldr(__ w1, YuhuAddress(__ x19, BytesPerInt));
```

**Explanation**: The bug was loading 8 bytes (64-bit) into `x1` instead of 4 bytes (32-bit) into `w1`. This caused the switch table counter to be combined with the next memory value, resulting in a huge invalid counter value `0x0100000002000000` instead of the correct value `0x02000000` (2 decimal).

## Technical Details

### ARM64 Register Width Confusion
This bug highlights a common ARM64 programming error:
- **x1**: 64-bit register (8 bytes)
- **w1**: 32-bit register (4 bytes) - lower 32 bits of x1
- **Java bytecode**: Uses 32-bit values for most data structures

### Switch Table Format (Java Bytecode)
```
lookupswitch instruction format:
- opcode (1 byte): 0xe3
- padding (0-3 bytes): align to 4-byte boundary  
- default_offset (4 bytes): signed offset for default case
- npairs (4 bytes): number of key-value pairs
- pairs: [key (4 bytes), offset (4 bytes)] × npairs
```

### Why This Bug Occurred
The YuhuInterpreter code generation incorrectly assumed that loading into a 64-bit register would automatically handle 32-bit values, but ARM64's load instructions have different behaviors:
- `ldr x1, [addr]`: Loads 8 bytes from memory
- `ldr w1, [addr]`: Loads 4 bytes from memory into lower 32 bits of x1

## Related Issues
- This is the fourth SIGSEGV in the YuhuInterpreter debugging session
- Previous issues: ADRP encoding, register misuse, bitfield operations, constant pool access
- All issues stem from incorrect instruction generation or memory access in YuhuInterpreter
- Pattern: ARM64-specific register width and addressing mode issues

## Lessons Learned
1. **Always use correct register widths** for data sizes
2. **Test with real bytecode** to catch format mismatches
3. **ARM64 register semantics** require careful attention to x vs w registers
4. **Memory layout verification** is crucial for bytecode interpreters

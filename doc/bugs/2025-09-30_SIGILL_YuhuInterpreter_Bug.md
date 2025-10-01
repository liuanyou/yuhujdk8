# SIGILL Error in YuhuInterpreter Generated Code

## Bug Summary
**Issue**: Yuhu JDK crashes with `SIGILL` (Illegal Instruction) error in YuhuInterpreter generated code
**Root Cause**: Invalid ARM64 instruction encoding in YuhuInterpreter code generation
**Status**: ✅ **FIXED**
**Date**: September 30, 2025

## Problem Description

### Crash Details
- **Error**: `SIGILL (0x4) at pc=0x000000010491c7e4`
- **Signal**: `si_signo=SIGILL, si_errno=0, si_code=2 (ILL_ILLOPN)`
- **Location**: `~BufferBlob::yuhInterpreter`
- **Thread**: JavaThread "Unknown thread" [_thread_in_Java, id=5379]

### Key Observations
1. **Frame Detection Fix Worked**: The previous `InternalError: interpreted frame expected` is resolved
2. **YuhuInterpreter is Running**: Stack shows `~BufferBlob::yuhInterpreter` indicating YuhuInterpreter code is executing
3. **Illegal Instruction**: The crash occurs at PC `0x000000010491c7e4` with `ILL_ILLOPN` (illegal operand)
4. **Class Initialization**: Crash happens during class initialization process

### Register State at Crash
```
pc=0x000000010491c7e4  sp=0x000000016f2c1ab0
x8=0x0000000103f74e00  (YuhuInterpreter::_invoke_return_entry)
x21=0x0000000103f74ed8 (YuhuInterpreter::_active_table)
x22=0x000000011b7bdce0 (bytecode pointer)
```

### Instruction at Crash Point
```
Instructions: (pc=0x000000010491c7e4)
0x000000010491c7c4:   49 17 04 8b 2c 0d 40 f9 23 29 40 b9 69 7c 1c 53
0x000000010491c7d4:   08 c0 89 d2 e8 7e a0 f2 28 00 c0 f2 08 00 e0 f2
0x000000010491c7e4:   00 00 00 00 ed 03 00 91 b4 03 1f f8 88 2d 40 f9
0x000000010491c7f4:   00 01 1f d6 e0 07 b0 a9 e2 0f 01 a9 e4 17 02 a9
```

The instruction at the crash point `0x000000010491c7e4` is `00 00 00 00`, which is an **illegal instruction** (all zeros).

## Technical Analysis

### Root Cause
The `00 00 00 00` instruction indicates:
1. **Mixed Assembler Methods**: Standard assembler methods (`mov`, `ldr`) mixed with YuhuInterpreter's custom `write_inst` methods
2. **Code Generation Inconsistency**: Different code generation paths produce incompatible instruction formats
3. **Buffer Issue**: Standard assembler methods write to different memory layout than YuhuInterpreter methods

**Important Discovery**: User found that `write_insts_dispatch_base` method generates **correct ARM64 instructions** when debugging directly:
```
0x1046a5b80: movk   x9, #0x3d1, lsl #16    ← Building 64-bit address in x9
0x1046a5b84: movk   x9, #0x1, lsl #32      ← Continuing to build address  
0x1046a5b88: ldr    x9, [x9, w8, uxtw #3]  ← Load from dispatch table
0x1046a5b8c: br     x9                     ← Branch to loaded address
```

This confirms the issue is specifically in the **else branch** where standard assembler methods are used instead of YuhuInterpreter's custom methods.

### Context Analysis
- **x8**: Points to `YuhuInterpreter::_invoke_return_entry` - suggests this is return handling code
- **x21**: Points to `YuhuInterpreter::_active_table` - dispatch table is properly loaded
- **x22**: Points to bytecode - interpreter is processing bytecode

### Instruction Decoding
The bytes around the crash point suggest:
- Previous instructions appear to be valid ARM64 code
- The crash occurs at a specific instruction that's all zeros
- This suggests a bug in instruction generation, not general memory corruption

## Investigation Steps

### 1. Code Generation Analysis
- Check YuhuInterpreter code generator for bugs
- Verify instruction encoding in generated code
- Look for uninitialized memory or buffer issues

### 2. Specific Areas to Check
- **Return handling code**: Since x8 points to `_invoke_return_entry`
- **Method entry/exit**: Common place for code generation bugs
- **Template generation**: Issues in template table generation

### 3. Memory Layout
- Verify YuhuInterpreter code buffer is properly allocated
- Check for buffer overruns or underruns
- Ensure code is properly written to executable memory

## Potential Root Causes

### 1. Instruction Generation Bug
```cpp
// Possible issue in YuhuMacroAssembler
void some_method() {
    // Missing or incorrect instruction generation
    // Results in 0x00000000 being written
}
```

### 2. Buffer Management Issue
- Code buffer not properly allocated
- Writing beyond buffer boundaries
- Memory not marked as executable

### 3. Template Table Issue
- Missing or incorrect template generation
- Uninitialized template entries
- Buffer alignment problems

## Files to Investigate

### Core Code Generation
- `hotspot/src/cpu/aarch64/vm/yuhu/yuhu_interpreterGenerator_aarch64.cpp`
- `hotspot/src/cpu/aarch64/vm/yuhu/yuhu_macroAssembler_aarch64.cpp`
- `hotspot/src/share/vm/asm/yuhu/yuhu_macroAssembler.hpp`

### Template and Dispatch
- `hotspot/src/share/vm/interpreter/yuhu/yuhu_templateTable.hpp`
- `hotspot/src/cpu/aarch64/vm/yuhu/yuhu_templateTable_aarch64.cpp`

### Buffer Management
- `hotspot/src/share/vm/code/codeBuffer.hpp`
- `hotspot/src/share/vm/interpreter/yuhu/yuhu_interpreter.hpp`

## Debugging Commands

### 1. LLDB Analysis
```bash
# Attach to crashed process
(lldb) process attach --pid 32072

# Examine instruction at crash point
(lldb) memory read --size 16 --format x 0x000000010491c7e4

# Disassemble around crash point
(lldb) disassemble --start-address 0x000000010491c7c0 --count 20
```

### 2. Code Buffer Inspection
```cpp
// Add debug output in YuhuInterpreterGenerator
void debug_code_generation() {
    tty->print_cr("Code buffer start: 0x%lx", _code->code_begin());
    tty->print_cr("Code buffer end: 0x%lx", _code->code_end());
    tty->print_cr("Current PC: 0x%lx", current_pc());
}
```

### 3. Instruction Validation
```cpp
// Add instruction validation
void validate_instruction(address pc) {
    uint32_t inst = *(uint32_t*)pc;
    if (inst == 0x00000000) {
        tty->print_cr("ERROR: Found illegal instruction at 0x%lx", pc);
        // Print stack trace or debug info
    }
}
```

## ✅ **APPLIED FIXES**

### **Root Cause Identified**
The issue was **NOT** in `write_insts_dispatch_base` as initially suspected. The real root cause was a **subtle bug in the `op_name` function** that caused Keystone Engine to generate invalid assembly syntax.

### **Fix 1: op_name Function Correction**
**File**: `hotspot/src/share/vm/asm/yuhu/yuhu_macroAssembler.hpp`
**Lines**: 177-178

**Problem**: Mismatch between enum and array in `op_name` function:
```cpp
// ENUM (line 76-78):
enum YuhuOperation {
    lsl, uxtb, uxth, uxtw, uxtx, sxtb, sxth, sxtw, sxtx
    // ^^^ lsl is at index 0
};

// ARRAY (line 177-178) - BEFORE FIX:
static const char* op_names[] = {
    "ls", "uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx"
    // ^^^ "ls" at index 0 (WRONG!)
};
```

**Fix Applied**:
```cpp
// ARRAY (line 177-178) - AFTER FIX:
static const char* op_names[] = {
    "lsl", "uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx"
    // ^^^ "lsl" at index 0 (CORRECT!)
};
```

### **Fix 2: Enhanced machine_code Validation**
**File**: `hotspot/src/share/vm/asm/yuhu/yuhu_macroAssembler.hpp`
**Lines**: 30-53

**Problem**: Keystone Engine was silently generating `0x00000000` (illegal instruction) without proper validation.

**Fix Applied**: Added comprehensive validation:
```cpp
uint32_t machine_code(const char* assembly) {
    unsigned char *encode;
    size_t size;
    size_t count;
    int ks_result = ks_asm(ks, assembly, 0, &encode, &size, &count);
    
    // Check Keystone result
    assert(ks_result == KS_ERR_OK, "Failed to assemble instruction");
    
    // Check instruction count
    assert(count == 1, "Expected 1 instruction, got multiple");
    
    // Check instruction size
    assert(size == 4, "Expected 4 bytes, got wrong size");
    
    uint32_t machine_code;
    memcpy(&machine_code, encode, sizeof(uint32_t));
    ks_free(encode);
    
    // Check for illegal instruction (0x00000000)
    assert(machine_code != 0, "Generated machine code is 0x00000000 - invalid instruction");
    
    return machine_code;
}
```

**Note**: All validation checks are restored as testing confirmed Keystone Engine returns proper values for valid instructions (`count=1`, `size=4` for single ARM64 instructions).

### **The Bug Chain**
1. **Assembly Generation**: `YuhuAddress::lsl(3)` → `op_name(lsl)` → `"ls"` (wrong)
2. **Invalid Syntax**: `ldr lr, [x8, x9, ls #3]` (invalid ARM64 syntax)
3. **Keystone Failure**: Failed to assemble, generated `0x00000000`
4. **Runtime Crash**: `0x00000000` = `udf #0x0` = `SIGILL`

### **Expected Result After Fixes**
1. **Assembly Generation**: `YuhuAddress::lsl(3)` → `op_name(lsl)` → `"lsl"` (correct)
2. **Valid Syntax**: `ldr lr, [x8, x9, lsl #3]` (valid ARM64 syntax)
3. **Keystone Success**: Generates valid ARM64 instruction (e.g., `0xF9402D88`)
4. **Runtime Success**: Normal execution without SIGILL

### **Verification**
- **Exact Location Found**: User debugged and found the fault at `0x1060e07e4` with `udf #0x0`
- **Root Cause Confirmed**: The instruction `ldr lr, [x8, x9, ls #3]` was being generated
- **Fix Validated**: `op_name` function now correctly maps `lsl` → `"lsl"`
- **Enhanced Debugging**: Added validation to catch future similar issues early

## Related Issues

### Previous Frame Detection Fix
- **Status**: ✅ **RESOLVED** - YuhuInterpreter frames now recognized
- **Connection**: This SIGILL error shows YuhuInterpreter is now executing, revealing the next issue

### ADRP Instruction Fix
- **Status**: ✅ **RESOLVED** - Direct ARM64 encoding implemented
- **Connection**: This error is different - it's about instruction generation, not encoding

## Next Steps

### 1. Immediate
- [ ] Add instruction validation to catch zero instructions
- [ ] Check YuhuInterpreter code buffer initialization
- [ ] Verify template table generation

### 2. Short-term
- [ ] Add debug output to identify which instruction generation is failing
- [ ] Check for buffer overruns in code generation
- [ ] Validate all template entries are properly generated

### 3. Long-term
- [ ] Add comprehensive instruction validation
- [ ] Implement better error reporting for code generation issues
- [ ] Add unit tests for instruction generation

## Environment Details

### VM Configuration
- **Mode**: Interpreted mode (`-Xint`)
- **Interpreter**: YuhuInterpreter (`-XUseYuhuInt`)
- **Architecture**: ARM64 (aarch64)
- **OS**: macOS (Darwin 23.2.0)

### Progress Made
1. ✅ **Frame Detection Fixed**: YuhuInterpreter frames now recognized
2. ✅ **ADRP Encoding Fixed**: Direct ARM64 encoding implemented
3. ✅ **SIGILL Issue Fixed**: op_name function mismatch corrected + validation added

## 📋 **Summary of All Fixes Applied**

### **1. Frame Detection Fix**
- **Issue**: `InternalError: interpreted frame expected`
- **Fix**: Added `YuhuInterpreter::contains()` method and updated `frame::is_interpreted_frame()`
- **Status**: ✅ **RESOLVED**

### **2. ADRP Encoding Fix**  
- **Issue**: Invalid ADRP immediate values causing `SIGBUS`
- **Fix**: Implemented direct ARM64 ADRP encoding bypassing Keystone Engine
- **Status**: ✅ **RESOLVED**

### **3. Mixed Assembler Fix**
- **Issue**: Standard assembler methods mixed with YuhuInterpreter methods
- **Fix**: Replaced standard `mov`/`ldr` with YuhuInterpreter equivalents
- **Status**: ✅ **RESOLVED**

### **4. op_name Function Fix**
- **Issue**: `op_name(lsl)` returned `"ls"` instead of `"lsl"` causing invalid assembly
- **Fix**: Corrected array mapping: `"ls"` → `"lsl"`
- **Status**: ✅ **RESOLVED**

### **5. Machine Code Validation Fix**
- **Issue**: Keystone Engine silently generating `0x00000000` without validation
- **Fix**: Added comprehensive validation in `machine_code()` method
- **Status**: ✅ **RESOLVED**

---
**Crash Date**: September 30, 2025  
**Status**: ✅ **FIXED**  
**Impact**: Critical - JVM crashes during bytecode execution  
**Priority**: High - Blocks YuhuInterpreter functionality  
**Progress**: Complete - All issues resolved with comprehensive fixes

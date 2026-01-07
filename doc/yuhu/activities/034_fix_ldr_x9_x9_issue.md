# 034: Fix ldr x9, [x9] Issue in Method Call Generation

## Problem Summary

Yuhu compiler generates problematic assembly code sequence `mov x9, #0x28` followed by `ldr x9, [x9]` which treats a small integer constant as a memory address to load from, causing crashes when the address is invalid.

### Assembly Example

```
0x000000010ac4d1c0: mov w9, #0x28                   	// #40
0x000000010ac4d1c4: ldr x9, [x9]
0x000000010ac4d1c8: ldr x9, [x9, #72]   ; OopMap{...}
```

### Error Log Example

```
#
# A fatal error has been detected by the Java Runtime Environment:
#
#  SIGSEGV (0xb) at pc=0x000000010c8e9744, pid=89473, tid=5123
#
# JRE version: OpenJDK Runtime Environment (8.0) (build 1.8.0-internal-debug-liuanyou_2025_12_21_08_54-b00)
# Java VM: OpenJDK 64-Bit Server VM (25.0-b70-debug mixed mode bsd- compressed oops)
# Problematic frame:
# J 331 yuhu sun.nio.cs.UTF_8$Encoder.encodeArrayLoop(Ljava/nio/CharBuffer;Ljava/nio/ByteBuffer;)Ljava/nio/charset/CoderResult; (489 bytes) @ 0x000000010c8e9744 [0x000000010c8e9680+0xc4]
#
# Failed to write core dump. Core dumps have been disabled. To enable core dumping, try "ulimit -c unlimited" before starting Java again
#
# If you would like to submit a bug report, please visit:
#   http://bugreport.sun.com/bugreport/crash.jsp
#

---------------  T H R E A D  ---------------

Current thread (0x0000000129016800):  JavaThread "main" [_thread_in_Java, id=5123, stack(0x000000016d9a0000,0x000000016dba3000)]

siginfo:si_signo=SIGSEGV: si_errno=0, si_code=2 (SEGV_ACCERR), si_addr=0x0000000000000028

Thread (from x28): 0x0000000129016800
  JavaThread "main"
  last_Java_pc = 0x000000010c8e972c
  CodeBlob: nmethod   4089  331       6       sun.nio.cs.UTF_8$Encoder::encodeArrayLoop (489 bytes)

  code_begin = 0x000000010c8e9680, code_end = 0x000000010c8ed288
  nmethod:  sun.nio.cs.UTF_8$Encoder::encodeArrayLoop
[error occurred during error reporting (printing registers, top of stack, instructions near pc), id 0xe0000000]
```

### Root Cause

The issue occurs in `get_direct_callee()` method which uses `CreateInlineMetadata()` incorrectly:

```cpp
Value *YuhuTopLevelBlock::get_direct_callee(ciMethod* method) {
  return builder()->CreateBitCast(
    builder()->CreateInlineMetadata(method, YuhuType::Method_type()),
                                    YuhuType::Method_type(),
                                    "callee");
}
```

The `CreateInlineMetadata` implementation generates:
```llvm
%44 = load ptr, ptr inttoptr (i64 40 to ptr), align 8    ; Load from address 40 (0x28)
```

This treats the metadata reference as a **slot address to load from**, not as a **direct method pointer**.

## Technical Analysis

### What Should Happen

For direct method calls, the compiler should generate:
```llvm
%method_ptr = inttoptr i64 <actual_method_address> to ptr    ; Direct method address
%from_compiled_entry = load i64, ptr %method_ptr, align 8   ; Load _from_compiled_entry
```

### What Actually Happens

The current code generates:
```llvm
%slot_addr = inttoptr i64 40 to ptr                         ; Address 40 as slot
%method_ptr = load ptr, ptr %slot_addr, align 8             ; Load from slot 40
%from_compiled_entry = load i64, ptr %method_ptr, align 8   ; Load from method
```

This creates the problematic pattern where small constants (40=0x28, 56=0x38, 72=0x48) are treated as absolute addresses.

### Affected Patterns

The assembly shows multiple instances:
- `mov w9, #0x28` → `ldr x9, [x9]` → `ldr x9, [x9, #72]` (address 40)
- `mov w12, #0x38` → `ldr x12, [x12]` → `ldr x12, [x12, #72]` (address 56) 
- `mov w12, #0x48` → `ldr x12, [x12]` → `ldr x12, [x12, #72]` (address 72)

## Solution Approach

### Option 1: Fix CreateInlineMetadata Implementation (Temporary Fix)

Ensure `CreateInlineMetadata` returns the **direct method pointer** instead of a **slot address**:

```cpp
Value *YuhuTopLevelBlock::get_direct_callee(ciMethod* method) {
  // Get direct method address, not load from slot
  return builder()->CreateInlineMetadata(method, YuhuType::Method_type());
}
```

The `CreateInlineMetadata` should embed the actual `Method*` address directly, not a slot number.

**Issue with this approach**: The temporary code buffer is released after compilation, but the generated code still references addresses from it. This requires proper relocation mechanism to work correctly.

### Option 2: Proper Slot Management

If slots are intended, implement proper slot allocation and management:
- Allocate proper memory slots for method metadata
- Ensure slot addresses are valid memory locations
- Initialize slots with correct method pointers

**Issue with this approach**: The `code_buffer_address()` calculation is problematic when `base_pc` is NULL, resulting in small constants (40, 56, 72) being treated as absolute addresses.

### Option 3: Use C1/C2-style Stub Mechanism (Recommended Long-term Solution)

Implement a stub-based approach similar to C1/C2 compilers:

#### How C1/C2 Handle Direct Calls

1. **Compilation Time**:
   - Generate call to a stub instead of direct Method* access
   - Create relocation information for the call site
   - Emit stub code to Code Cache

2. **Runtime**:
   - Stub handles method lookup and invocation
   - HotSpot patches addresses through relocation mechanism
   - Stub remains at fixed location in Code Cache

#### Implementation Steps

**Step 1: Create Static Call Stub**
```cpp
// Generate call to static call stub instead of inline metadata
Value *YuhuTopLevelBlock::get_direct_callee(ciMethod* method) {
  // Create a call to static call stub
  // The stub will be responsible for:
  // 1. Loading the Method* from a safe location
  // 2. Accessing _from_compiled_entry field
  // 3. Jumping to the actual method
  
  // Generate stub entry point address
  address stub_addr = generate_static_call_stub(method);
  return builder()->CreateIntToPtr(
    LLVMValue::intptr_constant((intptr_t)stub_addr),
    YuhuType::Method_type(),
    "static_call_stub");
}
```

**Step 2: Stub Implementation**
```cpp
address YuhuCompiler::generate_static_call_stub(ciMethod* method) {
  // Allocate stub in Code Cache (permanent location)
  CodeBuffer* cb = new CodeBuffer(...);
  MacroAssembler* masm = new MacroAssembler(cb);
  
  // Stub code (AArch64):
  // 1. Load Method* from relocation data
  // 2. Load _from_compiled_entry field
  // 3. Jump to the compiled entry
  
  // Example stub layout:
  // ldr x9, [pc, #offset]     ; Load Method* from constant pool
  // ldr x9, [x9, #72]         ; Load _from_compiled_entry
  // br x9                     ; Jump to compiled code
  // .align 8
  // .quad <Method* address>   ; Method* constant (relocated by HotSpot)
  
  return cb->insts_begin();
}
```

**Step 3: Relocation Support**
```cpp
// Add relocation information for the Method* in stub
void add_metadata_relocation(CodeBuffer* cb, Metadata* method) {
  // Create metadata relocation entry
  // HotSpot will update this when:
  // - Method is moved
  // - Method is recompiled
  // - GC occurs
  RelocIterator::create_metadata_reloc(cb, method);
}
```

#### Advantages of Stub Approach

1. **Address Stability**: Stubs are in Code Cache with fixed locations
2. **Relocation Support**: Proper integration with HotSpot's relocation mechanism
3. **GC Safety**: Method* references properly tracked and updated
4. **Consistency**: Same mechanism as C1/C2, easier to maintain
5. **Deoptimization**: Stubs can be patched for deoptimization

#### Comparison with Current Approach

| Aspect | Current (Inline Metadata) | Stub-based |
|--------|--------------------------|------------|
| Address calculation | Problematic with NULL base_pc | Stable, fixed in Code Cache |
| Relocation | Not properly handled | Full HotSpot support |
| GC safety | Questionable | Properly tracked |
| Code Cache integration | Poor | Excellent |
| Complexity | Lower | Higher (but more robust) |

## Implementation Plan

### Phase 1: Immediate Fix

1. **Review CreateInlineMetadata implementation** in builder
2. **Fix get_direct_callee** to return proper method pointer
3. **Test with UTF_8$Encoder.encodeArrayLoop** to verify fix

### Phase 2: Verification

1. **Compile and test** the problematic method
2. **Verify assembly generation** no longer uses small constants as addresses
3. **Ensure method calls work correctly**

## Modified Files

### 1. `yuhuTopLevelBlock.cpp`
- Update `get_direct_callee()` method
- Ensure proper method pointer handling

### 2. `yuhuBuilder.cpp` (if needed)
- Review and fix `CreateInlineMetadata` implementation
- Ensure it returns direct pointers, not slot addresses

## Testing Strategy

### Test Cases

1. **Direct Method Calls**
   ```java
   static void method() { }
   method();  // Should generate direct call without slot loading
   ```

2. **Virtual Method Calls**
   ```java
   obj.method();  // Should work correctly with vtable lookup
   ```

3. **Complex Scenarios**
   - Static method calls
   - Private method calls  
   - Constructor calls

### Verification

1. **No Small Address Loading**: Assembly should not contain `mov reg, #small_const` → `ldr reg, [reg]`
2. **Correct Method Calls**: Methods execute without crashes
3. **Proper Entry Points**: `_from_compiled_entry` accessed correctly

## Impact

### Positive
- Fixes critical crash in method call generation
- Eliminates dangerous small-constant address loading
- Makes Yuhu more robust for real Java code

### Negative
- None expected - this is a correctness fix

## Priority

**CRITICAL** - This bug causes crashes in method call generation and makes Yuhu unusable for Java programs with method calls.

## Related Work

- **033**: Enhanced null checks (complementary fix)
- **UTF_8$Encoder.encodeArrayLoop**: Primary test case
- **CreateInlineMetadata**: Core implementation to review

## Timeline

- **Phase 1**: 1 day (implementation and basic testing)
- **Phase 2**: 1 day (verification with complex cases)

## Conclusion

The `ldr x9, [x9]` issue stems from incorrect metadata handling where small slot numbers are treated as absolute addresses. The fix involves ensuring direct method references are embedded properly rather than loading from small-numbered slots.
# 033: Fix Missing Implicit Null Checks in Field Access

## Problem Summary

Yuhu compiler generates code that directly accesses object fields without proper null pointer checking, causing segmentation faults instead of throwing `NullPointerException` as required by Java semantics.

### Crash Example

```
SIGSEGV (0xb) at pc=0x000000010c8e9744, pid=89473, tid=5123
si_addr=0x0000000000000028, si_code=2 (SEGV_ACCERR)

Problematic frame:
J 331 yuhu sun.nio.cs.UTF_8$Encoder.encodeArrayLoop(...) @ 0x000000010c8e9744
```

### Root Cause

The generated LLVM IR shows direct field access without null checking:

```llvm
%135 = load ptr, ptr %134, align 8      ; Load object reference (could be NULL)
%149 = getelementptr i8, ptr %135, i64 28  ; ← CRASH if %135 is NULL!
%150 = load i32, ptr %149, align 4      ; Access field at offset 28
```

## Java Semantics vs Current Implementation

### Java Language Specification

In Java, accessing a field on a null object should throw `NullPointerException`:

```java
Object obj = someMethod();  // Could return null
int value = obj.field;      // Should throw NPE if obj is null, not crash
```

### Current Yuhu Behavior

- Generates direct memory access: `obj.field` → `load ptr [obj + offset]`
- If `obj` is null, accesses address `0 + offset` → SIGSEGV
- No exception handling → Process crashes

### Expected Behavior

- Should throw `NullPointerException` when accessing field on null object
- Should not crash the JVM process
- Should match C1/C2 compiler behavior

## Solution Approaches

### Option 1: Explicit Null Checks (Recommended for Immediate Fix)

Generate explicit null check before field access:

#### Before (Current):
```cpp
// Direct field access - unsafe
Value* field_addr = builder()->CreateGEP(obj_ptr, field_offset);
Value* field_val = builder()->CreateLoad(field_addr);
```

#### After (Safe):
```cpp
// Explicit null check
Value* is_null = builder()->CreateICmpEQ(obj_ptr, LLVMValue::null_ptr());
BasicBlock* null_check_ok = function()->CreateBlock("null_check_ok");
BasicBlock* throw_npe = function()->CreateBlock("throw_npe");

builder()->CreateCondBr(is_null, throw_npe, null_check_ok);

// Throw NPE block
builder()->SetInsertPoint(throw_npe);
builder()->CreateCall(throw_npe_runtime_func, {thread()});
builder()->CreateUnreachable();

// Safe access block  
builder()->SetInsertPoint(null_check_ok);
Value* field_addr = builder()->CreateGEP(obj_ptr, field_offset);
Value* field_val = builder()->CreateLoad(field_addr);
```

### Option 2: Implicit Null Checks (Long-term Solution)

Implement HotSpot-style implicit null checks:

1. **Signal Handler Integration**: Configure OS signal handler to catch SIGSEGV
2. **OopMap Registration**: Register field access sites as potential null check locations  
3. **Exception Stubs**: Generate NPE throwing stubs for signal handler to jump to

## Implementation Plan

### Phase 1: Quick Fix (Explicit Checks)

#### 1. Update Field Access Code Generation

Modify `yuhuBlock.cpp` and `yuhuTopLevelBlock.cpp` to insert explicit null checks for:
- `getfield` bytecode
- `putfield` bytecode  
- Array access operations (`aaload`, `iaload`, etc.)

#### 2. Helper Function for Null Checks

```cpp
void YuhuBlock::insert_implicit_null_check(Value* obj_ptr) {
  Value* is_null = builder()->CreateICmpEQ(obj_ptr, LLVMValue::null_ptr());
  BasicBlock* continue_block = function()->CreateBlock("null_check_continue");
  BasicBlock* throw_block = function()->CreateBlock("throw_npe");
  
  builder()->CreateCondBr(is_null, throw_block, continue_block);
  
  // Setup throw block
  builder()->SetInsertPoint(throw_block);
  builder()->CreateCall(throw_npe_runtime_func, {thread()});
  builder()->CreateUnreachable();
  
  // Continue with field access
  builder()->SetInsertPoint(continue_block);
}
```

#### 3. Apply to All Field Access Operations

- **getfield**: Check receiver before field load
- **putfield**: Check receiver before field store  
- **array access**: Check array reference before element access
- **method calls**: Check receiver before call (already implemented in recent fix)

### Phase 2: Performance Optimization

1. **Null Check Elimination**: Use data flow analysis to eliminate redundant checks
2. **Check Merging**: Merge multiple checks on the same object
3. **Branch Prediction**: Use LLVM metadata for better branch prediction

### Phase 3: Implicit Null Checks (Optional)

For future optimization, implement C2-style implicit null checks:
- Register field access PCs in OopMap
- Integrate with HotSpot signal handling
- Generate exception stubs

## Modified Files

### 1. `yuhuBlock.cpp`
- Add null check insertion for `getfield`/`putfield` operations
- Update field access code generation

### 2. `yuhuTopLevelBlock.cpp` 
- Add null check insertion for array access operations
- Update array access code generation

### 3. `yuhuBuilder.cpp`
- Add helper methods for null check generation

## Testing Strategy

### Test Cases

1. **Simple Field Access**
   ```java
   class Test { int field; }
   Test obj = null;
   int x = obj.field;  // Should throw NPE
   ```

2. **Method Chain with Null**
   ```java
   obj.method().field;  // Should throw NPE on first access
   ```

3. **Array Access with Null**
   ```java
   int[] arr = null;
   int x = arr[0];  // Should throw NPE
   ```

4. **Complex Scenarios**
   - Nested field access
   - Method calls on null objects
   - Array operations on null arrays

### Verification

1. **Crash Prevention**: No more SIGSEGV crashes
2. **Correct Exceptions**: Proper `NullPointerException` thrown
3. **Performance**: Reasonable overhead for explicit checks
4. **Compatibility**: Matches C1/C2 behavior

## Impact

### Positive
- Fixes critical crash bug
- Makes Yuhu compatible with Java semantics
- Enables safe compilation of real-world Java code

### Negative
- Small performance overhead from explicit checks
- Larger generated code size

## Priority

**CRITICAL** - This bug makes Yuhu unusable for any Java code that could encounter null references. Must be implemented before further development.

## Related Work

- **032**: Java method call fixes (similar approach needed)
- **C2 compiler**: Reference implementation for implicit null checks
- **HotSpot signal handling**: Integration point for long-term solution

## Timeline

- **Phase 1**: 1-2 days (explicit checks implementation)
- **Phase 2**: 3-5 days (optimizations)  
- **Phase 3**: Future work (implicit checks)

## Conclusion

The explicit null check approach provides immediate safety with manageable performance overhead. The long-term implicit null check solution can be implemented later for better performance, but this fix is essential for making Yuhu usable.
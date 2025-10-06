# YuhuInterpreter MethodHandle BootstrapMethodError Bug

**Date**: October 6, 2025  
**Severity**: Critical  
**Component**: YuhuInterpreter - Method Handle Support  
**Architecture**: ARM64 (AArch64)  

## Summary

A critical bug in YuhuInterpreter was causing `BootstrapMethodError` with `NullPointerException` when Java applications tried to use method handles, particularly affecting Spring Boot applications that rely on `java.lang.invoke` functionality.

## Root Cause

The issue had two main components:

### 1. Missing Method Handle Adapter Generation
YuhuInterpreter was **not properly generating method handle adapters** during initialization. The issue was that `MethodHandles::generate_adapters()` only sets up adapters for `AbstractInterpreter::_entry_table`, but `YuhuInterpreter` has its own separate `_entry_table` that was never populated with method handle adapters.

### 2. Appendix Loading Bug in prepare_invoke
After fixing the adapter generation, a second bug was discovered in `YuhuTemplateTable::prepare_invoke()` where the appendix loading was incorrect:

```cpp
// BUG: This treats the register 'index' as an immediate value
__ write_insts_mov_imm64(__ x19, index);  // WRONG

// FIX: This correctly copies the register value
__ write_inst_mov_reg(__ x19, index);     // CORRECT
```

This caused the appendix to be loaded with the wrong cache index, resulting in `expected=null` in `Invokers.checkGenericType()`.

### The Complete Problem Chain:
1. **`MethodHandle.invoke()`** gets mapped to `vmIntrinsics::_invokeGeneric`
2. **YuhuInterpreter** initializes method handle entries to abstract method entries
3. **`MethodHandles::generate_adapters()`** only sets `AbstractInterpreter::_entry_table`
4. **`YuhuInterpreter::_entry_table`** remains with abstract method entries
5. **When `MethodHandle.invoke()` is called**, it hits the abstract method entry
6. **Result**: `BootstrapMethodError` with `NullPointerException`

### After First Fix - Second Bug:
1. **Method handle adapters** are now properly generated for YuhuInterpreter
2. **`invokehandle` bytecode** executes correctly
3. **`prepare_invoke`** is called to load appendix
4. **`write_insts_mov_imm64(__ x19, index)`** treats register as immediate
5. **Wrong cache index** is used to load appendix
6. **Appendix becomes `null`**, causing `NullPointerException` in `checkGenericType`

## Impact

### Symptoms
- **`BootstrapMethodError`** in Java applications
- **`NullPointerException`** in `Invokers.checkGenericType()` at line 362
- **Spring Boot applications** failing to start
- **Method handle invocations** completely broken
- **Lambda expressions** and **method references** not working

### Affected Code Pattern
Any code using method handles:

```java
// Spring Boot's MultiValueMapAdapter.add() method
// This calls method handles internally for generic type checking
public void add(K key, V value) {
    // Method handle invocation fails here
}

// Lambda expressions
Function<String, Integer> func = s -> s.length();

// Method references  
List<String> list = Arrays.asList("a", "b", "c");
list.forEach(System.out::println);
```

### Error Stack Trace
```
Exception in thread "main" java.lang.reflect.InvocationTargetException
	at sun.reflect.NativeMethodAccessorImpl.invoke0(Native Method)
	at sun.reflect.NativeMethodAccessorImpl.invoke(NativeMethodAccessorImpl.java:62)
	...
Caused by: java.lang.BootstrapMethodError: call site initialization exception
	at java.lang.invoke.CallSite.makeSite(CallSite.java:328)
	at java.lang.invoke.MethodHandleNatives.linkCallSite(MethodHandleNatives.java:296)
	...
Caused by: java.lang.NullPointerException
	at java.lang.invoke.Invokers.checkGenericType(Invokers.java:362)
	at java.lang.invoke.CallSite.makeSite(CallSite.java:289)
	...
```

## Technical Details

### Method Handle Architecture
1. **Signature Polymorphic Methods**: `MethodHandle.invoke()` and `MethodHandle.invokeExact()` are signature polymorphic
2. **Native Method Placeholders**: The native methods throw `UnsupportedOperationException` when called directly
3. **Interpreter Entries**: Real implementation is through interpreter entry points
4. **Adapter Generation**: `MethodHandles::generate_adapters()` creates proper entry points

### The Missing Piece
YuhuInterpreter was missing proper method handle adapter generation. The standard `MethodHandles::generate_adapters()` only populates `AbstractInterpreter::_entry_table`, but `YuhuInterpreter` has its own separate `_entry_table` that needs to be populated separately.

### Architecture Issue
```cpp
// MethodHandlesAdapterGenerator::generate() - Original version
void MethodHandlesAdapterGenerator::generate() {
    // Only sets AbstractInterpreter::_entry_table
    for (Interpreter::MethodKind mk = Interpreter::method_handle_invoke_FIRST;
         mk <= Interpreter::method_handle_invoke_LAST; mk++) {
        // ...
        Interpreter::set_entry_for_kind(mk, entry);  // Only sets AbstractInterpreter
    }
    // YuhuInterpreter::_entry_table remains unchanged!
}
```

## Fix Implementation

### Files Modified
- `hotspot/src/share/vm/interpreter/yuhu/yuhu_interpreter.hpp`
- `hotspot/src/share/vm/interpreter/yuhu/yuhu_interpreter.cpp`
- `hotspot/src/share/vm/prims/methodHandles.cpp`
- `hotspot/src/cpu/aarch64/vm/yuhu/yuhu_templateTable_aarch64.cpp`

### Changes Made

#### Fix 1: Method Handle Adapter Generation

1. **Added Method Handle Intrinsic Conversion Method**:
```cpp
// In yuhu_interpreter.hpp
static vmIntrinsics::ID method_handle_intrinsic(MethodKind kind) {
    if (kind >= method_handle_invoke_FIRST && kind <= method_handle_invoke_LAST)
        return (vmIntrinsics::ID)( vmIntrinsics::FIRST_MH_SIG_POLY + (kind - method_handle_invoke_FIRST) );
    else
        return vmIntrinsics::_none;
}
```

2. **Added set_entry_for_kind Method**:
```cpp
// In yuhu_interpreter.hpp
static void set_entry_for_kind(MethodKind k, address e);

// In yuhu_interpreter.cpp
void YuhuInterpreter::set_entry_for_kind(YuhuInterpreter::MethodKind kind, address entry) {
    assert(kind >= method_handle_invoke_FIRST &&
           kind <= method_handle_invoke_LAST, "late initialization only for MH entry points");
    assert(_entry_table[kind] == _entry_table[abstract], "previous value must be AME entry");
    _entry_table[kind] = entry;
}
```

3. **Enhanced MethodHandlesAdapterGenerator::generate()**:
```cpp
// In methodHandles.cpp
void MethodHandlesAdapterGenerator::generate() {
    // Generate interpreter entries for AbstractInterpreter
    for (Interpreter::MethodKind mk = Interpreter::method_handle_invoke_FIRST;
         mk <= Interpreter::method_handle_invoke_LAST; mk++) {
        // ... existing code ...
        Interpreter::set_entry_for_kind(mk, entry);
    }
    
    // Generate interpreter entries for YuhuInterpreter
    for (YuhuInterpreter::MethodKind mk = YuhuInterpreter::method_handle_invoke_FIRST;
         mk <= YuhuInterpreter::method_handle_invoke_LAST; mk++) {
        vmIntrinsics::ID iid = YuhuInterpreter::method_handle_intrinsic(mk);
        StubCodeMark mark(this, "YuhuMethodHandle::interpreter_entry", vmIntrinsics::name_at(iid));
        address entry = MethodHandles::generate_method_handle_interpreter_entry(_masm, iid);
        if (entry != NULL) {
            YuhuInterpreter::set_entry_for_kind(mk, entry);
        }
    }
}
```

#### Fix 2: Appendix Loading Bug in prepare_invoke

4. **Fixed register usage in prepare_invoke**:
```cpp
// In yuhu_templateTable_aarch64.cpp - prepare_invoke method
// BEFORE (BUG):
__ write_insts_mov_imm64(__ x19, index);  // Treats register as immediate

// AFTER (FIX):
__ write_inst_mov_reg(__ x19, index);     // Correctly copies register value
```

### Key Changes
- **Added dual adapter generation** for both AbstractInterpreter and YuhuInterpreter
- **Added method handle intrinsic conversion** for YuhuInterpreter MethodKind
- **Added set_entry_for_kind method** to YuhuInterpreter
- **Enhanced MethodHandlesAdapterGenerator** to support multiple interpreters
- **Fixed appendix loading** by using correct register-to-register move instruction

### Why This Approach Works
This solution is elegant because:

1. **No Architecture Changes**: YuhuInterpreter remains `AllStatic` and doesn't need to inherit from `AbstractInterpreter`
2. **Centralized Generation**: Method handle adapters are still generated in one place (`MethodHandlesAdapterGenerator::generate()`)
3. **Automatic Support**: Any new interpreter that follows the same pattern can be easily added
4. **Maintains Separation**: Each interpreter keeps its own `_entry_table` without sharing state
5. **Reuses Existing Logic**: The same `MethodHandles::generate_method_handle_interpreter_entry()` is used for both interpreters

## Testing

### Test Case
```java
import java.util.function.Function;
import java.util.Arrays;
import java.util.List;

public class MethodHandleTest {
    public static void main(String[] args) {
        try {
            // Test lambda expressions
            Function<String, Integer> func = s -> s.length();
            System.out.println("Lambda test: " + func.apply("hello"));
            
            // Test method references
            List<String> list = Arrays.asList("a", "b", "c");
            list.forEach(System.out::println);
            
            System.out.println("MethodHandle test passed!");
        } catch (Exception e) {
            System.out.println("MethodHandle test failed: " + e);
            e.printStackTrace();
        }
    }
}
```

### Verification Commands
```bash
# Test with YuhuInterpreter
java -Xint -XX:+UseYuhuInt -XX:-UseCompressedOops MethodHandleTest

# Compare with standard interpreter
java -Xint -XX:-UseYuhuInt MethodHandleTest

# Test Spring Boot application
java -Xint -XX:+UseYuhuInt -XX:-UseCompressedOops -jar your-spring-boot-app.jar
```

### Debugging Results
After applying both fixes, the appendix loading works correctly:

**Before Fix:**
```
(lldb) register read x0
     x0 = 0x0000000000000017    # Cache index
(lldb) register read x19  
     x19 = 0x0000000000000020   # Wrong value (register enum treated as immediate)
# After load_resolved_reference_at_index:
(lldb) register read x0
     x0 = 0x0000000000000000    # NULL appendix (wrong cache entry)
```

**After Fix:**
```
(lldb) register read x0
     x0 = 0x0000000000000017    # Cache index
(lldb) register read x19
     x19 = 0x0000000000000017   # Correct value (register copied correctly)
# After load_resolved_reference_at_index:
(lldb) register read x0
     x0 = 0x000000076b510810    # Valid appendix address ✅
```

The appendix now contains a valid heap address pointing to the `MethodType` or `CallSite` object, resolving the `NullPointerException` in `Invokers.checkGenericType()`.

## Prevention

### Code Review Guidelines
1. **Ensure method handle support** is properly initialized in custom interpreters
2. **Verify adapter generation** is called during interpreter initialization
3. **Test method handle functionality** thoroughly with lambda expressions and method references
4. **Check BootstrapMethodError** issues are not related to missing method handle support

### Future Improvements
1. **Add unit tests** for method handle functionality in YuhuInterpreter
2. **Implement method handle validation** during interpreter initialization
3. **Add debugging output** for method handle adapter generation

## Related Issues

- **BootstrapMethodError** in Spring Boot applications
- **Method handle invocations** failing completely
- **Lambda expressions** not working in YuhuInterpreter
- **Method references** throwing exceptions

## Files Modified

- `hotspot/src/share/vm/interpreter/yuhu/yuhu_interpreter.hpp` - Added method_handle_intrinsic() and set_entry_for_kind() methods
- `hotspot/src/share/vm/interpreter/yuhu/yuhu_interpreter.cpp` - Implemented set_entry_for_kind() method
- `hotspot/src/share/vm/prims/methodHandles.cpp` - Enhanced generate() method to support YuhuInterpreter
- `hotspot/src/cpu/aarch64/vm/yuhu/yuhu_templateTable_aarch64.cpp` - Fixed appendix loading bug in prepare_invoke method

## References

- [JSR 292: Method Handles](https://jcp.org/en/jsr/detail?id=292)
- [HotSpot Method Handles Implementation](https://openjdk.org/groups/hotspot/docs/InvokeDynamic.html)
- [YuhuInterpreter Architecture Documentation](../architecture/YuhuInterpreter_Architecture.md)

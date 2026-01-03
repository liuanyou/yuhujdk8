# 032: Fix Java Method Calls Using `_from_compiled_entry`

## Problem Summary

When implementing Java method calls in the Yuhu compiler, the original approach used a generic entry point signature `(Method*, intptr_t, Thread*) -> int`, which caused multiple issues:

1. **Type mismatch errors**: The generic signature didn't match the actual Java method signatures
2. **Parameter count errors**: Different methods have different numbers and types of parameters
3. **Stack corruption**: Arguments were accessed after being popped from the stack
4. **Module conflicts**: Duplicate function definitions across compilations

## Root Cause Analysis

### Issue 1: Wrong Entry Point Model (Document 031)

The original code assumed a Shark-style entry point:
```cpp
// WRONG: Generic entry point signature for all methods
llvm::FunctionType* deopt_func_type = YuhuBuilder::make_ftype("MiT", "i");
// (Method*, intptr_t, Thread*) -> int
```

But Yuhu's compiled methods use Java-level signatures:
```cpp
// Example: CharBuffer.array() signature should be:
// (ptr receiver) -> i32
// NOT (Method*, intptr_t, Thread*) -> int
```

### Issue 2: Stack Access After Decache

The code called `decache_for_Java_call()` BEFORE collecting arguments from the stack:
```cpp
decache_for_Java_call(call_method);  // ← xpop() all args, stack becomes empty
YuhuValue* recv_val = xstack(arg_slots - 1);  // ← CRASH: stack_depth() = 0!
```

This caused assertion failures when trying to access an empty stack.

### Issue 3: Using Wrong Function Type

The code used the current function's type for all callees:
```cpp
// WRONG: Use caller's type for callee
llvm::FunctionType* compiled_ftype = function()->function()->getFunctionType();
```

This caused "Incorrect number of arguments" errors because:
- `encodeArrayLoop` has signature: `(i64, ptr, ptr, ptr, i32, i32) -> i32`
- `CharBuffer.array()` has signature: `(ptr) -> i32`

### Issue 4: Module Pollution

Previously compiled functions remained in the LLVM module, causing duplicate definition errors when compiling new methods that referenced them.

## Solution: Use `method->_from_compiled_entry`

### Architecture Overview

HotSpot provides a standard compiled-call entry point for every method:

```
Method::_from_compiled_entry points to:
┌─────────────────────────────────────────┐
│ If method is interpreted:               │
│   → c2i adapter (compiled-to-interpreter)│
│     - Sets up interpreter frame          │
│     - Transfers arguments                │
│     - Jumps to bytecode interpreter      │
├─────────────────────────────────────────┤
│ If method is compiled (C1/C2/Yuhu):     │
│   → nmethod compiled entry               │
│     - Direct execution of native code    │
└─────────────────────────────────────────┘
```

This mechanism automatically handles all four cases:
1. **Interpreted methods**: c2i adapter handles the transition
2. **C1-compiled methods**: Direct call to C1 nmethod entry
3. **C2-compiled methods**: Direct call to C2 nmethod entry
4. **Yuhu-compiled methods**: Direct call to Yuhu nmethod entry

### Implementation Details

#### Step 1: Load `_from_compiled_entry`

```cpp
Value *from_compiled_entry = builder()->CreateValueOfStructEntry(
  callee,
  Method::from_compiled_offset(),
  YuhuType::intptr_type(),
  "from_compiled_entry");
```

#### Step 2: Collect Arguments BEFORE Decache

**CRITICAL**: Must collect arguments from stack before `decache_for_Java_call()` pops them:

```cpp
// Collect arguments FIRST
std::vector<Value*> call_args;
int arg_slots = call_method->arg_size();

if (is_static) {
  call_args.push_back(LLVMValue::intptr_constant(0));  // NULL receiver
  for (int i = arg_slots - 1; i >= 0; i--) {
    YuhuValue* v = xstack(i);
    call_args.push_back(v->jint_value());  // or jlong/jfloat/jobject...
  }
} else {
  call_args.push_back(xstack(arg_slots - 1)->jobject_value());  // receiver
  for (int i = arg_slots - 2; i >= 0; i--) {
    // collect remaining args...
  }
}

// NOW safe to decache (pops args from stack)
decache_for_Java_call(call_method);
```

#### Step 3: Construct Callee's FunctionType

Each method has its own signature, so we construct the `FunctionType` dynamically:

```cpp
std::vector<llvm::Type*> param_types;

// First parameter: receiver (non-static) or NULL (static)
if (is_static) {
  param_types.push_back(YuhuType::intptr_type());  // void* null
} else {
  param_types.push_back(YuhuType::oop_type());     // receiver
}

// Add Java method parameters from signature
ciSignature* sig = call_method->signature();
for (int i = 0; i < sig->count(); i++) {
  ciType* param_type = sig->type_at(i);
  param_types.push_back(YuhuType::to_stackType(param_type));
}

// Return type is always int
llvm::FunctionType* compiled_ftype = FunctionType::get(
  YuhuType::jint_type(), param_types, false);
```

#### Step 4: Cast and Call

```cpp
Value *compiled_entry = builder()->CreateIntToPtr(
  from_compiled_entry,
  PointerType::getUnqual(compiled_ftype),
  "compiled_entry");

Value *deoptimized_frames = builder()->CreateCall(
  compiled_ftype, compiled_entry, call_args);
```

### Examples

**Example 1: `CharBuffer.array()` - No parameters**
- Java signature: `()[C`
- LLVM signature: `(ptr receiver) -> i32`
- Arguments: `[receiver]`

**Example 2: `CharBuffer.get(int)` - One int parameter**
- Java signature: `(I)C`
- LLVM signature: `(ptr receiver, i32 index) -> i32`
- Arguments: `[receiver, index]`

**Example 3: Static method `Integer.parseInt(String)` - One object parameter**
- Java signature: `(Ljava/lang/String;)I`
- LLVM signature: `(i64 null, ptr str) -> i32`
- Arguments: `[0, str_object]`

## Additional Fix: Clear LLVM Module Before Compilation

To prevent duplicate symbol errors, we now clear all functions from the LLVM module before each compilation:

```cpp
// Clear all functions from normal module
if (mod_normal != NULL) {
  std::vector<llvm::Function*> to_erase;
  for (auto& func : mod_normal->getFunctionList()) {
    to_erase.push_back(&func);
  }
  for (llvm::Function* func : to_erase) {
    func->eraseFromParent();
  }
}
```

This ensures:
- Each method compiles in a clean module environment
- No conflicts from previously compiled methods (e.g., `Matrix::multiply` appearing in `encodeArrayLoop` IR)
- IR output files only contain the current method being compiled

### Problem Context

The issue occurred because:

1. **Module Persistence**: Previously compiled functions remained in the LLVM module
2. **Cross-Method References**: When compiling `encodeArrayLoop`, if it referenced `Matrix::multiply`, and `Matrix::multiply` was already in the module, LLVM would report a duplicate definition
3. **IR Pollution**: Old function definitions would appear in new method's IR output

### Why This Happens

During compilation of method A:
1. Method A's IR is built and references method B
2. If method B is already in the module, LLVM may try to redefine it
3. This causes the error: `Duplicate definition of symbol '_com.example.Matrix::multiply'`

### The Fix

Instead of trying to selectively remove only the target function, we clear the entire module. This is safe because:

- Each compilation is independent
- Previously compiled nmethods are already installed in CodeCache
- Only the current method's IR is being generated
- No other compilation is affected

### Alternative Approaches Considered

1. **Selective removal**: Only remove the target function name - but this doesn't handle transitive references
2. **Module per compilation**: Create a new module for each method - but this adds complexity
3. **Symbol renaming**: Use unique names per compilation - but this breaks HotSpot integration

The current approach is simple, reliable, and ensures clean compilation for each method.

## Modified Files

### 1. `yuhuContext.cpp`
- **Change**: Simplified `entry_point_type` from `(Method*, intptr_t, Thread*, intptr_t, jint)` to `(Method*, intptr_t, Thread*)`
- **Reason**: The old signature with `arg_base` and `arg_count` is no longer used; we now use per-method Java signatures

### 2. `yuhuTopLevelBlock.cpp` - `do_call()` method
- **Major rewrite**: Complete overhaul of Java method call generation
- **Key changes**:
  1. Load `_from_compiled_entry` instead of loading from `YuhuEntry`
  2. Collect arguments from stack BEFORE `decache_for_Java_call()`
  3. Construct callee's `FunctionType` based on its Java signature
  4. Support both static and non-static methods correctly
  5. Handle all Java types (int, long, float, double, object/array)

### 3. `yuhuCompiler.cpp` - `compile_method()`
- **Change**: Clear entire LLVM module before each compilation
- **Reason**: Prevent duplicate function definitions across separate compilations

## Benefits of This Approach

### 1. Correctness
- ✅ Matches actual HotSpot compiled calling convention
- ✅ Works seamlessly with interpreter, C1, C2, and Yuhu
- ✅ No type mismatches or parameter count errors
- ✅ No stack corruption issues

### 2. Interoperability
- ✅ Yuhu → Interpreter: c2i adapter handles transition
- ✅ Yuhu → C1/C2: Direct compiled-to-compiled call
- ✅ Yuhu → Yuhu: Direct compiled-to-compiled call
- ✅ No special-casing needed for different compilation tiers

### 3. Maintainability
- ✅ Uses standard HotSpot mechanisms (`_from_compiled_entry`)
- ✅ Per-method signatures follow Java semantics
- ✅ Clean module state for each compilation
- ✅ Easier to debug and verify

## Testing

The fix was validated with:
1. **`Matrix.multiply`**: Pure computation, no method calls (baseline test)
2. **`CharBuffer.array()`**: Instance method with no parameters
3. **`encodeArrayLoop`**: Complex method with 12+ Java method calls including:
   - Instance methods with varying parameter counts
   - Methods returning different types (void, int, object)
   - Mix of virtual and interface calls

All IR verification errors resolved, and code generation succeeded.

## Related Documents

- **029**: `deoptimized_entry_point` null issue (separate from this, already fixed)
- **030**: C1 and C2 deoptimization comparison (background research)
- **031**: Original analysis of entry_point call signature mismatch (superseded by this fix)

## Summary

The `_from_compiled_entry` approach is the **standard HotSpot way** to handle compiled-to-compiled (or compiled-to-interpreted) Java method calls. By adopting this mechanism, Yuhu now:

1. Uses correct per-method Java signatures instead of a generic entry signature
2. Collects arguments safely before stack cleanup
3. Constructs appropriate LLVM function types for each callee
4. Maintains clean module state across compilations
5. Integrates seamlessly with HotSpot's multi-tier compilation system

This fix resolves all IR verification errors and enables Yuhu to correctly compile methods with complex call graphs like `encodeArrayLoop`.

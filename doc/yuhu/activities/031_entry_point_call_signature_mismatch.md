# 031: entry_point Call Signature Mismatch in encodeArrayLoop

## Problem Summary

When compiling `sun.nio.cs.UTF_8$Encoder::encodeArrayLoop`, LLVM IR verification fails with:

```text
Call parameter type does not match function signature!
  %base_pc = load i64, ptr %42, align 8
 i32  %53 = call i32 %entry_point(ptr %41, i64 %base_pc, ptr %thread_ptr)
```

This error appears **multiple times (about 12 calls)** in the generated IR for `encodeArrayLoop`, all on calls to `%entry_point`-like functions. However, an earlier test method `com.example.Matrix::multiply` compiled successfully and **did not hit this error**.

The key difference: **`Matrix.multiply` has no Java method calls**, while `encodeArrayLoop` contains many `invoke*` bytecodes (calling `CharBuffer`, `ByteBuffer` methods, etc.). The bug only appears when Yuhu generates code for Java method calls.

## Observed Behavior

### In `encodeArrayLoop` IR

The failing pattern looks like this (simplified):

```llvm
%base_pc = load i64, ptr %42, align 8
%entry_point = load ptr, ptr %44, align 8
%i = call i32 %entry_point(ptr %method, i64 %base_pc, ptr %thread_ptr)
```

LLVM complains that the **call-site signature** `(ptr, i64, ptr) -> i32` does **not match** the **actual function type** of `%entry_point`.

### In `Matrix.multiply` IR

The method entry is declared as:

```llvm
define i32 @"com.example.Matrix::multiply"(i64 %null_arg, ptr %0, ptr %1, ptr %2, i32 %3) {
```

Key points:
- The entry function has a **Java-specific signature**, not `(Method*, intptr_t, Thread*)`.
- Inside `Matrix.multiply` there are **no Java method calls**; all work is done with arithmetic, array accesses, and safepoints.
- Therefore, Yuhu **never uses `YuhuTopLevelBlock::do_call`** for this method, and never emits any call to `entry_point` in the IR.

So the problematic path is simply **not exercised** in `Matrix.multiply`, which is why the IR for that method passes verification.

## Code Path in Yuhu

The failing IR comes from `YuhuTopLevelBlock::do_call()` in `yuhuTopLevelBlock.cpp`:

```cpp
// Load the entry point from the YuhuEntry
Value *entry_point = builder()->CreateLoad(
  PointerType::getUnqual(YuhuType::entry_point_type()),  // ← uses a fixed FunctionType
  builder()->CreateIntToPtr(
    builder()->CreateAdd(
      base_pc,
      LLVMValue::intptr_constant(in_bytes(YuhuEntry::entry_point_offset()))),
    PointerType::getUnqual(
      PointerType::getUnqual(YuhuType::entry_point_type()))),
  "entry_point");

// Make the call
// entry_point signature: (Method*, intptr_t, Thread*) -> int
llvm::FunctionType* deopt_func_type = YuhuBuilder::make_ftype("MiT", "i");
std::vector<Value*> deopt_args;
deopt_args.push_back(callee);   // Method*
deopt_args.push_back(base_pc);  // intptr_t
deopt_args.push_back(thread()); // Thread*
Value *deoptimized_frames = builder()->CreateCall(
  deopt_func_type, entry_point, deopt_args);
```

Important observations:
- `YuhuType::entry_point_type()` currently describes some **fixed function type** for entry points.
- `make_ftype("MiT", "i")` also assumes a *generic* signature `(Method*, intptr_t, Thread*) -> int`.
- But in the actual LLVM module, the function stored in the entry point slot is the **compiled Java method**, whose signature is like the `Matrix.multiply` example (i64 + Java arguments), *not* `(Method*, intptr_t, Thread*)`.

With LLVM 20+ and opaque pointers, this mismatch between:
- the type used in `CreateLoad` (`YuhuType::entry_point_type()`), and
- the type used in `CreateCall` (`make_ftype("MiT", "i")`),

is **no longer silently tolerated**. The verifier detects that `%entry_point` does not have the declared type and reports a signature mismatch.

## Why `Matrix.multiply` Worked but `encodeArrayLoop` Fails

- `Matrix.multiply` is purely arithmetic and array-based, with **no Java calls**:
  - `do_call()` is never used.
  - No `entry_point` calls are emitted in its IR.
  - The broken entry-point calling convention stays hidden.

- `encodeArrayLoop` contains many `invoke*` bytecodes:
  - Each invoke goes through `YuhuTopLevelBlock::do_call()`.
  - `do_call()` loads an `entry_point` from a `YuhuEntry` and calls it as if it had signature `(Method*, intptr_t, Thread*) -> int`.
  - In reality, the callee is a compiled Java method with a **different, Java-specific signature** (e.g. `i32 (i64, ptr, ptr, ...)`).
  - LLVM checks the function type vs. call-site type and rejects the IR.

Thus the bug is a **latent calling-convention mismatch** that only becomes visible once we compile a method that actually performs Java calls.

## Conceptual Root Cause

1. **Out-of-date entry point model**:
   - The code in `do_call()` assumes a Shark-style or interpreter-style entry point: `(Method*, intptr_t, Thread*) -> int`.
   - Yuhu's actual compiled method entry is now a **Java-level signature**, matching the method's arguments (see `Matrix.multiply`).

2. **Single global `entry_point_type`**:
   - `YuhuType::entry_point_type()` appears to define a **single function type** for all entry points.
   - But compiled Java methods have **per-method signatures** (different number and types of parameters).

3. **Opaque pointers in LLVM 20+**:
   - With opaque pointers, we must always supply explicit `FunctionType` to `CreateLoad` / `CreateCall`.
   - If the loaded function pointer type and call-site function type disagree, LLVM will detect it.

4. **Mismatch between design and implementation**:
   - Design-wise, internal Java method calls should use the **same parameter list** as the LLVM function definition created by Yuhu for that method.
   - Implementation-wise, `do_call()` still tries to call a generic `(Method*, intptr_t, Thread*)` entry point, which no longer matches the generated functions.

## Desired Behavior

For each Java call inside compiled code:

- The IR call should target the **callee's LLVM function** with its **full Java-level signature**, exactly the same as the function definition Yuhu generated for that method.
- The argument list and types should be **identical** to those used at the method's top-level entry.
- Deoptimization support (checking for deoptimized frames, re-execution in interpreter) must be layered on top of this, but **must not break the call signature**.

In other words, something conceptually like:

```cpp
// Pseudo-code: call compiled Java callee with its real signature
llvm::FunctionType* callee_type = callee_function->getFunctionType();
std::vector<Value*> args = build_actual_java_args_for_callee(...);
Value* result = builder()->CreateCall(callee_type, callee_function, args);
```

instead of calling a generic `entry_point(Method*, intptr_t, Thread*)`.

## Solution Directions (Design Only, No Code Yet)

### Option 1: Call the Callee's LLVM Function Directly (Recommended)

**Idea**: For compiled-to-compiled Java calls, stop using the generic `entry_point` function type and instead:

- Look up the callee's `YuhuFunction` and its LLVM `Function*`.
- Use that function's **actual `FunctionType`** for `CreateCall`.
- Build the argument list using the same convention as for the method's entry (e.g. `null_arg`, receiver, parameters, etc.).
- Keep deoptimization logic separate (e.g. by passing a deopt flag/result, or by using the existing deopt blob mechanism after the call).

This aligns with the user's expectation ("参数个数、类型要和实际 java 方法及 Yuhu 生成的 LLVM 函数入口保持一致") and is consistent with how C1/C2 treat compiled Java calls (use nmethod entry signatures, not interpreter-style entries).

### Option 2: Rework `YuhuType::entry_point_type` to Match the Real Signature

A more intrusive but consistent approach:

- Redefine `YuhuType::entry_point_type()` and `YuhuEntry` so that an entry point is a pointer to the **actual compiled Java method** with its real signature.
- This would require `YuhuEntry` to be specialized per-method (or to treat entry points as `i8*` and cast to the specific function type at the call site).
- `do_call()` would then:
  - Load a raw function pointer (e.g. `i8*`).
  - Bitcast it to the callee's `FunctionType*` before calling.

This removes the hard-coded `(Method*, intptr_t, Thread*)` assumption, but still requires careful per-callee type handling.

### Option 3: Temporary Type-Only Workaround (Not Recommended Long-Term)

As a short-term experiment (not a real fix), one could:

- Change `YuhuType::entry_point_type()` and `make_ftype("MiT", "i")` to match the *actual* function type currently stored in the entry point slot.
- Or treat the entry point as `i32 (i8*, i64, i8*)` and bitcast the callee function pointer to that type.

This might silence LLVM's type checker but would still be **semantically wrong** if the callee expects different arguments (e.g. receiver and Java parameters), so it is only useful for debugging, not as a real solution.

### Option 4: Use `method->_from_compiled_entry` (HotSpot-Standard, Recommended)

**Idea**: Instead of trying to call the callee’s LLVM function directly, always call the **standard HotSpot compiled entry** for the target method:

- At compile time, Yuhu computes the callee `Method*` as it already does in `do_call()`.
- At runtime, the call target is `method->_from_compiled_entry`:
  - If the method is **interpreted only**, `_from_compiled_entry` points to a **c2i adapter**, which sets up an interpreter frame and jumps into the interpreter.
  - If the method has been compiled by **C1 or C2 (or Yuhu)**, `_from_compiled_entry` points to the method’s **compiled entry** in its `nmethod`.
- From Yuhu’s perspective, every Java call is just a normal **compiled call** to `_from_compiled_entry` using the standard HotSpot compiled ABI.

Properties:

- This automatically handles **all four cases** uniformly:
  - Interpreted callee → call goes through c2i adapter.
  - C1 callee → call goes to C1 nmethod entry.
  - C2 callee → call goes to C2 nmethod entry.
  - Yuhu callee → call goes to Yuhu’s own nmethod entry (once installed), same as C1/C2.
- Yuhu does **not** need to know whether the callee is currently interpreted, C1, C2, or Yuhu; HotSpot’s runtime and adapters handle that.
- The only requirement is that **Yuhu’s compiled code uses the same compiled calling convention** (ABI) as HotSpot’s other compilers.

IR-wise, this means:

- Model `_from_compiled_entry` as a function pointer with the **compiled-call ABI** (e.g. a uniform prototype, or even `void()` from LLVM’s point of view, relying on the backend’s calling convention).
- Build the argument list for the call according to the compiled ABI, not the Java-level signature used at the LLVM function definition.
- The existing bug (wrong `(Method*, intptr_t, Thread*)` signature) disappears because we no longer pretend the entry point is the Java method body; we call the **true compiled entry** instead.

Compared to Option 1:
- Option 1 (“call the callee’s LLVM function directly”) is only safe when both caller and callee are Yuhu and you stay inside the same LLVM world.
- Option 4 (“call `_from_compiled_entry`”) is the **normal HotSpot way** and works correctly for interpreter, C1, C2, and Yuhu, without special cases in Yuhu.

## Proposed Plan (High-Level)

1. **Confirm current entry signature**:
   - Inspect the function definitions for typical Java methods (like `Matrix.multiply`, the methods called by `encodeArrayLoop`).
   - Document the exact calling convention Yuhu uses (parameter order, types, `null_arg`, `Thread*`, etc.).

2. **Trace `do_call()` usage**:
   - For each `invoke*` in `encodeArrayLoop`, see how `callee` and `entry_point` are obtained.
   - Verify that `entry_point` currently points to the compiled method entry, not an interpreter stub.

3. **Design the new call sequence**:
   - Decide whether `do_call()` should:
     - (a) call the compiled callee directly and manage deopt separately, or
     - (b) always go through a unified deopt/entry stub with a fixed signature.
   - Ensure that whatever we call from IR has a **function type matching the callee's LLVM function** when we are doing compiled-to-compiled calls.

4. **Only after the design is clear, adjust the code** in `YuhuTopLevelBlock`, `YuhuType`, and possibly `YuhuEntry` / `YuhuFunction` to implement the chosen approach.

At this stage, we have:
- Identified the **exact reason** for the IR verification error: a mismatch between the generic entry point signature used in `do_call()` and the actual LLVM function signature of compiled Java methods.
- Explained **why `Matrix.multiply` did not expose the bug**.
- Outlined **design-level solution directions** without making any code changes yet, as requested.

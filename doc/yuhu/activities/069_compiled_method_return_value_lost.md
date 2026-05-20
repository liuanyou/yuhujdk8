# Activity 069: Compiled Method Return Value Always Zero

## Issue Summary

**Symptom:** A Yuhu-compiled method returns the correct value internally
(verified by printing `sum` from inside the method) but the caller always
receives `0`.

**Test case:** `test_yuhu/com/example/NineParameterTest.java`

```
=== Nine Parameter Test ===
p1 = 99999
p2 = 2
...
p9 = 9
Sum = 999930          <-- correct value computed inside the method
===========================
Instance method result: 0   <-- caller sees 0 instead of 999930
```

**Comparison with reference JVM (IDEA / standard JDK 8):**

| Run target           | Instance method result |
|----------------------|------------------------|
| Standard JDK 8 (IDEA)| 999930 (correct)       |
| Custom JDK 8 + Yuhu  | 0       (wrong)        |

The interpreter path returns the right value, only the Yuhu compiled path
fails. The bug is therefore in Yuhu's return-value emission, not in the test.

## Root Cause Analysis

### Where the value is supposed to flow

Java caller (interpreted) → **i2c adapter** → Yuhu compiled function → `ret`

After the compiled method's `ret` executes, control returns to the i2c
adapter (and ultimately the interpreter's result-handler logic). In the
AArch64 calling convention used by HotSpot, an integer/long return value
must be in **x0**. The interpreter's i2c return path explicitly relies on
this:

```
hotspot/src/cpu/aarch64/vm/yuhu/yuhu_interpreterGenerator_aarch64.cpp
  867    __ write_insts_pop(ltos);        // pops x0 onto the expression stack
  868    __ write_insts_pop(dtos);
  870    __ write_inst_blr(result_handler);
```

So the **only** thing that matters for the caller is the value placed in
**x0 by the LLVM `ret` instruction**.

### What Yuhu currently emits

#### 1. The unified exit block always returns `0`

`hotspot/src/share/vm/yuhu/yuhuFunction.cpp` lines 259-283:

```cpp
_unified_exit_block = llvm::BasicBlock::Create(
    YuhuContext::current(), "unified_exit", function());

builder()->SetInsertPoint(_unified_exit_block);
// ...
builder()->CreateRet(LLVMValue::jint_constant(0));   // <-- BUG: hard-coded 0
```

Every return path in the function ends with
`br label %unified_exit`, and the unified exit block contains a single
`ret i32 0`. **No matter what the method computed, x0 is loaded with 0
and the caller observes 0.**

#### 2. `handle_return` writes the value, but to the wrong place

`hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp` lines 838-845:

```cpp
Value *result_addr = stack()->CreatePopFrame(type2size[type]);
if (type != T_VOID) {
  builder()->CreateStore(
      pop_result(type)->generic_value(),
      builder()->CreateIntToPtr(
          result_addr,
          PointerType::getUnqual(YuhuType::to_stackType(type))));
}
builder()->CreateBr(function()->unified_exit_block());
```

`handle_return` does store the LLVM value (the actual `sum`), but it stores
it to `result_addr`, an address computed by `CreatePopFrame`. That address
points into the **caller's interpreter expression stack** — a location the
i2c adapter never reads after a compiled-method return. The store therefore
has no observable effect on the caller; only x0 matters, and x0 comes from
the unified exit block's `ret 0`.

#### 3. `CreatePopFrame` is interpreter-style frame teardown, inappropriate here

`hotspot/src/share/vm/yuhu/yuhuStack.cpp` lines 504-521:

```cpp
Value* YuhuStack::CreatePopFrame(int result_slots) {
  int locals_to_pop = max_locals() - result_slots;

  Value *fp = CreateLoadFramePointer();
  Value *sp = builder()->CreateAdd(
      fp,
      LLVMValue::intptr_constant((1 + locals_to_pop) * wordSize));

  CreateStoreExpressionStackPointer(sp);
  CreateStoreFramePointer(
      builder()->CreateLoad(
          YuhuType::intptr_type(),
          builder()->CreateIntToPtr(
              fp, PointerType::getUnqual(YuhuType::intptr_type()))));

  return sp;
}
```

What it does:

1. Computes `sp = fp + (1 + locals_to_pop) * wordSize` — an address
   located **above** the current frame, i.e. in the caller's interpreter
   expression stack region.
2. Writes that `sp` to the thread-local Yuhu expression-stack-pointer
   slot.
3. Loads `[fp]` (the saved previous FP) and writes it to the thread-local
   Yuhu frame-pointer slot.
4. Returns `sp` (the would-be result slot in the caller's expression
   stack).

This is interpreter-style explicit frame unwinding. It is **redundant
and incorrect** for compiled code:

- Frame teardown is already done by the LLVM-generated function epilogue
  (`ldp x29, x30, [sp], #...; ret`). LLVM owns the frame layout.
- The interpreter's expression stack does not need to be touched by a
  Yuhu compiled method; the i2c adapter only consumes x0.
- It introduces a write into the caller's frame that should not be there.

### Why is the bug only visible now?

The compiled return path has always emitted `ret 0`. Earlier tests
returned `0` either because:

- the expected value was `0`, or
- the test was small enough that the interpreter (not Yuhu) was used.

The nine-parameter test is the first one that:

1. Is large enough to be JIT-compiled by Yuhu, and
2. Returns a non-zero integer that exercises the value path.

It therefore exposes the always-`ret 0` defect for the first time.

## Why the alternatives don't work

### A. "Read the result from the expression stack and copy it to x0"

The address computed by `CreatePopFrame` lives in the caller's
interpreter expression stack. By the time control reaches the i2c
adapter's return-handling code, the i2c adapter does not read that
address — it reads x0. We cannot retroactively populate x0 from the
caller's stack at the call site without rewriting the i2c adapter
contract, which is shared with C1/C2 and the interpreter.

The only legitimate channel from compiled callee to interpreter caller
in this contract is the AArch64 ABI return register (x0).

### B. PHI node in the unified exit block

Conceptually possible: each `handle_return` block becomes an incoming
edge of a PHI node in the unified exit block, and the unified exit
block does `ret <phi>`.

Drawbacks:

- The unified exit block is created up-front in `YuhuFunction` (before
  any `handle_return` runs). The PHI's incoming list must be appended
  every time a new return block is created. This requires either
  back-patching the PHI from `handle_return`, or deferring the creation
  of the `ret` instruction until all return blocks have been emitted.
- Several return paths exist (normal returns, exception unwinding, the
  stack-overflow check fall-through, native wrappers). Each must
  maintain the PHI invariant.
- More state to track and easier to break.

### C. Function-local Alloca as a return slot (chosen)

Allocate a single `alloca` in the entry block at function-creation time.
Each `handle_return` does:

```
store <ret_value>, %return_slot
br label %unified_exit
```

The unified exit block does:

```
%retv = load <ty>, %return_slot
ret <ty> %retv
```

Properties:

- One write site per return path; no edge bookkeeping for the exit
  block.
- Mem2Reg/SROA promotes the alloca to a register before code generation,
  so the runtime cost is **zero**: the final machine code is equivalent
  to a PHI-based version.
- This is the canonical pattern Clang emits for C functions with
  multiple `return` statements.

## Resolution: Option C (Alloca-based return slot)

### High-level steps (no code yet)

1. **Add a function-scope return slot in `YuhuFunction`.**
   - New private field, e.g. `llvm::AllocaInst* _return_slot;` plus an
     accessor `return_slot()`.
   - Created in the entry block of the LLVM function, with a type
     matching the Java return type (`i32` for `T_INT/T_BOOLEAN/...`,
     `i64` for `T_LONG`, etc.; not allocated when return type is
     `T_VOID`).
   - Allocated only once, in the entry block, so Mem2Reg can promote
     it.

2. **Have `handle_return` store into the return slot instead of the
   caller's expression stack.**
   - Replace the `CreateStore` to `result_addr` with
     `CreateStore(value, function()->return_slot())`.
   - Remove the `CreatePopFrame`-derived store path for compiled
     methods (the call to `CreatePopFrame` itself can be removed from
     this path; LLVM handles frame teardown via the function epilogue).
   - Keep the existing `CreateBr(unified_exit_block)`.

3. **Have the unified exit block return the loaded slot value.**
   - Replace `CreateRet(LLVMValue::jint_constant(0))` with:
     - For non-`T_VOID`: `CreateRet(CreateLoad(_return_slot))`.
     - For `T_VOID`: `CreateRetVoid()`.

4. **Audit other callers of `CreatePopFrame`.**
   - `yuhuNativeWrapper.cpp` lines 306 and 343 also invoke
     `CreatePopFrame`. The native-wrapper return path needs the same
     treatment (write into the return slot, exit through the unified
     exit). The exception path at line 306-307 currently does
     `CreateRet(LLVMValue::jint_constant(0))` directly — it must also
     funnel through the unified exit (storing `0` into the return slot
     before branching), or be left as-is if the exception convention
     truly requires returning `0`. Decide once and document.

5. **Decide the fate of `CreatePopFrame`.**
   - Once compiled methods no longer use it, `CreatePopFrame` becomes
     dead in `YuhuTopLevelBlock::handle_return`. If no other compiled
     code path needs it, remove it. If the native wrapper still needs
     interpreter-style teardown, keep it but document its scope as
     "native wrapper only".

### Verification plan

- Re-run `NineParameterTest`. Expected:
  `Instance method result: 999930`.
- Re-run the existing Yuhu test suite under `test_yuhu/com/example/` to
  confirm no regression on void-returning methods, oop-returning
  methods, and `long`-returning methods.
- Inspect the final machine code for `testNineParameters`: confirm that
  the function epilogue places the computed `sum` into x0 (the alloca
  must be promoted to a register; no spill-and-reload should remain).
- Confirm that exception unwinding still returns the appropriate value
  (via the unified exit) and that GC-safe behavior at safepoints in the
  return path is unchanged.

## Files Involved

| File                                                    | Role                                  |
|---------------------------------------------------------|---------------------------------------|
| `hotspot/src/share/vm/yuhu/yuhuFunction.cpp` (L259-283) | Hard-coded `ret 0` in unified exit    |
| `hotspot/src/share/vm/yuhu/yuhuFunction.hpp`            | Will hold `_return_slot` field        |
| `hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp` (L813-872) | `handle_return`                  |
| `hotspot/src/share/vm/yuhu/yuhuStack.cpp` (L504-521)    | `CreatePopFrame` (to be removed/scoped) |
| `hotspot/src/share/vm/yuhu/yuhuNativeWrapper.cpp` (L306, L343) | Native wrapper return paths     |

## Status

- **Diagnosis:** complete
- **Resolution:** chosen — Alloca-based function-scope return slot
- **Implementation:** **not started** (awaiting approval before any code
  changes)

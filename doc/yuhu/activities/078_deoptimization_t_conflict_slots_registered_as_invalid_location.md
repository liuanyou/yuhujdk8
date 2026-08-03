# Activity 078: Deoptimization T_CONFLICT Slots Registered as invalid_location — Monitor Object Lost, Infinite Loop in Exception Handler

## Date
2026-08-01

## Status
**Resolved** — root cause confirmed by lldb memory inspection + bytecode analysis. Fix: register `T_CONFLICT` slots using `state->local(i)->basic_type()` instead of `state->local_type_at(i)->basic_type()`.

## Problem Description

A Spring Boot application hangs during startup with no error output. The thread dump shows the main thread stuck in `Throwable.fillInStackTrace` (native method), inside a `NullPointerException` construction. The NPE is thrown from `ConcurrentHashMap.putVal` at the `monitorexit` of a `synchronized (f)` block.

```
"main" #1 ... State: _call_back _has_called_back 1 _at_poll_safepoint 0
   at java.lang.Throwable.fillInStackTrace(Native Method)
   at java.lang.Throwable.fillInStackTrace(Throwable.java:783)
   - locked <0x000000076f6641f0> (a java.lang.NullPointerException)
   ...
   at java.util.concurrent.ConcurrentHashMap.putVal(ConcurrentHashMap.java:1064)
   - locked <0x00000006c104c460> (a java.util.concurrent.ConcurrentHashMap$TreeBin)
```

Initial investigation focused on Yuhu's lock implementation (`acquire_lock`/`release_lock`) and the `ObjectSynchronizer::slow_exit` path. These were wrong leads — `YuhuRuntime::monitorexit` was never called (confirmed by adding logging that never fired).

## Diagnostic Path

### False Lead 1: Yuhu's lock implementation

The `acquire_lock` recursion test was identified as using the wrong operand (`disp` instead of the CAS's observed value). This was a real defect but **not** the hang cause.

### False Lead 2: release_lock slow path

Analysis of `release_lock`'s CFG showed it is straight-line code with no loop. The `call_vm(monitorexit)` was suspected to hang inside `ObjectSynchronizer::slow_exit`. Added logging to `YuhuRuntime::monitorexit` — **zero `[YuhuExit]` lines appeared**. The slow path was never reached.

### False Lead 3: Infinite loop in the inner `for` loop

The `for (Node<K,V> e = f;; ++binCount)` loop at line 1033 was suspected to spin on a circular chain. This was plausible but did not match the stack trace showing `fillInStackTrace`.

### The actual root cause: deoptimization + T_CONFLICT

The stack trace showed the thread in `fillInStackTrace` — the stack walker that populates an exception's stack trace. The infinite loop was in `java_lang_Throwable::fill_in_stack_trace` at `javaClasses.cpp:1555-1638`. But this was a symptom, not the cause.

The actual hang mechanism was discovered by the user through lldb inspection:

1. A Yuhu-compiled method is deoptimized and transitions to interpreter mode.
2. During deoptimization, slots 7-16 are registered in the deoptimization bundle with type `T_CONFLICT` (from `ciTypeFlow`'s type lattice — the meet of incompatible types at a basic block merge).
3. The deoptimization code registers `T_CONFLICT` slots as `invalid_location`:
   ```cpp
   locals->append(new LocationValue(invalid_location));
   ```
4. The interpreter frame is reconstructed with **null values** in slots 7-16. Slot 12 holds the monitor object for `synchronized (f)` — it becomes null.
5. The interpreter resumes inside the `synchronized` block.
6. An NPE is thrown (e.g., from a null dereference inside the block).
7. The exception handler at bci 443 executes `aload 12` → loads null from slot 12.
8. `monitorexit` tries to dereference the null monitor → throws NPE.
9. The new NPE triggers the same exception handler → infinite loop.

Verified by lldb:
```
(lldb) x/1g 0x000000016d7bed20-96
0x16d7becc0: 0x0000000000000000   // slot 12 (monitor) is null
(lldb) x/1g 0x000000016d7bed20-56
0x16d7bece8: 0x0000000000000000   // slot 7 is also null
```

## Root Cause

In `yuhuTopLevelBlock.cpp`, the deoptimization bundle generation for local variables used `state->local_type_at(i)->basic_type()` to determine the slot type. For slots where different control flow paths assigned incompatible types (e.g., one path stores an `int`, another stores an `oop`), `ciTypeFlow` merges them as `T_CONFLICT`. The code then registered these slots as `invalid_location`, discarding the live value.

`T_CONFLICT` means **type unknown, slot still live**. The slot has a real value at runtime — the type flow analysis just couldn't determine what type it is. The deoptimization must preserve the value regardless of the type being unknown.

## Fix

Changed the deoptimization bundle generation to use `state->local(i)->basic_type()` (the actual value's type) instead of `state->local_type_at(i)->basic_type()` (the typeflow's merged type):

```cpp
// 1. Local variables (in order 0..max_locals-1)
for (int i = 0; i < max_locals(); i++) {
    YuhuValue* local_val = state->local(i);
    if (local_val != NULL) {
        BasicType basic_type = local_val->basic_type();
        deopt_operands.push_back(
            llvm::ConstantInt::get(builder()->getInt64Ty(), basic_type));
        if (basic_type == T_LONG || basic_type == T_DOUBLE) {
            assert(i + 1 < max_locals(), "padding slot should be next");
            YuhuValue* next = state->local(i + 1);
            assert(next == NULL, "next must be padding value");
            BasicType padding_type = (basic_type == T_LONG)
                ? (BasicType)ciTypeFlow::StateVector::T_LONG2
                : (BasicType)ciTypeFlow::StateVector::T_DOUBLE2;
            deopt_operands.push_back(
                llvm::ConstantInt::get(builder()->getInt64Ty(), padding_type));
            i++;
        }
        continue;
    }
    ciType* type = state->local_type_at(i);
    BasicType slot_type = type->basic_type();
    assert(slot_type == ciTypeFlow::StateVector::T_BOTTOM,
           "basic type should be T_CONFLICT");
    deopt_operands.push_back(
        llvm::ConstantInt::get(builder()->getInt64Ty(), slot_type));
}
```

When `state->local(i)` is non-null, the slot has a live value — use its actual type. When it is null, the slot is genuinely dead (T_BOTTOM) — register as `T_BOTTOM`. The previous code used `state->local_type_at(i)` which returns `T_CONFLICT` for slots with conflicting types, and then registered them as `invalid_location`.

## Key Insight

`T_CONFLICT` in `ciTypeFlow`'s type lattice is the **meet** of incompatible types at a basic block merge:

| Path A type | Path B type | Merged result |
|---|---|---|
| `T_INT` | `T_INT` | `T_INT` |
| `T_OBJECT` | `T_OBJECT` | `T_OBJECT` |
| `T_LONG` | `T_LONG` | `T_LONG` |
| `T_INT` | `T_OBJECT` | `T_CONFLICT` |
| `T_LONG` | `T_INT` | `T_CONFLICT` |

`T_CONFLICT` does not mean the slot is dead. It means the type is unknown. The slot has a live value at runtime. The deoptimization must preserve the value.

## Lessons Learned

1. **`T_CONFLICT` ≠ dead slot.** The deoptimization code must preserve the slot's value regardless of the type being unknown. Registering as `invalid_location` discards the live value.
2. **Use the actual value's type, not the typeflow's merged type.** `state->local(i)->basic_type()` gives the type of the actual value in the slot. `state->local_type_at(i)->basic_type()` gives the typeflow's merged type, which may be `T_CONFLICT`.
3. **The `fillInStackTrace` hang is a symptom, not the cause.** The infinite loop in `fillInStackTrace` was caused by the NPE-throws-NPE-throws-NPE cycle in the exception handler. The root cause was the null monitor slot from deoptimization.
4. **Logging the runtime entry points is essential.** Adding `[YuhuExit]` logging to `YuhuRuntime::monitorexit` confirmed that the slow path was never reached, which eliminated the lock-implementation hypothesis and redirected the investigation to deoptimization.

## Related Activities

- **075**: Deoptimization monitor lock corruption — related deoptimization bug affecting monitor state.
- **039**: Deoptimization parameter restoration failure — related deoptimization bug in parameter passing.
- **037**: Yuhu deoptimization root cause — earlier deoptimization investigation.

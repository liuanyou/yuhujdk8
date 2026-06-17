# Long/Double Slot Iteration Bug in YuhuStateScanner

## Problem

Yuhu iterates over local variable slots and stack slots assuming each slot index contains an independent value. This is incorrect for `long` and `double` types, which occupy **2 consecutive slots** for a single variable.

## Root Cause

### Bytecode Slot Semantics

In JVM bytecode:
- `max_locals()` counts **4-byte stack slots**, not variables
- `max_stack()` counts **4-byte stack slots**, not values
- `long` and `double` occupy **2 consecutive slots** (slot N and N+1)
- All other types (int, Object, float, etc.) occupy **1 slot**

From `globalDefinitions.cpp`:
```cpp
int type2size[T_CONFLICT+1] = { -1, 0, 0, 0, 1, 1, 1, 2, 1, 1, 1, 2, 1, 1, 0, 1, 1, 1, 1, -1};
// Index: 4=T_BOOLEAN, ..., 10=T_INT, 11=T_LONG, 12=T_OBJECT, 13=T_ARRAY
// T_LONG = 2 slots, T_OBJECT = 1 slot
```

### The Buggy Iteration Pattern

**In `YuhuNormalEntryCacher::process_local_slot()`**:
```cpp
for (int i = 0; i < max_locals(); i++) {
  process_local_slot(
    i,
    state->local_addr(i),
    stack()->locals_slots_offset() + max_locals() - 1 - i);
}
```

**In stack slot processing**:
```cpp
for (int i = state->stack_depth() - 1; i >= 0; i--) {
  process_stack_slot(
    i,
    state->stack_addr(i),
    stack()->stack_slots_offset() + i + max_stack() - state->stack_depth());
}
```

**The problem**: Both loops iterate over **slot indices**, treating each slot as an independent value. But for `long`/`double`:
- Slots N and N+1 refer to the **same variable**
- Processing them separately causes incorrect behavior

## Concrete Example

### Method: `java.lang.Long::getChars(long i, int index, char[] buf)`

**LocalVariableTable**:
```
Start  Length  Slot  Name   Signature
   0     221     0     i   J        ← long occupies slots 0-1
   0     221     2 index   I        ← int occupies slot 2
   0     221     3   buf   [C       ← Object occupies slot 3
  34      52     4     q   J        ← long occupies slots 4-5
  54      32     6     r   I        ← int occupies slot 6
   3     218     7 charPos   I      ← int occupies slot 7
   6     215     8  sign   C        ← char occupies slot 8
 124      33     6     r   I        ← int occupies slot 6 (reused)
 104      53     9    q2   I        ← int occupies slot 9
 181      40     6     r   I        ← int occupies slot 6 (reused)
 167      54     9    q2   I        ← int occupies slot 9 (reused)
  90     131    10    i2   I        ← int occupies slot 10

max_locals() = 11
```

**Buggy iteration**:
```
i=0:  process_local_slot(0) → processes 'this' ✅
i=1:  process_local_slot(1) → processes slot 1, but this is SECOND HALF of long 'i' ❌
i=2:  process_local_slot(2) → processes slot 2 (index) ✅
i=3:  process_local_slot(3) → processes slot 3 (buf) ✅
i=4:  process_local_slot(4) → processes slot 4 (q long), but...
i=5:  process_local_slot(5) → processes slot 5, SECOND HALF of long 'q' ❌
i=6:  process_local_slot(6) → processes slot 6 (r) ✅
...
i=10: process_local_slot(10) → processes slot 10 (i2) ✅
```

**What goes wrong**:
- Slot 1 is the padding slot for long `i` (slots 0-1)
- Slot 5 is the padding slot for long `q` (slots 4-5)
- These slots are processed as if they contain independent values, but they're part of a larger value

## Crash Manifestation

### In `YuhuNormalEntryCacher::get_function_arg()`:

```cpp
llvm::Argument* YuhuNormalEntryCacher::get_function_arg(int local_index) {
  llvm::Function* func = function()->function();
  llvm::Function::arg_iterator ai = func->arg_begin();
  ai++;  // Skip dummy argument at arg 0
  
  for (int i = 0; i < local_index; i++) {
    ai++;
    if (ai == func->arg_end()) {
      ShouldNotReachHere();  // ← CRASH HERE
      return NULL;
    }
  }
  return &*ai;
}
```

**When `local_index = 4` (slot for long parameter)**:
- `arg_size() = 5` (slot count: this=1 + i=2 + index=1 + buf=1)
- LLVM function has 5 arguments: dummy, this, i (long), index, buf
- `get_function_arg(4)` advances: dummy → this → i → index → buf → **arg_end()**
- **Crash**: iterator exceeds function arguments

**Why it crashes for long but not Object**:
- `long` parameter: 2 bytecode slots → 1 LLVM argument
- `Object` parameter: 1 bytecode slot → 1 LLVM argument
- When iterating by slot index, long creates a mismatch between slot count and LLVM argument count

## Impact

1. **Compilation crash**: Methods with `long`/`double` parameters crash during `YuhuNormalEntryState` construction
2. **Incorrect state mapping**: Even if it doesn't crash, the second slot of long/double would be processed incorrectly
3. **GC safety risk**: OopMap might mark the wrong slots as containing references

## Affected Code Locations

1. `YuhuNormalEntryCacher::process_local_slot()` - iterates over all local slots
2. `YuhuJavaCallDecacher::process_stack_slot()` - iterates over all stack slots
3. `YuhuStateScanner::scan()` - orchestrates the slot scanning
4. Any code that assumes `arg_size()` equals the number of LLVM function arguments

## Fix Strategy

### Option 1: Skip Second Slots (Simple)

```cpp
for (int i = 0; i < max_locals(); i++) {
  // Skip if this is the second slot of a long/double
  if (i > 0 && is_second_half_of_wide_local(i, state)) {
    continue;
  }
  
  process_local_slot(
    i,
    state->local_addr(i),
    stack()->locals_slots_offset() + max_locals() - 1 - i);
}
```

**Pros**: Minimal code change
**Cons**: Still iterates over wasted slots

### Option 2: Iterate by Parameters (Correct)

```cpp
// For entry cacher: iterate over function parameters, not slots
ciSignature* sig = target()->signature();
int slot_idx = 0;
for (int param_idx = 0; param_idx < sig->count(); param_idx++) {
  ciType* param_type = sig->type_at(param_idx);
  
  process_parameter_slot(
    slot_idx,
    param_type,
    get_function_arg(slot_idx));
  
  slot_idx += param_type->size();  // +2 for long/double, +1 for others
}
```

**Pros**: Correctly maps to LLVM arguments
**Cons**: Requires refactoring the iteration logic

### Option 3: Use C1/C2 Approach (Optimal)

Implement register allocation and liveness analysis to:
- Only allocate slots for actually-live values
- Skip padding slots entirely
- Optimize stack frame size

**Pros**: Eliminates waste, optimal performance
**Cons**: Major architectural change, requires linear scan or graph coloring allocator

## Related Issues

- **Issue 039**: Deoptimization parameter restoration failure (may be related to slot mapping)
- **Issue 041**: Calling convention parameter mismatch (long/double handling)
- **Crash log**: `hs_err_pid35552.log` - crash in `YuhuNormalEntryCacher::get_function_arg()`

## Testing

Methods that trigger this bug:
- Any method with `long` or `double` parameters
- `java.lang.Long::getChars(long i, int index, char[] buf)`
- `java.lang.Double::toString(double d)`
- Any method where the expression stack contains long/double values

## References

- JVM Specification §2.3.1: Local Variables and Operand Stack
- `ciSignature::size()` - returns slot count, not parameter count
- `type2size[]` array in `globalDefinitions.cpp`
- `ciMethod::arg_size()` - returns slot count including long/double as 2 slots

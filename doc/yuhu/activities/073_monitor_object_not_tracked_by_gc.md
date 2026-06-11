# Monitor Object Not Tracked by GC at Safepoint

## Problem

`monitorexit` crashes with `SIGSEGV` at `ObjectMonitor::header()` with address `0xbaadbabebaadbabc` (zapped heap memory).

## Root Cause

The monitor object slot (`BasicObjectLock::_obj`) is NOT included in the OopMap at safepoints, so GC does not track it.

### Why RS4GC Doesn't Track Monitor Slot

1. `acquire_lock()` stores the lockee to `monitor_object_addr` once at `monitorenter`
2. After the store, the lockee SSA value has zero uses in the compiled code
3. RS4GC only tracks **live SSA values** at statepoints — dead values are excluded from the stack map
4. The monitor slot is a "fire and forget" store: oop stored once, never loaded until `monitorexit` runtime call
5. Result: monitor oop slot NOT in OopMap → GC can collect the object → heap zapped with `0xbaadbabe` → crash

### Why Locals/Expression Stack Work

Locals and expression stack slots ARE tracked because they are continuously loaded/consumed by bytecode, keeping their SSA values live across instructions. RS4GC sees them as live and includes them in the stack map.

## Crash Sequence

```
monitorenter:
  lockee (young gen) → stored to monitor slot → lockee SSA dies
  
[safepoint poll call]
  StackMap only tracks live SSA values (sp+32, sp+40)
  Monitor slot (sp+48) NOT in StackMap

GC runs:
  Updates sp+32, sp+40 (tracked)
  Ignores sp+48 (not tracked) → lockee collected → zapped to 0xbaadbabe

monitorexit:
  Load from monitor slot → 0xbaadbabe (stale)
  ObjectMonitor::header(0xbaadbabe) → SIGSEGV
```

## Frame Layout (BufferedWriter.flushBuffer)

```
SP+32:  spill slot (tracked) → 0x00000006c000c360 (old gen, valid)
SP+40:  spill slot (tracked) → 0x00000006c000c378 (old gen, valid)
SP+48:  monitor object     → 0x000000076ad8c3e8 (young gen, STALE after GC)
```

LLVM generates code to copy spill slots (sp+32, sp+40) to locals area after safepoint:
```
ldp x8, x9, [sp, #32]
stp x8, x9, [sp, #160]  ← locals area gets updated values
```

But monitor slot is NOT part of this copy chain.

## Fix Strategy

Add monitor object slots to OopMap explicitly in `yuhuDebugInformationRecorder.cpp`:

```cpp
// After processing RS4GC stack map entries:
for (int m = 0; m < max_monitors; m++) {
    int obj_offset = stack->monitor_object_offset(m);
    oopmap->set_oop(YuhuStack::slot2reg(obj_offset));
}
```

This tells GC to scan and update monitor slots directly at every safepoint.

## Related

- C1/C2 do this explicitly — monitor slots are JVM-managed frame state, not LLVM SSA state
- RS4GC only covers compiler-generated spills, not JVM-managed frame slots
- Frame layout is deterministic: monitor slots at fixed offsets from SP

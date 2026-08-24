# 080: Unverified Entry Receiver Type Mismatch Crash

## Problem Summary

C2-compiled `AbstractPipeline::copyInto` dispatches `invokeinterface forEachRemaining` to the wrong method implementation. The `this` oop is `HashMap$KeySpliterator`, but the call jumps to `ArrayList$ArrayListSpliterator.forEachRemaining`, causing a SIGSEGV when accessing fields at wrong offsets.

## Error Details

- **Crash Location**: `ArrayList$ArrayListSpliterator.forEachRemaining` at `ldr w10, [x9, #20]`
- **Signal**: SIGSEGV (invalid memory access)
- **Root Cause**: `x9 = 0x8` — decoded from compressed oop value `1` (nonsensical field value)
- **Receiver**: `HashMap$KeySpliterator` (at `this+0`)
- **Expected Method**: `HashMap$KeySpliterator.forEachRemaining`
- **Actual Method Executed**: `ArrayList$ArrayListSpliterator.forEachRemaining`

## Investigation Timeline

### Stage 1: Stack Dump Analysis

The crashing nmethod is `ArrayList$ArrayListSpliterator.forEachRemaining` at `0x00000001491eaf80`. The `this` oop (in `x1`/`c_rarg1`) is `HashMap$KeySpliterator`, which is a completely different class.

Key observation: the `list` field at offset 24 of `ArrayList$ArrayListSpliterator` contains compressed oop value `1`, which when decoded (shift 3, zero-based) gives address `0x8` — the faulting address at `ldr w10, [x9, #20]`.

### Stage 2: Caller Code Analysis

The caller is C2-compiled `AbstractPipeline::copyInto`:
```
0x0000000149056234: bl 0x0000000149056670   ; {virtual_call}
0x0000000149056238: ldr x1, [sp, #32]
```

The `bl` target `0x0000000149056670` is within the same nmethod — a jump stub/trampoline:
```
0x0000000149056670: ldr x8, 0x0000000149056678   ; {other}
0x0000000149056674: br x8
```

The data word at `0x0000000149056678` contains `0x00000001491eaf80` — the entry of `ArrayList$ArrayListSpliterator.forEachRemaining`.

### Stage 3: Call Stub Verification

All three Yuhu call stubs (static, virtual, interface) correctly set `x12` to the callee `Method*` before `blr`:
- Static: `yuhuRuntime.cpp:462` — `masm.write_inst_mov_reg(x12, x9)`
- Virtual: `yuhuRuntime.cpp:583` — `masm.write_inst_mov_reg(x12, x9)`
- Interface: `yuhuRuntime.cpp:746` — `masm.write_inst_mov_reg(x12, x9)`

The `x12` value observed (`ReduceOps$8ReducingSink.begin`) did NOT match the actual code being executed, confirming the call did NOT go through any Yuhu call stub.

### Stage 4: C2 Type Profile Optimization

C2's `call_generator` in `doCall.cpp` (lines 211-242) uses the type profile (`profile.receiver(0)`) to resolve virtual/interface calls to a specific implementation:

```cpp
receiver_method = callee->resolve_invoke(jvms->method()->holder(), profile.receiver(0));
CallGenerator* hit_cg = this->call_generator(receiver_method,
      vtable_index, !call_does_dispatch, jvms, allow_inline, prof_factor);
```

When the type profile says the receiver is always `ArrayList$ArrayListSpliterator`, C2 generates a direct call to that implementation, bypassing itable lookup. The jump stub/trampoline is patched to point directly to the resolved method.

**The type profile was stale** — it said `ArrayList$ArrayListSpliterator` but the actual receiver was `HashMap$KeySpliterator`.

### Stage 5: Missing Safety Net

Standard HotSpot compiled methods have an **unverified entry** that checks the receiver klass before entering the method body. If the klass doesn't match, it jumps to `SharedRuntime::get_ic_miss_stub()` to re-resolve the call.

**Yuhu's unverified entry was just `b verified_entry` — no type check at all.** This meant:
1. C2 calls with wrong receiver type (stale profile)
2. Yuhu method has no type check at unverified entry
3. Wrong method body executes with wrong field offsets
4. Crash

## How C1/C2 Handle This

Standard HotSpot's `c2i_unverified_entry` in `sharedRuntime_aarch64.cpp:662-668`:
```cpp
__ load_klass(rscratch1, receiver);
__ ldr(tmp, Address(holder, CompiledICHolder::holder_klass_offset()));
__ cmp(rscratch1, tmp);
__ ldr(rmethod, Address(holder, CompiledICHolder::holder_method_offset()));
__ br(Assembler::EQ, ok);
__ far_jump(RuntimeAddress(SharedRuntime::get_ic_miss_stub()));
```

On type mismatch, it jumps to the **IC miss stub** (not uncommon trap/deopt). The IC miss stub:
1. Calls `SharedRuntime::handle_wrong_method_ic_miss`
2. Which calls `handle_ic_miss_helper` to re-resolve the correct method
3. Returns the correct method's `verified_code_entry()`
4. The stub jumps to the correct method

## Why Uncommon Trap Blob Is Wrong

Initially we considered jumping to `SharedRuntime::uncommon_trap_blob()` (deoptimization). This is **incorrect** because:

1. **No deopt metadata**: The unverified entry is before the method body. There are no LLVM-generated scope descriptors for its PC. `Deoptimization::uncommon_trap` would fail to find a `ScopeDesc`.

2. **No frame**: No frame has been allocated. No locals, no expression stack, no BCI.

3. **Wrong receiver oop**: Even if deopt succeeded, the callee's frame would have `this` = `HashMap$KeySpliterator`, but the method is `ArrayList$ArrayListSpliterator.forEachRemaining`. The interpreter would re-execute the wrong method with the wrong receiver — same crash.

4. **The oop doesn't match the method**: Deopt metadata describes "locals from incoming arguments", but the `this` argument is the wrong type for the method. The deopt would reconstruct an invalid interpreter frame.

## Why IC Miss Stub May Not Be Sufficient Either

The IC miss stub is designed for **inline cache misses** in the c2i adapter. It expects:
- The call site has an inline cache that can be patched
- The stub resolves the correct method and patches the cache

For Yuhu methods with direct calls through jump stubs:
- ✅ The IC miss stub resolves the correct method for the current call
- ✅ The stub jumps to the correct method
- ❌ The jump stub (trampoline) still points to the wrong method
- ❌ Future calls would still go to the wrong method, causing repeated IC misses

## Fix Applied

Added receiver type check at the unverified entry of Yuhu-compiled instance methods.

### Files Modified

#### `hotspot/src/share/vm/yuhu/yuhuCompiler.hpp`
Added `ciMethod* target` parameter to adapter functions:
```cpp
static int measure_normal_adapter_size(int frame_size_in_bytes, ciMethod* target = NULL);
int generate_normal_adapter_into(CodeBuffer& cb, address* verified_entry_point,
    int frame_size_in_bytes, ciMethod* target = NULL);
```

#### `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp`

**Added includes:**
```cpp
#include "runtime/sharedRuntime.hpp"
#include "runtime/deoptimization.hpp"
```

**`measure_normal_adapter_size`** — For instance methods, generates load_klass + lea + cmp + b.eq + far_jump to IC miss stub. Buffer increased from 64 to 128 bytes.

**`generate_normal_adapter_into`** — Same type check with metadata relocation for the holder klass pointer (survives class unloading):
```cpp
if (target != NULL && !target->is_static() && target->holder() != NULL) {
    InstanceKlass* holder_ik = target->holder()->get_instanceKlass();
    masm.write_insts_load_klass(x8, x1);                    // receiver's klass
    int metadata_index = cb.oop_recorder()->allocate_metadata_index(holder_ik);
    RelocationHolder rspec = metadata_Relocation::spec(metadata_index);
    cb.relocate(masm.current_pc(), rspec);
    masm.write_insts_lea(x9, YuhuAddress((address)holder_ik, relocInfo::metadata_type));
    masm.write_inst_regs("cmp %s, %s", x8, x9);            // compare
    masm.write_inst_b(eq, verified_entry);                  // match → proceed
    masm.write_insts_far_jump(YuhuRuntimeAddress(SharedRuntime::get_ic_miss_stub())); // mismatch → IC miss
}
```

**Call site** — `initialize_oop_recorder` moved before adapter generation (so the adapter can record metadata relocations). `target` is passed to both adapter functions.

### Generated Assembly

```assembly
unverified_entry:
    load_klass  x8, x1          ; receiver's klass (handles compressed/full oops)
    lea         x9, <holder>    ; expected klass (movz + 3 movk, with metadata relocation)
    cmp         x8, x9          ; compare
    b.eq        verified_entry  ; match → proceed
    far_jump    ic_miss_stub    ; mismatch → re-resolve (adrp+add+br or b)
verified_entry:
    nop                         ; verified entry point
    <stack overflow check>
    nop
```

## Open Questions

1. **IC miss stub suitability for Yuhu**: The IC miss stub is designed for inline cache misses. For Yuhu direct calls through jump stubs, it resolves the correct method but may not patch the jump stub. This could cause repeated IC misses on every call. A custom mechanism that patches the jump stub may be needed.

2. **Caller is C2 only**: The current fix assumes the caller is C2 (which generates direct calls based on type profiles). If the caller is the interpreter or C1, the call goes through proper dispatch and the type check at the unverified entry would always pass.

3. **Metadata relocation correctness**: The holder klass pointer uses metadata relocation to survive class unloading. This needs verification — if the klass is unloaded, the relocation should update the pointer or trigger deoptimization.

## References

- C2 type profile optimization: `hotspot/src/share/vm/opto/doCall.cpp:211-242`
- Standard c2i unverified entry: `hotspot/src/cpu/aarch64/vm/sharedRuntime_aarch64.cpp:643-677`
- IC miss handling: `hotspot/src/share/vm/runtime/sharedRuntime.cpp:1338-1356`
- IC miss helper: `hotspot/src/share/vm/runtime/sharedRuntime.cpp:1445-1505`
- Yuhu call stubs: `hotspot/src/share/vm/yuhu/yuhuRuntime.cpp:416-800`

## Status

- **Priority**: P0 (Crash in production)
- **Status**: Fix applied, pending testing
- **Risk**: IC miss stub may not fully resolve repeated calls with wrong receiver type

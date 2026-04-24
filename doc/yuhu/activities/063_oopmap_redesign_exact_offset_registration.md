# Activity 063: OopMap Redesign - Exact Offset Registration and Last Java PC Fix

**Status**: Planning  
**Created**: 2026-03-19  
**Priority**: Critical (blocks Activity 054 fix verification)

## Problem Statement

After fixing Activity 054 (ORC JIT CreateInlineOop SIGSEGV), a new crash occurs in `hs_err_pid15328.log`:

```
# Internal Error (.../compiler/oopMap.cpp:311), pid=15328, tid=0x0000000000016e03
# assert(m->offset() == pc_offset) failed: oopmap not found
```

**Stack trace** (hs_err_pid15328.log lines 24-26):
```
V  [libjvm.dylib+0x31c63c]  CodeBlob::oop_map_for_return_address(unsigned char*)+0x9c
V  [libjvm.dylib+0x8ad418]  OopMapSet::update_register_map(frame const*, RegisterMap*)+0x1c8
V  [libjvm.dylib+0x494f5c]  frame::sender_for_compiled_frame(RegisterMap*) const+0x12c
```

## Root Cause Analysis

### Issue 1: Last Java PC Setting Timing Problem

**Current behavior** (yuhuStack.hpp:193-250, yuhuStack.cpp:164-232):

`CreateSetLastJavaFrame()` uses `CreateReadCurrentPC()` which generates:
```assembly
adr x0, .              // Reads PC of THIS instruction
str x0, [thread, #last_Java_pc_offset]
```

**Problem**: The `adr` instruction reads the PC of itself, NOT the return address after the call.

**Example sequence**:
```assembly
Offset 0x100: adr x0, .              // Reads 0x100
Offset 0x104: str x0, [thread, #pc]  // Stores 0x100 (WRONG!)
Offset 0x108: ... decache ...
Offset 0x120: bl yuhu_resolve_static_field
Offset 0x124: ... return address (SHOULD be stored, but isn't!)
```

When GC occurs:
1. GC calls `frame::sender_for_compiled_frame()`
2. Gets `last_Java_pc = 0x100` (adr instruction address)
3. Looks up OopMap at offset 0x100
4. **No OopMap registered at 0x100** → assertion fails!

**Correct behavior** (from C1, c1_Runtime1_aarch64.cpp:62-69):
```cpp
Label retaddr;
set_last_Java_frame(sp, rfp, retaddr, rscratch1);  // Unbound label
blr(rscratch1);                                     // The call
bind(retaddr);                                      // Bind AFTER call
int call_offset = offset();                         // = offset AFTER blr
oop_maps->add_gc_map(call_offset, oop_map);         // Register at return address!
```

**C1 uses Label patching**: The `adr` instruction is patched AFTER the `blr` to point to the return address.

### Issue 2: OopMap Not Registered at Correct Offset

**Current behavior** (yuhuBuilder.cpp:1209-1310):

`scan_and_update_offset_markers()` scans for offset markers, then looks for `adr` instruction:
```cpp
// Line 1274-1289: Scans for ADR instruction
if ((inst & ADR_MASK) == ADR_PATTERN) {
  actual_offset = scan_offset;  // ← WRONG! Uses ADR offset
  break;
}
```

**Problem**: Registers OopMap at `adr` instruction offset, NOT at return address after `bl`/`blr`.

**Correct behavior**: OopMap must be registered at the **return address** (instruction AFTER the call), because:
- GC traverses stack and gets return address from saved LR
- GC looks up OopMap at return address: `cb->oop_map_for_return_address(fr->pc())`
- OopMap must exist at that exact offset

### Issue 3: Missing OopMap for Runtime Helper Calls

**Example**: `CreateInlineOopForStaticField()` (yuhuBuilder.cpp:943-1007)

```cpp
Value* YuhuBuilder::CreateInlineOopForStaticField(int cp_index, const char* name) {
  // Get helper address via dlsym
  static void* helper_addr = dlsym(RTLD_DEFAULT, "yuhu_resolve_static_field");
  
  // Create call with inttoptr constant
  llvm::Value* fn_ptr = CreateIntToPtr(
    llvm::ConstantInt::get(i64_ty, (uint64_t)(uintptr_t)helper_addr), ptr_ty);
  
  return CreateCall(func_ty, fn_ptr, args, name);  // ← No OopMap!
}
```

**Generated machine code**:
```assembly
movz x8, #addr_high, lsl #32
movk x8, #addr_mid, lsl #16
movk x8, #addr_low
blr x8
```

**Missing**:
1. ❌ No `CreateSetLastJavaFrame()` before call (last_Java_pc not updated)
2. ❌ No OopMap registered at return address
3. ❌ No decache/cache around call (live oops not saved to stack)

**Why it needs OopMap**: `yuhu_resolve_static_field()` uses `ThreadInVMfromJava` (yuhuRuntime.cpp:257), which transitions to VM state and can trigger safepoint/GC. If the field is an object reference, it returns an oop in x0 register that GC must track.

### Issue 4: OopMap Incompleteness Due to LLVM Register Allocator

**The core problem**: Unlike C1/C2 which control register allocation and know all oop locations, Yuhu uses LLVM's register allocator (a black box) and cannot track where oops end up after code generation.

**Current gap for backedge safepoints** (yuhuTopLevelBlock.cpp:700-710):

```cpp
void YuhuTopLevelBlock::maybe_add_backedge_safepoint() {
  if (current_state()->has_safepointed())
    return;
  
  for (int i = 0; i < num_successors(); i++) {
    if (successor(i)->can_reach(this)) {
      maybe_add_safepoint();  // ← Does NOT decache oops!
      break;
    }
  }
}
```

**The bug**: No decache/cache around backedge safepoints. If an oop is live across the backedge in a register (including LLVM auto-spill slots), and GC moves it, the register/slot contains a stale address → crash.

**Root cause**: Yuhu's `current_state()` only tracks Java-level state (locals + expression stack), but LLVM creates additional SSA values that may contain oops in registers or auto-spill slots that are invisible to the decacher.

**Solution**: Use LLVM's RewriteStatepointsForGC + PlaceSafepoints passes to automatically track all oop locations and insert safepoint polls.

## Scope of Redesign: ALL Java Calls, Not Just Runtime Helpers

**CRITICAL**: This redesign applies to **ALL types of calls** that can trigger safepoints, not just runtime helper calls like `yuhu_resolve_static_field`.

### Categories of Calls Requiring OopMap Registration

#### 1. VM Runtime Helper Calls (Via `call_vm()`)

**Examples** (yuhuTopLevelBlock.cpp):
- Safepoint polls: `call_vm(builder()->safepoint(), EX_CHECK_FULL)` (line 689)
- Exception handling: `call_vm(builder()->find_exception_handler(), ...)` (line 626)
- Object allocation: `call_vm(builder()->new_instance(), ...)` (line 2034)
- Array operations: `call_vm(builder()->newarray(), ...)` (line 2063)
- Monitor operations: `call_vm(builder()->monitorenter(), ...)` (line 2232)
- Finalizers: `call_vm(builder()->register_finalizer(), ...)` (line 803)
- NullPointerException: `call_vm(builder()->throw_NullPointerException(), ...)` (line 409)

**Current pattern** (yuhuTopLevelBlock.hpp:298-327):
```cpp
llvm::CallInst* call_vm(llvm::Value* callee, ...) {
  decache_for_VM_call();
  stack()->CreateSetLastJavaFrame();  // ← WRONG: Uses ADR instruction
  CallInst *res = builder()->CreateCall(func_type, callee, args);
  stack()->CreateResetLastJavaFrame();
  cache_after_VM_call();
  return res;
}
```

**Problem**: ALL these calls have the same bugs:
- ❌ `CreateSetLastJavaFrame()` reads wrong PC (ADR instead of return address)
- ❌ No OopMap registered at correct offset
- ❌ Multiple calls to same helper (e.g., `new_instance`) can't be distinguished

#### 2. Static Field Access (GetStatic/PutStatic)

**Example** (yuhuBlock.cpp:1069):
```cpp
Value* field_result = builder()->CreateInlineOopForStaticField(cp_index);
```

**Problems**:
- ❌ Not using `call_vm()` wrapper (direct call)
- ❌ No `CreateSetLastJavaFrame()` before call
- ❌ No OopMap registered
- ❌ Can return oop in x0 that GC must track

#### 3. Native Method Wrappers

**Example** (yuhuNativeWrapper.cpp:194):
```cpp
stack()->CreateSetLastJavaFrame();  // ← WRONG: Uses ADR instruction
Value *result = builder()->CreateCall(func_type, native_function, param_values);
```

**Problems**:
- ❌ `CreateSetLastJavaFrame()` reads wrong PC
- ❌ OopMap registered at wrong offset (ADR instead of return address)

#### 4. Other Runtime Calls (Not via `call_vm()`)

**Examples**:
- `CreateInlineOop()` - embedding oop constants
- `CreateInlineMetadata()` - embedding metadata constants
- Inline cache miss handlers
- Deoptimization calls

**All require**: Proper last_Java_pc setting + OopMap registration at return address

### Why ALL Calls Need the Redesign

**Common pattern across ALL call types**:
1. Decache registers to stack (so GC can find live oops)
2. Set last_Java_frame (tell GC where we are)
3. Make the call (can trigger safepoint/GC)
4. Reset last_Java_frame (transition back to Java)
5. Cache registers from stack

**The bug is in step 2**: `CreateSetLastJavaFrame()` uses `adr x0, .` which reads the PC of the ADR instruction, NOT the return address after the call.

**When GC occurs during the call**:
- GC gets `last_Java_pc` = address of ADR instruction (WRONG!)
- GC looks for OopMap at that address
- Either: No OopMap exists, or OopMap exists but at wrong offset
- **Result**: Assertion failure or incorrect GC behavior

### Impact Analysis

**Estimated number of affected call sites**:
- VM runtime calls via `call_vm()`: ~20-30 locations in yuhuTopLevelBlock.cpp
- Static field accesses: Variable (depends on compiled methods)
- Native wrappers: One per native method
- Other runtime calls: ~10-15 locations

**All of these need**:
1. ✅ Metadata with unique virtual offset
2. ✅ Correct last_Java_pc (return address after call)
3. ✅ OopMap registered at correct offset
4. ✅ Deferred registration via JITLink plugin

## Design Solution

### Approach: Virtual Address Placeholders + Stack Map Correlation

**Why this approach**:
1. ✅ Each call site gets a unique virtual address at IR generation time
2. ✅ Virtual address serves as placeholder for last_Java_pc
3. ✅ Virtual address is scannable pattern in machine code
4. ✅ Stack maps provide exact offsets after code generation
5. ✅ Backward scanning from statepoint call finds the placeholder
6. ✅ No GlobalVariable (doesn't work with nmethod CodeBlob)
7. ✅ No wrapper functions (breaks stack walking)
8. ✅ No custom metadata (dropped by RewriteStatepointsForGC)

### Phase 1: IR Generation - Store Dual Virtual Address Placeholders

**Apply to ALL call types**, not just `CreateInlineOopForStaticField()`:

**Key Change**: For each call site, we create **two** virtual addresses encoded with the **same** virtual offset but **different magic numbers**:
1. **Last Java PC placeholder** (`0xDEADxxxx`): Stored to `thread->last_Java_pc`
2. **Call target placeholder** (`0xBEEFxxxx`): Used as the call target address

Both placeholders share the same `virtual_offset` to establish correlation between the last_Java_pc location and the call target for JITLink patching.

#### Dual Virtual Address Encoding

```cpp
// At each call site:
int virtual_offset = code_buffer()->create_unique_offset();  // e.g., 0x1000, 0x1004, 0x1008...

// Two virtual addresses with different magic numbers, same virtual_offset
uint64_t last_java_pc_va = 0xDEAD0000 | virtual_offset;  // e.g., 0xDEAD1000
uint64_t call_target_va = 0xBEEF0000 | virtual_offset;   // e.g., 0xBEEF1000
```

**Why different magic numbers**:
- `0xDEADxxxx`: Magic for last_Java_pc placeholders (easily identifiable when scanning backwards from statepoint)
- `0xBEEFxxxx`: Magic for call target placeholders (easily identifiable when patching call instructions)
- Same `xxxx` (virtual_offset): Correlates last_Java_pc placeholder with call target for JITLink patching

#### 1. VM Runtime Calls via `call_vm()`

**Modify**: `YuhuTopLevelBlock::call_vm()` (yuhuTopLevelBlock.hpp:298-327)

```cpp
llvm::CallInst* call_vm(llvm::Value* callee, ...) {
  decache_for_VM_call();
  
  // 1. Get unique virtual offset for this call site
  int virtual_offset = code_buffer()->create_unique_offset();
  
  // 2. Create dual virtual addresses with same virtual_offset
  uint64_t last_java_pc_va = 0xDEAD0000 | virtual_offset;  // For last_Java_pc
  uint64_t call_target_va = 0xBEEF0000 | virtual_offset;   // For call target
  
  // 3. Store virtual address as placeholder for last_Java_pc
  Value* ljpc_placeholder = ConstantInt::get(i64_ty, last_java_pc_va);
  builder()->CreateStore(ljpc_placeholder, last_Java_pc_addr());
  
  // 4. Create the call using virtual address as call target
  Value* callee_ptr = CreateIntToPtr(
    ConstantInt::get(i64_ty, call_target_va), 
    func_type->getPointerTo());
  CallInst *res = builder()->CreateCall(func_type, callee_ptr, args);
  
  // 5. Create deferred OopMap keyed by virtual_offset
  OopMap* oopmap = create_oopmap_for_runtime_call();
  function()->add_deferred_oopmap(virtual_offset, oopmap);
  
  stack()->CreateResetLastJavaFrame();
  cache_after_VM_call();
  return res;
}
```

#### 2. Static Field Access

**Modify**: `YuhuBuilder::CreateInlineOopForStaticField()` (yuhuBuilder.cpp:943-1007)

```cpp
Value* YuhuBuilder::CreateInlineOopForStaticField(int cp_index, const char* name) {
  // 1. Get unique virtual offset
  int virtual_offset = code_buffer()->create_unique_offset();
  
  // 2. Create dual virtual addresses
  uint64_t last_java_pc_va = 0xDEAD0000 | virtual_offset;
  uint64_t call_target_va = 0xBEEF0000 | virtual_offset;
  
  // 3. Store placeholder for last_Java_pc
  Value* ljpc_placeholder = ConstantInt::get(i64_ty, last_java_pc_va);
  builder()->CreateStore(ljpc_placeholder, last_Java_pc_addr());
  
  // 4. Create the call using virtual address as call target
  Value* callee_ptr = CreateIntToPtr(
    ConstantInt::get(i64_ty, call_target_va), 
    func_ty->getPointerTo());
  CallInst* call = CreateCall(func_ty, callee_ptr, args, name);
  
  // 5. Create deferred OopMap
  OopMap* oopmap = new OopMap(frame_size, arg_size);
  if (is_object_field) {
    oopmap->set_reg(OopMap::kReturnRegister);  // x0 contains oop
  }
  function()->add_deferred_oopmap(virtual_offset, oopmap);
  
  return call;
}
```

#### 3. Native Method Wrappers

**Modify**: `YuhuNativeWrapper::initialize()` (yuhuNativeWrapper.cpp:194)

```cpp
void YuhuNativeWrapper::initialize() {
  // ... existing setup code ...
  
  // 1. Get unique virtual offset
  int virtual_offset = code_buffer()->create_unique_offset();
  
  // 2. Create dual virtual addresses
  uint64_t last_java_pc_va = 0xDEAD0000 | virtual_offset;
  uint64_t call_target_va = 0xBEEF0000 | virtual_offset;
  
  // 3. Store placeholder for last_Java_pc
  Value* ljpc_placeholder = ConstantInt::get(i64_ty, last_java_pc_va);
  builder()->CreateStore(ljpc_placeholder, last_Java_pc_addr());
  
  // 4. Create the native call using virtual address as call target
  Value* callee_ptr = CreateIntToPtr(
    ConstantInt::get(i64_ty, call_target_va), 
    func_type->getPointerTo());
  Value *result = builder()->CreateCall(func_type, callee_ptr, param_values);
  
  // 5. Create deferred OopMap
  OopMap* oopmap = create_oopmap_for_native_wrapper();
  function()->add_deferred_oopmap(virtual_offset, oopmap);
  
  // ... rest of wrapper code ...
}
```

#### 4. Other Runtime Calls

**Apply same pattern to**:
- `CreateInlineOop()`
- `CreateInlineMetadata()`
- Inline cache miss handlers
- Deoptimization calls
- Any other call that can trigger safepoint

**Pattern** (for all call types):
1. Get unique virtual offset: `code_buffer()->create_unique_offset()`
2. Create dual virtual addresses:
   - `last_java_pc_va = 0xDEAD0000 | virtual_offset`
   - `call_target_va = 0xBEEF0000 | virtual_offset`
3. Store `last_java_pc_va` as placeholder for `last_Java_pc`
4. Use `call_target_va` as the call target (via `CreateIntToPtr`)
5. Create deferred OopMap: `function()->add_deferred_oopmap(virtual_offset, oopmap)`

### Phase 2: adr + Marker Approach for Last Java PC

**Problem**: Current `CreateSetLastJavaFrame()` uses `adr x0, .` which reads the PC of the `adr` instruction itself, NOT the return address after the call.

**Solution**: Use `adr` instruction with marker pattern to store the return address, then patch the `adr` offset after code generation.

#### Why adr Instead of movz/movk?

**advantage of adr**:
- `adr` is **PC-relative** - calculates `target = current_PC + offset`
- When code is copied from CodeBuffer to nmethod, `adr` still works correctly
- Both the `adr` instruction and the target move by the same amount, so the relative offset stays correct
- No need to patch again after copying to nmethod

**Why we still need patching**:
- At IR generation time, we don't know where the `blr` instruction will be
- We generate `adr` with a dummy offset pointing to a temporary location
- After JITLink compilation, we calculate the actual offset to the return address (after `blr`)
- We patch the `adr` instruction with the correct PC-relative offset

#### How It Works

**At IR generation time**:
```llvm
; Use inline asm to generate marker + adr instruction
call void asm sideeffect "
  mov w19, #0xDEAD\n\t         @ Marker magic
  movk w19, #0x0001, lsl #16\n\t @ Marker for last_Java_pc
  adr x20, 1f\n\t               @ adr to label (dummy offset initially)
  str x20, [$0]\n\t              @ Store to last_Java_pc
  1:\n\t                         @ Label (will be after blr)
", "r"(i64* %last_Java_pc_addr)

; The actual call
%callee_ptr = inttoptr i64 %call_target to ptr
call void %callee_ptr()
```

**LLVM generates machine code**:
```assembly
; Marker + adr instruction (generated by inline asm)
Offset 0x100: mov w19, #0xDEAD              ; Marker magic
Offset 0x104: movk w19, #0x0001, lsl #16    ; Marker identifier
Offset 0x108: adr x20, #8                   ; PC-relative offset (dummy, will be patched)
Offset 0x10C: str x20, [x28, #last_Java_pc] ; Store address to thread

; Call setup and execution
Offset 0x110: ... setup arguments ...
Offset 0x114: bl helper_function             ; The actual call
Offset 0x118: ... (return address - where adr should point)
```

**Key insight**: The `adr` instruction at 0x108 currently points to 0x110 (offset +8), but it should point to 0x118 (offset +16, the return address after `bl`).

**After code generation**, we have:
- Stack map record: `StatepointID 5 → InstructionOffset 0x114` (points to bl)
- Deferred OopMap: `virtual_offset 0x1000 → OopMap_A`
- **Goal**: Find the marker, patch the `adr` instruction to point to 0x118 (return address after bl)

#### The Correlation Problem

**StackMapRecord does NOT contain call target information**:
```cpp
struct StackMapRecord {
  uint64_t StatepointID;        // Just a number (e.g., 5, 12, 18...)
  uint64_t InstructionOffset;   // PC offset of the statepoint call (e.g., 0x110)
  uint16_t NumLocations;        // Number of live oop locations
  StackMapLocation Locations[]; // Where the live oops are
  // NO callee function address!
  // NO callee function name!
  // NO correlation to original IR call!
};
```

#### Solution: Backward Scanning from Statepoint Call

**Algorithm**:
1. For each `StackMapRecord.InstructionOffset` (e.g., 0x114, points to `blr`)
2. Scan backwards in machine code to find **both** placeholders:
   - `0xDEADxxxx` (last_Java_pc placeholder)
   - `0xBEEFxxxx` (call target placeholder)
3. Verify they share the same `virtual_offset` (low 16 bits match)
4. Now we have the complete 1-1-1 mapping:
   - `virtual_offset 0x1000 → StatepointID 5 → InstructionOffset 0x114`
   - Both placeholders can be patched with actual values

**Why this works**:
- The `str` to `last_Java_pc` MUST precede the statepoint call
- Distance is predictable (within ~20-40 bytes)
- Virtual address pattern (`0xDEADxxxx`) is unique and easily identifiable
- No ordering assumptions needed
- No metadata preservation required

### Phase 3: Machine Code Scanning & Virtual Address Extraction

**Task**: For each statepoint, find the corresponding virtual address placeholder by scanning backwards.

#### AArch64 Instruction Pattern

**Expected pattern** for `store i64 0xDEAD1000, i64* %last_Java_pc_addr`:

```assembly
Offset N+0: movz xR, #0xDEAD, lsl #32    ; Load high 16 bits with shift
Offset N+4: movk xR, #0x1000             ; Load low 16 bits
Offset N+8: str xR, [x0, #last_Java_pc_offset]  ; Store to thread->last_Java_pc
```

**Total**: 12 bytes (3 instructions)

#### Scanning Algorithm

```cpp
struct PlaceholderInfo {
  uint64_t last_java_pc_va;         // e.g., 0xDEAD1000
  uint64_t last_java_pc_offset;     // Offset of movz instruction
  uint64_t call_target_va;          // e.g., 0xBEEF1000
  uint64_t call_target_offset;      // Offset of movz instruction
  uint64_t virtual_offset;          // e.g., 0x1000 (shared by both)
};

PlaceholderInfo scan_backwards_for_placeholders(
  const uint8_t* code_buffer,
  uint64_t statepoint_call_offset,
  uint64_t max_scan_distance = 100  // bytes
) {
  PlaceholderInfo result = {};
  uint64_t scan_start = statepoint_call_offset;
  uint64_t scan_end = (statepoint_call_offset > max_scan_distance) 
                      ? statepoint_call_offset - max_scan_distance : 0;
  
  bool found_ljpc = false;
  bool found_call_target = false;
  
  // Scan backwards from statepoint call
  for (uint64_t offset = scan_start; offset >= scan_end; offset -= 4) {
    uint32_t inst = *(uint32_t*)(code_buffer + offset);
    
    // Check for movz with lsl #32 (potential placeholder load)
    if ((inst & 0xFF800000) == 0xD2800000) {  // movz with shift
      uint32_t imm16 = (inst >> 5) & 0xFFFF;
      uint32_t shift = (inst >> 21) & 0x3;
      
      if (shift == 2) {  // lsl #32
        // Check high 16 bits for magic numbers
        if (imm16 == 0xDEAD) {
          // Found last_Java_pc placeholder's movz
          // Extract low 16 bits from preceding movk
          uint32_t movk_inst = *(uint32_t*)(code_buffer + offset + 4);
          if ((movk_inst & 0xFF800000) == 0xF2800000) {  // movk
            uint32_t low16 = (movk_inst >> 5) & 0xFFFF;
            result.last_java_pc_va = ((uint64_t)imm16 << 32) | low16;
            result.last_java_pc_offset = offset;
            result.virtual_offset = low16;
            found_ljpc = true;
          }
        } else if (imm16 == 0xBEEF) {
          // Found call target placeholder's movz
          uint32_t movk_inst = *(uint32_t*)(code_buffer + offset + 4);
          if ((movk_inst & 0xFF800000) == 0xF2800000) {  // movk
            uint32_t low16 = (movk_inst >> 5) & 0xFFFF;
            result.call_target_va = ((uint64_t)imm16 << 32) | low16;
            result.call_target_offset = offset;
            // Verify same virtual_offset
            if (result.virtual_offset == 0) {
              result.virtual_offset = low16;
            } else if (result.virtual_offset != low16) {
              report_error("Mismatched virtual_offsets!");
            }
            found_call_target = true;
          }
        }
      }
    }
    
    // Stop if we found both placeholders
    if (found_ljpc && found_call_target) {
      return result;
    }
  }
  
  report_error("Could not find both placeholders for statepoint at offset " + 
               std::to_string(statepoint_call_offset));
  return {};
}
```

#### Matching Algorithm

```cpp
void match_statepoints_to_virtual_offsets() {
  for (auto& record : stack_map_records) {
    uint64_t statepoint_id = record.StatepointID;
    uint64_t call_offset = record.InstructionOffset;  // Points to blr instruction
    
    // Scan backwards to find BOTH placeholders
    PlaceholderInfo info = scan_backwards_for_placeholders(
      code_buffer, 
      call_offset,
      max_scan_distance = 100  // bytes
    );
    
    if (!info.last_java_pc_va || !info.call_target_va) {
      report_error("Could not find both placeholders for statepoint " + 
                   std::to_string(statepoint_id));
      continue;
    }
    
    // Verify 1-1-1 relationship
    uint64_t ljpc_virtual_offset = info.last_java_pc_va & 0x0000FFFF;
    uint64_t call_virtual_offset = info.call_target_va & 0x0000FFFF;
    
    assert(ljpc_virtual_offset == call_virtual_offset);
    assert((info.last_java_pc_va & 0xFFFF0000) == 0xDEAD0000);
    assert((info.call_target_va & 0xFFFF0000) == 0xBEEF0000);
    
    // Build 1-1-1 mapping
    mapping[ljpc_virtual_offset] = {
      .statepoint_id = statepoint_id,
      .instruction_offset = call_offset,
      .last_java_pc_va = info.last_java_pc_va,
      .last_java_pc_placeholder_offset = info.last_java_pc_offset,
      .call_target_va = info.call_target_va,
      .call_target_placeholder_offset = info.call_target_offset
    };
  }
}
```

#### Placeholder Detection

**Scan backwards looking for**:
1. `str xR, [x0, #last_Java_pc_offset]` instruction
2. Verify the stored value came from `movz` + `movk` pair
3. Reconstruct the 64-bit value from the two 16-bit immediates
4. Check if value is in range `0xDEAD0000 - 0xDEADFFFF`

**AArch64 instruction encodings**:
- `movz` with `lsl #32`: `0xD2800000 | (imm16 << 5) | rd | (2 << 21)`
- `movk` with no shift: `0xF2800000 | (imm16 << 5) | rd | (0 << 21)`
- `str` immediate: `0xF9000000 | (imm12 << 10) | rn | rt`

#### adr Instruction Patching

**After JITLink compilation**, the OopMapExtractorPlugin patches the `adr` instruction:

```cpp
void patch_adr_for_last_java_pc() {
  for (auto& [virtual_offset, info] : matched_mappings) {
    // 1. Find the marker pattern (mov w19, #0xDEAD; movk w19, #0x0001, lsl #16)
    uint32_t* marker_addr = scan_for_marker(code_buffer, call_offset);
    if (!marker_addr) continue;
    
    // 2. Find the adr instruction (4 bytes after marker start)
    uint32_t* adr_instr = marker_addr + 2;  // Offset 0x108 in example
    
    // 3. Calculate the actual return address
    uint64_t blr_address = info.instruction_offset;  // 0x114
    uint64_t return_address = blr_address + 4;       // 0x118 (after bl)
    
    // 4. Calculate PC-relative offset for adr
    uint64_t adr_address = (uint64_t)adr_instr;
    int64_t pc_offset = return_address - adr_address;  // Should be +16
    
    // 5. Patch the adr instruction
    // ADR encoding: bits 5-23 = immhi, bits 29-30 = immlo
    uint32_t new_adr = 0x10000000;  // ADR opcode
    new_adr |= ((pc_offset & 0x3) << 29);        // immlo (bits 0-1)
    new_adr |= ((pc_offset & 0x7FFFC) << 3);    // immhi (bits 2-20)
    new_adr |= 20;  // x20 (destination register)
    
    *adr_instr = new_adr;
    
    // Example:
    // Before: adr x20, #8   (0x10000148)
    // After:  adr x20, #16  (0x10000288)
  }
}
```

**AArch64 adr encoding**:
```
adr xd, label
= 0001 0000 | immlo (2 bits) | 00000 | immhi (19 bits) | rd (5 bits)
= bits 30-29: 00
= bit 28: 0
= bits 27-24: 0001
= bits 23-5: 21-bit PC-relative offset (immhi:immlo)
= bits 4-0: rd (destination register)
```

**Key benefit**: Once patched, the `adr` instruction is position-independent. When the code is copied from CodeBuffer to nmethod, the `adr` still calculates the correct return address because it's PC-relative.

#### Key Changes from Old Approach

1. **Use adr instead of movz/movk**: `adr` is PC-relative, works after code copying
2. **Marker-based identification**: Unique marker pattern identifies the `adr` instruction to patch
3. **No call target matching**: Don't need to match helper addresses for last_Java_pc
4. **No ordering assumptions**: Don't rely on sequential ordering of statepoints
5. **Marker scanning**: Scan for marker pattern instead of backward scanning from statepoint
6. **Single patching**: Patch `adr` once during JITLink, no need to patch again after nmethod copy

### Phase 4: Patch adr Instruction & Register OopMaps

**After matching virtual offsets to statepoint IDs**, patch the `adr` instruction and register OopMaps:

#### Task 1: Patch adr Instruction for Last Java PC

```cpp
void patch_adr_instructions() {
  for (auto& [virtual_offset, info] : matched_mappings) {
    // 1. Find marker pattern near the statepoint call
    uint32_t* marker_addr = scan_for_marker(
      code_buffer, 
      info.instruction_offset,
      max_scan_distance = 100
    );
    if (!marker_addr) continue;
    
    // 2. The adr instruction is at marker_addr + 2 (8 bytes after marker start)
    uint32_t* adr_instr = marker_addr + 2;
    
    // 3. Calculate return address (after bl instruction)
    uint64_t bl_address = info.instruction_offset;
    uint64_t return_address = bl_address + 4;
    
    // 4. Calculate PC-relative offset
    uint64_t adr_address = (uint64_t)adr_instr;
    int64_t pc_offset = return_address - adr_address;
    
    // 5. Verify offset fits in 21-bit signed range (-1MB to +1MB)
    assert(pc_offset >= -0x100000 && pc_offset < 0x100000);
    
    // 6. Encode new adr instruction
    uint32_t new_adr = 0x10000000;  // ADR opcode base
    new_adr |= ((pc_offset & 0x3) << 29);         // immlo (bits 0-1 of offset)
    new_adr |= (((pc_offset >> 2) & 0x7FFFF) << 5); // immhi (bits 2-20 of offset)
    new_adr |= 20;  // x20 as destination register
    
    // 7. Patch the instruction
    *adr_instr = new_adr;
    
    // Example:
    // Before: adr x20, #8   (points to instruction after adr)
    // After:  adr x20, #16  (points to return address after bl)
  }
}
```

**Why patch adr?**: The `adr` instruction needs to point to the return address after the `bl` instruction, not to some temporary location. Once patched with the correct PC-relative offset, `adr` will calculate the correct absolute address at runtime, even after the code is copied to nmethod.

#### Task 2: Patch Call Target Placeholder

```cpp
void patch_call_target_placeholders() {
  for (auto& [virtual_offset, info] : matched_mappings) {
    uint64_t call_target_placeholder_offset = info.call_target_placeholder_offset;
    
    // Get the actual helper function address for this call site
    void* real_helper_addr = lookup_helper_address(virtual_offset);
    
    // Patch the movz/movk instructions with actual helper address
    patch_movz_movk_instructions(
      code_buffer,
      call_target_placeholder_offset,
      (uint64_t)real_helper_addr
    );
    
    // Example:
    // Before: movz x9, #0xBEEF, lsl #32 / movk x9, #0x1000
    // After:  movz x9, #0x1234, lsl #32 / movk x9, #0x5678
    //         (real_helper_addr = 0x12345678, e.g., yuhu_resolve_static_field)
  }
}
```

**Why patch?**: The call target placeholder (0xBEEF1000) is not a valid function address. We must replace it with the actual helper function address obtained via `dlsym()` or lookup table.

#### Task 3: Register OopMaps at Exact Offsets

```cpp
void register_oopmaps() {
  for (auto& [virtual_offset, info] : matched_mappings) {
    uint64_t actual_offset = info.instruction_offset;
    
    // Get deferred OopMap by virtual_offset
    OopMap* oopmap = function()->get_deferred_oopmap(virtual_offset);
    
    // Register with HotSpot using exact offset from stack map
    debug_info->add_safepoint(actual_offset, oopmap);
    
    // Stack map also tells us where live oops are located
    StackMapRecord& record = info.stack_map_record;
    for (auto& location : record.Locations) {
      if (location.is_register()) {
        oopmap->set_reg(location.get_register());
      } else if (location.is_direct()) {
        oopmap->set_oop(location.get_stack_offset());
      }
    }
  }
}
```

**Key workflow**:
1. Parse `__llvm_stackmaps` section → get StatepointID + InstructionOffset
2. Scan backwards from InstructionOffset → find BOTH placeholders (0xDEADxxxx + 0xBEEFxxxx)
3. Verify they share the same virtual_offset (1-1-1 relationship)
4. Patch last_Java_pc placeholder with actual return offset
5. Patch call target placeholder with actual helper function address
6. Register OopMap at InstructionOffset with exact oop locations from stack map

## AArch64 Instruction Encodings

### movz (Move with Zero)
```
Encoding: 0xD2800000 | (imm16 << 5) | rd | (shift << 21)
Shift values: 0=lsl #0, 1=lsl #16, 2=lsl #32, 3=lsl #48
```

### movk (Move and Keep)
```
Encoding: 0xF2800000 | (imm16 << 5) | rd | (shift << 21)
```

### blr (Branch with Link to Register)
```
Encoding: 0xD63F0000 | rn
```

### Pattern for 48-bit Address Load
```assembly
movz x8, #imm1, lsl #32    // Bits 47-32
movk x8, #imm2, lsl #16    // Bits 31-16
movk x8, #imm3             // Bits 15-0
blr x8
```

Total: 4 instructions × 4 bytes = 16 bytes

## OopMap Completeness: Solving the LLVM Register Allocator Gap

### The Core Problem

**C1/C2 approach**: C1's LinearScan register allocator (c1_LinearScan.cpp:2376-2442) tracks live intervals for all values. At each safepoint, it walks active intervals and marks their locations (register or spill slot) in the OopMap. C1 controls register allocation, so it knows ALL oop locations.

**Yuhu's challenge**: LLVM's register allocator is a black box that runs after IR generation. We have NO visibility into:
- Which physical registers hold oops
- Which stack slots LLVM auto-spills contain oops
- Whether an oop is in x0, x19, or [sp+80]

**The risk**: If LLVM keeps an oop in a register or auto-spill slot across a safepoint, and GC moves the object, that location contains a stale address → crash.

### Solution: LLVM's Statepoint Infrastructure (PlaceSafepoints + RewriteStatepointsForGC)

**LLVM's official GC support mechanism** provides complete oop tracking without manual force-spill:

#### Step 1: Mark Functions with GC Strategy

```cpp
// At function creation time
F.setGC("statepoint-example");
```

Produces IR:
```llvm
define ptr addrspace(1) @test(ptr addrspace(1) %obj) gc "statepoint-example" {
  ...
}
```

#### Step 2: Use Non-Integral Pointer Types for Oops

Change all oop types from `i8*` to `ptr addrspace(1)`:
```llvm
; Old: Yuhu generates i8* for oops
%obj = call i8* @new_instance()

; New: Use GC-tracked pointer type
%obj = call ptr addrspace(1) @new_instance()
```

#### Step 3: PlaceSafepoints Pass - Automatic Safepoint Insertion

**What it does**: Automatically inserts safepoint polls at:
- Method entry
- Loop backedges

**How it works**: Looks for a function named `gc.safepoint_poll` and calls it at appropriate locations.

**Example**:
```llvm
; Before PlaceSafepoints
define void @loop(ptr addrspace(1) %obj) gc "statepoint-example" {
loop:
  %val = call ptr addrspace(1) @process(ptr addrspace(1) %obj)
  br label %loop
}

; After PlaceSafepoints
define void @loop(ptr addrspace(1) %obj) gc "statepoint-example" {
entry:
  call void @gc.safepoint_poll()  ; ← Method entry poll
  br label %loop

loop:
  %val = call ptr addrspace(1) @process(ptr addrspace(1) %obj)
  call void @gc.safepoint_poll()  ; ← Backedge poll (auto-inserted!)
  br label %loop
}
```

**For Yuhu**: Implement `gc.safepoint_poll()` to check safepoint state using `do_call_back()`:
```cpp
extern "C" void gc_safepoint_poll() {
  // Use do_call_back() which checks (_state != _not_synchronized)
  // This catches both _synchronizing and _synchronized states
  if (SafepointSynchronize::do_call_back()) {
    SafepointSynchronize::block(JavaThread::current());
  }
}
```

#### Step 4: RewriteStatepointsForGC Pass - Complete Oop Tracking

**What it does**: Wraps ALL calls (including `gc.safepoint_poll`) in statepoint relocation sequences.

**Transformation example**:
```llvm
; Before RewriteStatepointsForGC
call void @gc.safepoint_poll()
%result = call ptr addrspace(1) @process(ptr addrspace(1) %obj)

; After RewriteStatepointsForGC
%token = call token (i64, i32, ptr, i32, i32, ...) 
  @llvm.experimental.gc.statepoint.p0f_isVoidf(
    i64 0,           ; statepoint ID
    i32 0,           ; flags
    ptr @gc.safepoint_poll,
    i32 0, i32 0, i32 0, i32 0)
  ["gc-live" (ptr addrspace(1) %obj)]  ; ← Marks %obj as live oop!

%obj.relocated = call ptr addrspace(1) 
  @llvm.experimental.gc.relocate.p1(
    token %token, i32 0, i32 0)

%token2 = call token (i64, i32, ptr, i32, i32, ...)
  @llvm.experimental.gc.statepoint.p0f_isPtrf(
    i64 1, i32 0, ptr @process, 
    i32 1, i32 0, i32 1, i32 0,
    ptr addrspace(1) %obj.relocated)
  ["gc-live" (ptr addrspace(1) %obj.relocated)]

%result.relocated = call ptr addrspace(1)
  @llvm.experimental.gc.relocate.p1(token %token2, i32 0, i32 0)

%result = call ptr addrspace(1)
  @llvm.experimental.gc.result.p1(token %token2)
```

**Key features**:
1. **Automatic liveness tracking**: LLVM identifies ALL live `ptr addrspace(1)` values at each call
2. **Explicit relocation**: `gc.relocate` creates new SSA values for post-safepoint uses
3. **Complete OopMap**: LLVM generates stack maps with exact locations (register or stack slot) for every live oop
4. **Handles auto-spills**: LLVM tracks oops even in its own spill slots

#### Step 5: Parse LLVM Stack Maps

After code generation, LLVM emits `__llvm_stackmaps` section containing:
- Statepoint ID
- Instruction offset
- Location of each live oop (register or stack slot)

**Parse this section to build HotSpot OopMaps**:
```cpp
struct StackMapHeader {
  uint8_t Version;
  uint8_t Reserved[3];
  uint32_t NumFunctions;
  uint32_t NumConstants;
  uint64_t Constants[];
};

struct StackMapRecord {
  uint64_t StatepointID;
  uint64_t InstructionOffset;
  uint16_t NumLocations;
  uint16_t Padding;
  uint32_t NumOpSlots;
  uint16_t NumPatchBytes;
  uint16_t NumDunwindOpSlots;
  StackMapLocation Locations[];  ; Register or stack slot for each oop
};
```

#### Integration with Yuhu

**Pass pipeline order**:
```
1. Yuhu IR generation (use ptr addrspace(1) for oops)
2. PlaceSafepoints (insert gc.safepoint_poll at entry/backedges)
3. RewriteStatepointsForGC (wrap calls in statepoints, insert gc.relocate)
4. Standard LLVM optimization passes
5. Code generation
6. Parse __llvm_stackmaps section
7. Build HotSpot OopMaps from stack map records
8. Register OopMaps with DebugInformationRecorder
```

**What changes in Yuhu**:
1. **IR generation**: Use `ptr addrspace(1)` instead of `i8*` for all oops
2. **Function attribute**: Call `F.setGC("statepoint-example")` on all Yuhu functions
3. **Remove manual safepoints**: Remove `maybe_add_backedge_safepoint()` (PlaceSafepoints handles it)
4. **Remove decache/cache**: No longer needed (RewriteStatepointsForGC inserts gc.relocate)
5. **Stack map parsing**: Add code to parse `__llvm_stackmaps` section after JITLink
6. **OopMap registration**: Convert stack map records to HotSpot OopMaps

**Pros**:
- ✅ Complete oop tracking (including LLVM auto-spills)
- ✅ Automatic safepoint placement (method entry + backedges)
- ✅ LLVM handles all liveness analysis
- ✅ No manual force-spill overhead
- ✅ Official LLVM GC support (used by production JVMs)
- ✅ Works with any LLVM optimization

**Cons**:
- ⚠️ Major architectural change (all oop types must change)
- ⚠️ Requires implementing `gc.safepoint_poll()` function
- ⚠️ Need to parse LLVM stack maps and convert to HotSpot OopMaps
- ⚠️ Need to integrate gc.relocate values into `current_state()` tracking

## Backedge Safepoints: Handled by PlaceSafepoints Pass

### The Current Gap

**Current implementation** (yuhuTopLevelBlock.cpp:700-710):

```cpp
void YuhuTopLevelBlock::maybe_add_backedge_safepoint() {
  if (current_state()->has_safepointed())
    return;
  
  for (int i = 0; i < num_successors(); i++) {
    if (successor(i)->can_reach(this)) {
      maybe_add_safepoint();  // ← Manual safepoint insertion
      break;
    }
  }
}
```

**Problems**:
1. No decache/cache around safepoint (oops may be in registers/auto-spills)
2. Only handles backedges, not method entry
3. Relies on manual tracking which is incomplete

### The Solution: Remove Manual Safepoint Insertion

**With PlaceSafepoints + RewriteStatepointsForGC**:
1. **Remove** `maybe_add_backedge_safepoint()` entirely
2. **Implement** `gc.safepoint_poll()` function
3. **Run** PlaceSafepoints pass (automatically inserts polls at entry + backedges)
4. **Run** RewriteStatepointsForGC pass (wraps polls in statepoints, inserts gc.relocate)

**Result**: Complete oop tracking at ALL safepoints without manual decache/cache.

## Implementation Plan

### Step 1: Replace ADR with Virtual Address Placeholders in All Call Sites
- Modify `YuhuTopLevelBlock::call_vm()` to store virtual address placeholder
- Modify `YuhuBuilder::CreateInlineOopForStaticField()` to store virtual address placeholder
- Modify `YuhuBuilder::CreateInlineOop()` to store virtual address placeholder
- Modify `YuhuBuilder::CreateInlineMetadata()` to store virtual address placeholder
- Modify `YuhuNativeWrapper::initialize()` to store virtual address placeholder
- Change deferred OopMap registry to use `uint64_t virtual_address` as key (not `int virtual_offset`)

### Step 2: Remove Old Offset Marker Infrastructure
- Remove `YuhuBuilder::CreateOffsetMarker()` (yuhuBuilder.cpp:1159-1207)
- Remove `YuhuBuilder::scan_and_update_offset_markers()` (yuhuBuilder.cpp:1209-1310)
- Remove offset marker creation in `YuhuCacheDecache::start_frame()`
- Remove or repurpose `YuhuOffsetMapper` class

### Step 3: Implement Virtual Address to Statepoint Matching
- Implement `scan_backwards_for_placeholder()` to find `str` instruction
- Implement `extract_virtual_address()` to decode movz/movk instructions
- Implement `match_statepoints_to_virtual_addresses()` correlation algorithm
- Test with simple cases (1 call, 2 calls, multiple calls)

### Step 4: Implement Placeholder Patching
- Implement `patch_movz_movk_instructions()` to replace virtual address with actual offset
- Verify patched machine code is correct (movz/movk encoding)
- Ensure patched values are visible to GC (memory barriers if needed)

### Step 5: Integrate LLVM Statepoint Infrastructure
- Change all oop types from `i8*` to `ptr addrspace(1)`
- Set `gc "statepoint-example"` attribute on all Yuhu functions
- Implement `gc.safepoint_poll()` function
- Remove `maybe_add_backedge_safepoint()` (PlaceSafepoints handles it)
- Add PlaceSafepoints pass to LLVM pass pipeline
- Add RewriteStatepointsForGC pass to LLVM pass pipeline

### Step 6: JITLink Plugin for Stack Map Parsing & OopMap Registration
- Create `OopMapExtractorPlugin` to parse `__llvm_stackmaps` section
- Extract StatepointID + InstructionOffset from stack map records
- Call `match_statepoints_to_virtual_addresses()` to build correlation
- Call `patch_virtual_address_placeholders()` to update machine code
- Call `register_oopmaps()` to register with DebugInformationRecorder
- Integrate with ObjectLinkingLayer as plugin

### Step 7: Testing
- Run existing test suite to verify no regressions
- Test GC during static field access (original crash scenario)
- Test stack traversal with multiple VM calls
- Verify OopMap coverage for all VM call sites
- Test GC during loop backedges (statepoint correctness)
- Test object movement across safepoints (gc.relocate correctness)
- Verify `__llvm_stackmaps` section contains all live oops
- Test virtual address patching correctness
- Verify GC can find OopMap at `last_Java_pc` offset

## Key Design Decisions

### Decision 1: Why Not Offset Markers?
**Rejected**: User preference against offset marker approach.

**Alternative**: Use virtual address placeholders that are scannable in machine code.

### Decision 2: Why Not Wrapper Functions?
**Rejected**: Each wrapper adds 2 extra instructions (bl + ret), causing performance degradation.

**Critical issue**: Wrapper functions break stack frame walking because they're part of nmethod CodeBlob, change LR, and have 0 frame_size.

**Alternative**: Direct call scanning with backward search from statepoint.

### Decision 3: Why Not Encode Offset in Address?
**Rejected**: Cannot encode metadata into function addresses without breaking the call (address must be valid pointer).

**Alternative**: Use virtual address encoding (`0xDEAD0000 | virtual_offset`) for placeholder stores.

### Decision 4: Why Not Custom Metadata?
**Rejected**: Empirical testing shows RewriteStatepointsForGC does NOT preserve custom metadata on call instructions (test results: 0/3 metadata annotations preserved).

**Alternative**: Scan backwards from statepoint call to find virtual address placeholder.

### Decision 5: Why Virtual Address Instead of Statepoint ID?
**Reason**: Statepoint IDs are only assigned by RewriteStatepointsForGC AFTER IR generation. We need a correlation key that's known at IR time.

**Solution**: Virtual addresses are created at IR time (`0xDEAD0000 | virtual_offset`), stored as placeholders, and found by scanning backwards from statepoint calls.

### Decision 6: Why Backward Scanning?
**Reason**: StackMapRecord does NOT contain call target information or any link to the original IR call. We cannot match by statepoint ID directly.

**Solution**: Scan backwards from `StackMapRecord.InstructionOffset` to find the `str` instruction that stores the virtual address placeholder. The distance is predictable (~20-40 bytes) and the pattern is unique.

**Direct call** (bl): Used for known functions at link time
**Indirect call** (blr): Used for function pointers from `inttoptr` constants

### Decision 5: Why Use LLVM Statepoint Infrastructure Instead of Manual Force-Spill? (NEW)
**Reason**: Manual force-spill cannot track LLVM auto-spill slots, leading to incomplete OopMaps.

**Statepoint approach**:
- **Pros**: Complete oop tracking (including auto-spills), automatic safepoint placement, official LLVM GC support, used by production JVMs
- **Cons**: Major architectural change (all oop types must change), requires stack map parsing
- **Status**: Selected solution, replaces force-spill approach

## Code to Remove

After implementing the new OopMap mechanism, the following old code should be removed or replaced:

### 1. Offset Marker Infrastructure (REMOVED)

**Files**: hotspot/src/share/vm/yuhu/yuhuBuilder.cpp, yuhuBuilder.hpp, yuhuCacheDecache.cpp

**Remove**:
- `YuhuBuilder::CreateOffsetMarker()` (yuhuBuilder.cpp:1159-1207)
  - Creates inline asm markers with magic number 0xDEADBEEF
  - No longer needed: using virtual address placeholders instead
  
- `YuhuBuilder::scan_and_update_offset_markers()` (yuhuBuilder.cpp:1209-1310)
  - Scans machine code for offset marker patterns
  - Has bugs: looks for ADR instruction instead of return address after BL/BLR
  - Replaced by: `match_statepoints_to_virtual_addresses()` in JITLink plugin
  
- `YuhuBuilder::insert_offset_marker()` declaration (yuhuBuilder.hpp)
- Offset marker creation in `YuhuCacheDecache::start_frame()`

### 2. ADR-Based last_Java_pc Setting (REPLACED)

**Files**: hotspot/src/share/vm/yuhu/yuhuStack.hpp, yuhuStack.cpp

**Replace**:
- `YuhuStack::CreateReadCurrentPC()` - Uses ADR instruction (WRONG!)
- `YuhuStack::CreateSetLastJavaFrame()` - Calls CreateReadCurrentPC (WRONG!)

**New implementation**:
- Direct store of virtual address placeholder to `thread->last_Java_pc`
- No ADR instruction needed
- Patching happens after code generation when actual offsets are known

- `YuhuBuilder::insert_offset_marker()` (yuhuBuilder.hpp:267)
  - Declaration for offset marker insertion
  - No longer needed

- `YuhuDecacher::start_frame()` offset marker creation (yuhuCacheDecache.cpp:39-45)
  ```cpp
  _pc_offset = code_buffer()->create_unique_offset();
  builder()->CreateOffsetMarker(_pc_offset);  // ← REMOVE THIS
  ```
  - Replaced by: virtual offset tracking with deferred OopMap registry

**Keep**:
- `YuhuOffsetMapper` class (yuhuOffsetMapper.hpp)
  - Still needed for virtual → actual offset mapping
  - But populated by JITLink plugin instead of marker scanning

### 2. Incorrect Last Java PC Implementation

**File**: hotspot/src/share/vm/yuhu/yuhuStack.hpp

**Replace**:
- `CreateSetLastJavaFrame()` (yuhuStack.hpp:193-250)
  - Current implementation uses `CreateReadCurrentPC()` which generates `adr x0, .`
  - This reads the PC of the ADR instruction itself, NOT the return address
  - **Replace with**: Placeholder mechanism that gets patched after compilation
  
**Specific code to remove** (yuhuStack.hpp:220-247):
```cpp
// CRITICAL: Also update last_Java_pc for proper stack walking during VM calls
// Use inline assembly like in initialize() to prevent LLVM optimization
// Get current PC using CreateReadCurrentPC which reads the current program counter
llvm::Value* current_pc = builder()->CreateReadCurrentPC();  // ← REMOVE THIS

// Convert PC address to i64 for inline assembly
llvm::Value *pc_addr_i64 = builder()->CreatePtrToInt(last_Java_pc_addr(), YuhuType::intptr_type(), "pc_addr_i64");

// Use the same inline assembly pattern as in initialize()
YuhuContext& ctx2 = YuhuContext::current();
llvm::FunctionType* store_asm_type2 = llvm::FunctionType::get(
  llvm::Type::getVoidTy(ctx2),
  {YuhuType::intptr_type(), YuhuType::intptr_type()},  // addr, value
  false);

llvm::InlineAsm* store_pc_asm = llvm::InlineAsm::get(
  store_asm_type2,
  "str $1, [$0]",  // AArch64: store value ($1) to address ($0)
  "r,r,~{memory}",  // Both inputs in registers, clobber memory
  true,            // Has side effects: yes (writes to memory)
  false,           // Is align stack: no
  llvm::InlineAsm::AD_ATT
);

std::vector<llvm::Value*> store_pc_args;
store_pc_args.push_back(pc_addr_i64);
store_pc_args.push_back(current_pc);  // ← WRONG: This is ADR's PC, not return address!
builder()->CreateCall(store_asm_type2, store_pc_asm, store_pc_args);
```

**Replace with**: Store placeholder value that will be patched by JITLink plugin

### 3. CreateReadCurrentPC() Usage for Last Java PC

**File**: hotspot/src/share/vm/yuhu/yuhuBuilder.cpp

**Question**: Can we remove `CreateReadCurrentPC()` entirely?

**Answer**: NO - it's still used in `YuhuStack::initialize()` at function entry (yuhuStack.cpp:218), which is correct for setting initial last_Java_pc.

**Keep**: `YuhuBuilder::CreateReadCurrentPC()` (yuhuBuilder.cpp:695-713)
- Used at function entry to set initial last_Java_pc
- Should NOT be used for VM call sites (that's the bug!)

### 4. Deferred OopMap Registration (Old Approach)

**File**: hotspot/src/share/vm/yuhu/yuhuFunction.cpp, yuhuCacheDecache.cpp

**Review and potentially replace**:
- `YuhuFunction::add_deferred_oopmap()` 
  - Current approach: stores OopMap with virtual offset, later relocated
  - **Keep if**: Still works with new JITLink-based offset mapping
  - **Replace if**: Need to change how virtual offsets are matched to call sites

- `YuhuBuilder::relocate_oopmaps()`
  - Current approach: uses offset mapper from marker scanning
  - **Replace with**: Uses offset mapper from JITLink plugin

### 5. ADR Instruction Scanning Logic

**File**: hotspot/src/share/vm/yuhu/yuhuBuilder.cpp

**Remove** (yuhuBuilder.cpp:1268-1295):
```cpp
// CRITICAL: Scan forward from the marker to find the 'adr' instruction
// that records the actual PC for last_java_pc
// The 'adr' instruction typically appears within 100 bytes after the marker
int actual_offset = marker_offset;  // Default to marker offset
bool found_adr = false;

for (size_t scan_offset = offset + 20; scan_offset < offset + 200 && scan_offset + 4 <= code_size; scan_offset += 4) {
  uint32_t inst = *((uint32_t*)(code_start + scan_offset));
  
  // Check if this is an ADR instruction
  // ADR encoding: |0|immlo|10000|immhi|Rd|
  // Mask: 1F000000, Pattern: 10000000
  if ((inst & ADR_MASK) == ADR_PATTERN) {  // ← WRONG LOGIC!
    actual_offset = scan_offset;
    found_adr = true;
    break;
  }
}
```

**Reason**: This scans for ADR instruction, but OopMap should be registered at return address AFTER BL/BLR, not at ADR instruction. Replaced by JITLink plugin scanning for movz/movk/blr patterns.

### 6. VM Call Wrapper (call_vm)

**Files**: hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.hpp

**Update**: `call_vm()` function (yuhuTopLevelBlock.hpp:298-327)

**Current implementation**:
```cpp
llvm::CallInst* call_vm(llvm::Value* callee, ...) {
  decache_for_VM_call();
  stack()->CreateSetLastJavaFrame();  // ← WRONG: Uses ADR instruction
  
  CallInst *res = builder()->CreateCall(func_type, callee, args);
  
  stack()->CreateResetLastJavaFrame();
  cache_after_VM_call();
  return res;
}
```

**Must be updated to**:
```cpp
llvm::CallInst* call_vm(llvm::Value* callee, ...) {
  decache_for_VM_call();
  
  // NEW: Get virtual offset and use placeholder mechanism
  int virtual_offset = code_buffer()->create_unique_offset();
  stack()->CreateSetLastJavaFrameWithPlaceholder(virtual_offset);
  
  CallInst *res = builder()->CreateCall(func_type, callee, args);
  
  // NEW: Create deferred OopMap
  function()->add_deferred_oopmap(virtual_offset, create_oopmap());
  
  stack()->CreateResetLastJavaFrame();
  cache_after_VM_call();
  return res;
}
```

**Impact**: This single change affects **ALL VM runtime calls** (~20-30 locations):
- Safepoint polls (yuhuTopLevelBlock.cpp:689)
- Exception handling (yuhuTopLevelBlock.cpp:626)
- Object allocation (yuhuTopLevelBlock.cpp:2034)
- Array operations (yuhuTopLevelBlock.cpp:2063, 2083, 2113)
- Monitor operations (yuhuTopLevelBlock.cpp:2232, 2294)
- Finalizers (yuhuTopLevelBlock.cpp:803)
- Exception throwing (yuhuTopLevelBlock.cpp:409, 418, 445, etc.)
- And many more...

**Keep**: The decache/cache logic (still needed for all VM calls)

## Summary of Code Changes

| Component | Location | Action | Reason |
|-----------|----------|--------|--------|
| `CreateOffsetMarker()` | yuhuBuilder.cpp:1159-1207 | **REMOVE** | Replaced by statepoint infrastructure |
| `scan_and_update_offset_markers()` | yuhuBuilder.cpp:1209-1310 | **REMOVE** | Replaced by stack map parsing |
| `insert_offset_marker()` | yuhuBuilder.hpp:267 | **REMOVE** | Part of old marker infrastructure |
| Offset marker in `start_frame()` | yuhuCacheDecache.cpp:45 | **REMOVE** | No longer creating markers |
| ADR-based PC setting | yuhuStack.hpp:220-247 | **REMOVE** | Replaced by stack map offsets |
| ADR scanning logic | yuhuBuilder.cpp:1268-1295 | **REMOVE** | Wrong instruction to scan for |
| `CreateReadCurrentPC()` | yuhuBuilder.cpp:695-713 | **KEEP** | Still needed for function entry |
| `YuhuOffsetMapper` | yuhuOffsetMapper.hpp | **REMOVE** | No longer needed (stack maps give exact offsets) |
| Placeholder patching | yuhuStack.hpp | **REMOVE** | No longer needed with statepoints |
| `add_deferred_oopmap()` | yuhuFunction.cpp | **UPDATE** | Key by statepoint ID instead of virtual offset |
| `call_vm()` | yuhuTopLevelBlock.hpp:298-327 | **SIMPLIFY** | Remove placeholder mechanism, use statepoint ID |
| `CreateInlineOopForStaticField()` | yuhuBuilder.cpp:943-1007 | **SIMPLIFY** | Remove placeholder mechanism |
| `YuhuNativeWrapper::initialize()` | yuhuNativeWrapper.cpp:194 | **SIMPLIFY** | Remove placeholder mechanism |
| All oop types | Throughout Yuhu | **CHANGE** | `i8*` → `ptr addrspace(1)` |
| `maybe_add_backedge_safepoint()` | yuhuTopLevelBlock.cpp | **REMOVE** | PlaceSafepoints handles it |
| `decache_for_VM_call()` / `cache_after_VM_call()` | yuhuTopLevelBlock.hpp | **REMOVE** | RewriteStatepointsForGC inserts gc.relocate |

## Files to Modify

1. **hotspot/src/share/vm/yuhu/yuhuBuilder.cpp**
   - Change all oop types from `i8*` to `ptr addrspace(1)`
   - Set `gc "statepoint-example"` attribute on all functions
   - Remove `CreateOffsetMarker()` and `scan_and_update_offset_markers()`
   
2. **hotspot/src/share/vm/yuhu/yuhuStack.hpp**
   - **REMOVE** placeholder mechanism from `CreateSetLastJavaFrame()`
   - Simplify to just store thread state (offsets come from stack maps)

3. **hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.hpp**
   - Remove `maybe_add_backedge_safepoint()` (PlaceSafepoints handles it)
   - Simplify `call_vm()` (remove placeholder, use statepoint ID)
   - Remove `decache_for_VM_call()` / `cache_after_VM_call()` (gc.relocate handles it)

4. **hotspot/src/share/vm/yuhu/yuhuORCPlugins.hpp/cpp**
   - Implement `OopMapExtractorPlugin` to parse `__llvm_stackmaps`
   - Extract exact offsets and oop locations from stack map records
   - Register OopMaps with DebugInformationRecorder

5. **hotspot/src/share/vm/yuhu/yuhuCompiler.cpp**
   - Add PlaceSafepoints pass to LLVM pass pipeline
   - Add RewriteStatepointsForGC pass to LLVM pass pipeline
   - Register `OopMapExtractorPlugin` with ObjectLinkingLayer
   - Implement `gc.safepoint_poll()` function

6. **hotspot/src/share/vm/yuhu/yuhuFunction.cpp**
   - Update `add_deferred_oopmap()` to key by statepoint ID

7. **hotspot/src/share/vm/yuhu/yuhuRuntime.cpp**
   - Implement `gc.safepoint_poll()` function for PlaceSafepoints

## References

- **hs_err_pid15328.log**: Crash log showing OopMap assertion failure
- **Activity 026** (`doc/yuhu/activities/026_oopmap_usage.md`): Original OopMap usage documentation
- **Activity 054** (`doc/yuhu/activities/054_orc_jit_createinlineoop_sigsegv.md`): Static field access fix that exposed this issue
- **c1_Runtime1_aarch64.cpp:62-69**: C1's call_RT implementation showing correct Label patching approach
- **macroAssembler_aarch64.cpp:352-363**: C1's set_last_Java_frame with Label-based deferred patching
- **yuhuBuilder.cpp:1209-1310**: Current scan_and_update_offset_markers implementation (has bugs)
- **yuhuStack.hpp:193-250**: Current CreateSetLastJavaFrame implementation (uses adr incorrectly)

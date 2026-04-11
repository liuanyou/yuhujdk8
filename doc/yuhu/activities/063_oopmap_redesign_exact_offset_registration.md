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

**The bug**: No decache/cache around backedge safepoints. If an oop is live across the backedge in a register, and GC moves it, the register contains a stale address → crash.

**Required fix**: Apply same decache/cache pattern to backedge safepoints as used for VM calls:
1. Force-spill all live oops to known stack slots before safepoint
2. Mark those slots in OopMap
3. Reload all oops from stack after safepoint (ensures GC-updated addresses)

**Short-term solution**: Force-spill all live oops (type-driven, not register-driven)
**Long-term solution**: Investigate LLVM's RewriteStatepointsForGC pass

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

### Approach: Inline Metadata + JITLink PostFixupPass

**Why this approach**:
1. ✅ Each call site gets a unique virtual offset at IR generation time
2. ✅ Virtual offset is tracked in Yuhu's deferred OopMap registry
3. ✅ JITLink scans machine code for call patterns (movz/movk/blr)
4. ✅ JITLink matches call target addresses to deferred OopMaps
5. ✅ No wrapper functions (avoids extra bl + ret overhead)
6. ✅ No offset markers (user preference)

### Phase 1: IR Generation - Add Metadata to ALL Call Sites

**Apply to ALL call types**, not just `CreateInlineOopForStaticField()`:

#### 1. VM Runtime Calls via `call_vm()`

**Modify**: `YuhuTopLevelBlock::call_vm()` (yuhuTopLevelBlock.hpp:298-327)

```cpp
llvm::CallInst* call_vm(llvm::Value* callee, ...) {
  decache_for_VM_call();
  
  // 1. Get unique virtual offset for this call site
  int virtual_offset = code_buffer()->create_unique_offset();
  
  // 2. Store placeholder for last_Java_pc (will be patched later)
  stack()->CreateSetLastJavaFrameWithPlaceholder(virtual_offset);
  
  // 3. Create the call
  CallInst *res = builder()->CreateCall(func_type, callee, args);
  
  // 4. Create deferred OopMap
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
  
  // 2. Set last_Java_frame with placeholder
  stack()->CreateSetLastJavaFrameWithPlaceholder(virtual_offset);
  
  // 3. Create the call
  CallInst* call = CreateCall(func_ty, fn_ptr, args, name);
  
  // 4. Create deferred OopMap
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
  
  // 2. Set last_Java_frame with placeholder
  stack()->CreateSetLastJavaFrameWithPlaceholder(virtual_offset);
  
  // 3. Create the native call
  Value *result = builder()->CreateCall(func_type, native_function, param_values);
  
  // 4. Create deferred OopMap
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
2. Set last_Java_pc with placeholder
3. Create the call
4. Create deferred OopMap: `function()->add_deferred_oopmap(virtual_offset, oopmap)`

### Phase 2: Update Last Java PC Mechanism

**Problem**: Current `CreateSetLastJavaFrame()` uses `adr x0, .` which reads the PC of the `adr` instruction itself.

**Solution**: Use deferred patching (similar to C1's Label mechanism):

```cpp
void CreateSetLastJavaFrame() {
  // Store placeholder for last_Java_pc
  int pc_virtual_offset = code_buffer()->create_unique_offset();
  
  // Generate code that stores placeholder (will be patched later)
  builder()->CreateStore(
    LLVMValue::intptr_constant(0xDEAD0000 | pc_virtual_offset),
    last_Java_pc_addr()
  );
  
  // Record for patching
  function()->add_last_java_pc_patch_site(pc_virtual_offset);
}
```

After machine code generation, scan for `bl`/`blr` instructions, calculate return addresses, and patch the placeholders.

### Phase 3: JITLink Plugin - Extract Call Sites

**Create `OopMapExtractorPlugin`**:

```cpp
class OopMapExtractorPlugin : public llvm::orc::ObjectLinkingLayer::Plugin {
public:
  void modifyPassConfig(MaterializationResponsibility &MR,
                        jitlink::LinkGraph &LG,
                        jitlink::PassConfiguration &PassConfig) override {
    PassConfig.PostFixupPasses.push_back(
      [this, &MR, &LG](jitlink::LinkGraph &G) -> Error {
        return extractCallSites(G, MR);
      });
  }

private:
  Error extractCallSites(jitlink::LinkGraph &G, MaterializationResponsibility &MR) {
    for (auto *Sym : G.defined_symbols()) {
      if (!Sym->hasName() || !Sym->getName().startswith("yuhu_")) continue;
      
      auto &Block = Sym->getBlock();
      auto Content = Block.getContent();
      auto Size = Block.getSize();
      uint64_t BaseAddr = Sym->getAddress();
      
      // Scan for movz/movk/blr pattern (48-bit addresses)
      for (size_t offset = 0; offset + 16 <= Size; offset += 4) {
        uint32_t* inst = (uint32_t*)(Content + offset);
        
        // Pattern: movz (lsl #32) -> movk (lsl #16) -> movk (lsl #0) -> blr
        uint32_t rd = inst[0] & 0x1F;
        
        bool is_movz_32 = (inst[0] & 0xFF800000) == 0xD2800000 && 
                          ((inst[0] >> 21) & 0x3) == 2 &&
                          (inst[0] & 0x1F) == rd;
        
        bool is_movk_16 = (inst[1] & 0xFF800000) == 0xF2800000 && 
                          ((inst[1] >> 21) & 0x3) == 1 &&
                          (inst[1] & 0x1F) == rd;
        
        bool is_movk_0 = (inst[2] & 0xFF800000) == 0xF2800000 && 
                         ((inst[2] >> 21) & 0x3) == 0 &&
                         (inst[2] & 0x1F) == rd;
        
        bool is_blr = (inst[3] & 0xFFFFFC1F) == 0xD63F0000 && 
                      (inst[3] & 0x1F) == rd;
        
        if (is_movz_32 && is_movk_16 && is_movk_0 && is_blr) {
          // Reconstruct call target address
          uint64_t addr = 0;
          addr |= ((uint64_t)((inst[0] >> 5) & 0xFFFF)) << 32;
          addr |= ((uint64_t)((inst[1] >> 5) & 0xFFFF)) << 16;
          addr |= ((uint64_t)((inst[2] >> 5) & 0xFFFF)) << 0;
          
          // Check if this calls a known VM helper
          if (isVMHelperAddress(addr)) {
            uint64_t ReturnPC = BaseAddr + offset + 16;  // After blr
            int actual_offset = ReturnPC - BaseAddr;
            
            // Find matching deferred OopMap by call target address
            int virtual_offset = findVirtualOffsetByCallTarget(addr);
            
            if (virtual_offset >= 0) {
              // Map virtual → actual offset
              offset_mapper->add_mapping(virtual_offset, actual_offset);
              
              // Patch last_Java_pc placeholder
              patchLastJavaPC(BaseAddr, Size, virtual_offset, actual_offset);
            }
          }
          
          offset += 12;  // Skip processed instructions
        }
      }
    }
    
    return Error::success();
  }
};
```

### Phase 4: Register OopMaps with Actual Offsets

**After JITLink completes**, use the offset mappings to register OopMaps:

```cpp
Error notifyEmitted(MaterializationResponsibility &MR) override {
  for (auto& mapping : offset_mapper->get_mappings()) {
    int virtual_offset = mapping.virtual_offset;
    int actual_offset = mapping.actual_offset;
    
    // Get deferred OopMap
    OopMap* oopmap = function()->get_deferred_oopmap(virtual_offset);
    
    // Register with HotSpot using ACTUAL machine code offset
    debug_info->add_safepoint(actual_offset, oopmap);
  }
  
  return Error::success();
}
```

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

## OopMap Completeness: The LLVM Register Allocator Gap

### The Core Problem

**C1/C2 approach**: C1's LinearScan register allocator (c1_LinearScan.cpp:2376-2442) tracks live intervals for all values. At each safepoint, it walks active intervals and marks their locations (register or spill slot) in the OopMap. C1 controls register allocation, so it knows ALL oop locations.

**Yuhu's challenge**: LLVM's register allocator is a black box that runs after IR generation. We have NO visibility into:
- Which physical registers hold oops
- Which stack slots LLVM auto-spills contain oops
- Whether an oop is in x0, x19, or [sp+80]

**The risk**: If LLVM keeps an oop in a register across a safepoint, and GC moves the object, the register contains a stale address → crash.

### Solution 1: Force-Spill All Live Oops (Short-Term)

**Mechanism**: Before every safepoint, explicitly store ALL live oops to known stack slots, mark those slots in OopMap, then reload after the safepoint.

#### How It Works

**At IR generation time** (we still have full type information):

```cpp
// In maybe_add_safepoint() - BEFORE LLVM code generation
void YuhuTopLevelBlock::force_spill_all_live_oops() {
  // Iterate current_state() - we know types of all values
  for (int i = 0; i < max_locals(); i++) {
    YuhuValue* val = current_state()->local_at(i);
    if (val->is_jobject()) {  // ← We KNOW this is an oop
      // Force-spill to known stack slot
      write_value_to_frame(val->generic_value(), locals_offset(i));
      oopmap->set_oop(slot2reg(locals_offset(i)));  // ← Mark in OopMap
      
      // Create reload for post-safepoint uses
      YuhuValue* reloaded = read_value_from_frame(locals_offset(i));
      current_state()->set_local_at(i, reloaded);  // ← Replace old value
    }
  }
  
  // Same for expression stack
  for (int i = 0; i < stack_depth(); i++) {
    YuhuValue* val = current_state()->stack_at(i);
    if (val->is_jobject()) {
      write_value_to_frame(val->generic_value(), stack_offset(i));
      oopmap->set_oop(slot2reg(stack_offset(i)));
      
      YuhuValue* reloaded = read_value_from_frame(stack_offset(i));
      current_state()->set_stack_at(i, reloaded);
    }
  }
}
```

**Key insight**: We don't track registers. We track LLVM Values. After force-spill and reload, all post-safepoint uses of oops will generate loads from our known stack slots (which GC updates), not uses of stale register values.

#### What Gets Marked in OopMap

**All oop locations are STACK SLOTS, never registers**:

```cpp
// All of these are stack slots in OopMap:
oopmap->set_oop(slot2reg(16));   // locals area
oopmap->set_oop(slot2reg(48));   // expression/stack area
oopmap->set_oop(slot2reg(80));   // force-spill area (for backedge safepoint)
```

**Why no register slots?** Because we can't track what LLVM does with registers after IR generation.

#### The Three Areas We Control

1. **Locals area** — `current_state()->local_at(i)`
2. **Expression/stack area** — `current_state()->stack_at(i)`
3. **Force-spill area** — explicit spills before backedge safepoints

#### The One Area We Don't Control: LLVM Auto-Spills

**LLVM may additionally spill values during code generation**:

```llvm
; Yuhu IR - we control up to this point
%oop1 = call i8* @get_object()
store i8* %oop1, i8** %force_spill_0   ; ← We control this

; LLVM CodeGen - we lose visibility here
; LLVM decides to also spill some intermediate values:
;   [sp+200] = %temp_computed_value  ← We don't know this exists!
```

**Is this a problem?** Generally **no**, because:
1. LLVM's spills happen during code generation (MachineInstr level), not at IR level
2. If LLVM spills before the safepoint call, it will reload before using it
3. The safepoint is a **call** — LLVM treats calls as potential side-effect points
4. LLVM will reload from our known slots **after** the call, not use stale register values

**The mitigation**: By force-spilling and immediately reloading, we ensure:
- All live oops are in **known** stack slots at the safepoint
- All post-safepoint uses reload from those known slots
- LLVM's internal spills are short-lived (spill → immediate reload) and won't span the safepoint call boundary

#### Interaction with Callee-Saved Save/Restore

**They serve different purposes and can coexist**:

| Aspect | Force-Spill for OopMap | Callee-Saved Save/Restore |
|--------|------------------------|---------------------------|
| **What it saves** | All live oops (any register) | x19, x20, x23, x25, x27 (fixed registers) |
| **Why** | GC needs to find all live oops | Interpreter may corrupt these registers |
| **When** | Before ANY safepoint | Before Java method calls only |
| **Where** | Explicit stack slots per oop | Fixed location: `[sp, #80]` |
| **OopMap** | MARKED in OopMap | NOT marked in OopMap |

**Example sequence before Java call**:

```llvm
; 1. Force-spill all live oops (for GC)
store i8* %x0_oop, i8** [sp, #32]   ; oop from x0
store i8* %x19_oop, i8** [sp, #40]  ; oop from x19
OopMap marks: [sp+32], [sp+40]

; 2. Callee-saved save (protects ALL values from interpreter)
stp x19, x20, [sp, #80]   ; Saves x19 (oop) + x20 (jint)
stp x23, x25, [sp, #96]   ; Saves x23 (oop) + x25 (jlong)

; 3. Java call
call void @interpreter_entry()

; 4. Callee-saved restore
ldp x19, x20, [sp, #80]   ; Restores x19 (may be stale oop!)
ldp x23, x25, [sp, #96]   ; Restores x23 (may be stale oop!)

; 5. Force-spill reload (gets GC-updated oop addresses)
ldr x0, [sp, #32]   ; Reload oop
ldr x19, [sp, #40]  ; Reload oop ← Overwrites stale x19!
```

**Force-spill is type-driven, not register-driven**:
- We don't ask: "Is this value in x19?"
- We only ask: "Is this value an oop?" If yes, force-spill it.

### Solution 2: RewriteStatepointsForGC (Long-Term)

**LLVM's official GC support mechanism** (LLVM >= 3.9):

```llvm
%token = call token (i64, i32, void ()*, i32, i32, ...) 
  @llvm.experimental.gc.statepoint.p0f_isVoidf(
    i64 0, i32 0, void ()* @SafepointSynchronize_block, 
    i32 0, i32 0,  ; flags
    ; list of live oops as operands
    i8* %oop1, i8* %oop2, i8* %oop3)
```

**LLVM's RewriteStatepointsForGC pass automatically**:
1. Identifies all live oops at the statepoint
2. Generates gc.relocates for each oop
3. Produces accurate OopMap-equivalent metadata

**Pros**:
- LLVM handles liveness tracking
- Official LLVM GC support mechanism
- Works with any register allocator
- No manual force-spill needed

**Cons**:
- Requires restructuring Yuhu to use LLVM's GC infrastructure
- Complex integration
- Needs investigation and prototyping

**Status**: **To be investigated** as a long-term solution. Current short-term approach is force-spill.

## Backedge Safepoints: The Missing Decache/Cache

### The Current Gap

**VM calls** (current implementation — works correctly):

From [yuhuTopLevelBlock.hpp line 307](file:///Users/liuanyou/CLionProjects/jdk8/hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.hpp#L307):

```cpp
decache_for_VM_call(virtual_offset);
// ... 
CallInst *res = builder()->CreateCall(virtual_callee, args_array);
cache_after_VM_call();  // ← RELOADS from stack after call!
```

**`cache_after_VM_call()`** reloads values from the stack slots back into LLVM's value tracking, ensuring post-call uses get GC-updated oop addresses.

**Backedge safepoints** (current implementation — BROKEN):

From [yuhuTopLevelBlock.cpp line 700-710](file:///Users/liuanyou/CLionProjects/jdk8/hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp#L700-L710):

```cpp
void YuhuTopLevelBlock::maybe_add_backedge_safepoint() {
  if (current_state()->has_safepointed())
    return;
  
  for (int i = 0; i < num_successors(); i++) {
    if (successor(i)->can_reach(this)) {
      maybe_add_safepoint();  // ← Does NOT decache!
      break;
    }
  }
}
```

**The bug**: There is NO decache, NO cache — just a state check and conditional call to `SafepointSynchronize::block()`.

**The crash scenario**:
```llvm
%oop = load i8*, i8** %local_0    ; Load oop into LLVM Value
; LLVM puts %oop in x19

; Backedge safepoint (no decache!)
ldr w0, [SafepointSynchronize::_state]
cbnz w0, do_safepoint
continue:
; After safepoint, LLVM uses x19 directly
; If GC moved the object, x19 has stale address!
call void @use(i8* %oop)  ; ← CRASH: uses stale x19!
```

### The Fix: Apply Same Decache/Cache Pattern to Backedges

```cpp
void YuhuTopLevelBlock::maybe_add_backedge_safepoint() {
  if (current_state()->has_safepointed())
    return;
  
  // Check if any successor can reach this block (backedge)
  for (int i = 0; i < num_successors(); i++) {
    if (successor(i)->can_reach(this)) {
      // NEW: Decache all live oops before safepoint
      decache_live_oops_for_safepoint();  // ← Spill oops to stack, create OopMap
      
      // Emit safepoint check
      maybe_add_safepoint();  // This calls SafepointSynchronize::block()
      
      // NEW: Reload oops from stack (so LLVM uses GC-updated addresses)
      recache_live_oops_after_safepoint();  // ← Reload from stack!
      
      break;
    }
  }
}
```

**The sequence**:
1. Iterate `current_state()` to find all live oops
2. For each oop: store to stack, mark in OopMap
3. Emit safepoint check (call to `SafepointSynchronize::block()`)
4. For each oop: reload from stack, update `current_state()`
5. All post-safepoint uses of oops will generate loads from stack (GC-updated)

### Why This Works Without Knowing Registers

**We don't track registers. We track LLVM Values.**

```cpp
// Before safepoint
YuhuValue* oop = current_state()->local_at(0);  
// oop->generic_value() = %42 (LLVM Value, NOT a register)

// We DON'T care if LLVM puts %42 in x0, x19, or [sp+80]
// We FORCE it to [sp+32] by generating an explicit store:
CreateStore(%42, [sp+32]);

// After safepoint, we reload:
%42_reloaded = CreateLoad([sp+32]);

// Now all future uses of this oop use %42_reloaded, not the old %42
current_state()->set_local_at(0, %42_reloaded);
```

**LLVM's register allocator can do whatever it wants** with %42 before the store and %42_reloaded after the load — we don't care. The important thing is:
- Before safepoint: oop is in [sp+32] (we put it there)
- GC updates [sp+32] if object moved
- After safepoint: we load from [sp+32], getting the new address

## Implementation Plan (Updated)

### Step 1: Add Virtual Offset Tracking to VM Calls
- Modify `CreateInlineOopForStaticField()` to use virtual offset tracking
- Add similar tracking to other VM call generators (safepoints, runtime calls, etc.)
- Create deferred OopMap registry keyed by virtual offset

### Step 2: Create JITLink Plugin
- Implement `OopMapExtractorPlugin` as ObjectLinkingLayer::Plugin
- Add PostFixupPass to scan for movz/movk/blr patterns
- Match call target addresses to deferred OopMaps
- Build virtual → actual offset mapping

### Step 3: Fix Last Java PC Mechanism
- Change `CreateSetLastJavaFrame()` to use placeholder values
- Implement placeholder patching in JITLink plugin
- Ensure last_Java_pc points to return address (after blr)

### Step 4: Register OopMaps
- Implement `notifyEmitted()` to register OopMaps with actual offsets
- Integrate with existing `DebugInformationRecorder`
- Verify OopMap coverage for all VM call sites

### Step 5: Implement Force-Spill for Backedge Safepoints (NEW)
- Add `decache_live_oops_for_safepoint()` to force-spill all live oops
- Modify `maybe_add_backedge_safepoint()` to call decache/cache
- Ensure all backedge safepoints have complete OopMap coverage
- Test with loops that have live oops across backedges

### Step 6: Testing
- Run existing test suite to verify no regressions
- Test GC during static field access (original crash scenario)
- Test stack traversal with multiple VM calls
- Verify OopMap correctness with `-XX:+TraceSafepoints`
- **NEW**: Test GC during loop backedges (force-spill correctness)
- **NEW**: Test object movement across backedge safepoints

## Key Design Decisions (Updated)

### Decision 1: Why Not Offset Markers?
**Rejected**: User preference against offset marker approach.

**Alternative**: Use virtual offset tracking with call target address matching in JITLink.

### Decision 2: Why Not Wrapper Functions?
**Rejected**: Each wrapper adds 2 extra instructions (bl + ret), causing performance degradation.

**Alternative**: Direct call scanning with metadata correlation.

### Decision 3: Why Not Encode Offset in Address?
**Rejected**: Cannot encode metadata into function addresses without breaking the call (address must be valid pointer).

**Alternative**: Use virtual offset registry with call target address matching.

### Decision 4: Why Scan movz/movk/blr Instead of bl?
**Reason**: LLVM generates `inttoptr` + `CallInst` as indirect calls (movz/movk/blr), not direct calls (bl).

**Direct call** (bl): Used for known functions at link time
**Indirect call** (blr): Used for function pointers from `inttoptr` constants

### Decision 5: Why Force-Spill Instead of Tracking LLVM Registers? (NEW)
**Reason**: LLVM's register allocator is opaque — we cannot know which physical registers hold oops after code generation.

**Force-spill approach**:
- **Pros**: Simple, predictable, doesn't require LLVM internals knowledge
- **Cons**: Performance overhead (extra stores/loads at every safepoint)
- **Status**: Short-term solution, proven correct by C1's decache/cache pattern

### Decision 6: Why Not Use RewriteStatepointsForGC Immediately? (NEW)
**Reason**: Requires significant restructuring of Yuhu to use LLVM's GC infrastructure.

**RewriteStatepointsForGC**:
- **Pros**: LLVM handles liveness tracking, official GC support, better performance
- **Cons**: Complex integration, needs investigation, may require IR redesign
- **Status**: Long-term solution, to be investigated after force-spill is working

## Code to Remove

After implementing the new OopMap mechanism, the following old code should be removed or replaced:

### 1. Offset Marker Infrastructure (REJECT: User preference)

**Files**: hotspot/src/share/vm/yuhu/yuhuBuilder.cpp, yuhuBuilder.hpp, yuhuCacheDecache.cpp

**Remove**:
- `YuhuBuilder::CreateOffsetMarker()` (yuhuBuilder.cpp:1159-1207)
  - Creates inline asm markers with magic number 0xDEADBEEF
  - No longer needed: using virtual offset tracking instead
  
- `YuhuBuilder::scan_and_update_offset_markers()` (yuhuBuilder.cpp:1209-1310)
  - Scans machine code for offset marker patterns
  - Has bugs: looks for ADR instruction instead of return address after BL/BLR
  - Replaced by: `OopMapExtractorPlugin::extractCallSites()` in JITLink

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
| `CreateOffsetMarker()` | yuhuBuilder.cpp:1159-1207 | **REMOVE** | Replaced by virtual offset tracking |
| `scan_and_update_offset_markers()` | yuhuBuilder.cpp:1209-1310 | **REMOVE** | Replaced by JITLink plugin |
| `insert_offset_marker()` | yuhuBuilder.hpp:267 | **REMOVE** | Part of old marker infrastructure |
| Offset marker in `start_frame()` | yuhuCacheDecache.cpp:45 | **REMOVE** | No longer creating markers |
| ADR-based PC setting | yuhuStack.hpp:220-247 | **REPLACE** | Uses wrong PC (ADR instead of return address) |
| ADR scanning logic | yuhuBuilder.cpp:1268-1295 | **REMOVE** | Wrong instruction to scan for |
| `CreateReadCurrentPC()` | yuhuBuilder.cpp:695-713 | **KEEP** | Still needed for function entry |
| `YuhuOffsetMapper` | yuhuOffsetMapper.hpp | **KEEP** | Still needed for offset mapping |
| `add_deferred_oopmap()` | yuhuFunction.cpp | **UPDATE** | Use with new JITLink-based offset mapper |
| `relocate_oopmaps()` | yuhuBuilder.cpp | **UPDATE** | Use JITLink-based offset mapper |
| `call_vm()` | yuhuTopLevelBlock.hpp:298-327 | **UPDATE** | Add metadata, placeholder mechanism (~20-30 call sites affected) |
| `CreateInlineOopForStaticField()` | yuhuBuilder.cpp:943-1007 | **UPDATE** | Add metadata, placeholder mechanism |
| `YuhuNativeWrapper::initialize()` | yuhuNativeWrapper.cpp:194 | **UPDATE** | Add metadata, placeholder mechanism |
| `CreateInlineOop()` | yuhuBuilder.cpp | **UPDATE** | Add metadata, placeholder mechanism |
| `CreateInlineMetadata()` | yuhuBuilder.cpp | **UPDATE** | Add metadata, placeholder mechanism |

## Files to Modify

1. **hotspot/src/share/vm/yuhu/yuhuBuilder.cpp**
   - `CreateInlineOopForStaticField()`: Add metadata, create deferred OopMap
   
2. **hotspot/src/share/vm/yuhu/yuhuStack.hpp**
   - `CreateSetLastJavaFrame()`: Use placeholder mechanism

3. **hotspot/src/share/vm/yuhu/yuhuORCPlugins.hpp**
   - Declare `OopMapExtractorPlugin` class

4. **hotspot/src/share/vm/yuhu/yuhuORCPlugins.cpp**
   - Implement `OopMapExtractorPlugin`
   - Implement call target address matching
   - Implement placeholder patching

5. **hotspot/src/share/vm/yuhu/yuhuCompiler.cpp**
   - Register `OopMapExtractorPlugin` with ObjectLinkingLayer
   - Integrate OopMap registration after JITLink completion

## References

- **hs_err_pid15328.log**: Crash log showing OopMap assertion failure
- **Activity 026** (`doc/yuhu/activities/026_oopmap_usage.md`): Original OopMap usage documentation
- **Activity 054** (`doc/yuhu/activities/054_orc_jit_createinlineoop_sigsegv.md`): Static field access fix that exposed this issue
- **c1_Runtime1_aarch64.cpp:62-69**: C1's call_RT implementation showing correct Label patching approach
- **macroAssembler_aarch64.cpp:352-363**: C1's set_last_Java_frame with Label-based deferred patching
- **yuhuBuilder.cpp:1209-1310**: Current scan_and_update_offset_markers implementation (has bugs)
- **yuhuStack.hpp:193-250**: Current CreateSetLastJavaFrame implementation (uses adr incorrectly)

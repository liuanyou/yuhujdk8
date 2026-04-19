# Activity 064: RS4GC Inline Asm Incompatibility - Analysis and Solutions

**Date:** 2026-03-19  
**Status:** Analysis Complete  
**Related:** Activity 063 (RewriteStatepointsForGC Integration)

## 1. Problem Statement

### 1.1 Root Cause

**LLVM's RewriteStatepointsForGC (RS4GC) pass cannot process inline assembly calls.**

When RS4GC encounters a `CallInst` that calls inline asm, it attempts to wrap it with `gc.statepoint` intrinsic. However, inline asm has **no callable address** (it's embedded directly in the call instruction), causing RS4GC to fail with:

```
Cannot take the address of an inline asm!
%statepoint_token = call token @llvm.experimental.gc.statepoint.p0(...)
```

### 1.2 Why RS4GC Fails

**From LLVM source (`RewriteStatepointsForGC.cpp`):**

RS4GC iterates over all `CallBase` instructions and tries to:
1. Extract the called function's address
2. Create a statepoint that wraps the call
3. Record the function pointer for relocation

**Inline asm has no function pointer** - the assembly code is embedded directly in the instruction itself. There's no address to extract, no function to relocate.

### 1.3 Current Impact

**Yuhu uses inline asm in 19 locations across 2 files:**
- `yuhuBuilder.cpp`: 13 inline asm calls
- `yuhuStack.cpp`: 6 inline asm calls

**All of these will crash RS4GC.**

---

## 2. Complete Inventory of Inline Asm Usage

### 2.1 Category A: Register Read Operations (5 locations)

**Purpose:** Read values from specific CPU registers that LLVM intrinsics cannot access.

| # | Method | File:Line | Assembly | Returns | Purpose |
|---|--------|-----------|----------|---------|---------|
| 1 | `CreateReadFramePointer()` | yuhuBuilder.cpp:590 | `mov $0, x29` | i64 | Read frame pointer (x29/FP) |
| 2 | `CreateReadRmethod()` | yuhuBuilder.cpp:621 | `mov $0, x12` | i64 | Read rmethod register (x12) |
| 3 | `CreateReadThreadRegister()` | yuhuBuilder.cpp:644 | `mov $0, x28` | i64 | Read rthread register (x28) |
| 4 | `CreateSaveX0ToX22()` | yuhuBuilder.cpp:665 | `mov x22, x0` | void | Save x0 to x22 (preserve p7) |
| 5 | `CreateReadX22Register()` | yuhuBuilder.cpp:684 | `mov $0, x22` | i64 | Read saved p7 from x22 |

**Why inline asm is used:**
- LLVM's `read_register` intrinsic doesn't support x29, x12, x28, x22
- These are general-purpose registers used for JVM-specific purposes

**RS4GC Impact:** ✓ **WILL CRASH** - RS4GC tries to wrap these calls with statepoints

---

### 2.2 Category B: PC and SP Operations (3 locations)

**Purpose:** Read/write program counter and stack pointer.

| # | Method | File:Line | Assembly | Returns | Purpose |
|---|--------|-----------|----------|---------|---------|
| 6 | `CreateReadCurrentPC()` | yuhuBuilder.cpp:705 | `adr $0, .` | i64 | Get current PC address |
| 7 | `CreateWriteStackPointer()` | yuhuBuilder.cpp:737 | `mov sp, $0` | void | Write to SP register |
| 8 | `CreateEpiloguePlaceholder()` | yuhuBuilder.cpp:764 | `.inst 0xcafebabe` | void | Marker for epilogue patching |

**Why inline asm is used:**
- No LLVM intrinsic to read current PC
- SP modification restricted in LLVM
- Need unique instruction pattern for marker

**RS4GC Impact:** ✓ **WILL CRASH** - RS4GC tries to wrap these calls with statepoints

---

### 2.3 Category C: Oop and Metadata Markers (2 locations)

**Purpose:** Embed unique patterns in machine code for post-compilation patching.

| # | Method | File:Line | Assembly | Returns | Purpose |
|---|--------|-----------|----------|---------|---------|
| 9 | `CreateInlineOop()` | yuhuBuilder.cpp:1103 | `mov w19, #...`<br>`movk w19, #...`<br>`nop`<br>`nop`<br>`mov ${0:x}, #placeholder` | i64 | Oop marker with placeholder |
| 10 | `EmitVirtualOffsetMarker()` | yuhuBuilder.cpp:1229 | `mov w19, #0xBEEF`<br>`movk w19, #0xDEAD`<br>`mov w20, #offset`<br>`nop`<br>`nop` | void | Virtual offset correlation marker |

**Why inline asm is used:**
- Need to embed specific instruction patterns for scanning
- Must include placeholder values for later patching
- Cannot be expressed in LLVM IR

**RS4GC Impact:** ✓ **WILL CRASH** - RS4GC tries to wrap these calls with statepoints

---

### 2.4 Category D: Callee-Saved Register Preservation (2 locations)

**Purpose:** Save/restore callee-saved registers across Java method calls.

| # | Method | File:Line | Assembly | Returns | Purpose |
|---|--------|-----------|----------|---------|---------|
| 11 | `CreateSaveCalleeSavedRegisters()` | yuhuBuilder.cpp:1710 | `stp x19, x20, [sp, #80]`<br>`stp x23, x25, [sp, #96]`<br>`str x27, [sp, #112]` | void | Save callee-saved regs |
| 12 | `CreateRestoreCalleeSavedRegisters()` | yuhuBuilder.cpp:1725 | `ldr x27, [sp, #112]`<br>`ldp x23, x25, [sp, #96]`<br>`ldp x19, x20, [sp, #80]` | void | Restore callee-saved regs |

**Why inline asm is used:**
- LLVM's register allocator doesn't know about custom save area
- Need to save at specific stack offset (#80, #96, #112)
- LLVM would normally handle this via frame lowering, but Yuhu uses custom layout

**RS4GC Impact:** ✓ **WILL CRASH** - RS4GC tries to wrap these calls with statepoints

---

### 2.5 Category E: last_Java_pc Storage Operations (7 locations)

**Purpose:** Store frame anchor values (SP, FP, PC) for stack walking.

| # | Method | File:Line | Assembly | Purpose |
|---|--------|-----------|----------|---------|
| 13 | `CreateSetLastJavaFrame()` | yuhuStack.cpp:188 | `str $1, [$0]` | Store last_Java_sp |
| 14 | `CreateSetLastJavaFrame()` | yuhuStack.cpp:204 | `str $1, [$0]` | Store last_Java_fp |
| 15 | `CreateSetLastJavaFrame()` | yuhuStack.cpp:220 | `str $1, [$0]` | Store last_Java_pc |
| 16 | `CreateSetLastJavaFrameWithMarker()` | yuhuStack.cpp:434 | `str $1, [$0]` | Store last_Java_pc (with marker variant) |
| 17 | `CreateSetLastJavaFrameWithPlaceholderPC()` | yuhuStack.cpp:495 | `mov w19, #0xDEAD`<br>`movk w19, #offset`<br>`adr x20, .+8`<br>`str x20, [$0]` | Store last_Java_pc with adr |
| 18 | `CreateSetLastJavaFrameWithPlaceholder()` | yuhuStack.cpp:531 | `str $1, [$0]` | Store last_Java_pc placeholder |
| 19 | `YuhuStack::patch_last_java_pc()` | yuhuStack.cpp:495 | `movk x0, #...`<br>`str x0, [addr]` | Patch last_Java_pc address |

**Why inline asm is used:**
- Store to thread-local address that LLVM cannot optimize
- Need ADR instruction for PC-relative address (method 17)
- Must prevent LLVM from reordering stores

**RS4GC Impact:** ✓ **WILL CRASH** - RS4GC tries to wrap these calls with statepoints

---

## 3. Solution Analysis

### 3.1 Solution 1: Remove RS4GC Entirely

**Approach:** Don't run RewriteStatepointsForGC pass.

```cpp
// In yuhuIRTransformer.cpp:
FPM.addPass(PlaceSafepointsPass());        // ✓ Keep - inserts gc.safepoint_poll
// MPM.addPass(RewriteStatepointsForGC()); // ✗ Remove - incompatible with inline asm
```

**Pros:**
- ✓ No code changes needed to existing inline asm
- ✓ Works immediately
- ✓ PlaceSafepointsPass still inserts safepoint polls

**Cons:**
- ✗ No oop relocation tracking
- ✗ Cannot support relocating GC (e.g., G1, ZGC)
- ✗ Only works with non-moving collectors

**Viability:** **HIGH** if Yuhu only uses non-relocating GC

---

### 3.2 Solution 2: Replace Register Read Inline Asm with Real Functions

**Applies to:** Category A (5 locations) + Category B methods 6-7

**Approach:** Create external C functions that read registers.

**Example for frame pointer:**

```cpp
// In yuhuRuntime.cpp (native code):
extern "C" uint64_t yuhu_read_frame_pointer() {
  uint64_t fp;
  __asm__ volatile("mov %0, x29" : "=r"(fp));
  return fp;
}

// In yuhuBuilder.cpp (LLVM IR generation):
// Declare the function in LLVM Module:
FunctionType* read_fp_type = FunctionType::get(
    Type::getInt64Ty(ctx), false);
Function* read_fp_func = Function::Create(
    read_fp_type, 
    Function::ExternalLinkage,
    "yuhu_read_frame_pointer",
    module);

// Call it (this is a REAL function call, not inline asm):
Value* fp = builder()->CreateCall(read_fp_func, {}, "fp");
```

**RS4GC behavior:**
- ✓ RS4GC CAN wrap this call with statepoint
- ✓ The function has a real address
- ✓ No crash

**Caveat:**
- Function call overhead (BLR/RET instructions)
- May affect performance if called frequently
- RS4GC will track this as a safepoint (which may be desired)

**Viability:** **HIGH** - straightforward replacement

---

### 3.3 Solution 3: Replace Marker Inline Asm with Unique Constant Pattern

**Applies to:** Category C (2 locations) + Category E method 17

**Current approach (inline asm):**
```cpp
// Inline asm with unique pattern
InlineAsm* marker = InlineAsm::get(
    type,
    "mov w19, #0xBEEF\n\tmovk w19, #0xDEAD, lsl #16\n\t...",
    ...);
builder()->CreateCall(marker);
```

**New approach (LLVM constants):**
```cpp
// Store unique constant as marker
Value* marker = LLVMValue::intptr_constant(
    0xBEEFDEAD0000 | virtual_offset);
Value* last_java_pc_addr = get_last_java_pc_global();
builder()->CreateStore(marker, last_java_pc_addr);
```

**Generated machine code:**
```assembly
; Old (inline asm):
BLR inline_asm_marker    ← RS4GC crashes here

; New (constants):
MOVZ X0, #0x0000         ← Regular instruction
MOVK X0, #0xBEEF, LSL #48
MOVK X0, #0xDEAD, LSL #32
STR X0, [Xn]             ← Regular store
```

**After code generation - scan and patch:**
```cpp
void YuhuCompiler::patch_markers(address code_start, size_t code_size) {
  for (address pc = code_start; pc < code_end; pc += 4) {
    // Look for unique pattern: MOVZ/MOVK with 0xBEEFDEAD
    if (is_marker_pattern(*(uint32_t*)pc, *(uint32_t*)(pc+4))) {
      address blr_addr = find_blr_after(pc);
      address return_addr = blr_addr + 4;
      
      // Patch the constants with actual return address
      patch_movz_movk(pc, return_addr);
    }
  }
  ICache::invalidate_range(code_start, code_size);
}
```

**RS4GC behavior:**
- ✓ No inline asm calls
- ✓ RS4GC only wraps the actual BLR (to stub/runtime functions)
- ✓ MOVZ/MOVK/STR are NOT calls, RS4GC ignores them

**Viability:** **HIGH** - aligns with Activity 063 discussion

---

### 3.4 Solution 4: Replace Callee-Saved Inline Asm with LLVM Intrinsics

**Applies to:** Category D (2 locations)

**Current approach (inline asm):**
```cpp
InlineAsm* save_asm = InlineAsm::get(
    type,
    "stp x19, x20, [sp, #80]\n\t...",
    ...);
builder()->CreateCall(save_asm);
```

**New approach (LLVM frame lowering):**

Option A: **Use LLVM's callee-saved register mechanism**

```cpp
// In YuhuTargetMachine setup:
// Tell LLVM which registers are callee-saved
// LLVM will automatically generate save/restore code

// This requires implementing:
// - YuhuRegisterInfo::getCalleeSavedRegs()
// - YuhuFrameLowering::determineCalleeSaves()
```

**Pros:**
- ✓ LLVM handles everything
- ✓ No inline asm needed
- ✓ Properly integrated with register allocator

**Cons:**
- ✗ Requires significant target infrastructure work
- ✗ May conflict with Yuhu's custom frame layout

Option B: **Use LLVM intrinsics for stack operations**

```cpp
// Use LLVM's frameindex and stacksave/stackrestore
Value* save_area = builder()->CreateAlloca(
    Type::getInt64Ty(ctx), 
    ConstantInt::get(Type::getInt32Ty(ctx), 5));

// Store registers
builder()->CreateStore(reg19, 
    builder()->CreateGEP(save_area, {0, 0}));
builder()->CreateStore(reg20, 
    builder()->CreateGEP(save_area, {0, 1}));
// ... etc
```

**Pros:**
- ✓ Pure LLVM IR
- ✓ RS4GC compatible

**Cons:**
- ✗ LLVM decides stack offset (not #80, #96, #112)
- ✗ May not match Yuhu's frame layout requirements

**Viability:** **MEDIUM** - requires architecture decisions

---

### 3.5 Solution 5: Replace Store Inline Asm with LLVM Store Instructions

**Applies to:** Category E (7 locations - methods 13-16, 18-19)

**Current approach (inline asm):**
```cpp
InlineAsm* store_asm = InlineAsm::get(
    type,
    "str $1, [$0]",
    "r,r,~{memory}",
    ...);
builder()->CreateCall(store_asm, {addr, value});
```

**New approach (LLVM store):**
```cpp
// Direct LLVM store instruction
Value* addr_ptr = builder()->CreateIntToPtr(
    addr, 
    PointerType::get(Type::getInt64Ty(ctx), 0));
builder()->CreateStore(value, addr_ptr);
```

**Generated machine code:**
```assembly
; Old (inline asm):
BLR inline_asm_store    ← RS4GC crashes here

; New (LLVM store):
STR X0, [X1]            ← Regular instruction, NOT a call
```

**RS4GC behavior:**
- ✓ STR is NOT a call instruction
- ✓ RS4GC only processes CALL/INVOKE instructions
- ✓ No crash, no statepoint wrapping

**Viability:** **VERY HIGH** - trivial replacement

**Why inline asm was used originally:**
- Prevent LLVM from reordering the stores
- Ensure stores happen at exact program points

**Solution:** Use LLVM memory barriers if ordering is critical:
```cpp
builder()->CreateFence(SequentiallyConsistent);  // Memory barrier
builder()->CreateStore(value, addr_ptr);
builder()->CreateFence(SequentiallyConsistent);
```

---

### 3.6 Solution 6: Modify LLVM Source to Skip Inline Asm

**Approach:** Patch LLVM's `RewriteStatepointsForGC.cpp` to skip inline asm calls.

```cpp
// In LLVM's RewriteStatepointsForGC.cpp:
for (CallBase &Call : calls(F)) {
  // Skip inline asm calls
  if (isa<InlineAsm>(Call.getCalledOperand())) {
    continue;  // ← Add this line
  }
  
  // Normal statepoint processing...
  rewriteCall(Call);
}
```

**Pros:**
- ✓ No changes to Yuhu code needed
- ✓ All inline asm continues to work
- ✓ RS4GC processes regular calls normally

**Cons:**
- ✗ Must maintain LLVM fork
- ✗ Upgrade LLVM requires re-applying patch
- ✗ May hide legitimate issues (inline asm with GC pointers)

**Viability:** **MEDIUM** - depends on LLVM maintenance strategy

---

## 4. Recommended Solution Strategy

### 4.1 Phase 1: Quick Fix (Remove RS4GC)

**If Yuhu doesn't need relocating GC:**

1. Remove `RewriteStatepointsForGC()` from pass pipeline
2. Keep `PlaceSafepointsPass()` for safepoint polls
3. All inline asm continues to work
4. **Zero code changes needed**

**Timeline:** Immediate

---

### 4.2 Phase 2: Selective Replacement (If RS4GC Required)

**If Yuhu needs relocating GC support:**

Replace inline asm in priority order:

| Priority | Category | Locations | Effort | Impact |
|----------|----------|-----------|--------|--------|
| **P0** | Category E (stores) | 7 locations | Low | High - most frequently used |
| **P1** | Category A (register reads) | 5 locations | Medium | High - called often |
| **P2** | Category B (PC/SP) | 3 locations | Medium | Medium |
| **P3** | Category C (markers) | 2 locations | High | Low - can use constants |
| **P4** | Category D (callee-saved) | 2 locations | High | Medium |

**Total effort:** ~2-3 days for full replacement

---

### 4.3 Phase 3: Long-term (LLVM Integration)

1. Implement proper target infrastructure (RegisterInfo, FrameLowering)
2. Let LLVM handle callee-saved registers automatically
3. Eliminate need for inline asm entirely
4. Full RS4GC compatibility

**Timeline:** ~1-2 weeks

---

## 10. Detailed Replacement Examples

### 5.1 Category E: Store Operations (Easiest)

**File:** `yuhuStack.cpp:188-202`

**Current (inline asm):**
```cpp
llvm::InlineAsm* store_sp_asm = llvm::InlineAsm::get(
    store_asm_type,
    "str $1, [$0]",
    "r,r,~{memory}",
    true, false, llvm::InlineAsm::AD_ATT);

std::vector<Value*> store_sp_args;
store_sp_args.push_back(sp_addr_i64);
store_sp_args.push_back(stack_pointer);
builder()->CreateCall(store_asm_type, store_sp_asm, store_sp_args);
```

**Replacement (LLVM store):**
```cpp
// Convert address to pointer
Value* sp_addr_ptr = builder()->CreateIntToPtr(
    sp_addr_i64,
    PointerType::get(Type::getInt64Ty(ctx), 0));

// Store directly
builder()->CreateStore(stack_pointer, sp_addr_ptr);
```

**Why this works:**
- `str $1, [$0]` inline asm does exactly what `CreateStore` does
- LLVM generates `STR Xn, [Xm]` instruction
- No function call, no BLR, no RS4GC issue

---

### 5.2 Category A: Register Reads (Medium)

**File:** `yuhuBuilder.cpp:590-600`

**Current (inline asm):**
```cpp
llvm::InlineAsm* asm_func = llvm::InlineAsm::get(
    asm_type,
    "mov $0, x29",
    "=r", false, false, llvm::InlineAsm::AD_ATT);

return CreateCall(asm_type, asm_func, {}, "fp");
```

**Replacement (external function):**
```cpp
// Step 1: Declare external function (once, in module initialization)
FunctionType* read_fp_type = FunctionType::get(
    Type::getInt64Ty(ctx), false);
Function* read_fp_func = Function::Create(
    read_fp_type,
    Function::ExternalLinkage,
    "yuhu_read_frame_pointer",
    module);

// Step 2: Call the function
return CreateCall(read_fp_func, {}, "fp");

// Step 3: Implement in native code (yuhuRuntime.cpp)
extern "C" uint64_t yuhu_read_frame_pointer() {
  uint64_t fp;
  __asm__ volatile("mov %0, x29" : "=r"(fp));
  return fp;
}
```

**RS4GC behavior:**
- This is now a real function call with address
- RS4GC wraps it with statepoint: `%sp = call token @llvm.gc.statepoint(... @yuhu_read_frame_pointer ...)`
- Works correctly, no crash

---

### 5.3 Category C: Markers (Complex)

**File:** `yuhuBuilder.cpp:1229-1239`

**Current (inline asm):**
```cpp
char asm_string[256];
snprintf(asm_string, sizeof(asm_string),
         "mov w19, #0xBEEF\n"
         "movk w19, #0xDEAD, lsl #16\n"
         "mov w20, #%d\n"
         "nop\n"
         "nop",
         virtual_offset & 0xFFFF);

llvm::InlineAsm* marker_asm = llvm::InlineAsm::get(
    asm_type, asm_string,
    "~{w19},~{w20},~{memory}",
    true, false, llvm::InlineAsm::AD_ATT);

CreateCall(asm_type, marker_asm, {});
```

**Replacement (unique constant pattern):**
```cpp
// Create unique marker constant
uint64_t marker = 0xBEEFDEAD0000ULL | (virtual_offset & 0xFFFF);
Value* marker_val = LLVMValue::intptr_constant(marker);

// Store to a known location (or just create the pattern)
Value* last_java_pc = get_last_java_pc_global();
builder()->CreateStore(marker_val, last_java_pc);

// Record for later patching
method_markers.push_back({virtual_offset, marker_val});
```

**Generated machine code:**
```assembly
; Old:
BLR inline_asm_marker    ← RS4GC crashes

; New:
MOVZ X0, #0x0000         ← Regular instruction
MOVK X0, #0xBEEF, LSL #48
MOVK X0, #0xDEAD, LSL #32
STR X0, [Xn]             ← Regular store
```

**After code generation:**
```cpp
void patch_markers(address code_start, size_t code_size) {
  for (address pc = code_start; pc < code_end; pc += 4) {
    if (is_beeefdead_pattern(pc)) {
      // Found marker, patch it
      address blr = find_blr_after(pc);
      patch_with_return_addr(pc, blr + 4);
    }
  }
}
```

---

## 11. Testing Strategy

### 6.1 Unit Tests

For each replacement:
1. Verify generated machine code matches intent
2. Verify RS4GC doesn't crash
3. Verify functionality is preserved

### 6.2 Integration Tests

1. Run full Yuhu test suite with RS4GC enabled
2. Verify GC safepoints work correctly
3. Verify stack walking works with new code

### 6.3 Performance Tests

1. Measure overhead of function calls vs inline asm
2. Ensure marker scanning doesn't add significant compilation time

---

## 7. Recommended Solution: YuhuRS4GC Custom Pass

### 7.1 The Concept

**Create `YuhuRewriteStatepointsForGC` - A custom LLVM pass that:**
1. Processes calls like LLVM's RS4GC
2. **Skips all inline asm calls** (which don't affect GC)
3. Wraps only "real" function calls with gc.statepoint
4. Maintains full GC oop tracking

### 7.2 Architecture Change: Move Callee-Saved to Stubs

**CRITICAL DESIGN DECISION: Move callee-saved register save/restore from LLVM IR to stubs.**

#### Current Architecture (Problem):

```
LLVM Function:
  ... code ...
  inline_asm: stp x19, x20, [sp, #80]  ← LLVM IR inline asm (RS4GC crashes!)
  inline_asm: stp x23, x25, [sp, #96]
  inline_asm: str x27, [sp, #112]
  BLR stub                              ← Call to stub
  
Stub:
  ... stub logic ...
  BLR compiled_entry
```

**Problem:** Callee-saved save/restore is inline asm in LLVM IR → RS4GC crashes

#### Proposed Architecture (Solution):

```
LLVM Function:
  ... code ...
  BLR stub                              ← Regular call, YuhuRS4GC wraps this ✓
  
Stub (native machine code, not LLVM IR):
  STP x19, x20, [sp, #80]              ← Native instructions (no RS4GC!)
  STP x23, x25, [sp, #96]
  STR x27, [sp, #112]
  ... stub logic ...
  LDR X17, [X17, #entry_offset]
  BLR X17                              ← Call compiled method
  
  LDR x27, [sp, #112]                  ← Restore
  LDP x23, x25, [sp, #96]
  LDP x19, x20, [sp, #80]
  RET
```

**Benefits:**
1. ✓ **No inline asm for callee-saved** - Removed from LLVM IR entirely
2. ✓ **YuhuRS4GC can wrap BLR** - Regular function call, no crash
3. ✓ **Cleaner separation** - LLVM IR doesn't know about register save/restore
4. ✓ **Stubs handle ABI** - Stubs already manage calling convention
5. ✓ **Matches HotSpot design** - Stubs handle their own register management

#### Why This Works:

**Stubs are ALREADY native machine code** (not LLVM IR), so:
- They can use any instructions they want
- No RS4GC interference (RS4GC only processes LLVM IR)
- Complete control over register save/restore
- Matches HotSpot's stub architecture pattern

**From HotSpot's design:**
- Stubs (RuntimeStubs, ICStubs, etc.) handle their own register management
- Compiled methods don't worry about callee-saved across stub calls
- The stub ABI is part of the stub's contract

### 7.3 YuhuRS4GC Implementation

**File:** `hotspot/src/share/vm/yuhu/YuhuRewriteStatepointsForGC.cpp`

```cpp
// Custom RS4GC pass that skips inline asm
class YuhuRewriteStatepointsForGC : public PassInfoMixin<YuhuRewriteStatepointsForGC> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
    for (Function &F : M) {
      if (F.hasGC()) {
        rewriteFunction(F);
      }
    }
    return PreservedAnalyses::all();
  }

private:
  void rewriteFunction(Function &F) {
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (auto *Call = dyn_cast<CallBase>(&I)) {
          // KEY: Skip all inline asm calls
          if (isa<InlineAsm>(Call->getCalledOperand())) {
            continue;  // ← Skip inline asm, no statepoint needed
          }
          
          // Process regular function calls
          if (shouldRewriteStatepointsIn(Call)) {
            rewriteCall(Call);
          }
        }
      }
    }
  }
};
```

**Registration in pass pipeline:**

```cpp
// In yuhuIRTransformer.cpp:

// Instead of LLVM's RS4GC:
// MPM.addPass(RewriteStatepointsForGC());  // ← Crashes on inline asm

// Use our custom pass:
MPM.addPass(YuhuRewriteStatepointsForGC());  // ← Skips inline asm safely
```

### 7.4 Inline Asm Analysis After Architecture Change

**With callee-saved moved to stubs, ALL remaining inline asm is GC-safe:**

| Category | Inline Asm Purpose | Contains GC Pointers? | YuhuRS4GC Action |
|----------|-------------------|----------------------|------------------|
| **A: Register reads** | Read x29, x12, x28, x22 | ✗ NO (register values) | ✓ Skip |
| **B: PC/SP operations** | Read PC, write SP | ✗ NO (control flow) | ✓ Skip |
| **C: Markers** | Embed patterns | ✗ NO (constants) | ✓ Skip |
| **D: Callee-saved** | ~~Save/restore~~ | **MOVED TO STUBS** | **N/A** |
| **E: last_Java_pc stores** | Store frame anchors | ✗ NO (addresses) | ✓ Skip |

**All inline asm can be safely skipped - no special cases needed!**

### 7.5 Why Inline Asm Doesn't Need Statepoints

**Statepoints are needed when:**
1. A call might trigger GC (safepoint)
2. GC might move heap objects
3. Live pointers in registers/stack need to be updated

**Inline asm in Yuhu does NOT meet these criteria:**
- **Category A (register reads):** Just reading registers, no GC can happen
- **Category B (PC/SP):** Control flow operations, no heap objects involved
- **Category C (markers):** Embedding constants, no function call semantics
- **Category E (frame stores):** Storing addresses (not oops), GC doesn't move addresses

**The ONLY calls that need statepoints:**
- Calls to Java methods (via stubs)
- Calls to runtime functions that might trigger GC
- Calls that have live oops in registers/stack

**All of these are REGULAR function calls (not inline asm), so YuhuRS4GC handles them correctly.**

### 7.6 Comparison with Other Solutions

| Solution | Code Changes | GC Support | Inline Asm | Maintenance |
|----------|-------------|------------|------------|-------------|
| **Remove RS4GC** | 0 lines | ❌ No relocating | Works | Easy |
| **Replace all inline asm** | ~200 lines | ✓ Full GC | Eliminated | Medium |
| **Fork LLVM RS4GC** | LLVM patch | ✓ Full GC | Works | Hard (LLVM upgrades) |
| **YuhuRS4GC + stub refactor** | ~150 lines | ✓ Full GC | Works | **Easy** ✓ |

**YuhuRS4GC + stub refactor is the best solution because:**
1. ✓ Full GC support (relocating collectors work)
2. ✓ All remaining inline asm preserved (no code changes)
3. ✓ Callee-saved moved to stubs (cleaner architecture)
4. ✓ No LLVM fork needed (easy maintenance)
5. ✓ Matches HotSpot's stub-based design

### 7.7 Implementation Phases

**Phase 1: Move Callee-Saved to Stubs**
- Remove `CreateSaveCalleeSavedRegisters()` and `CreateRestoreCalleeSavedRegisters()` from LLVM IR
- Add save/restore code to stub generation (native machine code)
- Test that stubs correctly preserve x19, x20, x23, x25, x27

**Phase 2: Implement YuhuRS4GC**
- Create `YuhuRewriteStatepointsForGC` pass
- Implement inline asm skip logic
- Register in pass pipeline

**Phase 3: Integration Testing**
- Run Yuhu test suite with YuhuRS4GC enabled
- Verify GC safepoints work correctly
- Verify stack walking works with new stub architecture

**Estimated Effort:** 2-3 days total

---

## 8. Summary

| Aspect | Current State | After YuhuRS4GC + Stub Refactor |
|--------|--------------|--------------------------------|
| **Inline asm in LLVM IR** | 19 locations | 14 locations (5 callee-saved moved to stubs) |
| **Callee-saved handling** | LLVM IR inline asm | Native code in stubs |
| **RS4GC compatibility** | ✗ Crashes | ✓ Works (YuhuRS4GC skips inline asm) |
| **GC support** | Non-relocating only | Full relocating GC support |
| **Code changes** | N/A | ~150 lines (stub refactor + YuhuRS4GC) |
| **Architecture** | Mixed IR/native | Clean separation (IR = logic, stubs = ABI) |

---

## 9. Decision Matrix

**Choose YuhuRS4GC + stub refactor if:**
- Yuhu needs relocating GC support (G1, ZGC, etc.)
- Want clean separation between LLVM IR and stub ABI
- Can invest 2-3 days in implementation
- Prefer maintaining custom code over forking LLVM

**Choose Phase 1 only (Remove RS4GC) if:**
- Yuhu only uses non-relocating GC
- Need immediate fix with zero code changes
- Can defer full GC support

**Recommendation:** Implement YuhuRS4GC + stub refactor - it's the cleanest long-term solution that aligns with HotSpot's architecture.

| Aspect | Current State | After Fix |
|--------|--------------|-----------|
| **Inline asm calls** | 19 locations | 0 locations (or RS4GC removed) |
| **RS4GC compatibility** | ✗ Crashes | ✓ Works |
| **Code changes** | N/A | Phase 1: 0 lines<br>Phase 2: ~200 lines |
| **GC support** | Non-relocating only | Relocating GC supported |
| **Performance impact** | Baseline | Phase 1: None<br>Phase 2: Minimal (function call overhead) |

---

## 12. Decision Matrix (Legacy)

**Choose Phase 1 (Remove RS4GC) if:**
- Yuhu only uses non-relocating GC
- Quick fix needed
- Minimal code changes desired

**Choose Phase 2 (Replace inline asm) if:**
- Yuhu needs relocating GC (G1, ZGC, etc.)
- Long-term LLVM compatibility desired
- Can invest 2-3 days in refactoring

**Choose Phase 3 (Full LLVM integration) if:**
- Building production-quality JIT
- Want to minimize custom code
- Can invest 1-2 weeks in infrastructure

---

**Recommendation:** See Section 9 for updated recommendation (YuhuRS4GC + stub refactor).

# Activity 052: Yuhu Compiler Fails to Read Stack Parameters via x20 Register

**Date**: 2026-02-28  
**Author**: User  
**Related Files**: 
- `hotspot/src/share/vm/yuhu/yuhuCacheDecache.cpp`
- `_tmp_yuhu_ir_com.example.NineParameterTest__testNineParametersStatic.ll`

---

## Problem Summary

Yuhu compiler fails when trying to read stack-passed parameters (parameters >= 8) via x20 register in LLVM IR, resulting in:

```
LLVM ERROR: Invalid register name "x20"
```

---

## Root Cause Analysis

### **ARM64 Calling Convention**

According to ARM64 AAPCS calling convention:
- **Parameters 0-7**: Passed in registers x0-x7
- **Parameters 8+**: Passed on the stack

For a method with 9 parameters like:
```java
public static int testNineParametersStatic(int p1, int p2, int p3, int p4, 
                                            int p5, int p6, int p7, int p8, int p9)
```

The parameter distribution is:
- x0: NULL (for static methods)
- x1-x7: p1-p7 (parameters 0-6)
- **Stack [sp+0]**: p8 (parameter 7)
- **Stack [sp+8]**: p9 (parameter 8)

### **Yuhu's Current Implementation**

In `yuhuCacheDecache.cpp` Line 189-208, Yuhu attempts to read stack parameters via x20:

```cpp
llvm::Value* YuhuNormalEntryCacher::read_stack_arg(int arg_index) {
  // Read x20 (esp) register
  llvm::Value* esp = builder()->CreateReadRegister("x20");  // ❌ FAILS HERE!
  
  // Calculate stack argument address: esp + (arg_index - 8) * 8
  int stack_offset = (arg_index - 8) * wordSize;
  llvm::Value* stack_arg_addr = builder()->CreateGEP(
    YuhuType::intptr_type(),
    builder()->CreateIntToPtr(esp, llvm::PointerType::getUnqual(YuhuType::intptr_type())),
    ...
  );
}
```

### **Generated LLVM IR**

From `_tmp_yuhu_ir_com.example.NineParameterTest__testNineParametersStatic.ll` Line 336-340:

```llvm
%x20 = call i64 @llvm.read_register.i64(metadata !1)  ; ❌ INVALID!
%102 = inttoptr i64 %x20 to ptr
%stack_arg_addr = getelementptr i64, ptr %102, i64 0
%stack_arg = load i64, ptr %stack_arg_addr, align 8
%stack_arg_typed = trunc i64 %stack_arg to i32
```

The metadata `!1` contains `"x20"`, which LLVM rejects as an invalid register name.

---

## Why This Happens

### **LLVM's `llvm.read_register` Intrinsic**

LLVM provides the `llvm.read_register` intrinsic for reading specific registers, but:
1. **Not all registers are supported** - only architectural registers that have well-defined meanings
2. **Platform-specific** - each platform defines its own set of readable registers
3. **x20 is a callee-saved general-purpose register** - not typically exposed via intrinsics

### **Comparison with Other Registers**

Yuhu successfully reads:
- `x12` (Method*) - via inline assembly: `asm "mov $0, x12"`
- `x28` (Thread*) - via inline assembly: `asm "mov $0, x28"`
- `sp` (Stack Pointer) - via `@llvm.read_register.i64(metadata !{!"sp"})` ✅

**Key difference**: `sp` is a special architectural register that LLVM recognizes and supports via the intrinsic.

---

## Impact

### **Methods Affected**

Any method with **more than 7 parameters** (for non-static) or **more than 8 parameters** (for static) will fail to compile with Yuhu.

### **Current Behavior**

The IR is generated but cannot be JIT-compiled, causing:
1. Compilation failure at LLVM JIT phase
2. Method falls back to interpreter/C1/C2
3. No crash, but Yuhu cannot optimize such methods

---

## Potential Solutions

### **Option 1: Use Inline Assembly (Recommended)**

Instead of `@llvm.read_register`, use inline assembly like we do for x12 and x28:

```cpp
llvm::Value* YuhuNormalEntryCacher::read_stack_arg(int arg_index) {
  // Read x20 (esp) register via inline assembly
  llvm::Value* esp = builder()->CreateInlineAsm(
    llvm::FunctionType::get(YuhuType::intptr_type(), false),
    "mov $0, x20",  // Assembly string
    "=r",          // Constraint: output-only, register
    false,         // Side effect
    false,         // Align stack
    llvm::InlineAsm::AD_Intel  // Dialect
  );
  
  // Continue with stack loading...
}
```

**Pros**:
- Consistent with how x12/x28 are read
- No LLVM register name validation issues
- Works on all LLVM versions

**Cons**:
- Inline assembly is less portable
- Requires careful constraint specification

---

### **Option 2: Use SP-Relative Addressing Directly**

Instead of reading x20, compute the address relative to the current stack pointer:

```cpp
llvm::Value* YuhuNormalEntryCacher::read_stack_arg(int arg_index) {
  // Get current sp
  llvm::Value* sp = builder()->CreateReadSP();
  
  // The caller's arguments are at [sp] in the caller's frame
  // We need to account for the return address and any prologue
  int stack_offset = (arg_index - 8) * wordSize;
  llvm::Value* stack_arg_addr = builder()->CreateGEP(
    YuhuType::intptr_type(),
    builder()->CreateIntToPtr(sp, ...),
    ...
  );
}
```

**Pros**:
- More explicit about stack layout
- Doesn't rely on x20 being preserved

**Cons**:
- Complex offset calculation
- May not work if x20 doesn't point to the right location

---

### **Option 3: Pass Stack Arguments as Function Parameters**

Modify the function signature to include all parameters, even those beyond 8:

```llvm
define i32 @testNineParametersStatic(
  i64 %null_arg,
  i32 %p1, i32 %p2, ..., i32 %p7,  ; x1-x7
  i32 %p8, i32 %p9                  ; Formerly on stack, now direct params
)
```

**Pros**:
- Simplest solution
- No runtime register reading needed
- Already works (as seen in the generated IR!)

**Cons**:
- Requires changes to function type generation
- May conflict with ARM64 calling convention expectations
- Doesn't scale for very large parameter lists

---

## Current Status

**Investigation Status**: ✅ Root cause identified  
**Implementation Status**: ✅ Fixed - using inline assembly  

**Fix Applied**:
Modified `hotspot/src/share/vm/yuhu/yuhuCacheDecache.cpp` Line 189-208 to use inline assembly instead of `@llvm.read_register`.

**Code Change**:
```cpp
// Before (Line 191):
llvm::Value* esp = builder()->CreateReadRegister("x20");  // ❌ FAILS!

// After (Lines 191-201):
llvm::FunctionType* asm_type = llvm::FunctionType::get(YuhuType::intptr_type(), false);
llvm::InlineAsm* asm_func = llvm::InlineAsm::get(
  asm_type,
  "mov $0, x20",  // AArch64 assembly: move x20 to output register
  "=r",           // Output constraint: =r means output to a register
  false,          // Has side effects: no
  false,          // Is align stack: no
  llvm::InlineAsm::AD_ATT    // Dialect
);
llvm::Value* esp = builder()->CreateCall(asm_type, asm_func, std::vector<llvm::Value*>(), "x20");
```

This matches the pattern used for reading x12 (Method*) and x28 (Thread*) in `YuhuBuilder::CreateReadMethodRegister()` and `CreateReadThreadRegister()`.

**Key Findings**:
1. LLVM does not support reading x20 via `@llvm.read_register`
2. Inline assembly is the proven approach (already used for x12/x28)
3. The fix is minimal and consistent with existing code patterns

**Next Steps**:
- Test compilation with 9+ parameter methods
- Verify runtime correctness
- Consider documenting this pattern for future register readers

---

## Test Case

**File**: `test_yuhu/com/example/NineParameterTest.java`

**Method Signature**:
```java
public static int testNineParametersStatic(int p1, int p2, int p3, int p4, 
                                            int p5, int p6, int p7, int p8, int p9)
```

**Expected Behavior**:
- Should compile successfully with Yuhu
- Should correctly read all 9 parameters
- Should return correct computation result

**Actual Behavior**:
- IR generation succeeds
- LLVM JIT fails with "Invalid register name 'x20'"

---

## References

- ARM64 Calling Convention: https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst
- LLVM Language Reference: https://llvm.org/docs/LangRef.html#llvm-read-register-intrinsic
- Related code: `hotspot/src/share/vm/yuhu/yuhuCacheDecache.cpp` Line 189-208
- Generated IR: `test_yuhu/_tmp_yuhu_ir_com.example.NineParameterTest__testNineParametersStatic.ll` Line 336-340

---

## Appendix: i2c Adapter Stack Frame Unwinding Mechanism

### Discovery Date: 2026-02-28

During analysis of 9-parameter method compilation, investigated how HotSpot handles stack frame unwinding when i2c (interpreter-to-compiled) adapters are involved.

### Problem Statement

When an interpreted method calls a compiled method with >7 parameters:
1. Interpreter prepares parameters on its stack
2. i2c adapter adjusts `sp` downward and copies stack parameters to new locations
3. i2c adapter jumps (not calls) to compiled method using `br` instruction
4. Compiled method's prolog allocates its own frame space

**Question**: During stack unwinding, how does the system correctly skip the i2c adapter's space and find the interpreter's actual frame?

### Key Findings

#### **AdapterBlob Has No Frame**

From `hotspot/src/share/vm/code/codeBlob.cpp` Line 204, 228:
```cpp
BufferBlob::BufferBlob(...) 
  : CodeBlob(name, sizeof(BufferBlob), size, CodeOffsets::frame_never_safe, /*locs_size:*/ 0)
```

- `CodeOffsets::frame_never_safe` indicates **no frame**
- `_frame_size = 0` for AdapterBlob
- i2c adapter is **frameless** - it just modifies sp and jumps

#### **Two-Phase Unwinding Process**

**Phase 1: Compiled Method → Interpreter**

From `hotspot/src/cpu/aarch64/vm/frame_aarch64.cpp` Line 506-532:
```cpp
intptr_t* l_sender_sp = unextended_sp() + _cb->frame_size();
// l_sender_sp points to sp AFTER i2c adapter adjustment

address sender_pc = (address) *(l_sender_sp-1);
intptr_t** saved_fp_addr = (intptr_t**) (l_sender_sp - frame::sender_sp_offset);

return frame(l_sender_sp, unextended_sp, *saved_fp_addr, sender_pc);
```

This constructs an interpreter frame with:
- `_sp = l_sender_sp` (includes i2c adapter space)
- `_fp = *saved_fp_addr` (read from stack at `l_sender_sp - 2`)
- `_cb = CodeBlob::find_blob(sender_pc)` (points to Interpreter code blob)

**Phase 2: Interpreter → Its Caller**

From `hotspot/src/cpu/aarch64/vm/frame_aarch64.inline.hpp` Line 221 (ASM interpreter):
```cpp
inline intptr_t* frame::sender_sp() const { 
  return addr_at(sender_sp_offset); 
}

// From frame.hpp Line 200:
intptr_t* addr_at(int index) const { return &fp()[index]; }
```

**Critical Insight**: `sender_sp()` for interpreter frames uses **fp-relative offset**, not sp-relative!

From `hotspot/src/cpu/aarch64/vm/frame_aarch64.hpp` Line 109:
```cpp
sender_sp_offset = 2,
```

So: `sender_sp = *(fp + 2)`

### Why This Works

The interpreter's `fp` is **retrieved from the compiled frame** during Phase 1 unwinding:
- `saved_fp_addr = l_sender_sp - 2` reads the fp value that was pushed onto the compiled method's stack
- This fp was saved by the interpreter **before calling i2c adapter** (during method invocation setup)
- Therefore, `*(fp + 2)` computes the caller's sp based on the **pre-i2c-adapter** fp
- The i2c adapter's sp adjustment is **automatically ignored**
- The unwinding finds the correct interpreter caller frame

### Visual Representation

```
Stack Layout (high to low addresses):

[Interpreter Caller Frame]
...
[p8]                    ← esp+0 (interpreter's parameter area)
[p9]                    ← esp+8
------------------------  ← Interpreter's original sp (saved in fp-based computation)
[i2c adapter allocates 16 bytes]
[p8]                    ← i2c_adapter_sp+0 (copied by i2c)
[p9]                    ← i2c_adapter_sp+8 (copied by i2c)
------------------------  ← i2c_adapter_sp (entering compiled method)
[Compiled Method Frame - frame_size=96]
...
[fp]                    ← unextended_sp
[lr]
[return address]
[p8]                    ← sp+0 (within frame_size)
[p9]                    ← sp+8 (within frame_size)
------------------------

Unwinding:
1. compiled.unextended_sp + compiled.frame_size = i2c_adapter_sp ✓
2. Construct interpreter frame with fp = *(i2c_adapter_sp - 2)
3. interpreter.sender_sp() = *(fp + 2) = Interpreter's original sp ✓
   (automatically skips i2c adapter's 16 bytes!)
```

### Conclusion

**i2c adapter space is NOT included in any frame_size**. Instead:
- Compiled method's `frame_size` covers only its own allocation
- i2c adapter is frameless (just a jump with sp adjustment)
- Interpreter's fp is retrieved from compiled frame at `l_sender_sp - 2` (Line 512)
- This fp was established by the interpreter before i2c adapter execution
- Interpreter uses fp-derived sender computation (`*(fp + 2)`), which predates i2c adapter
- The mechanism is **layout-computed via pre-i2c fp, not dynamically saved**

This explains why stack unwinding works correctly despite i2c adapter modifying sp without updating any metadata.

---

## Appendix 2: Compiled-to-Compiled Call Parameter Passing and Frame Size Allocation

### Discovery Date: 2026-02-28

During analysis of 9-parameter method compilation, investigated how HotSpot handles parameter passing and frame size allocation when compiled methods call other compiled methods with varying parameter counts.

### Problem Statement

When a compiled method calls multiple compiled methods with different parameter counts:
- Some calls have 6 parameters (all in registers x0-x7)
- Some calls have 9 parameters (p8, p9 on stack)
- Some calls have 11 parameters (p8-p11 on stack)

**Question**: How is the outgoing argument space determined? Is it dynamic or static?

### Key Findings

#### **Static Allocation Using MAX2 Strategy**

The outgoing argument space is **statically allocated** at method compilation time using a **MAX2 (take maximum)** strategy across all call sites.

#### **C1 Implementation**

From `hotspot/src/share/vm/c1/c1_FrameMap.hpp` Line 145-147:
```cpp
void update_reserved_argument_area_size (int size) {
  _reserved_argument_area_size = MAX2(_reserved_argument_area_size, size);
}
```

**Process**:
1. During compilation, for each method call encountered:
   - Call `java_calling_convention(...)` to compute required stack parameter space
   - Call `update_reserved_argument_area_size()` to update reserved space

2. Each call updates the reserved area:
   ```cpp
   // From hotspot/src/share/vm/c1/c1_FrameMap.cpp Line 112
   if (outgoing) {
     update_reserved_argument_area_size(out_preserve * BytesPerWord);
   }
   ```

3. Final frame size includes the maximum reserved area:
   ```cpp
   // Line 209-213
   _framesize = round_to(in_bytes(sp_offset_for_monitor_base(0)) +
                         _num_monitors * sizeof(BasicObjectLock) +
                         sizeof(intptr_t) +
                         frame_pad_in_bytes,
                         StackAlignmentInBytes) / 4;
   ```

   Where `sp_offset_for_monitor_base(0)` includes `_reserved_argument_area_size` (Line 298)

#### **C2 Implementation**

From `hotspot/src/share/vm/opto/matcher.cpp` Line 1313-1314:
```cpp
if( _out_arg_limit < out_arg_limit_per_call)
  _out_arg_limit = out_arg_limit_per_call;
```

**Process**:
1. Iterate through all call sites in the IR
2. For each call, compute required outgoing argument space
3. Keep the maximum value in `_out_arg_limit`
4. Include this maximum when allocating final frame size

### Example

Consider a method with three calls:
```java
void myMethod() {
    call6Params(a, b, c, d, e, f);       // Needs 0 bytes stack space (all in registers)
    call9Params(a, b, c, d, e, f, g, h, i);  // Needs 16 bytes (p8, p9)
    call11Params(...);                   // Needs 32 bytes (p8-p11)
}
```

**Reserved space calculation**:
- Encounter `call6Params`: `_reserved_argument_area_size = MAX2(0, 0) = 0`
- Encounter `call9Params`: `_reserved_argument_area_size = MAX2(0, 16) = 16`
- Encounter `call11Params`: `_reserved_argument_area_size = MAX2(16, 32) = 32`

**Final result**: The caller's `frame_size` will include **32 bytes** of outgoing argument area.

### Why This Design?

1. **Static allocation**: Compiled method frame size is determined at `register_method` time, cannot be dynamic
2. **Simplicity**: No need for runtime frame size adjustment
3. **Conservative but safe**: Always reserves enough space for the largest call
4. **Space for time tradeoff**: Slightly more stack usage, avoids complex dynamic resizing

### Stack Layout

```
Caller's Frame Layout:
Low addresses
[Local variables]
[Spill area]
[Outgoing argument area]  ← Reserved for callee's stack parameters (MAX2 of all calls)
...
[p11]                     ← sp + 24 (for 11-parameter call)
...
[p9]                      ← sp + 8
[p8]                      ← sp + 0
--------------------------  ← unextended_sp (caller's)
[fp]
[lr]
[return address]
--------------------------  ← caller's sender_sp
```

### Unwinding Process

When callee unwinds:
```cpp
l_sender_sp = callee.unextended_sp + callee.frame_size
            = caller.unextended_sp ✓
```

This lands exactly at the start of the caller's outgoing argument area, which is correct!

### Conclusion

**Outgoing argument space is statically allocated using MAX2 strategy**:
- Compiler scans all call sites during compilation
- Takes the maximum required stack parameter space
- Includes this maximum in the method's frame_size
- Same mechanism works for both C1 and C2
- Ensures correct parameter passing and stack unwinding for all call sites

---

## Appendix 3: Compiled-to-Interpreted Call Parameter Passing (c2i Adapter)

### Discovery Date: 2026-02-28

During analysis of 9-parameter method compilation, investigated how HotSpot handles parameter passing when a compiled method calls an interpreted method with >7 parameters.

### Problem Statement

When a compiled method calls an interpreted method with varying parameter counts:
- Some calls have 6 parameters (all in registers x0-x7)
- Some calls have 9 parameters (p8, p9 on stack)
- Some calls have 11 parameters (p8-p11 on stack)

**Question**: How does the c2i (compiled-to-interpreted) adapter handle parameter passing? Is the space dynamic or static?

### Key Findings

#### **Two-Phase Allocation: Static + Dynamic**

The c2i adapter uses a **hybrid approach**:
1. **Caller (compiled method)**: Statically reserves outgoing argument space using MAX2 strategy (Appendix 2)
2. **C2I adapter**: Dynamically allocates interpreter-specific space at runtime

#### **C2I Adapter Implementation**

From `hotspot/src/cpu/aarch64/vm/sharedRuntime_aarch64.cpp` Line 333-463 (`gen_c2i_adapter`):

**Step 1: Allocate Interpreter Stack Space** (Line 353-361):
```cpp
int extraspace = total_args_passed * Interpreter::stackElementSize;
extraspace = round_to(extraspace, 2*wordSize);

if (extraspace)
  __ sub(sp, sp, extraspace);  // ← Adjust sp downward
```

**Step 2: Copy Parameters to New Locations** (Line 364-457):
```cpp
// Now write the args into the outgoing interpreter space
for (int i = 0; i < total_args_passed; i++) {
  int st_off = (total_args_passed - i - 1) * Interpreter::stackElementSize;
  
  VMReg r_1 = regs[i].first();
  
  if (r_1->is_stack()) {
    // Stack parameter: copy from compiled frame to interpreter frame
    int ld_off = (r_1->reg2stack() * VMRegImpl::stack_slot_size
                  + extraspace
                  + words_pushed * wordSize);
    
    __ ldrw(rscratch1, Address(sp, ld_off));  // Read from old position
    __ str(rscratch1, Address(sp, st_off));   // Write to new position
    
  } else if (r_1->is_Register()) {
    // Register parameter: write from register to stack
    Register r = r_1->as_Register();
    __ str(r, Address(sp, st_off));  // Write register to stack position
  }
}
```

**Step 3: Set ESP and Jump to Interpreter** (Line 459-462):
```cpp
__ mov(esp, sp);  // Interp expects args on caller's expression stack
__ ldr(rscratch1, Address(rmethod, in_bytes(Method::interpreter_entry_offset())));
__ br(rscratch1);  // ← Jump to interpreter entry
```

#### **Frameless Design**

From Line 486-488:
```cpp
// A c2i adapter is frameless because the *callee* frame, which is
// interpreted, routinely repairs its caller's sp (from sender_sp,
// which is set up via the senderSP register).
```

**Key Points**:
- C2I adapter has **no frame** (`_frame_size = 0`)
- It temporarily adjusts sp, copies parameters, then jumps
- The interpreter repairs the caller's sp during unwinding

### Example: 9-Parameter Call

Consider compiled method A calling interpreted method B (9 int parameters):

**Caller A's Frame Layout (before call)**:
```
Low addresses
[Local variables]
[Spill area]
[Outgoing argument area]  ← Statically reserved (MAX2 of all calls, e.g., 32 bytes)
...
[p11]                     ← sp + 24 (unused for this call)
...
[p9]                      ← sp + 8
[p8]                      ← sp + 0
--------------------------  ← unextended_sp (A's)
[fp]
[lr]
[return address]
--------------------------  ← A's sender_sp
```

**After C2I Adapter Execution**:
```
Low addresses
[Interpreter argument area]  ← Dynamically allocated by c2i adapter
[p9]                         ← sp + 8 (copied from A's [sp+8])
[p8]                         ← sp + 0 (copied from A's [sp+0])
-----------------------------  ← New sp (adjusted by c2i adapter)
[A's outgoing argument area]
[p9]                         ← Original position (sp+8)
[p8]                         ← Original position (sp+0)
-----------------------------  ← A.unextended_sp
[A's other frame data]
```

**Parameter Distribution**:
- **p1-p7**: From registers x1-x7 → written to interpreter stack
- **p8-p9**: From A's stack `[A.unextended_sp + 0/8]` → copied to interpreter stack

### Unwinding Process

When interpreted method B unwinds:
```cpp
// frame::sender_for_interpreter_frame
intptr_t* sender_sp = this->sender_sp();
// For ASM interpreter: sender_sp = *(fp + sender_sp_offset)
//                                = *(fp + 2)

intptr_t* unextended_sp = interpreter_frame_sender_sp();
// Reads caller's unextended_sp from interpreter state
```

**Why This Works**:
- The interpreter's `sender_sp` was set **before** c2i adapter execution
- It points to **A's original unextended_sp**
- The c2i adapter's temporary allocation is **automatically ignored**
- Unwinding finds the correct caller frame!

### Comparison: i2c vs c2i Adapters

| Aspect | i2c Adapter | c2i Adapter |
|--------|-------------|-------------|
| **Direction** | Interpreter → Compiled | Compiled → Interpreter |
| **Frame** | Frameless | Frameless |
| **Space Allocation** | Adjusts sp, copies params | Adjusts sp, copies params |
| **Caller** | Interpreted | Compiled |
| **Callee** | Compiled | Interpreted |
| **Unwinding** | Uses callee's fp-derived sender_sp | Uses callee's fp-derived sender_sp |
| **Symmetry** | ✅ Perfectly symmetric design |

### Conclusion

**C2I adapter parameter passing**:
1. Caller statically reserves space using MAX2 strategy (compile-time)
2. C2I adapter dynamically allocates interpreter-specific space (runtime)
3. Parameters copied from caller's locations to interpreter locations
4. Adapter is frameless, temporary sp adjustment
5. Interpreter's fp-derived sender_sp automatically skips adapter space
6. Symmetric design with i2c adapter

This hybrid approach ensures:
- **Efficiency**: Static reservation avoids per-call allocation logic
- **Flexibility**: Dynamic sizing matches actual callee requirements
- **Correctness**: Frameless design enables proper unwinding

---

## Appendix 4: Yuhu Compiler Interop - Five Call Scenarios for >7 Parameters

### Discovery Date: 2026-02-28

Based on analysis of HotSpot's calling conventions for methods with >7 parameters, identified five interop scenarios that Yuhu compiler must support to be compatible with HotSpot's ABI.

### Problem Statement

Yuhu compiler must interoperate with HotSpot's existing compilation infrastructure (interpreter, C1, C2) while maintaining correct parameter passing for methods with >=8 parameters.

**Key Requirement**: Yuhu frame layout and calling convention must be compatible with all five call scenarios.

---

### ARM64 JVM Calling Convention (HotSpot)

**Register Assignment for Parameters**:
```
p0 → x1  (1st param, receiver for non-static)
p1 → x2  (2nd param)
p2 → x3  (3rd param)
p3 → x4  (4th param)
p4 → x5  (5th param)
p5 → x6  (6th param)
p6 → x7  (7th param)
p7 → x0  (8th param) ← Note: x0 is used for 8th parameter!
p8 → [sp+0]   (9th param, on stack)
p9 → [sp+8]   (10th param, on stack)
...
```

**Critical Points**:
- **Parameters 0-6**: Passed in x1-x7 (7 registers)
- **Parameter 7 (8th param)**: Passed in **x0** (not x8!)
- **Parameters 8+**: Passed on stack at `[sp + offset]`

**Why x0 for 8th parameter?**
- x0 is the return value register
- For method calls, x0 carries the 8th parameter
- After method returns, x0 carries the return value

---

### Scenario 1: Interpreter → Yuhu (>=8 params)

**Direction**: Interpreted method calls Yuhu-compiled method

**Mechanism**: i2c adapter
```
Interpreter frame
  ↓ (i2c adapter adjusts sp, copies p8+)
Yuhu method entry
```

**Requirements**:
- Yuhu method entry must read p0-p6 from x1-x7 registers
- Yuhu method entry must read **p7 (8th param) from x0**
- Yuhu method entry must read p8+ from `[sp + offset]`
- Current implementation: `CreateReadEspRegister()` reads x20, then `sp + offset` access

**Verification Needed**:
- Is x20 register correctly pointing to post-i2c-adapter sp?
- Does i2c adapter's sp adjustment match Yuhu's expectations?
- **Is x0 correctly handled as the 8th parameter?**

**Status**: ⚠️ Implementation exists for p8+, needs verification for p7 (x0)

---

### Scenario 2: C1/C2 → Yuhu (>=8 params)

**Direction**: C1/C2 compiled method calls Yuhu-compiled method

**Mechanism**: Direct compiled-to-compiled call
```
C1/C2 frame (with outgoing arg area reserved via MAX2)
  ↓ (p7 in x0, p8+ already at [caller.unextended_sp + offset])
Yuhu method entry
```

**Requirements**:
- Caller (C1/C2) reserves outgoing argument space using MAX2 strategy
- At call time:
  - p0-p6: In x1-x7
  - **p7: In x0**
  - p8+: At `[caller.unextended_sp + offset]`
- Yuhu method entry reads p7 from x0, p8+ from `sp + offset`

**Yuhu Frame Layout**:
```llvm
; Yuhu method frame
%frame = alloca [80 x i64]  ; Fixed 80 bytes

; Incoming parameter locations (relative to frame base)
; p0-p6: Read from x1-x7, stored in frame[0-6]
; p7:    Read from x0, stored in frame[7]     ← 8th parameter!
; p8-p9: Read from [sp+offset], stored in frame[8-9]

; Outgoing argument area (if this method makes calls)
; Reserved at low-address area of frame using MAX2
```

**Key Constraint**:
- Yuhu prolog must not corrupt caller's outgoing argument area
- Incoming parameters should be placed at consistent frame locations
- **Must handle x0 specially as 8th parameter, not return value**

**Status**: ⚠️ Requires Yuhu frame layout standardization and x0 handling

---

### Scenario 3: Yuhu → Interpreter (>=8 params)

**Direction**: Yuhu-compiled method calls interpreted method

**Mechanism**: c2i adapter
```
Yuhu frame (with outgoing arg area reserved)
  ↓ (Yuhu stores p7 to x0, p8+ to outgoing area)
  ↓ (c2i adapter allocates interpreter space, copies p8+)
Interpreter entry
```

**Requirements**:
- Yuhu as caller must reserve outgoing argument space (MAX2 strategy)
- Before call, Yuhu must:
  - Place p0-p6 in x1-x7
  - **Place p7 in x0**
  - Place p8+ to outgoing area
- c2i adapter will copy p8+ to interpreter frame

**Yuhu Implementation**:
```llvm
; Reserve outgoing parameter area (e.g., max 11 params = 48 bytes)
%outgoing_area = alloca [6 x i64]  ; For p8-p13

; Prepare parameters before call
; p0-p6 already in x1-x7 (by convention)
; p7 must be moved to x0
mov x0, %p7                          ; ← Move 8th param to x0

; Store stack parameters
store i32 %p8, ptr %outgoing_area        ; p8 → [outgoing+0]
store i32 %p9, ptr %outgoing_area+8      ; p9 → [outgoing+8]

; Set up argument registers and jump
call void @prepare_call_and_jump()
```

**Status**: ⚠️ Requires x0 parameter preparation logic

---

### Scenario 4: Yuhu → Yuhu (>=8 params)

**Direction**: Yuhu-compiled method calls another Yuhu-compiled method

**Mechanism**: Direct Yuhu-to-Yuhu call
```
Caller Yuhu frame (with outgoing arg area via MAX2)
  ↓ (stores p7 to x0, p8+ to outgoing area)
Callee Yuhu frame
  ↓ (reads p7 from x0, p8+ from sp+offset)
```

**Symmetric Design**:
```llvm
; Caller (Yuhu method A)
%outgoing = alloca [6 x i64]  ; Reserve for p8-p13

; Prepare call arguments
; p0-p6 in x1-x7 (already there by convention)
mov x0, %p7                    ; ← Move 8th param to x0
store i32 %p8, ptr %outgoing   ; p8 → [outgoing+0]
store i32 %p9, ptr %outgoing+8 ; p9 → [outgoing+8]

; Callee (Yuhu method B)
%p7 = load i32, ptr x0         ; ← Read 8th param from x0
%p8 = load i32, ptr %sp        ; Read from [sp+0]
%p9 = load i32, ptr %sp+8      ; Read from [sp+8]
```

**Requirements**:
- Consistent frame layout across all Yuhu methods
- Caller reserves outgoing space using MAX2
- **Caller places p7 in x0**
- Callee reads p7 from x0, p8+ from sp+offset

**Status**: ⚠️ Requires standardized Yuhu calling convention with x0 handling

---

### Scenario 5: Yuhu → C1/C2 (>=8 params)

**Direction**: Yuhu-compiled method calls C1/C2 compiled method

**Mechanism**: Direct compiled-to-compiled call
```
Yuhu frame (with outgoing arg area)
  ↓ (Yuhu prepares p7 in x0, p8+ at [sp+offset])
C1/C2 method entry (expects p7 in x0, p8+ at specific locations)
```

**Requirements**:
- Yuhu as caller must follow C1/C2 calling convention
- p0-p6: Place in x1-x7 registers
- **p7: Place in x0 register**
- p8+: Place at `[sp + offset]` (where C1/C2 expects them)

**Challenge**:
- How to precisely control register allocation in LLVM IR?
- **Special handling for x0 as 8th parameter**

**Implementation Approach**:
```cpp
void YuhuEmitter::emit_call(LIR_Call* call) {
  int num_stack_args = call->num_stack_arguments();
  
  // 1. Prepare register arguments (p0-p6)
  for (int i = 0; i < min(7, call->num_args); i++) {
    emit_move(arg_reg[i+1], call->arg(i));  // x1-x7
  }
  
  // 2. Handle 8th parameter specially (p7 → x0)
  if (call->num_args > 7) {
    emit_move(x0, call->arg(7));  // ← p7 goes to x0!
  }
  
  // 3. Prepare stack arguments (p8+)
  for (int i = 7; i < call->num_args; i++) {
    int offset = (i - 7) * wordSize;
    emit_store(call->arg(i), Address(sp, offset));
  }
  
  // 4. Emit call instruction
  emit_bl(call->target());
}
```

**Status**: ⚠️ Requires call emission logic with precise ABI compliance including x0

---

### Proposed Yuhu Standard Frame Layout

```
Yuhu Frame (Total: 80 bytes + outgoing_area)

High addresses
[frame[14]]  ← local[4] (cache slot)
...
[frame[10]]  ← local[0]
[frame[9]]   ← stack[0] (call result temp)
...
[frame[5]]   ← stack[4]
[frame[4]]   ← spill slots
...
[frame[0]]   ← incoming p0 (receiver/this)
[frame[1]]   ← incoming p1
...
[frame[6]]   ← incoming p6
[frame[7]]   ← incoming p7 (from x0!) ← 8th parameter
[frame[8]]   ← incoming p8 (from sp+0)
[frame[9]]   ← incoming p9 (from sp+8)
[outgoing_area]  ← Reserved for callee's stack parameters
------------------  ← sp (on method entry)
[fp]
[lr]
[return address]
```

**Parameter Mapping**:
- **p0-p6**: From x1-x7 → stored in frame[0-6]
- **p7**: From **x0** → stored in frame[7] (special case!)
- **p8+**: From `[sp+offset]` → stored in frame[8+]

### Implementation Priority

**Phase 1: Verify Scenario 1** (Interpreter → Yuhu)
- Test 8-parameter and 9-parameter methods
- Verify x20 register reading is correct for p8+
- **Verify x0 handling for p7 is correct**
- Confirm stack parameter access works

**Phase 2: Implement Scenarios 2 & 4** (C1/C2 → Yuhu, Yuhu → Yuhu)
- Define standard Yuhu frame layout with **x0 as p7**
- Implement outgoing parameter reservation (MAX2)
- Modify call emission logic to handle x0 specially

**Phase 3: Implement Scenarios 3 & 5** (Yuhu → Interpreter, Yuhu → C1/C2)
- Implement parameter preparation logic
- **Special handling: move p7 to x0 before calls**
- May require inline assembly for register control
- Ensure full ABI compliance

### Key Design Decisions

1. **Incoming Parameter Handling**:
   - Use existing `CreateReadEspRegister()` for p8+
   - Read p7 from **x0** (not from stack!)
   - Store to standardized frame locations

2. **Outgoing Parameter Handling**:
   - Add `YuhuFrameLayout` class to track MAX2 reservations
   - Allocate outgoing area at frame's low-address end
   - **Move p7 to x0 before calls**

3. **Call Emission**:
   - Generate register moves for p0-p6 (x1-x7)
   - **Generate special move for p7 (to x0)**
   - Generate stack stores for p8+
   - Ensure proper alignment

### Next Steps

1. **Verify x20 mechanism** in Scenario 1
2. **Define Yuhu frame layout standard** with x0 handling for Scenarios 2 & 4
3. **Implement call emission** with x0 special case for Scenarios 3 & 5
4. **Test all five scenarios** with 8+ parameter methods
5. **Document calling convention** emphasizing x0 as 8th parameter

# Activity 066: Replace BufferBlob with RuntimeStub for Java Method Calls

## Problem Description

### x19 Register Corruption During Java-to-Java Calls

When Yuhu-compiled code calls another Java method (via interpreter or compiled code), register x19 gets corrupted because:

1. **Yuhu uses x19** as a callee-saved register across `blr` calls
2. **LLVM AArch64 does not support `+reserve-x19`** target feature — the error `'+reserve-x19' is not a recognized feature for this target` occurs because LLVM only accepts actual CPU features (like `+neon`, `+sve`), not compiler-level register reservation options
3. **Interpreter/other compiled methods may clobber x19** — the callee does not preserve x19 across the call

Example from generated code:
```assembly
0x000000010e0929e0: mov    x19, x11
0x000000010e0929e4: blr    x8              ; x19 used across this call
0x000000010e0929e8: ldr    x10, [sp, #64]  ; x19 may be corrupted after return
```

### Why Standard LLVM Register Reservation Fails

LLVM's AArch64 backend does not support `+reserve-xN` syntax. Register reservation requires:
- `TargetOptions::ReservedRegs = {AArch64::X19, ...}` (programmatic)
- `-reserve-register=x19` (command-line flag)

However, even with `ReservedRegs` set, LLVM AArch64 has limitations on which registers can be reserved, and x19 is a standard callee-saved register that LLVM expects to be available for register allocation.

## Current Approach: BufferBlob

Yuhu currently uses `BufferBlob` for Java method call adapters:
- BufferBlob is a simple code container without frame metadata
- No OopMap support
- No frame size registration
- GC cannot walk through BufferBlob frames correctly

This approach fails because:
1. **No x19 preservation**: BufferBlob doesn't save/restore x19
2. **No GC stack walking support**: BufferBlob has no OopMap or frame size metadata
3. **No proper frame setup**: Missing FP/SP chain that GC relies on

## Solution: Replace BufferBlob with RuntimeStub

### What is RuntimeStub?

RuntimeStub is a CodeBlob type used by C1/C2 compilers for runtime call adapters. Key properties:
- Supports OopMap generation for GC stack walking
- Has explicit frame size registration
- Properly sets up frame pointer chain
- Can be created dynamically at JIT compilation time via `RuntimeStub::new_runtime_stub()`

### RuntimeStub Design for Yuhu Java Method Calls

**Call chain:** Yuhu compiled method → RuntimeStub → Java method (interpreter or compiled)

**Stub responsibilities:**
1. **Save x19 to stack**: Preserve x19 before calling Java method
2. **Set up frame**: Push FP, adjust SP, establish frame pointer chain
3. **Call Java method**: Branch to target method
4. **Restore x19**: Recover x19 from stack before returning
5. **Return to Yuhu**: Restore caller's frame

**Frame layout:**
```
[higher addresses]
+------------------+
| saved FP (x29)   |  ← frame pointer chain (FP points here)
+------------------+
| saved LR (x30)   |  ← return address to Yuhu
+------------------+
| saved x19        |  ← x19 saved here
+------------------+
| local variables  |  (if any)
+------------------+
| argument area    |  (for Java method call)
+------------------+  ← SP points here (16-byte aligned)
[lower addresses]
```

### GC Stack Walking Support

**Frame size:** Must include saved x19 slot, saved FP, saved LR, and any locals. Passed to `RuntimeStub::new_runtime_stub()` via `frame_size_in_words` parameter.

**OopMap:** Not required for x19 if it holds non-oop values (raw pointers, integers). However, if x19 holds an oop, an OopMap entry is needed at the call site to inform GC of x19's stack location.

**Frame pointer chain:** RuntimeStub pushes FP, maintaining the chain: callee FP → RuntimeStub FP → Yuhu FP. GC walks this chain to traverse frames.

**last_Java_frame:** The stub does NOT set `last_Java_pc`, `last_Java_sp`, or `last_Java_fp` (Java-to-Java call, not VM call). GC stack walking works through frame chain, not anchor.

### RuntimeStub Generation in YuhuRuntime

Three stub generation methods are added to `YuhuRuntime`:

```cpp
class YuhuRuntime {
 public:
  // Generate static call stub for direct method calls
  static address generate_static_call_stub(ciMethod* target_method, 
                                           ciMethod* current_method);
  
  // Generate virtual call stub for virtual/interface method calls
  // This stub performs vtable/itable lookup at runtime and jumps to _from_compiled_entry
  static address generate_virtual_call_stub(ciMethod* target_method, 
                                            ciMethod* current_method, 
                                            int vtable_index);
  
  static address generate_interface_call_stub(ciMethod* target_method, 
                                              ciMethod* current_method);
};
```

Each method generates a RuntimeStub that:
1. Saves x19, LR, FP to stack
2. Sets up frame pointer (FP = SP)
3. Performs call-specific logic (static: direct call, virtual: vtable lookup, interface: itable lookup)
4. Calls target method via `_from_compiled_entry` (interpreter entry) or compiled entry
5. Restores x19, LR, FP
6. Returns to Yuhu compiled method

**Stub code generation pattern:**

```cpp
address YuhuRuntime::generate_static_call_stub(ciMethod* target_method,
                                                ciMethod* current_method) {
  ResourceMark rm;
  CodeBuffer buffer("yuhu_static_call_stub", 512, 512);
  MacroAssembler masm(&buffer);
  
  // Prologue: save registers
  __ stp(x29, x30, Address(__ pre(sp, -2 * wordSize)));
  __ str(x19, Address(__ pre(sp, -1 * wordSize)));
  __ mov(x29, sp);
  
  // Get _from_compiled_entry from Method*
  __ ldr(xscratch1, Address(rmethod, Method::from_compiled_offset()));
  
  // Call target
  __ blr(xscratch1);
  
  // Epilogue: restore registers
  __ ldr(x19, Address(__ post(sp, 1 * wordSize)));
  __ ldp(x29, x30, Address(__ post(sp, 2 * wordSize)));
  __ ret();
  
  // Create RuntimeStub
  int frame_size_in_words = 3; // x19, FP, LR
  RuntimeStub* stub = RuntimeStub::new_runtime_stub(
      "yuhu_static_call_stub",
      &buffer,
      CodeOffsets::frame_never_safe,
      frame_size_in_words,
      NULL,  // no oops saved
      false
  );
  
  return stub->entry_point();
}
```

**Frame size calculation:**
- Saved x19: 1 word (8 bytes on AArch64)
- Saved FP: 1 word
- Saved LR: 1 word
- Stack alignment: must be 16-byte aligned
- Total frame size: round_up(3 * 8, 16) / 8 = 4 words minimum (includes alignment padding)

**OopMap decision:**
- If x19 holds non-oop (klass pointer, integer, raw address): no OopMap needed
- If x19 holds oop: must create OopMap with entry at x19's stack slot

**last_Java_frame:** The stub does NOT set `last_Java_pc`, `last_Java_sp`, or `last_Java_fp` (Java-to-Java call, not VM call). GC stack walking works through frame chain, not anchor.

**Thread safety:** CodeCache allocation must be done under appropriate locks if concurrent compilation is possible. CodeCache has finite size; stub creation consumes space.

**Stub reuse:** Cache stubs per `(target_method, call_type)` pair to avoid creating duplicate stubs for identical call patterns. Use a hashtable or similar structure in YuhuRuntime.

## Benefits

1. **x19 preservation**: Stub saves/restores x19, preventing corruption
2. **GC stack walking**: Proper frame metadata allows GC to walk through Yuhu frames
3. **No LLVM register reservation needed**: Workaround LLVM's limitation on reserving x19
4. **Consistent with HotSpot**: Uses same RuntimeStub mechanism as C1/C2 compilers
5. **Dynamic creation**: Stubs created at JIT time, no compile-time overhead

## Risks

1. **CodeCache space**: Each stub consumes CodeCache memory
2. **Performance overhead**: Additional save/restore instructions per Java method call
3. **Complexity**: Managing stub lifecycle and caching

## Alternative Approaches Considered

### 1. LLVM ReservedRegs (Failed)
Setting `TM->Options.ReservedRegs = {AArch64::X19}` doesn't work reliably on AArch64 — x19 is a standard callee-saved register that LLVM's register allocator expects to use.

### 2. Don't use x19 in Yuhu (Not Viable)
Yuhu's inline asm for `last_Java_pc` setup uses x19 (and other registers). Removing x19 usage would require extensive refactoring of the stack/frame management code.

### 3. Modify interpreter to preserve x19 (Rejected)
Changing interpreter's register usage would affect all Java method calls, not just Yuhu, and risks breaking other compiler backends.

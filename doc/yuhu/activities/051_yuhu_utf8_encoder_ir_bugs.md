# Activity 051: Yuhu IR Generation Bugs in UTF-8 Encoder Surrogate and Exit Paths

**Date**: 2026-02-28  
**Author**: User  
**Related Files**: `test_yuhu/_tmp_yuhu_ir_sun.nio.cs.UTF_8$Encoder__encodeArrayLoop.ll`

---

## Problem Summary

Two critical bugs identified in Yuhu-generated LLVM IR for `sun.nio.cs.UTF_8$Encoder.encodeArrayLoop()`:

### **Bug #1: Missing Surrogate Pair Handling Logic**

**Location**: `bci_253` and `bci_242` blocks (Lines 1609-1695)

**Expected Java Logic** (UTF_8.java Lines 591-607):
```java
else if (Character.isSurrogate(c)) {
    // Have a surrogate pair
    if (sgp == null)
        sgp = new Surrogate.Parser();
    
    int uc = sgp.parse(c, sa, sp, sl);
    
    if (uc < 0) {
        updatePositions(src, sp, dst, dp);
        return sgp.error();
    }
    
    if (dl - dp < 4)
        return overflow(src, sp, dst, dp);
    
    // 4-byte UTF-8 encoding
    da[dp++] = (byte)(0xf0 | ((uc >> 18)));
    da[dp++] = (byte)(0x80 | ((uc >> 12) & 0x3f));
    da[dp++] = (byte)(0x80 | ((uc >>  6) & 0x3f));
    da[dp++] = (byte)(0x80 | (uc & 0x3f));
    
    sp++;  // 2 chars
}
```

**Actual Generated IR**:
```llvm
bci_253:                                          ; preds = %not_zero335
  ; ... frame state saving ...
  %703 = call i32 inttoptr (i64 4407554176 to ptr)(ptr %thread_ptr, i32 22)
  br label %unified_exit    ; ❌ Premature exit!

bci_242:                                          ; preds = %not_zero335
  ; ... frame state saving ...
  %720 = call i32 inttoptr (i64 4407554176 to ptr)(ptr %thread_ptr, i32 22)
  br label %unified_exit    ; ❌ Premature exit!
```

**Missing Components**:
1. ❌ `sgp.parse(c, sa, sp, sl)` method call
2. ❌ Return value check (`uc < 0`)
3. ❌ Error handling and early return path
4. ❌ Buffer space check (`dl - dp < 4`)
5. ❌ 4-byte UTF-8 encoding logic
6. ❌ `sp++` increment (for 2 chars)

**Impact**: Surrogate pairs cannot be encoded. Method exits prematurely without writing any output.

---

### **Bug #2: Illegal Absolute Address Load in Exit Path**

**Location**: `no_exception140` block (Line ~720)

**Buggy IR**:
```llvm
no_exception140:                                  ; preds = %bci_476
  %300 = load ptr, ptr inttoptr (i64 96 to ptr), align 8  ; ❌ ILLEGAL!
  %301 = load i64, ptr %1199, align 8
  %302 = add i64 %301, 96
  store i64 %302, ptr %sp_storage, align 8
  ; ... more operations ...
  br label %unified_exit
```

**Problem**: 
- `%300` attempts to load from **absolute address 96** (0x60)
- This is **not a valid Java object address**
- Causes **SIGSEGV** at runtime when dereferencing address 0x60

**Corresponding Assembly** (from debug/yuhu_new_code.txt):
```asm
0x000000010b127c38: mov	w8, #0x60                  	// #96
0x000000010b127c3c: ldr	x8, [x8]                    ; ❌ CRASH: tries to load from address 96
```

**Expected Behavior**: Should perform proper base+offset addressing or normal field access.

**Impact**: Immediate SIGSEGV crash when this code path is executed.

---

## Root Cause Analysis

### **Type Flow Analysis Output**

Enabled via `-XX:+YuhuPrintTypeflowOf=sun.nio.cs.UTF_8$Encoder.encodeArrayLoop`.

Output saved to: `debug/encodeArrayLoop.txt`

**Key Observations**:

All expected blocks are present in ciTypeFlow output:

```text
ciBlock [235 - 242) control : 239:ifnonnull
  Successors : 2
    #14 rpo#16 [242 - 253)
    #13 rpo#17 [253 - 274)

ciBlock [242 - 253) control : 253:fall through
  Successors : 0
  Traps on 243 with trap index 22

ciBlock [253 - 274) control : 271:ifge
  Successors : 0
  Traps on 264 with trap index 22
```

**BCI Ranges** (corresponding to UTF_8.java Lines 599-614):
- BCI 227-235: `Character.isSurrogate(c)` check
- BCI 235-242: `if (sgp == null)` check  
- BCI 242-253: `sgp = new Surrogate.Parser()`
- BCI 253-274: `sgp.parse(c, sa, sp, sl)` call
- BCI 274-291: `if (uc < 0)` error handling
- BCI 291-300: `if (dl - dp < 4)` space check
- BCI 300-310: `return overflow(...)`
- BCI 310-394: 4-byte UTF-8 encoding logic

---

### **Yuhu IR Generation Process**

Based on `yuhuTopLevelBlock.cpp` and `yuhuBlock.cpp`:

**Conversion Flow**:

```
ciTypeFlow CFG → YuhuTopLevelBlock → parse_bytecode() → LLVM IR
```

1. **Block Creation** (`YuhuFunction::build()` at Line 307-322):
   ```cpp
   for (int i = 0; i < block_count(); i++) {
     ciTypeFlow::Block *b = flow()->pre_order_at(i);
     _blocks[b->pre_order()] = new YuhuTopLevelBlock(this, b);
   }
   ```
   - Each `ciTypeFlow::Block` becomes a `YuhuTopLevelBlock`
   - Preserves BCI ranges and successor relationships

2. **Control Flow Traversal** (Line 336):
   ```cpp
   start_block->enter();  // Recursively traverses all reachable blocks
   ```

3. **IR Emission** (`YuhuTopLevelBlock::emit_IR()` at Line 337-347):
   ```cpp
   void YuhuTopLevelBlock::emit_IR() {
     builder()->SetInsertPoint(entry_block());
     parse_bytecode(start(), limit());  // ← Converts bytecode to IR
     if (falls_through() && !has_trap())
       do_branch(ciTypeFlow::FALL_THROUGH);
   }
   ```

4. **Bytecode Parsing** (`YuhuBlock::parse_bytecode()` at Line 40-900+):
   - Iterates through bytecode from `start` to `limit`
   - Switch statement handles each bytecode type:
     - `_invokevirtual`, `_invokestatic`, etc. → `do_call()` (Line 851-856)
     - `_ifge`, `_ifeq`, etc. → `do_if()` (Line 812-813)
     - `_goto` → `do_goto()` (Line 767-770)
     - Field access, arithmetic, loads/stores, etc.
   - Generates corresponding LLVM IR instructions

5. **Branch Handling** (`YuhuTopLevelBlock::do_branch()` at Line 1093-1097):
   ```cpp
   void YuhuTopLevelBlock::do_branch(int successor_index) {
     YuhuTopLevelBlock *dest = successor(successor_index);
     builder()->CreateBr(dest->entry_block());
     dest->add_incoming(current_state());
   }
   ```
   - Creates LLVM branch to successor's entry block
   - Passes current state for PHI node merging

---

### **Missing Blocks in Yuhu Traversal**

**Observation**: 
- Yuhu's `block_count()` returns 24
- ciTypeFlow print output shows 26 blocks
- **Missing 4 blocks**:
```
ciBlock [274 - 291) control : 290:areturn     ← No Blocks
ciBlock [291 - 300) control : 297:if_icmpge   ← No Blocks  
ciBlock [300 - 310) control : 309:areturn     ← No Blocks
ciBlock [310 - 394) control : 391:goto        ← No Blocks
```

**Root Cause in ciTypeFlow**:

In `ciTypeFlow::Block::successors()` (ciTypeFlow.cpp Line 1638-1643):

```cpp
bool has_successor = !has_trap() &&
                     (control() != ciBlock::fall_through_bci || limit() < analyzer->code_size());
if (!has_successor) {
  _successors = new (arena) GrowableArray<Block*>(arena, 1, 0, NULL);
  // No successors - does NOT call block_at() to create successor blocks
}
```

**BCI 253-274 has a trap** (`Traps on 264 with trap index 22`), so:
1. `has_trap()` returns `true`
2. `has_successor == false`
3. **Successor blocks are NEVER created** via `block_at()`
4. These blocks never get added to the CFG
5. `_next_pre_order` never increments for these blocks
6. **`block_count()` returns 24 instead of 26**

**Why Type Flow print shows 26 blocks**:
- Print output iterates all `ciBlock` objects from `ciMethodBlocks`
- But actual CFG (via `pre_order_at()`) only has 24 blocks
- The 4 "missing" blocks exist as `ciBlock` but not as `ciTypeFlow::Block` in the CFG

**Impact**: 
- BCI 274-394 contains critical surrogate pair handling logic
- These blocks don't exist in ciTypeFlow's CFG
- Yuhu cannot generate IR for non-existent blocks
- Method exits prematurely via `unified_exit`

---

## Suggested Debugging Approach

### **Step 1: Verify Type Flow Analysis**

Enable Type Flow printing:
```bash
-XX:YuhuPrintTypeflowOf=sun.nio.cs.UTF_8$Encoder::encodeArrayLoop
```

Output saved to: `debug/encodeArrayLoop.txt`

Compare original bytecode with generated IR:
```bash
javap -c sun.nio.cs.UTF_8$Encoder | grep -A 50 "encodeArrayLoop"
# vs
cat test_yuhu/_tmp_yuhu_ir_sun.nio.cs.UTF_8\$Encoder__encodeArrayLoop.ll
```

Identify exactly which bytecode instructions map to:
- `bci_253` and `bci_242` entry points
- `no_exception140` block

### **Step 2: Check Yuhu Pass Logs**

Enable verbose logging to see:
- Which Yuhu passes run on this method
- Whether any passes report errors or warnings
- Where in the pipeline these bugs are introduced

### **Step 3: Compare with Working Paths**

Analyze why ASCII and 2-byte encoding work correctly:
- What makes their IR generation succeed?
- Are there structural differences in the bytecode?
- Do they use similar patterns that should also fail?

### **Step 4: Inspect C++ Code Generation Points**

Focus on likely culprits:
1. **`BytecodeToIRGenerator::invoke()`** - for missing method calls
2. **`YuhuIRBuilder::CreateLoad()`** - for address computation bugs
3. **`CFGBuilder::addSuccessor()`** - for control flow issues
4. **`FrameStateSaver::save_all()`** - for frame manipulation bugs

---

## Impact Assessment

**Severity**: **Critical**

**Affected Functionality**:
- UTF-8 encoding of surrogate pairs (Unicode U+10000 to U+10FFFF)
- Any text containing emoji, rare CJK characters, musical notation, etc.
- Results in data corruption or crashes

**Frequency**: 
- Surrogate pairs are common in modern text (emoji alone...)
- Bug #2 causes immediate crash on certain code paths

**Workaround**: None at Yuhu level. Must fall back to C1/C2 compilation.

---

## Next Steps

1. **Document findings** in Yuhu bug tracker
2. **Create minimal test case** that reproduces the issue
3. **Add debugging instrumentation** to trace IR generation step-by-step
4. **Fix candidate locations** identified in hypotheses above
5. **Verify fixes** with comprehensive UTF-8 encoding tests

---

## Open Questions

### **Why Don't C1/C2 Have This Problem?**

If ciTypeFlow's CFG only has 24 blocks (missing 4), why do C1/C2 compilers work correctly?

**Resolution**: The 4 missing blocks are not a bug in C1/C2 — they are intentionally excluded from the CFG due to **trap handlers**.

**Root Cause Discovery**:

During investigation, it was discovered that:

1. **ciTypeFlow sets traps on blocks with unloaded classes**
   - BCI 243: `new Surrogate.Parser()` - class not loaded at compile time
   - BCI 264: `sgp.parse(c, sa, sp, sl)` - method holder not loaded
   - Trap index 22 = constant pool index of unloaded class/method

2. **When a block has `has_trap() == true`, ciTypeFlow does NOT create successor blocks**
   - Code in `ciTypeFlow::Block::successors()` (Line 1638-1643):
     ```cpp
     bool has_successor = !has_trap() &&
                          (control() != ciBlock::fall_through_bci || limit() < analyzer->code_size());
     if (!has_successor) {
       _successors = new (arena) GrowableArray<Block*>(arena, 1, 0, NULL);
       // No successors - does NOT call block_at() to create successor blocks
     }
     ```

3. **This is by design**: C1/C2 handle this by deoptimizing back to the interpreter when a trap block is reached at runtime.

4. **Yuhu's problem**: Yuhu does NOT support deoptimization, so it cannot handle methods with trap blocks.

---

## Solution Implemented

### **Compilation Policy for Methods with Trap Blocks**

**Location**: `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` (after Line 529)

**Implementation**:
```cpp
// Bail out if any block has a trap (unloaded class at compile time).
// Yuhu does not support deoptimization, so such methods cannot be compiled.
for (int i = 0; i < flow->block_count(); i++) {
  if (flow->pre_order_at(i)->has_trap()) {
    env->record_failure("block has trap (unloaded class)");
    return;
  }
}
```

**Rationale**:
- **Option 1**: Bail out at compilation time (chosen approach)
  - Simple, safe, consistent with existing `flow->failing()` bailouts
  - Methods with trap blocks fall back to C1/C2 or interpreter
  
- **Option 2**: Generate deoptimization code (rejected)
  - Would require full deopt infrastructure (stubs, state preservation, interpreter handoff)
  - Far too complex for Yuhu's current architecture

**Impact**:
- Methods like `UTF_8$Encoder.encodeArrayLoop()` will no longer be compiled by Yuhu
- They will execute in C1/C2 or interpreter mode instead
- This is acceptable because:
  - Such methods are rare (only those with unloaded classes at compile time)
  - Fallback to C1/C2 maintains correctness
  - Avoids generating broken IR

---

## References

- UTF_8.java source: `jdk/src/share/classes/sun/nio/cs/UTF_8.java` Lines 570-627
- Surrogate.Parser: `jdk/src/share/classes/sun/nio/cs/Surrogate.java`
- Crash log: `test_yuhu/hs_err_pid42692.log`
- Disassembly: `debug/yuhu_new_code.txt`
- Related memory: `unified_exit as incomplete-IR sentinel`

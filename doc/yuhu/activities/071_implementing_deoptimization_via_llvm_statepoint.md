# Activity 071: Implementing Deoptimization for Yuhu Compiler via LLVM Statepoint Infrastructure

**Date**: 2026-05-14  
**Author**: Yuhu Compiler Team  
**Related Files**: 
- `hotspot/src/share/vm/yuhu/yuhuRewriteStatepointsForGC.cpp`
- `hotspot/src/share/vm/yuhu/yuhuRuntime.cpp` (YuhuRuntime::uncommon_trap)
- `hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp` (do_trap implementation)
- `hotspot/src/share/vm/code/debugInfoRecorder.cpp` (ScopeDesc generation)

---

## Problem Summary

Yuhu currently **bails out** of compiling methods that contain trap blocks (unloaded classes, uninitialized statics, JSR 292 intrinsics, etc.). This limits Yuhu's applicability — methods like `UTF_8$Encoder.encodeArrayLoop()` cannot be compiled because they have trap sites.

**Current workaround** (from Activity 051):
```cpp
// yuhuCompiler.cpp
for (int i = 0; i < flow->block_count(); i++) {
  if (flow->pre_order_at(i)->has_trap()) {
    env->record_failure("block has trap (unloaded class)");
    return;  // Fall back to C1/C2
  }
}
```

**Goal**: Implement full deoptimization support so Yuhu can compile methods with trap sites, deoptimizing back to the interpreter when traps are reached at runtime.

---

## LLVM Infrastructure Available

### 1. **RS4GC (RewriteStatepointsForGC) Pass**

Yuhu's custom RS4GC pass (`yuhuRewriteStatepointsForGC.cpp`) already provides:

- ✅ **Deopt operand bundle preservation**: Extracts and passes through `[ "deopt"(value1, value2, ...) ]` bundles.
- ✅ **Live value tracking**: Ensures all deopt operand values are materialized at safepoints.
- ✅ **GC relocation for deopt oops**: If oop-typed values are in deopt bundle, they get relocated via `gc.relocate`.
- ✅ **StackMap emission**: LLVM can emit `__llvm_stackmaps` section with live value locations (register numbers, stack offsets).

**What RS4GC does**:
```llvm
; Before RS4GC:
call void @trap_function(i32 %trap_request) [ "deopt"(i64 %local0, i64 %local1, i64 %stack0) ]

; After RS4GC:
%token = call token (i64, i32, void ()*, ...) @llvm.experimental.gc.statepoint(
  i64 0, i32 0, void ()* @trap_function,
  i32 0, i32 0, i32 0, i32 0,
  i32 0,  ; GC arguments
  [ "deopt"(i64 %local0, i64 %local1, i64 %stack0) ],  ; Deopt bundle preserved
  <live oops>  ; GC live set
)
%relocal0 = call i64 @llvm.gc.relocate.i64(token %token, i32 0, i32 0)
%relocal1 = call i64 @llvm.gc.relocate.i64(token %token, i32 1, i32 1)
...
```

### 2. **StackMap Section**

LLVM emits metadata describing where live values are located at each statepoint:

```
__llvm_stackmaps section:
  Statepoint ID: 0x12345678  (function address passed to gc.statepoint)
  Instruction offset: 42  (bytes from function start)
  Live values:
    [0] Location: Register 5 (x5)
    [1] Location: Stack offset +48 from SP
    [2] Location: Register 8 (x8)
    ...
```

This can be parsed at runtime to extract live value locations.

---

## Critical Prerequisite: Fix Block Creation for Trapped Successors

**BLOCKER**: Before deoptimization can work, Yuhu must fix a fundamental bug in how it creates basic blocks from ciTypeFlow.

### The Problem

**ciTypeFlow intentionally omits successors of trapped blocks.** From `ciTypeFlow::Block::successors()` (ciTypeFlow.cpp line 1638-1643):

```cpp
bool has_successor = !has_trap() &&
                     (control() != ciBlock::fall_through_bci || limit() < analyzer->code_size());
if (!has_successor) {
  _successors = new (arena) GrowableArray<Block*>(arena, 1, 0, NULL);
  // No successors created — blocks after trap don't exist!
}
```

**Example from `debug/encodeArrayLoop.txt`:**

```
ciBlock [242 - 253) control : 253:fall through
  #14 rpo#16 [242 - 253)
  Successors : 0              ← NO SUCCESSORS!
  Traps on 243 with trap index 22

ciBlock [274 - 291) control : 290:areturn
  No Blocks                   ← Never created by ciTypeFlow!

ciBlock [291 - 300) control : 297:if_icmpge
  No Blocks                   ← Never created by ciTypeFlow!

ciBlock [300 - 310) control : 309:areturn
  No Blocks                   ← Never created by ciTypeFlow!

ciBlock [310 - 394) control : 391:goto
  No Blocks                   ← Never created by ciTypeFlow!
```

**Yuhu's current block creation** (yuhuFunction.cpp line 300-304):

```cpp
for (int i = 0; i < block_count(); i++) {
  ciTypeFlow::Block *b = flow()->pre_order_at(i);
  _blocks[b->pre_order()] = new YuhuTopLevelBlock(this, b);
}
```

**The bug**:
- `block_count()` returns **24** (only blocks that ciTypeFlow created).
- Blocks #15, #16, #17, #18, #19 (BCI 253-394) **don't exist** in ciTypeFlow's CFG.
- These blocks are **never created** as `YuhuTopLevelBlock` objects.
- `scan_for_traps()` never sees them.
- `parse_bytecode()` never emits IR for them.
- **The method's control flow is incomplete** — code after the trap is missing.

### Why This Works for C1/C2 (But Not Yuhu)

**C1/C2 behavior:**
- ciTypeFlow omits successors of trapped blocks **by design**.
- C1/C2 support deoptimization.
- At runtime, when the trap fires, the method deopts to the interpreter.
- The interpreter executes the missing blocks (BCI 253-394).
- C1/C2 never need to compile those blocks.

**Yuhu's current behavior (Activity 051 workaround):**
- Detect trapped blocks during `scan_for_traps()`.
- Bail out entirely: `env->record_failure("block has trap")`.
- Method falls back to C1/C2 or interpreter.
- **Safe, but limits Yuhu's applicability.**

**Yuhu's desired behavior (with deoptimization):**
- Compile the method INCLUDING blocks after traps.
- At runtime, if trap fires → deopt to interpreter.
- If trap doesn't fire → continue executing compiled code.
- **Requires ALL blocks to exist in Yuhu's CFG.**

### The Fix: Iterate All `ciBlock` Objects, Not Just ciTypeFlow's CFG

**Root cause**: Yuhu relies on `ciTypeFlow::Block` (which omits trapped successors) instead of `ciBlock` (which includes all basic blocks).

**Solution**: Change block creation to iterate all `ciBlock` objects from `ciMethodBlocks`.

#### Approach: Use `ciMethod::blocks()` Instead of `flow()->pre_order_at()`

**Current code** (yuhuFunction.cpp):
```cpp
// Build the blocks
for (int i = 0; i < block_count(); i++) {
  ciTypeFlow::Block *b = flow()->pre_order_at(i);
  _blocks[b->pre_order()] = new YuhuTopLevelBlock(this, b);
}
```

**Fixed code** (conceptual):
```cpp
// Get all ciBlock objects from ciMethodBlocks (includes ALL blocks)
ciMethodBlocks* method_blocks = target()->blocks();
int total_block_count = method_blocks->num_blocks();

for (int i = 0; i < total_block_count; i++) {
  ciBlock* cib = method_blocks->block(i);
  
  // Find the corresponding ciTypeFlow::Block (if it exists)
  ciTypeFlow::Block* flow_block = NULL;
  if (cib->pre_order() < flow()->block_count()) {
    flow_block = flow()->pre_order_at(cib->pre_order());
  }
  
  // Create YuhuTopLevelBlock with both ciBlock and optional ciTypeFlow::Block
  _blocks[cib->pre_order()] = new YuhuTopLevelBlock(this, cib, flow_block);
}
```

**Key changes:**

1. **Use `target()->blocks()`** instead of `flow()->pre_order_at()`.
   - `ciMethod::blocks()` returns ALL basic blocks from `ciMethodBlocks`.
   - This includes blocks that ciTypeFlow omitted due to traps.

2. **Map `ciBlock` to `ciTypeFlow::Block` (if available)**.
   - For blocks that ciTypeFlow DID create, use its trap metadata and type information.
   - For blocks that ciTypeFlow DIDN'T create (trapped successors), create them anyway with minimal metadata.

3. **Update `YuhuTopLevelBlock` constructor**.
   - Accept both `ciBlock*` (always present) and `ciTypeFlow::Block*` (optional).
   - If `ciTypeFlow::Block` is NULL, infer trap info from bytecode scanning only.

#### Handling Traps in Newly Created Blocks

**For blocks that ciTypeFlow created** (has `ciTypeFlow::Block`):
- Use `flow_block->has_trap()`, `flow_block->trap_bci()`, `flow_block->trap_index()`.
- This is the current behavior — no change needed.

**For blocks that ciTypeFlow did NOT create** (no `ciTypeFlow::Block`):
- `YuhuTopLevelBlock` must rely entirely on `scan_for_traps()` to detect traps.
- `scan_for_traps()` scans bytecodes and calls `set_trap()` if it finds unconditional traps.
- This already works — no change needed to the scanning logic.

**For blocks that are successors of trapped blocks**:
- They will be created (fix #1).
- `scan_for_traps()` will scan them.
- If they contain no traps, they get compiled normally.
- If they contain traps, those traps get recorded too.

#### Updating Successor/Predecessor Links

**Current code**: Uses `ciTypeFlow::Block::successors()` to determine control flow.

**Problem**: For trapped blocks, `successors()` returns empty array.

**Fix**: Compute successors from `ciBlock` directly:

```cpp
// In YuhuTopLevelBlock constructor or initialization
void YuhuTopLevelBlock::compute_successors() {
  ciBlock* cib = ciblock();
  
  // Get successor BCIs from ciBlock
  int fall_through_bci = cib->limit_bci();
  int branch_target = cib->control_bci();
  
  if (branch_target != ciBlock::fall_through_bci) {
    // Conditional or unconditional branch
    ciBlock* target_block = find_block_at(branch_target);
    if (target_block) add_successor(target_block);
    
    // Fall-through (if conditional branch)
    if (cib->has_fall_through()) {
      ciBlock* fall_through = find_block_at(fall_through_bci);
      if (fall_through) add_successor(fall_through);
    }
  } else {
    // Just falls through
    ciBlock* next_block = find_block_at(fall_through_bci);
    if (next_block) add_successor(next_block);
  }
  
  // Exception handlers
  for (int i = 0; i < cib->ex_handler_count(); i++) {
    ciBlock* handler = cib->ex_handler(i);
    if (handler) add_exception(handler);
  }
}
```

**This ensures**: All successor links are computed from bytecode structure, not from ciTypeFlow's filtered CFG.

### Impact on Existing Code

**What changes:**
1. `YuhuFunction::build()` — block creation loop.
2. `YuhuTopLevelBlock` constructor — accept optional `ciTypeFlow::Block`.
3. Successor/predecessor computation — use `ciBlock` instead of `ciTypeFlow::Block::successors()`.

**What stays the same:**
1. `scan_for_traps()` — already works correctly.
2. `parse_bytecode()` — already handles `has_trap()` correctly.
3. `do_trap()` — your new deopt bundle implementation works as-is.
4. Type flow analysis — ciTypeFlow still runs, still provides type info for blocks it creates.

### Testing Strategy

**Test case 1**: Method with single trap (e.g., `new` with unloaded class)
- Before fix: Method bails out.
- After fix: Method compiles, blocks after trap exist, deopt bundle emitted at trap BCI.

**Test case 2**: Method with trap in middle (e.g., `UTF_8$Encoder.encodeArrayLoop()`)
- Before fix: Blocks 274-394 missing, method bails out.
- After fix: All blocks 0-394 exist, trap at BCI 243 has deopt bundle, blocks 253-394 compile normally.

**Test case 3**: Runtime trap firing
- Compile method with trap.
- At runtime, trigger the trap (e.g., unload a class).
- Verify deoptimization occurs and interpreter resumes correctly.

---

## Design Principle: Separate Statepoint IDs for GC vs Deopt

**Critical decision**: Deopt statepoints must use **different Statepoint IDs** from GC safepoints to enable unambiguous runtime discrimination.

### The Problem

Both GC safepoints and deopt trap sites generate StackMap entries:

```llvm
; GC safepoint (for polling or call sites)
%token1 = call token @llvm.experimental.gc.statepoint(
  i64 0,  ; Statepoint ID = 0 (default)
  ...
)

; Deopt trap site
%token2 = call token @llvm.experimental.gc.statepoint(
  i64 0,  ; Statepoint ID = 0 (same!)
  ...
  [ "deopt"(...) ]
)
```

If both use `Statepoint ID = 0` (LLVM's default), the StackMap section contains entries that are **indistinguishable at runtime**:

```
__llvm_stackmaps:
  Entry 1: Statepoint ID 0, Offset 42, Live values [...]
  Entry 2: Statepoint ID 0, Offset 128, Live values [...]
  Entry 3: Statepoint ID 0, Offset 256, Live values [...]
```

At runtime, when `YuhuRuntime::uncommon_trap()` executes, it cannot tell:
- "Is offset 128 a GC safepoint or a deopt trap?"
- "Does this StackMap entry have a deopt bundle or not?"

### The Solution: Unique Statepoint IDs for Deopt

Assign **distinct Statepoint IDs** based on purpose:

```cpp
// In Yuhu, define Statepoint ID conventions:
enum YuhuStatepointID {
  GC_SAFEPOINT_ID = 0,           // Default for GC polling/calls
  DEOPT_TRAP_ID_BASE = 0x1000,   // Base for deopt traps
  DEOPT_TRAP_ID_UNLOADED = 0x1001,   // Reason_unloaded traps
  DEOPT_TRAP_ID_UNINITIALIZED = 0x1002, // Reason_uninitialized traps
  DEOPT_TRAP_ID_UNHANDLED = 0x1003,     // Reason_unhandled traps
  DEOPT_TRAP_ID_CUSTOM = 0x2000,        // Custom trap types
};
```

### Implementation: Assigning Deopt Statepoint IDs

In `YuhuTopLevelBlock::do_trap()`:

```cpp
void YuhuTopLevelBlock::do_trap(int trap_request) {
  // 1. Construct deopt bundle
  std::vector<llvm::Value*> deopt_operands = build_deopt_operands();
  
  // 2. Choose Statepoint ID based on trap reason
  uint64_t statepoint_id = choose_deopt_statepoint_id(trap_request);
  
  // 3. Attach Statepoint ID as attribute on the call
  llvm::FunctionType* func_type = YuhuBuilder::make_ftype("Ti", "i");
  
  std::vector<llvm::Value*> call_args;
  call_args.push_back(thread());
  call_args.push_back(LLVMValue::jint_constant(trap_request));
  
  llvm::OperandBundleDef deopt_bundle("deopt", deopt_operands);
  
  // Create call with deopt bundle
  llvm::CallInst* trap_call = builder()->CreateCall(
    func_type,
    builder()->uncommon_trap(),
    call_args,
    {deopt_bundle}
  );
  
  // 4. Attach Statepoint ID via attribute
  // LLVM's RS4GC reads this via parseStatepointDirectivesFromAttrs()
  trap_call->addFnAttr(
    "statepoint-id",
    llvm::toString(statepoint_id)
  );
  
  // After RS4GC runs, this becomes:
  // %token = call token @llvm.experimental.gc.statepoint(
  //   i64 0x1001,  ; ← Your custom Statepoint ID
  //   ...
  // )
  
  builder()->CreateBr(function()->unified_exit_block());
}

uint64_t YuhuTopLevelBlock::choose_deopt_statepoint_id(int trap_request) {
  // Decode trap request to get reason
  Deoptimization::DeoptReason reason = Deoptimization::trap_request_reason(trap_request);
  
  switch (reason) {
    case Deoptimization::Reason_unloaded:
      return 0x1001;
    case Deoptimization::Reason_uninitialized:
      return 0x1002;
    case Deoptimization::Reason_unhandled:
      return 0x1003;
    case Deoptimization::Reason_class_check:
      return 0x1004;
    // ... map all trap reasons to unique IDs
    default:
      return 0x2000;  // Custom/unknown
  }
}
```

### Runtime StackMap Discrimination

With unique Statepoint IDs, `YuhuRuntime::uncommon_trap()` can now distinguish:

```cpp
int YuhuRuntime::uncommon_trap(JavaThread* thread, int trap_request) {
  address trap_pc = thread->frame_anchor()->last_Java_pc();
  CodeBlob* cb = CodeCache::find_blob(trap_pc);
  nmethod* nm = cb->as_nmethod_or_null();
  
  // 1. Find StackMap entry at this PC
  StackMapRecord* sm = nm->stackmap_record_at(trap_pc);
  
  // 2. Check Statepoint ID to determine if this is a deopt trap
  if (sm->statepoint_id() < 0x1000) {
    // This is a GC safepoint, not a deopt trap
    // Something went wrong — we shouldn't be here
    fatal("uncommon_trap called at GC safepoint, not deopt trap");
  }
  
  // 3. Statepoint ID >= 0x1000 means this is a deopt trap
  // We can also decode the trap reason from the ID:
  Deoptimization::DeoptReason expected_reason = decode_statepoint_id(sm->statepoint_id());
  
  // 4. Verify consistency (debugging aid)
  Deoptimization::DeoptReason actual_reason = 
    Deoptimization::trap_request_reason(trap_request);
  assert(expected_reason == actual_reason, 
         "Statepoint ID doesn't match trap request");
  
  // 5. Now we know this StackMap entry has a deopt bundle
  // Extract deopt operand locations:
  intptr_t bundle_values[sm->num_deopt_operands()];
  for (int i = 0; i < sm->num_deopt_operands(); i++) {
    StackMapLocation& loc = sm->deopt_location(i);
    if (loc.is_register()) {
      bundle_values[i] = thread->get_register(loc.reg_number());
    } else {
      bundle_values[i] = *(intptr_t*)(thread->sp() + loc.stack_offset());
    }
  }
  
  // 6. Use ScopeDesc to interpret bundle and reconstruct interpreter frame
  // ...
}
```

### Why This Matters

1. **Unambiguous runtime behavior**: No confusion between GC safepoints and deopt traps.

2. **Debugging aid**: If `uncommon_trap()` is called at the wrong PC, we can detect it immediately via Statepoint ID check.

3. **StackMap parsing optimization**: We only need to parse deopt bundles for entries with ID >= 0x1000, skipping GC-only entries.

4. **Future extensibility**: Can encode trap reason in Statepoint ID for validation and optimized dispatch.

### Alternative: Check for Deopt Bundle Presence

An alternative approach is to check if the StackMap entry has a deopt bundle:

```cpp
// Check if StackMap entry has deopt operands
if (sm->has_deopt_bundle()) {
  // This is a deopt trap
} else {
  // This is a GC safepoint
}
```

**Why we prefer Statepoint ID approach**:
- More explicit and self-documenting.
- Doesn't require parsing deopt bundle structure.
- Enables early error detection (wrong PC → wrong Statepoint ID).
- Consistent with LLVM's design (Statepoint ID is meant for this purpose).

---

## Core Mechanism: Deopt Operand Bundle Construction

**This is the foundation of deoptimization.** Without correctly constructing the deopt operand bundle, we cannot reconstruct the interpreter frame.

### What the Deopt Operand Bundle Contains

The deopt operand bundle must contain **every LLVM SSA value that corresponds to JVM state** at the trap site:

```llvm
"deopt"(
  ; Local variables (mapped from JVM locals array)
  i64 %local_0_value,     ; Local variable slot 0
  i64 %local_1_value,     ; Local variable slot 1
  i64 %local_2_value,     ; Local variable slot 2
  ...
  
  ; Expression stack (mapped from JVM operand stack)
  i64 %stack_0_value,     ; Bottom of expression stack
  i64 %stack_1_value,     ; Next slot
  ...
  
  ; Monitor objects (if any synchronized blocks are held)
  i64 %monitor_0_object,  ; First held monitor
  i64 %monitor_1_object,  ; Second held monitor
  ...
  
  ; Rethrow exception (if in exception handler)
  i64 %rethrow_exception  ; Exception being rethrown (or null)
)
```

**The position of each value in the bundle is critical** — it creates the mapping that ScopeDesc uses to decode interpreter state:
- Position 0 = local variable 0
- Position 1 = local variable 1
- Position N = local variable N
- Position N+1 = expression stack slot 0
- Position N+2 = expression stack slot 1
- etc.

### How to Construct the Deopt Bundle in Yuhu

#### Step 1: Understand YuhuState Layout

At any trap site, `YuhuState` contains the complete JVM state:

```cpp
class YuhuState {
  // Local variables array (maps to JVM local variable slots)
  YuhuValue** _locals;        // _locals[0] = "this" or first arg
  int _locals_count;
  
  // Expression stack (maps to JVM operand stack)
  YuhuValue** _stack;         // _stack[0] = bottom of stack
  int _stack_size;
  
  // Monitors (for synchronized blocks)
  MonitorInfo** _monitors;
  int _monitor_count;
  
  // Rethrow exception (if in exception handler)
  YuhuValue* _rethrow_exception;
};
```

Each `YuhuValue` wraps an LLVM SSA value:
```cpp
class YuhuValue {
  llvm::Value* _llvm_value;  // The actual LLVM SSA value
  YuhuType* _type;           // JVM type (oop, jint, jlong, etc.)
};
```

#### Step 2: Extract LLVM Values in Correct Order

In `YuhuTopLevelBlock::do_trap()`:

```cpp
void YuhuTopLevelBlock::do_trap(int trap_request) {
  YuhuState* state = current_state();
  
  // Build deopt operand vector in EXACT order that ScopeDesc expects
  std::vector<llvm::Value*> deopt_operands;
  
  // 1. Local variables (in order 0..N)
  for (int i = 0; i < state->locals_count(); i++) {
    YuhuValue* local = state->local_at(i);
    
    // CRITICAL: The value must be materialized at this point
    // If it's currently in a register, LLVM will ensure it stays live
    // If it's been spilled to stack, LLVM will reload it
    llvm::Value* llvm_val = local->llvm_value();
    
    // Cast to i64 for uniform representation (LLVM statepoint requires pointers or integers)
    if (llvm_val->getType()->isPointerTy()) {
      llvm_val = builder()->CreatePtrToInt(llvm_val, builder()->getInt64Ty());
    } else if (llvm_val->getType()->getIntegerBitWidth() != 64) {
      llvm_val = builder()->CreateIntCast(llvm_val, builder()->getInt64Ty(), false);
    }
    
    deopt_operands.push_back(llvm_val);
  }
  
  // 2. Expression stack (in order bottom..top)
  for (int i = 0; i < state->stack_size(); i++) {
    YuhuValue* stack_val = state->stack_at(i);
    llvm::Value* llvm_val = stack_val->llvm_value();
    
    // Same casting to i64
    if (llvm_val->getType()->isPointerTy()) {
      llvm_val = builder()->CreatePtrToInt(llvm_val, builder()->getInt64Ty());
    } else if (llvm_val->getType()->getIntegerBitWidth() != 64) {
      llvm_val = builder()->CreateIntCast(llvm_val, builder()->getInt64Ty(), false);
    }
    
    deopt_operands.push_back(llvm_val);
  }
  
  // 3. Monitors (if any)
  for (int i = 0; i < state->monitor_count(); i++) {
    MonitorInfo* monitor = state->monitor_at(i);
    llvm::Value* monitor_obj = monitor->obj()->llvm_value();
    
    if (monitor_obj->getType()->isPointerTy()) {
      monitor_obj = builder()->CreatePtrToInt(monitor_obj, builder()->getInt64Ty());
    }
    
    deopt_operands.push_back(monitor_obj);
  }
  
  // 4. Rethrow exception (if applicable)
  if (state->rethrow_exception() != NULL) {
    llvm::Value* exc = state->rethrow_exception()->llvm_value();
    
    if (exc->getType()->isPointerTy()) {
      exc = builder()->CreatePtrToInt(exc, builder()->getInt64Ty());
    }
    
    deopt_operands.push_back(exc);
  }
  
  // 5. Choose deopt-specific Statepoint ID (see Design Principle section)
  uint64_t statepoint_id = choose_deopt_statepoint_id(trap_request);
  
  // 6. Generate the uncommon_trap call WITH the deopt bundle AND Statepoint ID
  decache_for_trap();
  
  llvm::FunctionType* func_type = YuhuBuilder::make_ftype("Ti", "i");
  
  // Regular arguments: thread + trap_request
  std::vector<llvm::Value*> call_args;
  call_args.push_back(thread());
  call_args.push_back(LLVMValue::jint_constant(trap_request));
  
  // Deopt bundle operand
  llvm::OperandBundleDef deopt_bundle("deopt", deopt_operands);
  
  // Create the call with bundle
  llvm::CallInst* trap_call = builder()->CreateCall(
    func_type,
    builder()->uncommon_trap(),
    call_args,
    {deopt_bundle}
  );
  
  // Attach Statepoint ID as attribute (RS4GC will read this)
  trap_call->addFnAttr(
    "statepoint-id",
    llvm::toString(statepoint_id)
  );
  
  // Jump to unified exit
  builder()->CreateBr(function()->unified_exit_block());
}
```

#### Step 3: Record the Mapping in ScopeDesc

While constructing the deopt bundle, simultaneously record the mapping in ScopeDesc:

```cpp
void YuhuTopLevelBlock::record_trap_scope(int trap_bci, int num_locals, int num_stack_slots) {
  DebugInformationRecorder* dir = compilation()->debug_info();
  
  // Create arrays to describe where each JVM slot maps to in the deopt bundle
  
  // Local variable mapping: "local i is at deopt bundle position i"
  objArrayOop local_offsets = oopFactory::new_objArray(
    SystemDictionary::Integer_klass(), num_locals, CHECK);
  for (int i = 0; i < num_locals; i++) {
    // Position i in bundle = local variable i
    local_offsets->obj_at_put(i, Integer::create(i));
  }
  
  // Expression stack mapping: "stack slot j is at deopt bundle position (num_locals + j)"
  objArrayOop stack_offsets = oopFactory::new_objArray(
    SystemDictionary::Integer_klass(), num_stack_slots, CHECK);
  for (int j = 0; j < num_stack_slots; j++) {
    // Position (num_locals + j) in bundle = stack slot j
    stack_offsets->obj_at_put(j, Integer::create(num_locals + j));
  }
  
  // Monitor mapping
  objArrayOop monitor_info = oopFactory::new_objArray(
    SystemDictionary::Integer_klass(), current_state()->monitor_count(), CHECK);
  for (int k = 0; k < current_state()->monitor_count(); k++) {
    // Position (num_locals + num_stack_slots + k) = monitor k
    monitor_info->obj_at_put(k, Integer::create(num_locals + num_stack_slots + k));
  }
  
  // Register this scope with the nmethod
  // The pc_offset will be filled in after code generation
  dir->describe_scope(
    0,              // pc_offset (placeholder, will be patched)
    target(),       // Method being compiled
    trap_bci,       // BCI where trap occurs
    local_offsets,  // Maps locals to bundle positions
    stack_offsets,  // Maps stack slots to bundle positions
    monitor_info,   // Monitor ownership
    NULL            // No rethrow exception (or exception bundle position)
  );
}
```

### How This Enables Interpreter Frame Reconstruction

At runtime, when `YuhuRuntime::uncommon_trap()` executes:

1. **LLVM has ensured all deopt operand values are live**:
   - RS4GC pass guaranteed these values are materialized (in registers or on stack).
   - StackMap section records their exact locations.

2. **Parse StackMap to get locations AND verify Statepoint ID**:
   ```cpp
   // At trap PC, look up StackMap entry
   StackMapRecord* sm = nm->stackmap_record_at(trap_pc);
   
   // CRITICAL: Verify this is a deopt trap, not a GC safepoint
   if (sm->statepoint_id() < 0x1000) {
     fatal("uncommon_trap called at GC safepoint, not deopt trap");
   }
   
   // Extract deopt operand locations (only for deopt Statepoint IDs)
   intptr_t bundle_values[sm->num_deopt_operands()];
   for (int i = 0; i < sm->num_deopt_operands(); i++) {
     StackMapLocation& loc = sm->deopt_location(i);
     if (loc.is_register()) {
       bundle_values[i] = thread->get_register(loc.reg_number());
     } else {
       bundle_values[i] = *(intptr_t*)(sp + loc.stack_offset());
     }
   }
   ```

3. **Use ScopeDesc to interpret bundle positions**:
   ```cpp
   // ScopeDesc says: "position 0 = local 0, position 1 = local 1, etc."
   for (int i = 0; i < num_locals; i++) {
     int bundle_pos = local_offsets->obj_at(i)->as_int();
     interpreter_frame->set_local(i, bundle_values[bundle_pos]);
   }
   
   for (int j = 0; j < num_stack_slots; j++) {
     int bundle_pos = stack_offsets->obj_at(j)->as_int();
     interpreter_frame->push_stack(bundle_values[bundle_pos]);
   }
   ```

4. **Reconstructed interpreter frame is ready**:
   - Local variables are populated.
   - Expression stack is populated.
   - Monitors are tracked.
   - BCI is set to trap BCI.
   - Interpreter resumes execution from trap BCI.

### Critical Constraints

1. **All values in deopt bundle must be SSA values that dominate the trap call**:
   - Cannot use values defined after the trap.
   - Cannot use values from other basic blocks (unless they dominate).

2. **All oop-typed values in deopt bundle must be tracked by RS4GC**:
   - RS4GC will relocate them if GC moves objects.
   - After relocation, StackMap locations point to relocated values.

3. **Bundle order must match ScopeDesc encoding exactly**:
   - Any mismatch will cause incorrect interpreter state reconstruction.
   - This is the most likely source of bugs.

4. **Values must be cast to uniform type (i64)**:
   - LLVM statepoint requires consistent types in deopt bundle.
   - Pointer types must be converted via `ptrtoint`.
   - Smaller integers must be sign/zero extended to i64.

---

## What Yuhu Must Implement

### Component 1: Trap Site Metadata Generation

**Problem**: LLVM's StackMap tells you **where** values are, but not **what they mean** in JVM terms.

**Solution**: Generate HotSpot-compatible `ScopeDesc` and `OopMap` metadata for each trap site.

**Detailed implementation**: See "Core Mechanism: Deopt Operand Bundle Construction" section above for the complete deopt bundle construction and ScopeDesc encoding.

#### 1.1 ScopeDesc Generation

The ScopeDesc generation is **tightly coupled with deopt bundle construction** — they must use the same ordering:

```cpp
void YuhuTopLevelBlock::do_trap(int trap_request) {
  // STEP 1: Choose deopt-specific Statepoint ID (see Design Principle section)
  uint64_t statepoint_id = choose_deopt_statepoint_id(trap_request);
  
  // STEP 2: Construct deopt bundle (see Core Mechanism section)
  std::vector<llvm::Value*> deopt_operands = build_deopt_operands();
  
  // STEP 3: Record ScopeDesc with matching encoding
  record_trap_scope(trap_bci(), deopt_operands.size());
  
  // STEP 4: Emit the actual call with deopt bundle AND Statepoint ID
  emit_trap_call_with_deopt(trap_request, deopt_operands, statepoint_id);
}
```

The key invariant: **ScopeDesc position N must refer to the same JVM slot as deopt bundle position N**.

```cpp
void YuhuTopLevelBlock::do_trap(int trap_request) {
#### 1.2 OopMap Generation

For each trap site, generate OopMap entries:

```cpp
void YuhuTopLevelBlock::generate_trap_oopmap(int pc_offset) {
  OopMap* map = new OopMap(frame_size_in_words(), 0);
  
  YuhuState* state = current_state();
  
  // Mark oop locations
  for (int i = 0; i < state->locals_count(); i++) {
    YuhuValue* local = state->local_at(i);
    if (local->type()->is_oop_type()) {
      // Find where this local will be (stack offset or register)
      int offset = get_location_offset(local);
      if (is_stack_location(local)) {
        map->set_oop(VMRegImpl::stack2reg(offset));
      } else {
        Register reg = get_register_for(local);
        map->set_oop(reg->as_VMReg());
      }
    }
  }
  
  // Register with nmethod
  compilation()->oop_map_set()->add_gc_map(pc_offset, map);
}
```

### Component 2: StackMap Parsing at Runtime

**Problem**: After code generation, LLVM's StackMap section contains live value locations, but Yuhu must parse this at runtime to reconstruct interpreter state.

**Solution**: Implement StackMap parser in `YuhuRuntime::uncommon_trap()`.

```cpp
int YuhuRuntime::uncommon_trap(JavaThread* thread, int trap_request) {
  Thread *THREAD = thread;
  JavaFrameAnchor* jfa = thread->frame_anchor();
  
  // 1. Get the return PC (where trap was triggered)
  address trap_pc = jfa->last_Java_pc();
  
  // 2. Find the nmethod containing this PC
  CodeBlob* cb = CodeCache::find_blob(trap_pc);
  nmethod* nm = cb->as_nmethod_or_null();
  assert(nm != NULL, "trap must be in nmethod");
  
  // 3. Find the ScopeDesc for this PC
  ScopeDesc* scope = nm->scope_desc_at(trap_pc);
  assert(scope != NULL, "must have scope for trap pc");
  
  // 4. Parse LLVM StackMap to get actual value locations
  //    (This requires parsing __llvm_stackmaps section)
  StackMapRecord* sm = nm->stackmap_record_at(trap_pc);
  
  // 5. Extract live values from their locations
  intptr_t* live_values = new intptr_t[sm->num_values()];
  for (int i = 0; i < sm->num_values(); i++) {
    StackMapLocation& loc = sm->location(i);
    if (loc.is_register()) {
      live_values[i] = thread->get_register(loc.reg_number());
    } else if (loc.is_stack()) {
      live_values[i] = *(intptr_t*)(thread->sp() + loc.stack_offset());
    }
  }
  
  // 6. Use ScopeDesc to map live values to JVM state
  GrowableArray<jvmdi::LocalVariable>* locals = scope->decode_locals(live_values);
  GrowableArray<oop>* expr_stack = scope->decode_stack(live_values);
  
  // 7. Call HotSpot's deoptimization infrastructure
  Deoptimization::UnrollBlock* unroll_block = Deoptimization::uncommon_trap(
    thread,
    nm,
    trap_pc,
    trap_request,
    locals,
    expr_stack,
    scope->decode_monitors()
  );
  
  // 8. The deopt stub will reconstruct interpreter frame and return to interpreter
  Deoptimization::uncommon_trap_unpack(thread, unroll_block);
  
  ShouldNotReachHere();
  return 0;
}
```

### Component 3: Frame Layout Compatibility

**Problem**: LLVM's register allocator may place live values in locations that HotSpot's deopt unpacker doesn't expect.

**Solutions**:

#### Option A: Reserve Registers (Simple)

Reserve specific registers via LLVM flags to ensure they're not used by allocator:

```bash
# In YUHULLVM compilation flags:
-reserve-register=x19  # Reserve for frame pointer
-reserve-register=x20  # Reserve for thread pointer
-reserve-register=x28  # Reserve for deopt scratch
```

Then force critical values to stack:

```cpp
// Instead of letting LLVM keep in register:
llvm::Value* local0 = ...;

// Force to stack slot:
llvm::AllocaInst* local0_slot = builder()->CreateAlloca(local0->getType(), nullptr, "local0_slot");
builder()->CreateStore(local0, local0_slot);
local0 = builder()->CreateLoad(local0->getType(), local0_slot, "local0_loaded");
```

#### Option B: Custom Register Allocation (Complex)

Implement a post-RS4GC pass that adjusts register assignments to match deopt unpacker expectations. This requires deep understanding of both LLVM's register allocator and HotSpot's frame layout.

**Recommendation**: Start with Option A (reserve registers + force to stack).

### Component 4: Deopt Stub Integration

**Problem**: After `YuhuRuntime::uncommon_trap()` returns, the deopt stub must reconstruct the interpreter frame.

**Solution**: Integrate with HotSpot's existing deoptimization infrastructure.

The flow:
1. `YuhuRuntime::uncommon_trap()` is called from compiled code.
2. It gathers live state and calls `Deoptimization::uncommon_trap()`.
3. This creates a `Deoptimization::UnrollBlock` describing the interpreter frame to create.
4. `Deoptimization::uncommon_trap_unpack()` patches the return address to jump to the deopt stub.
5. When `uncommon_trap()` returns, it jumps to the deopt stub instead of the caller.
6. The deopt stub:
   - Deallocates the compiled frame.
   - Allocates an interpreter frame.
   - Populates locals, expression stack, monitors.
   - Sets BCI to trap BCI.
   - Jumps to interpreter dispatch.

This is **already implemented in HotSpot** — Yuhu just needs to provide the correct input (live values + ScopeDesc).

---

## Implementation Phases

### Phase 1: Simple Trap (Reason_unloaded)

**Target**: Support deoptimization for the simplest trap type — unloaded class at `new` bytecode.

**Scope**:
- One trap site per method.
- Minimal live state (few locals, empty expression stack).
- No monitors held.

**Tasks**:
1. Modify `do_trap()` to emit deopt operand bundle with live values.
2. Generate minimal ScopeDesc for trap site.
3. Generate minimal OopMap for trap site.
4. Implement basic StackMap parser in `YuhuRuntime::uncommon_trap()`.
5. Test with method that has single unloaded class.

**Estimated effort**: 2-3 weeks.

### Phase 2: Multiple Trap Sites

**Target**: Support methods with multiple trap sites (e.g., multiple `new` bytecodes with different unloaded classes).

**Scope**:
- Multiple ScopeDesc entries per nmethod.
- Different live sets at different trap sites.
- Still no monitors.

**Tasks**:
1. Ensure ScopeDesc generation works for multiple trap sites.
2. Ensure OopMap generation works for multiple trap sites.
3. Test with methods that have 2-3 trap sites.

**Estimated effort**: 1-2 weeks.

### Phase 3: Complex Traps (Reason_uninitialized, Reason_unhandled)

**Target**: Support traps mid-method with deep expression stacks and many locals.

**Scope**:
- Methods like `UTF_8$Encoder.encodeArrayLoop()` with complex trap sites.
- Deep expression stacks (10+ slots).
- Many locals (20+ variables).

**Tasks**:
1. Ensure deopt bundle generation scales to 30+ live values.
2. Optimize register allocation to minimize stack spills.
3. Test with real-world methods that were previously bailed out.

**Estimated effort**: 2-3 weeks.

### Phase 4: Monitor Support

**Target**: Support deoptimization when monitors are held (synchronized blocks).

**Scope**:
- Track monitor ownership in YuhuState.
- Include monitor info in ScopeDesc.
- Ensure deopt unpacker can re-lock monitors.

**Tasks**:
1. Extend YuhuState to track held monitors.
2. Encode monitor info in ScopeDesc.
3. Test with synchronized methods that have trap sites.

**Estimated effort**: 2-3 weeks.

---

## Challenges and Risks

### Challenge 1: LLVM StackMap Format Stability

**Risk**: LLVM's StackMap format may change between versions.

**Mitigation**:
- Use LLVM's official `StackMapParser` API (if available in LLVM 20).
- Or implement version-specific parsers and update when LLVM upgrades.

### Challenge 2: Frame Layout Mismatch

**Risk**: LLVM's register allocator produces layouts that deopt unpacker can't handle.

**Mitigation**:
- Reserve critical registers (Option A above).
- Force values to stack slots with predictable offsets.
- Add assertions in debug builds to verify frame layout.

### Challenge 3: Performance Overhead

**Risk**: Deopt operand bundles and StackMap emission increase code size and compilation time.

**Mitigation**:
- Only emit deopt bundles at actual trap sites (not all safepoints).
- Profile and optimize if overhead is significant.

### Challenge 4: Testing Complexity

**Risk**: Deoptimization is hard to test — requires triggering traps at runtime.

**Mitigation**:
- Create test cases that force class unloading.
- Use `-XX:-BackgroundCompilation` to control timing.
- Add extensive logging in `YuhuRuntime::uncommon_trap()`.

---

## Success Criteria

1. **Methods with trap sites compile successfully**: No more bailouts for `has_trap()`.
2. **Deoptimization works correctly**: When trap is reached, method deopts to interpreter and continues execution.
3. **No correctness issues**: Deoptimized execution produces same results as interpreted execution.
4. **Performance acceptable**: Compilation time increase < 20%, code size increase < 10%.
5. **All trap types supported**: `Reason_unloaded`, `Reason_uninitialized`, `Reason_unhandled`.

---

## Open Questions

### Q1: Should we use `llvm.experimental.deoptimize` intrinsic?

**Analysis**: 
- Pros: LLVM knows it's a deopt point, may optimize accordingly.
- Cons: Doesn't add anything beyond deopt operand bundle on regular call.

**Recommendation**: Use regular call with deopt bundle to `YuhuRuntime::uncommon_trap()`. More control over calling convention and integration with HotSpot.

### Q2: How to handle derived pointers in deopt bundle?

**Analysis**: If a local variable holds a derived pointer (e.g., `obj.field`), RS4GC will track the base pointer separately.

**Solution**: Include both base and derived pointers in deopt bundle. RS4GC will relocate both correctly.

### Q3: Should we parse StackMap at runtime or compile time?

**Analysis**:
- Runtime parsing: Flexible, but adds runtime overhead.
- Compile-time extraction: Faster at runtime, but requires embedding metadata in nmethod.

**Recommendation**: Extract StackMap info at compile time (after LLVM code generation) and store in nmethod alongside ScopeDesc. Avoids runtime parsing overhead.

---

## References

- Activity 051: Yuhu UTF-8 Encoder IR bugs (trap block analysis)
- LLVM Statepoint Semantics: https://llvm.org/docs/Statepoints.html
- HotSpot Deoptimization: `hotspot/src/share/vm/runtime/deoptimization.cpp`
- ScopeDesc Generation: `hotspot/src/share/vm/code/debugInfoRecorder.cpp`
- Yuhu RS4GC Implementation: `hotspot/src/share/vm/yuhu/yuhuRewriteStatepointsForGC.cpp`
- LLVM StackMap Format: https://llvm.org/docs/StackMaps.html

---

## Next Steps

1. **Review this design** with Yuhu team.
2. **Prototype Phase 1**: Implement simple trap deoptimization.
3. **Test with minimal case**: Method with single unloaded class.
4. **Iterate**: Expand to more complex traps based on learnings.

# Activity 070: LLVM DCE Eliminates Call Site Blocks Causing Missing Return PC Offsets

## Issue Summary

**Symptom:** Yuhu compilation of `NineParameterTest::main` hits assertion failure:

```
assert(_stack_map_instruction_offsets->contains(return_pc_offset)) failed: Call site should contain stack map
```

**Crash location:** `yuhuDebugInformationRecorder.cpp:195`

**Error log:** `test_yuhu/hs_err_pid92073.log`

**Root cause:** LLVM's `InstCombinePass` and `SimplifyCFGPass` eliminate basic blocks containing call sites during the GC pass pipeline. When these blocks are removed, the call sites have no corresponding stack map entries, causing the assertion to fail when `convert_and_add_to_real_recorder()` tries to look up the `return_pc_offset`.

**IR files:**
- Before RS4GC: `test_yuhu/_tmp_yuhu_ir_com.example.NineParameterTest__main.ll`
- After RS4GC: `test_yuhu/_tmp_yuhu_ir_after_gc.ll`
- Machine code: `debug/sp_data_test.txt`

## Root Cause Analysis

### The pass order problem

In `yuhuIRTransformer.cpp:101-110`:

```cpp
FunctionPassManager FPM;
FPM.addPass(InstCombinePass());      // Runs FIRST
FPM.addPass(SimplifyCFGPass());      // Runs SECOND
// PlaceSafepointsPass is commented out
MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));

MPM.addPass(YuhuRewriteStatepointsForGC());  // Runs AFTER InstCombine + SimplifyCFG
```

**InstCombine and SimplifyCFG run BEFORE RS4GC.** This means LLVM's standard optimization passes analyze and transform the IR before Yuhu's RS4GC transformation has a chance to protect the call sites.

### How LLVM eliminates call site blocks in `main`

The Java code:
```java
int result2 = 0;
// for loop is commented out
if (result1 == 400 && result2 == 4000) {
    System.out.println("SUCCESS: All tests passed!");
}
```

Since `result2` is always `0`, the condition `result2 == 4000` is **always false**. LLVM's constant propagation proves this and eliminates the entire dead branch.

CFG before RS4GC:
```
not_zero316 → no_exception326 → bci_129 → bci_136 → not_zero605 → [asm offset 460 + println call]
```

CFG after RS4GC:
```
not_zero316 → bci_147
```

The entire chain `no_exception326 → bci_129 → bci_136 → not_zero605` is eliminated by InstCombine + SimplifyCFG running before RS4GC.

**The problem:** The call site for virtual offset 460 was registered in `YuhuDebugInformationRecorder` during IR generation:
```cpp
YuhuDebugInformationRecorder::get()->register_call_site(460, 1975715151599, helper_address, CallSiteType::java_call, bci);
```

But after LLVM eliminates the block containing this call site:
1. The inline asm placeholder is gone.
2. The call instruction is gone.
3. No stack map is generated for this call site (it doesn't exist in the machine code).
4. When `convert_and_add_to_real_recorder()` looks up `return_pc_offset` for virtual offset 460, it's not in `_stack_map_instruction_offsets`.
5. **Assertion fires at line 195:** `assert(_stack_map_instruction_offsets->contains(return_pc_offset))`



## Files Involved

| File | Role |
|------|------|
| `yuhuIRTransformer.cpp:101-110` | GC pass pipeline — InstCombine + SimplifyCFG run BEFORE RS4GC |
| `yuhuCompiler.cpp` | Generates call sites with inline asm placeholders and inttoptr calls |
| `yuhuTopLevelBlock.cpp:1462-1472` | Creates placeholders via `CreateCallSitePlaceholder` |
| `yuhuStack.cpp:430-471` | Generates `0xDEAD` placeholder asm |
| `yuhuORCPlugins.cpp` | CallSiteExtractorPlugin — only processes placeholders that survive to machine code |

## Resolution Options

### Option A: Skip eliminated call sites during debug info conversion (Recommended)

In `convert_and_add_to_real_recorder()`, check if the call site's `return_pc_offset` exists in the stack map before asserting:

```cpp
for (int i = 0; i < _call_site_entries->length(); i++) {
    CallSiteEntry* entry = _call_site_entries->at(i);
    uint64_t return_pc = entry->return_pc_offset;
    
    // Skip call sites that were eliminated by LLVM optimization
    if (!_stack_map_instruction_offsets->contains(return_pc)) {
        if (YuhuTraceOsrCompilation) {
            tty->print_cr("Yuhu: Skipping eliminated call site virtual_offset=%llu return_pc=%llu",
                          entry->virtual_offset, return_pc);
        }
        continue;
    }
    
    // ... proceed with OopMap registration
}
```

**Why this works:** Call sites in eliminated blocks legitimately have no stack map entries. Skipping them is correct — they don't exist in the machine code, so no OopMap is needed.

### Option B: Prevent LLVM from eliminating blocks with call sites

Add function attributes or inline asm clobbers to prevent DCE:

```cpp
// Add "memory" clobber to inline asm to prevent elimination
"~{x19},~{memory}"
```

**Why this is NOT recommended:** It prevents LLVM from optimizing dead code, which is correct behavior. The eliminated blocks are provably dead and should be removed.

### Option C: Remove call site registration for provably dead branches

Track which basic blocks are eliminated by LLVM and remove their call site registrations before `convert_and_add_to_real_recorder()`.

**Downside:** Requires tracking block elimination, which is complex and fragile.

## Verification Plan

1. Check which pass eliminates the blocks:
   ```bash
   opt -print-before-all -print-after-all -instcombine -simplifycfg <input.ll>
   ```
   Look for "Deleting dead block" messages.

2. Apply Option A or B and verify:
   - The inline asm placeholders appear in post-RS4GC IR.
   - The helper addresses appear in machine code (`debug/sp_data_test.txt`).
   - The `0xDEAD` markers are present at expected offsets.

3. Run `NineParameterTest` and verify:
   - No assertion failures in `YuhuDebugInformationRecorder`.
   - All expected call sites are present in machine code.
   - GC safepoint polling works correctly.

## Status

**Status:** Documented. Root cause identified. Not yet fixed.

**Priority:** High. Call site elimination breaks Yuhu's runtime metadata tracking — VM calls and Java calls that must execute are removed by LLVM.

**Key insight:** The original hypothesis (stale TLS recorder) was incorrect. The `release()` call at `yuhuCompiler.cpp:928` properly cleans up the recorder between compilations. The actual issue is that LLVM eliminates basic blocks containing call sites, leaving those call sites without corresponding stack map entries in `_stack_map_instruction_offsets`.

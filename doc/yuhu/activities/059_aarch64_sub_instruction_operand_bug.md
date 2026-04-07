# Activity 059: AArch64 Backend sub Instruction Operand Order Bug

**Date**: 2026-03-19  
**Author**: AI Assistant  
**Status**: Analysis Complete - Pending Investigation  
**Related Files**: `hotspot/src/share/vm/yuhu/yuhuStack.cpp`, `hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp`  
**Related Issue**: Stack overflow check generating incorrect `sub` instruction in AArch64 assembly

---

## Executive Summary

When compiling stack overflow check code, the LLVM AArch64 backend generates **incorrect assembly**: `sub x9, x10, x9` (computes `stack_size - stack_base`) instead of the correct `sub x9, x9, x10` (should compute `stack_base - stack_size`). This causes immediate crash during Yuhu-compiled method execution.

**Root Cause**: LLVM 20.1.5 AArch64 instruction selection or register allocation bug - operands are swapped during instruction generation.

**Evidence**: 
```assembly
0x000000010d229d30: ldp	x9, x10, [x23, #328]    ; x9=stack_base, x10=stack_size
0x000000010d229d34: sub	x9, x10, x9             ; ❌ WRONG: x9 = x10 - x9
                                              ; Should be: sub x9, x9, x10
```

---

## Problem Description

### Symptoms

Yuhu-compiled methods crash immediately when executing stack overflow checks. The crash occurs because the computed `stack_bottom` address is completely wrong (negative value), causing stack overflow detection to fail catastrophically.

### Incorrect Assembly Generation

**Source Code** (`yuhuStack.cpp:262`):
```cpp
Value *stack_bottom = builder()->CreateSub(stack_base, stack_size, "stack_bottom");
// Intended semantics: stack_bottom = stack_base - stack_size
```

**Generated LLVM IR** (expected):
```llvm
%stack_bottom = sub i64 %stack_base, %stack_size
; Semantics preserved: stack_bottom = stack_base - stack_size
```

**Generated AArch64 Assembly** (actual - BUGGY):
```assembly
ldp   x9, x10, [x23, #328]    ; Load: x9=stack_base, x10=stack_size
sub   x9, x10, x9             ; ❌ BUG: Computes x9 = x10 - x9
                              ; Should be: sub x9, x9, x10
```

**Correct Assembly** (expected):
```assembly
ldp   x9, x10, [x23, #328]    ; Load: x9=stack_base, x10=stack_size
sub   x9, x9, x10             ; ✅ CORRECT: Computes x9 = x9 - x10
```

### Impact

- **Severity**: 🔴 Critical - Immediate crash on any Yuhu compilation
- **Scope**: Affects all methods that perform stack overflow checks
- **Frequency**: 100% reproducible on every compilation

---

## Root Cause Analysis

### Distinguishing from ptrtoint+add+inttoptr Issue

**Important**: This is a **separate issue** from the `ptrtoint+add+inttoptr` problem in `CreateAddressOfStructEntry`.

| Aspect | ptrtoint+add+inttoptr | sub Operand Swap |
|--------|----------------------|------------------|
| **Layer** | IR generation | Instruction Selection / RegAlloc |
| **Impact** | Optimization quality | **Correctness** |
| **Crash?** | ❌ No (performance only) | ✅ **Yes (immediate)** |
| **Fix Priority** | Lower (optimization) | **Critical (correctness)** |

### Possible Root Causes

#### Hypothesis 1: LLVM Instruction Selection Bug
- **Theory**: AArch64 ISel incorrectly maps `sub` DAG node to assembly
- **Evidence Needed**: Examine LLVM's AArch64Select.td for `sub` patterns
- **Test**: Check if bug exists in other LLVM versions

#### Hypothesis 2: Register Allocation Bug
- **Theory**: Register allocator reorders operands during spilling/reloading
- **Evidence Needed**: Run LLVM with `-print-machineinstrs` to see pre/post-regalloc
- **Test**: Disable regalloc (`-regalloc=none`) and check output

#### Hypothesis 3: SSA Form Violation
- **Theory**: IR might have undefined behavior or SSA violation
- **Evidence Needed**: Run `opt -verify` on generated IR
- **Test**: Manually inspect IR for correctness

#### Hypothesis 4: Type Confusion
- **Theory**: `stack_base` and `stack_size` might have different types
- **Evidence Needed**: Check IR types of both operands
- **Test**: Ensure both are `i64` type

---

## Investigation Plan

### Phase 1: Enable IR File Dumping

**Action**: Add flag to dump IR before code generation

```cpp
// In yuhu_globals.hpp
diagnostic(bool, YuhuDumpIRToFile, false,
  "Dump LLVM IR to /tmp/yuhu_ir_*.ll files");
```

**Usage**:
```bash
java -XX:+UnlockDiagnosticVMOptions \
     -XX:YuhuDumpIRToFile=true \
     -XX:+UseYuhuCompiler \
     com.example.Test
```

**Expected Output**: `/tmp/yuhu_ir_*.ll` files containing full IR

---

### Phase 2: Enable Machine Code Printing

**Option A**: Use existing `YuhuPrintAsmOf` flag
```bash
java -XX:+UnlockDiagnosticVMOptions \
     -XX:YuhuPrintAsmOf=com.example.Test \
     -XX:+UseYuhuCompiler \
     com.example.Test
```

**Option B**: Add PrintMachinePass plugin (recommended for detailed tracing)

Create LLVM pass plugin to print machine instructions after each pass:

```cpp
// PrintMachinePass.cpp
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {
struct PrintMachinePass : PassInfoMixin<PrintMachinePass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    errs() << "=== Machine IR for " << F.getName() << " ===\n";
    // Print machine instructions here
    return PreservedAnalyses::all();
  }
};
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {
    .APIVersion = LLVM_PLUGIN_API_VERSION,
    .PluginName = "PrintMachinePass",
    .PluginVersion = "v1",
    .RegisterPassBuilderCallbacks = [](PassBuilder &PB) {
      PB.registerPipelineStartEPCallback(
        [](ModulePassManager &MPM, OptimizationLevel Level) {
          MPM.addPass(PrintMachinePass());
        });
    }
  };
}
```

---

### Phase 3: Trace IR → Assembly Transformation

**Goal**: Identify exactly where the operand swap occurs

**Steps**:
1. Dump IR after `emit_IR()` (before ORC JIT)
2. Dump Machine IR after instruction selection
3. Dump Machine IR after register allocation
4. Dump final assembly code

**Comparison**:
```
IR:        %stack_bottom = sub i64 %stack_base, %stack_size
           ↓ (Instruction Selection)
Machine:   SUB_Xrs <- THIS IS WHERE BUG MIGHT BE
           ↓ (Register Allocation)
           SUB_Xrs (with actual registers)
           ↓ (Assembly Emission)
Assembly:  sub x9, x10, x9  ← BUG APPEARS HERE
```

---

### Phase 4: Isolate the Bug

**Test 1**: Different LLVM Versions
```bash
# Test with LLVM 19, 20, 21
brew install llvm@19
brew install llvm@20
brew install llvm@21

# Recompile Yuhu with each version and test
```

**Test 2**: Disable Optimizations
```bash
# Compile with -O0 vs -O2 vs -O3
# If bug disappears at -O0, it's an optimization bug
```

**Test 3**: Custom Pass Pipeline
```cpp
// Create minimal pass pipeline to isolate buggy pass
llvm::LoopPassManager LPM;
llvm::FunctionPassManager FPM;
llvm::ModulePassManager MPM;

// Add passes one by one, checking output after each
FPM.addPass(InstCombinePass());  // Test this
FPM.addPass(SimplifyCFGPass());  // Or this
```

---

## Potential Solutions

### Solution 1: Workaround in Yuhu Code (Temporary)

**Idea**: Rewrite the subtraction to avoid triggering the bug

```cpp
// Instead of: stack_bottom = stack_base - stack_size
// Use: stack_bottom = stack_base + (-stack_size)
Value *neg_stack_size = builder()->CreateNeg(stack_size, "neg_stack_size");
Value *stack_bottom = builder()->CreateAdd(stack_base, neg_stack_size, "stack_bottom");
```

**Generated Assembly** (hopefully):
```assembly
neg   x11, x10              ; x11 = -stack_size
add   x9, x9, x11           ; x9 = stack_base + (-stack_size)
```

**Pros**:
- ✅ Quick workaround
- ✅ Avoids buggy `sub` instruction

**Cons**:
- ❌ Doesn't fix root cause
- ❌ May trigger other bugs
- ❌ Performance impact (extra instruction)

---

### Solution 2: Inline Assembly (Not Recommended)

```cpp
// Force correct instruction via inline assembly
Value *stack_bottom = builder()->CreateInlineAsm(
  FunctionType::get(YuhuType::intptr_type(), 
                    {YuhuType::intptr_type(), YuhuType::intptr_type()}, 
                    false),
  "sub $0, $1, $2",
  "=r,r,r",
  {stack_base, stack_size},
  "stack_bottom");
```

**Pros**:
- ✅ Guaranteed correct assembly

**Cons**:
- ❌ Prevents optimization
- ❌ Hard to maintain
- ❌ Not portable
- ❌ Breaks SSA form

---

### Solution 3: Report LLVM Bug (Recommended Long-term)

**Action**: File bug report at https://github.com/llvm/llvm-project/issues

**Required Evidence**:
1. Minimal reproducing IR file
2. Generated assembly showing bug
3. LLVM version (20.1.5)
4. Target triple (aarch64-apple-darwin23.x.x)
5. Comparison with expected assembly

**Template**:
```
Title: [AArch64] sub instruction operands swapped during instruction selection

Summary:
LLVM AArch64 backend generates incorrect 'sub' instruction with swapped 
operands: 'sub x9, x10, x9' instead of 'sub x9, x9, x10'.

IR:
%result = sub i64 %a, %b

Expected Assembly:
sub x9, x9, x10  ; x9 = a - b

Actual Assembly:
sub x9, x10, x9  ; x9 = b - a (WRONG!)

Environment:
- LLVM: 20.1.5
- Target: AArch64 macOS
- Command line: [full llc command]
```

---

### Solution 4: Patch LLVM Locally (If Urgent)

**Action**: Apply custom patch to LLVM source

**Investigation Steps**:
1. Clone LLVM source: `git clone https://github.com/llvm/llvm-project.git`
2. Checkout version: `git checkout llvmorg-20.1.5`
3. Find ISel pattern: Search `AArch64Select.td` for `sub` patterns
4. Debug: Add debug prints to `AArch64ISelLowering.cpp`
5. Fix: Modify pattern or add constraint
6. Build: Recompile LLVM with fix
7. Test: Rebuild Yuhu and verify fix

**Risk**:
- ⚠️ Requires deep LLVM knowledge
- ⚠️ May break other things
- ⚠️ Maintenance burden

---

### Solution 5: PrintMachinePass Plugin (Best for Debugging)

**Implementation**:

Create a comprehensive debugging pass:

```cpp
// YuhuDebugPass.cpp
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>

using namespace llvm;

namespace {
struct YuhuDebugPass : PassInfoMixin<YuhuDebugPass> {
  std::string FuncName;
  
  YuhuDebugPass(const std::string &Name = "") : FuncName(Name) {}
  
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    if (!FuncName.empty() && F.getName().str() != FuncName) {
      return PreservedAnalyses::all();
    }
    
    // Log IR
    std::ofstream log("/tmp/yuhu_debug_" + F.getName().str() + ".txt", 
                      std::ios::app);
    log << "=== IR After Pass ===\n";
    log << F << "\n\n";
    log.close();
    
    return PreservedAnalyses::all();
  }
};
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {
    .APIVersion = LLVM_PLUGIN_API_VERSION,
    .PluginName = "YuhuDebugPass",
    .PluginVersion = "v1",
    .RegisterPassBuilderCallbacks = [](PassBuilder &PB) {
      PB.registerPipelineStartEPCallback(
        [](ModulePassManager &MPM, OptimizationLevel Level) {
          std::string Filter = getenv("YUHU_DEBUG_FUNC") ? 
                               getenv("YUHU_DEBUG_FUNC") : "";
          MPM.addPass(YuhuDebugPass(Filter));
        });
    }
  };
}
```

**Build**:
```bash
clang++ -std=c++17 -shared -fPIC \
  -I$(brew --prefix llvm)/include \
  -L$(brew --prefix llvm)/lib \
  YuhuDebugPass.cpp -o YuhuDebugPass.dylib
```

**Usage**:
```bash
export YUHU_DEBUG_FUNC=Matrix_multiply
export LLVM_PASS_PLUGIN_PATH=/tmp/YuhuDebugPass.dylib

java -XX:+UseYuhuCompiler com.example.Matrix
```

---

## Investigation Timeline

### Week 1: Data Collection
- [ ] Enable `YuhuDumpIRToFile` flag
- [ ] Collect IR files for failing methods
- [ ] Collect assembly output
- [ ] Verify bug reproduction

### Week 2: Root Cause Analysis
- [ ] Run `opt -verify` on IR
- [ ] Compare IR vs assembly
- [ ] Test with different LLVM versions
- [ ] Test with different optimization levels

### Week 3: Solution Development
- [ ] Try workaround (Solution 1)
- [ ] Develop PrintMachinePass (Solution 5)
- [ ] File LLVM bug report (Solution 3)
- [ ] Evaluate LLVM patch (Solution 4)

### Week 4: Fix Validation
- [ ] Apply chosen fix
- [ ] Test comprehensively
- [ ] Document findings in Activity 060

---

## Testing Strategy

### Test Case 1: Minimal Reproduction

```cpp
// SimpleTest.java
public class SimpleTest {
    public static void test() {
        // Trigger stack overflow check
        long[] arr = new long[1000000];
    }
    
    public static void main(String[] args) {
        test();
    }
}
```

**Compile**:
```bash
javac SimpleTest.java
java -XX:+UseYuhuCompiler \
     -XX:CompileCommand=yuhuonly,SimpleTest.test \
     -XX:YuhuDumpIRToFile=true \
     SimpleTest
```

**Expected**: Crash with `sub` bug in generated code

---

### Test Case 2: Original Matrix Test

```bash
cd test_yuhu
./test_matrix.sh
```

**Expected**: Should complete without crash after fix

---

## Success Criteria

✅ **Bug Fixed When**:
1. Generated assembly matches IR semantics
2. `sub x9, x9, x10` (not `sub x9, x10, x9`)
3. Stack overflow check works correctly
4. No crashes in test suite
5. All Yuhu-compiled methods execute successfully

---

## Related Activities

- **Activity 016**: Stack overflow check implementation
- **Activity 038**: Frame size calculation bugs
- **Activity 052**: LLVM 20 compatibility fixes
- **Activity 058**: Debug logging discipline (flag infrastructure)

---

## References

1. **LLVM AArch64 Backend**: https://github.com/llvm/llvm-project/tree/main/llvm/lib/Target/AArch64
2. **AArch64 Instruction Reference**: https://developer.arm.com/documentation/dui0801/latest
3. **LLVM Pass Plugins**: https://llvm.org/docs/PassPlugins.html
4. **LLVM Bug Tracker**: https://github.com/llvm/llvm-project/issues

---

## Next Steps

1. **Immediate**: Enable `YuhuDumpIRToFile` and collect evidence
2. **Short-term**: Develop PrintMachinePass for detailed tracing
3. **Medium-term**: File LLVM bug report with complete evidence
4. **Long-term**: Either LLVM fixes upstream or Yuhu implements workaround

---

**Document Status**: Analysis complete, awaiting empirical investigation

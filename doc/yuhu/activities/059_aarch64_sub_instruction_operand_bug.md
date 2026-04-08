# Activity 059: AArch64 Backend sub Instruction Operand Order - NOT A BUG (LLVM Optimization)

**Date**: 2026-03-19  
**Author**: AI Assistant  
**Status**: **CLOSED - Not a Bug** (LLVM algebraic optimization)  
**Related Files**: 
- `hotspot/src/share/vm/yuhu/yuhuStack.cpp`, `hotspot/src/share/vm/yuhu/yuhuTopLevelBlock.cpp` (Original code)
- `hotspot/src/share/vm/yuhu/yuhuTracingIRCompiler.cpp` (Debug infrastructure)
- `hotspot/src/share/vm/yuhu/yuhuORCPlugins.cpp` (Debug infrastructure)
- LLVM Bug Report: https://github.com/llvm/llvm-project/issues/190788 (CLOSED)  
**Related Issue**: Initially reported as incorrect `sub` instruction, later identified as LLVM algebraic optimization

---

## Executive Summary

**This is NOT a bug.** What appeared to be an incorrect `sub` instruction operand order is actually **LLVM's algebraic optimization** transforming `a - (b - c)` into `a + c - b`.

**Original IR**:
```llvm
%stack_bottom = sub i64 %stack_base, %stack_size
%free_stack = sub i64 %current_sp, %stack_bottom
; Equivalent to: free_stack = current_sp - (stack_base - stack_size)
```

**Generated Assembly** (optimized):
```assembly
ldp x9, x10, [x23, #328]    ; x9=stack_base, x10=stack_size
sub x9, x10, x9             ; x9 = stack_size - stack_base (computes -stack_bottom)
add x9, x8, x9              ; x9 = current_sp + (-stack_bottom) = current_sp - stack_bottom
```

**Mathematical Equivalence**:
- Expected: `current_sp - (stack_base - stack_size)`
- LLVM generates: `current_sp + (stack_size - stack_base)`
- These are **mathematically identical**: `a - (b - c) = a + c - b`

**Conclusion**: LLVM's instruction selector and DAG combiner correctly optimized the expression. No bug exists.

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

## Root Cause Analysis (CLOSED - Not a Bug)

### Actual Behavior: LLVM Algebraic Optimization

**The "bug" was a misunderstanding of LLVM's optimization behavior.**

#### Original Source Code

```cpp
// yuhuStack.cpp
Value *stack_bottom = builder()->CreateSub(stack_base, stack_size, "stack_bottom");
Value *free_stack = builder()->CreateSub(current_sp, stack_bottom, "free_stack");
```

#### LLVM IR Generated

```llvm
%stack_bottom = sub i64 %stack_base, %stack_size
%free_stack = sub i64 %current_sp, %stack_bottom
```

This is mathematically: `free_stack = current_sp - (stack_base - stack_size)`

#### Expected Assembly (Naive Translation)

```assembly
ldp x9, x10, [x23, #328]    ; x9=stack_base, x10=stack_size
sub x9, x9, x10             ; x9 = stack_base - stack_size
sub x9, x8, x9              ; x9 = current_sp - stack_bottom
```

#### Actual Assembly (LLVM Optimized)

```assembly
ldp x9, x10, [x23, #328]    ; x9=stack_base, x10=stack_size
sub x9, x10, x9             ; x9 = stack_size - stack_base (= -(stack_base - stack_size))
add x9, x8, x9              ; x9 = current_sp + (stack_size - stack_base)
```

#### Why This is Correct

**Mathematical Identity**:
```
a - (b - c) = a + (c - b) = a + c - b
```

**Applied to our case**:
```
current_sp - (stack_base - stack_size)
= current_sp + (stack_size - stack_base)
= current_sp + stack_size - stack_base
```

**LLVM's DAG Combiner** recognized this optimization opportunity:
1. Instead of computing `stack_bottom = stack_base - stack_size` then `current_sp - stack_bottom`
2. It computes `-stack_bottom = stack_size - stack_base` (one `sub`)
3. Then adds: `current_sp + (-stack_bottom)` (one `add`)

**Benefits**:
- Same number of instructions (2 arithmetic ops)
- `add` may be faster than `sub` on some microarchitectures
- Better register allocation opportunities

### LLVM Bug Report

A bug was initially filed with LLVM: https://github.com/llvm/llvm-project/issues/190788

**Status**: **CLOSED** - Not a bug, expected optimization behavior.

---

## Key Takeaways

### 1. Don't Judge Individual Instructions in Isolation

Looking at `sub x9, x10, x9` alone appears wrong, but in context with the following `add`, the complete computation is correct.

**Lesson**: Always examine the **complete basic block** when analyzing compiler output, not individual instructions.

### 2. LLVM's DAG Combiner is Aggressive

LLVM's SelectionDAG performs extensive algebraic simplifications:
- `a - (b - c) → a + c - b`
- `a - (b + c) → a - b - c`
- `(a * b) + (a * c) → a * (b + c)`
- And many more...

These optimizations are **correct** but can make the assembly look "wrong" at first glance.

### 3. Debug Infrastructure is Still Valuable

Even though this specific issue was not a bug, the tracing infrastructure added (`YuhuTraceIRCompilation`, `YuhuTraceMachineCode`) remains valuable for:
- Understanding LLVM's optimization decisions
- Debugging real code generation issues
- Performance analysis
- Educational purposes

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

**Option A**: Use dedicated tracing flags (implemented)
```bash
java -XX:+UnlockDiagnosticVMOptions \
     -XX:+YuhuTraceIRCompilation \
     -XX:+YuhuTraceMachineCode \
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

## Debug Infrastructure Added (2026-03-19)

### Overview

Added comprehensive tracing infrastructure to observe the complete compilation pipeline from LLVM IR to final machine code.

### Components Added

#### 1. TracingIRCompiler (`yuhuTracingIRCompiler.cpp`)
- **Purpose**: Trace IRCompileLayer - IR to object file compilation
- **Location**: `hotspot/src/share/vm/yuhu/yuhuTracingIRCompiler.cpp`
- **Features**:
  - Prints complete LLVM IR before compilation
  - Identifies and prints all `sub` instructions in IR with operand details
  - Disassembles generated object file to verify machine code
  - Controlled by `YuhuTraceIRCompilation` switch

#### 2. MachineCodePrinterPlugin (`yuhuORCPlugins.cpp`)
- **Purpose**: Trace ObjectLinkingLayer - final machine code after linking
- **Location**: `hotspot/src/share/vm/yuhu/yuhuORCPlugins.cpp`
- **Features**:
  - Prints raw machine code bytes
  - Disassembles AArch64 instructions (sub, add, mov, ldr, str, bl, ret)
  - Integrated via JITLink PassConfiguration (PostFixupPasses)
  - Controlled by `YuhuTraceMachineCode` switch

#### 3. Standalone Test Program (`test_yuhu_orc_jit.cpp`)
- **Purpose**: Isolate and reproduce the bug outside Yuhu compiler
- **Location**: `test_yuhu/test_yuhu_orc_jit.cpp`
- **Features**:
  - Replicates exact Yuhu ORC JIT configuration
  - Includes TracingIRCompiler and MachineCodePrinterPlugin
  - Can be built and run independently (no Yuhu compiler rebuild needed)

### Usage

Enable tracing with dedicated flags:
```bash
# Trace IR to object file compilation (print IR + disassemble object file)
-XX:+YuhuTraceIRCompilation

# Trace final machine code from LinkGraph (AArch64 disassembly)
-XX:+YuhuTraceMachineCode

# Enable both
-XX:+YuhuTraceIRCompilation -XX:+YuhuTraceMachineCode
```

### Expected Output

When enabled, the tracing will show:

1. **IR Level** (TracingIRCompiler):
   ```
   === TracingIRCompiler: Before Compilation ===
   Module: com.example.Test.method
   %stack_bottom = sub i64 %stack_base, %stack_size
   Found sub in IR:
     Operand 0: %stack_base
     Operand 1: %stack_size
   ```

2. **Object File Level** (TracingIRCompiler):
   ```
   === Object File Disassembly ===
   Section: .text
   Instructions (32-bit):
     0x000001cb: sub x0, x0, x1  ← Check if operands are correct
   ```

3. **LinkGraph Level** (MachineCodePrinterPlugin):
   ```
   === Machine Code from LinkGraph ===
   Symbol: method
   Instructions (32-bit):
     0x000001cb: sub x0, x0, x1  ← Final machine code
   ```

### Debugging Strategy

By comparing these three levels, we can identify:
- **If IR is wrong**: Bug is in Yuhu's IR generation
- **If IR is correct but object file is wrong**: Bug is in LLVM instruction selection
- **If object file is correct but LinkGraph is wrong**: Bug is in LLVM code emission or linking

### Integration Points

Both tracing components are integrated into Yuhu's ORC JIT initialization:

```cpp
// yuhuCompiler.cpp
auto JIT = llvm::orc::LLJITBuilder()
  .setJITTargetMachineBuilder(JTMB)
  .setObjectLinkingLayerCreator(CreateObjectLinkingLayer)  // Adds MachineCodePrinterPlugin
  .setCompileFunctionCreator(...)  // Adds TracingIRCompiler
  .create();
```

### Status

- ✅ Tracing infrastructure implemented
- ✅ Dedicated flags added (no pattern matching issues)
- ✅ `YuhuTraceIRCompilation` - Traces IR → object file
- ✅ `YuhuTraceMachineCode` - Traces LinkGraph machine code
- ✅ Ready for testing
- ⏳ Pending: Run with `-XX:+YuhuTraceIRCompilation -XX:+YuhuTraceMachineCode` to collect evidence

---

**Document Status**: Analysis complete, awaiting empirical investigation

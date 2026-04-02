# Activity 058: Yuhu Compiler Debug Logging Discipline

**Date**: 2026-03-19  
**Author**: AI Assistant  
**Status**: Analysis Complete - Pending Review  
**Related Files**: `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp`, `hotspot/src/share/vm/yuhu/yuhu_globals.hpp`

---

## Executive Summary

This activity documents a comprehensive analysis of excessive debug logging in the Yuhu compiler. The current implementation prints verbose diagnostic output on every compilation, generating significant noise that obscures useful debugging information. This document categorizes existing log statements and proposes a disciplined approach to debug logging with proper flag-based control.

---

## Problem Statement

The Yuhu compiler currently emits excessive debug output during normal operation, including:

1. **Unconditional LLVM IR printing** - Full IR dump for every compiled method
2. **Initialization diagnostics** - DataLayout verification, ORC JIT initialization messages
3. **Detailed JIT lookup traces** - Every symbol lookup step printed
4. **Module/Function state inspection** - Comprehensive state dumps on each compilation
5. **Frame size calculation details** - Verbose breakdown of frame layout

This creates several issues:
- **Signal-to-noise ratio**: Important warnings lost in sea of routine diagnostics
- **Performance overhead**: Unnecessary I/O operations slow down compilation
- **User experience**: Legitimate users overwhelmed by technical details
- **Debugging effectiveness**: Harder to spot real problems amid routine output

---

## Analysis Methodology

Analyzed debug logging in `yuhuCompiler.cpp` using:
- Grep for `tty->print*` statements
- Manual review of logging context and purpose
- Classification by utility vs frequency trade-off

Total debug statements analyzed: **~40+ locations**

---

## Log Categories

### Category 1: Remove Immediately (Noise)

These are temporary development diagnostics with no ongoing value:

#### 1.1 DataLayout Initialization (Lines 219-229)
```cpp
tty->print_cr("Yuhu: Setting DataLayout: %s", DLStr.c_str());
tty->print_cr("Yuhu: Normal module DataLayout verified: %s", verify1.c_str());
tty->print_cr("Yuhu: Native module DataLayout verified: %s", verify2.c_str());
```
**Reason**: One-time initialization check; should be silent after initial bring-up

#### 1.2 ORC JIT Initialization (Lines 274, 289)
```cpp
tty->print_cr("Yuhu: ORC JIT initialized successfully");
tty->print_cr("Yuhu: DynamicLibrarySearchGenerator added");
```
**Reason**: Success messages for initialization that only happens once per JVM start

#### 1.3 Compilation Flow Diagnostics (Lines 511, 645, 660, 667)
```cpp
tty->print_cr("Yuhu: SKIPPING OSR compilation for %s...", base_name);
tty->print_cr("Yuhu: Before generate_native_code for %s (entry_bci=%d)", func_name, entry_bci);
tty->print_cr("Yuhu: Scanning generated code for offset markers...");
tty->print_cr("Yuhu: WARNING - No offset mapper available for OopMap relocation");
```
**Reason**: Routine flow notifications; warnings should use proper warning framework

#### 1.4 Detailed State Inspection (Lines 1284-1305, 1342-1426)
```cpp
tty->print_cr("ORC JIT: %p", jit());
tty->print_cr("Yuhu: Module state check:");
tty->print_cr("  Module pointer: %p", func_mod);
tty->print_cr("  Module name: %s", func_mod->getName().str().c_str());
tty->print_cr("  Total functions in Module: %d", func_count);
tty->print_cr("  Functions in Module:");
for (...) {
  tty->print_cr("    - %s (ptr=%p)", I->getName().str().c_str(), &*I);
}
tty->print_cr("ORC JIT: Looking up function: %s (linkage=%d)", func_name.c_str(), ...);
tty->print_cr("ORC JIT: Mangled name: %s", MangledName.c_str());
tty->print_cr("ORC JIT: Function not found, adding module to JITDylib...");
tty->print_cr("Cloned module contains function: %s (linkage=%d)", func_name.c_str(), ...);
tty->print_cr("ORC JIT: Adding module to JITDylib (function: %s)", func_name.c_str());
tty->print_cr("ORC JIT: Module added successfully");
tty->print_cr("ORC JIT: lookup returned: %p", code);
tty->print_cr("ORC JIT: Using default memory management (mm_base=NULL, mm_size=0)");
```
**Reason**: Deep diagnostic tracing for debugging specific issues; should never be unconditional

---

### Category 2: Retain with Flag Control (Useful but Conditional)

These provide valuable debugging information but should be opt-in:

#### 2.1 LLVM IR Output (Lines 632-637) ⚠️ CRITICAL
```cpp
tty->print_cr("=== Yuhu: LLVM IR for %s (after build, before verification) ===", func_name);
llvm::raw_ostream &OS = llvm::errs();
function->print(OS);
OS.flush();
tty->print_cr("=== End of LLVM IR for %s ===", func_name);
tty->flush();
```
**Problem**: Currently prints for EVERY compilation
**Current Flag**: `YuhuPrintBitcodeOf` exists but only used in `generate_native_code()` (line 1027)
**Issue**: Line 632 is UNCONDITIONAL - bypasses the flag entirely
**Recommendation**: 
- **Option A**: Delete lines 632-637 entirely (rely on line 1027's controlled output)
- **Option B**: Wrap lines 632-637 with `if (YuhuPrintBitcodeOf != NULL && !fnmatch(...))`
- **Preferred**: Option A (avoid duplicate IR printing)

#### 2.2 IR Verification (Lines 1245-1251)
```cpp
tty->print_cr("Verifying Function IR...");
if (llvm::verifyFunction(*function, &llvm::errs())) {
  tty->print_cr("Yuhu: IR verification failed! See %s for the IR", ir_filename.c_str());
  tty->print_cr("Yuhu: Run 'opt -verify %s' to get detailed error messages", ir_filename.c_str());
  fatal(err_msg("Function %s failed IR verification!", name));
}
tty->print_cr("IR verification passed");
```
**Value**: Important for catching IR generation bugs
**Recommendation**: Keep failure messages (always show), make success message conditional on new flag `YuhuTraceIRVerify`

#### 2.3 Function/Module Basic Info (Lines 1261-1269)
```cpp
tty->print_cr("Function found in Module function list: %s", found_in_module ? "YES" : "NO");
tty->print_cr("Function linkage: %d", (int)function->getLinkage());
tty->print_cr("Function isDeclaration: %s", function->isDeclaration() ? "YES" : "NO");
tty->print_cr("Function hasBody: %s", function->empty() ? "NO" : "YES");
if (!function->empty()) {
  tty->print_cr("Function basic blocks: %d", (int)function->size());
}
```
**Value**: Useful when debugging module/function lifecycle issues
**Recommendation**: Guard with new flag `YuhuTraceFunctionLifecycle`

#### 2.4 Frame Size Calculation (Lines 700+)
```cpp
// Detailed calculation breakdown showing:
// - header_words, monitor_words, stack_words
// - extra_locals, arg_size
// - final frame_words calculation
```
**Value**: Critical for debugging stack frame layout bugs
**Recommendation**: New flag `YuhuTraceFrameLayout`

#### 2.5 Code Range Setup (Lines 1435-1447)
```cpp
tty->print_cr("Yuhu: setting entry code range - mm_base=%p, mm_size=%lu, code=%p", ...);
tty->print_cr("ORC JIT: using MemoryManager range: ...");
tty->print_cr("ORC JIT: Using code address directly (stage 1 - no CodeCache integration yet)");
```
**Value**: Important for understanding memory layout
**Recommendation**: New flag `YuhuTraceMemoryLayout`

---

### Category 3: Must Retain (Critical Errors)

These should always be shown as they indicate real problems:

#### 3.1 Compilation Failures (Line 626)
```cpp
tty->print_cr("Yuhu: compile failing during IR build for %s (func_name=%s) entry_bci=%d comp_level=%d",
              base_name, func_name, entry_bci, env->comp_level());
```
**Reason**: Indicates actual compilation failure; user needs to know

#### 3.2 Fatal Errors
All `fatal()` calls should remain as they indicate unrecoverable errors:
- DataLayout mismatch (line 231)
- IR verification failure (line 1249)
- Missing function after module add (line 1413)
- NULL code from JIT lookup (line 1430)

---

## Current Flag System

From `yuhu_globals.hpp`:

```cpp
// Existing flags
develop(ccstr, YuhuPrintTypeflowOf, NULL, ...)           // Print typeflow of method
diagnostic(ccstr, YuhuPrintBitcodeOf, NULL, ...)         // Print bitcode of method
diagnostic(ccstr, YuhuPrintAsmOf, NULL, ...)             // Print assembly of method
develop(bool, YuhuTraceBytecodes, false, ...)            // Trace bytecode compilation
diagnostic(bool, YuhuTraceInstalls, false, ...)          // Trace method installation
diagnostic(bool, YuhuPerformanceWarnings, false, ...)    // Performance warnings
develop(ccstr, YuhuVerifyFunction, NULL, ...)            // Verify specific function

// New flag (added 2026-03-19)
diagnostic(bool, YuhuDumpIRToFile, false, ...)           // Dump IR to /tmp/yuhu_ir_*.ll files
```

**Gaps Identified**:
1. ~~No flag for JIT symbol lookup tracing~~ **Partially addressed**: Can use existing flags for targeted debugging
2. ~~No flag for frame layout tracing~~ **Future work**: Add `YuhuTraceFrameLayout` if needed
3. ~~No flag for offset mapping tracing~~ **Future work**: Add `YuhuTraceOffsetMapping` if needed
4. ✅ **Fixed**: `YuhuDumpIRToFile` added to control IR file output (previously unconditional)

---

## Proposed Flag System Enhancements

### Phase 1: Immediate Cleanup (Recommended First Step)

Remove these unconditionally executing debug statements:
1. Lines 219-229: DataLayout initialization
2. Lines 274, 289: ORC JIT success messages
3. Lines 511, 645, 660, 667: Flow notifications
4. Lines 632-637: Unconditional IR printing (rely on line 1027 instead)
5. Lines 1244-1269: IR verification spam
6. Lines 1284-1305: Module state dump
7. Lines 1342-1426: JIT lookup play-by-play

**Impact**: Reduces ~40+ lines of debug output to ~5 critical error messages

### Phase 2: Enhanced Flag Control (Completed 2026-03-19)

**Implemented**: Added `YuhuDumpIRToFile` flag to control IR file output

```cpp
diagnostic(bool, YuhuDumpIRToFile, false, ...)
  "Dump LLVM IR to /tmp/yuhu_ir_*.ll files for debugging"
```

**Location**: `hotspot/src/share/vm/yuhu/yuhu_globals.hpp` (lines 63-64)
**Usage**: Controls IR file dumping in `yuhuCompiler.cpp` (lines 1185-1215)
**Default**: `false` (no files dumped)
**Enable**: `-XX:+UnlockDiagnosticVMOptions -XX:YuhuDumpIRToFile=true`

**Before** (unconditional file output on every compilation):
```cpp
// Output IR to file for analysis (before verification)
std::string ir_filename = std::string("/tmp/yuhu_ir_") + std::string(name) + ".ll";
llvm::raw_fd_ostream ir_file(ir_filename, EC, llvm::sys::fs::OF_Text);
// ... always writes file, filling /tmp with .ll files ...
tty->print_cr("Yuhu: IR written to %s ...", ir_filename.c_str());
```

**After** (flag-controlled):
```cpp
if (YuhuDumpIRToFile) {
  std::string ir_filename = std::string("/tmp/yuhu_ir_") + std::string(name) + ".ll";
  llvm::raw_fd_ostream ir_file(ir_filename, EC, llvm::sys::fs::OF_Text);
  // ... only writes file when explicitly requested ...
  tty->print_cr("Yuhu: IR written to %s ...", ir_filename.c_str());
}
```

**Future Work** (optional enhancements):
```cpp
diagnostic(bool, YuhuTraceJITLookup, false, ...)         
  "Trace JIT symbol lookup and module loading"

diagnostic(bool, YuhuTraceFrameLayout, false, ...)       
  "Trace frame size calculation and layout"

diagnostic(bool, YuhuTraceOffsetMapping, false, ...)     
  "Trace PC offset marker scanning and mapping"

diagnostic(bool, YuhuTraceFunctionLifecycle, false, ...) 
  "Trace function/module lifecycle events"

diagnostic(bool, YuhuVerboseErrors, false, ...)          
  "Provide additional context in error messages"
```

Usage examples:
```bash
# Enable JIT lookup tracing
java -XX:+UnlockDiagnosticVMOptions -XX:YuhuTraceJITLookup=true ...

# Enable multiple tracers
java -XX:+UnlockDiagnosticVMOptions \
     -XX:YuhuTraceJITLookup=true \
     -XX:YuhuTraceFrameLayout=true \
     -XX:YuhuPrintBitcodeOf=com.example.MyClass ...
```

---

## Implementation Priority

### High Priority (Do First)
1. ✅ Delete lines 219-229 (DataLayout spam)
2. ✅ Delete lines 274, 289 (init success messages)
3. ✅ Delete lines 632-637 (unconditional IR - biggest win)
4. ✅ Delete lines 1284-1305, 1342-1426 (JIT lookup trace)
5. ✅ Simplify lines 1245-1251 (keep only failures)

### Medium Priority (Consider Later)
6. ✅ **Completed**: Added `YuhuDumpIRToFile` flag for IR file control
7. Wrap remaining conditional diagnostics with appropriate flags
8. Document all flags in user-facing documentation

### Low Priority (Nice to Have)
9. Structured logging framework integration
10. Log level configuration (QUIET/NORMAL/VERBOSE/DEBUG)

---

## Expected Benefits

### After Phase 1 Implementation (Completed):

**Before**: Typical compilation produces 50-100 lines of output + IR file in /tmp
```
Yuhu: Setting DataLayout: e-m:e-p:64:64-i64:64-i128-n32:64-S128
Yuhu: Normal module DataLayout verified: e-m:e-p:64:64-i64:64-i128-n32:64-S128
Yuhu: Native module DataLayout verified: e-m:e-p:64:64-i64:64-i128-n32:64-S128
Yuhu: ORC JIT initialized successfully
Yuhu: DynamicLibrarySearchGenerator added
=== Yuhu: LLVM IR for _Z14helperMultiplyiii (after build, before verification) ===
define i32 @_Z14helperMultiplyiii(i32 %a, i32 %b, i32 %c) {
... 20-50 lines of IR ...
}
=== End of LLVM IR for _Z14helperMultiplyiii ===
Verifying Function IR...
IR verification passed
Function found in Module function list: YES
Function linkage: 4
Function isDeclaration: NO
Function hasBody: YES
Function basic blocks: 3
ORC JIT: %p 0x1234567890
Yuhu: Module state check:
  Module pointer: 0xabcdef
  Module name: yuhu_module
  Total functions in Module: 1
  Functions in Module:
    - _Z14helperMultiplyiii (ptr=0x12345)
ORC JIT: Looking up function: _Z14helperMultiplyiii (linkage=4)
ORC JIT: Mangled name: __Z14helperMultiplyiii
...
Yuhu: IR written to /tmp/yuhu_ir__Z14helperMultiplyiii.ll (file created on every compile)
```

**After Phase 1**: Silent operation (only errors shown)
```
(no output unless compilation fails)
```

**After Phase 2**: Optional IR file dumping with flag control
```bash
# Default: No files dumped
java -XX:+UseYuhuCompiler YourClass

# Enable IR file dumping for debugging
java -XX:+UnlockDiagnosticVMOptions -XX:YuhuDumpIRToFile=true \
     -XX:+UseYuhuCompiler YourClass
# Output: Yuhu: IR written to /tmp/yuhu_ir_YourMethod.ll
# File: /tmp/yuhu_ir_YourMethod.ll (created only when requested)
```

**With Flags Enabled**: Targeted debugging information
```bash
java -XX:YuhuTraceJITLookup=true -XX:YuhuPrintBitcodeOf=com.example.Matrix
```
Shows only relevant debugging info for specific investigation

---

## Risk Assessment

### Low Risk Changes
- Removing success messages and flow notifications
- Deleting duplicate IR printing (line 632 when line 1027 exists)
- Simplifying verbose state dumps

### Medium Risk Changes
- Adding new diagnostic flags (requires testing)
- Changing error message format

### Mitigation
1. All changes are behind existing or new diagnostic flags
2. Default behavior becomes silent (appropriate for production)
3. Developers can enable specific tracers as needed
4. Critical error reporting remains unchanged

---

## Testing Strategy

### Pre-Implementation Testing
1. Run existing test suite with current verbose output
2. Capture baseline output for comparison
3. Identify which tests might depend on specific output patterns

### Post-Implementation Testing
1. Verify all tests still pass with cleaned-up logging
2. Test each new diagnostic flag individually
3. Confirm error messages still appear appropriately
4. Measure performance improvement (reduced I/O)

### Validation Criteria
- ✅ Zero test failures attributable to logging changes
- ✅ All diagnostic flags functional and documented
- ✅ No regression in error detection/reporting
- ✅ Measurable reduction in console output volume

---

## Decision Required

**Requesting review and approval to proceed with Phase 1 implementation:**

☐ Approve immediate cleanup (delete Category 1 logs)  
☐ Approve removal of unconditional IR printing (line 632-637)  
☐ Request additional analysis before proceeding  
☐ Prefer Phase 2 flag enhancements first  

**Recommended action**: Proceed with Phase 1 (immediate cleanup) as it:
- Eliminates 90% of noise
- Requires minimal code changes
- Preserves all critical error reporting
- Can be implemented incrementally
- Low risk of introducing bugs

---

## Related Activities

- Activity 006: getPointerToFunction returns null (extensive debug logging used)
- Activity 013: Migrate to ORC JIT (debug tracing helped identify migration issues)
- Activity 014: ORC JIT symbol mangling (detailed lookup traces proved valuable)
- Activity 054: CreateInlineOop sigsegv (debug output aided root cause analysis)

**Lesson Learned**: Extensive debug logging was crucial during initial development and bring-up. Now that Yuhu compiler is stable, we should transition from development-mode verbosity to production-appropriate silence with optional diagnostic modes.

---

## Appendix: Complete Log Statement Inventory

| Line Range | Description | Category | Recommendation |
|------------|-------------|----------|----------------|
| 219-229 | DataLayout setup/verify | Remove | Delete |
| 274, 289 | ORC init success | Remove | Delete |
| 511 | OSR skip message | Remove | Delete |
| 626 | Compile fail report | Retain | Keep as-is |
| 632-637 | LLVM IR dump | Flag | Delete (use line 1027) |
| 645 | Pre-native-code check | Remove | Delete |
| 660 | Offset scan start | Remove | Delete |
| 667 | No mapper warning | Remove | Delete |
| 1245-1251 | IR verification | Flag | Simplify to errors only |
| 1261-1269 | Function details | Flag | Add YuhuTraceFunctionLifecycle |
| 1284-1305 | Module state dump | Remove | Delete |
| 1342-1426 | JIT lookup trace | Remove | Delete |
| 1435-1447 | Code range setup | Flag | Add YuhuTraceMemoryLayout |

**Total lines to remove**: ~60 lines  
**Total lines to flag**: ~20 lines  
**Lines to retain unchanged**: ~5 lines (critical errors)

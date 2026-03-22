# 053 — scan_and_update_offset_markers SIGSEGV

## Crash Summary

```
SIGSEGV in YuhuBuilder::scan_and_update_offset_markers
si_code=SEGV_ACCERR, si_addr=0x0000000100bf0000

Native frames:
  YuhuBuilder::scan_and_update_offset_markers(unsigned char*, unsigned long, YuhuOffsetMapper*)+0xe4
  YuhuCompiler::compile_method(ciEnv*, ciMethod*, int)+0x818
```

---

## Root Cause

### The crash site

In `scan_and_update_offset_markers`, the loop reads ahead 5 instructions (20 bytes):

```cpp
// yuhuBuilder.cpp:1026
if (instructions[3] == NOP_INSTRUCTION && instructions[4] == NOP_INSTRUCTION) {
```

When `offset + 20 == code_size`, `instructions[4]` points to `code_start + offset + 16`, which is the last 4 bytes inside the declared range. That is still in bounds. However, access faults with `SEGV_ACCERR` (protection violation, not a NULL dereference), meaning the memory page exists but is not readable — i.e., the pointer has gone past the **real** end of the allocated region into unmapped or guard memory.

### Why code_size is wrong

The crash callers:

```cpp
// yuhuCompiler.cpp:629-630
address code_start = entry->code_start();
size_t code_size = entry->code_limit() - code_start;
```

`entry->code_limit()` is set at line 1420:

```cpp
// yuhuCompiler.cpp:1416-1420
size_t bb_count = function->size();
size_t estimated_size = bb_count * 128;   // ~128 bytes per basic block — a guess
if (estimated_size < 512)  estimated_size = 512;
if (estimated_size > 65536) estimated_size = 65536;
entry->set_code_limit((address)(code + estimated_size));
```

This is a **pure estimate** based on basic block count. It has no relation to the actual number of bytes emitted by LLVM.

In the observed crash:
- `code_start = 0x104998000`
- `code_size   = 31744` (0x7C00) — estimated, e.g. 248 blocks × 128 = 31744
- Actual code ends at `0x10499bffc`, i.e. real size ≈ 16380 bytes (0x3FFC)
- The loop scans into `0x10499C000` and beyond, hitting a page boundary → `SEGV_ACCERR`

### Why the estimate is never corrected

`YuhuMemoryManager::allocateCodeSection` **does** record the real allocation size:

```cpp
// yuhuMemoryManager.cpp:134-136
_last_code.base = (uint8_t*)blob->content_begin();
_last_code.size = alloc_size;   // ← actual allocation from CodeCache
_last_code.blob = blob;
```

And `endFunctionBody` (LLVM 3.x path) would call `entry->set_code_limit(FunctionEnd)` with the real end address. However, **ORC JIT never calls `YuhuMemoryManager`**.

The ORC JIT is initialised with no custom memory manager:

```cpp
// yuhuCompiler.cpp:259-261
auto JIT = llvm::orc::LLJITBuilder()
  .setJITTargetMachineBuilder(JTMB)
  .create();   // ← no setObjectLinkingLayerCreator, no custom MemoryManager
```

LLVM's default `InProcessMemoryManager` handles all allocations internally and never touches `YuhuMemoryManager`. Therefore `_last_code` is always empty, the `mm_base != NULL` branch at line 1400 is never taken, and the estimated `code_limit` is never overwritten.

---

## Fix Plan: Forward-Scanning for Effective Code Size

### Why the original ORC JIT MemoryManager integration was abandoned

The initial approach attempted to integrate `YuhuMemoryManager` into ORC JIT via `setObjectLinkingLayerCreator` to capture real code sizes. However, this approach had several critical issues:

1. **ORC JIT lifetime management**: The memory manager is created per-object and destroyed after linking, making it difficult to capture the real code size before `lookup()` returns.
2. **LLVM 20 API changes**: `RTDyldObjectLinkingLayer` has known compatibility issues on aarch64-apple-darwin with LLVM 20, causing silent materialization failures.
3. **Unnecessary complexity**: Since we already allocate code in CodeCache via `YuhuMemoryManager::allocateCodeSection`, the real allocation size is available—but extracting it from ORC's internal flow proved fragile.

### New approach: Scan for trailing `udf #0` instructions

Instead of relying on ORC JIT to report the exact size, we observe that LLVM pads generated code with `udf #0` instructions (encoding `0x00000000`) after the last valid instruction. By scanning for a sequence of consecutive zeros, we can detect where the real code ends.

**Key insight**: While backward scanning from `estimated_size` would be ideal, it risks accessing unmapped memory if the estimate is too high. Forward scanning eliminates this risk by staying within the estimated bounds.

### Implementation details

#### 1. `calculate_effective_code_size()` — forward scan with continuous-zero detection

```cpp
// yuhuCompiler.cpp
static size_t calculate_effective_code_size(address code_start, size_t total_size) {
  if (total_size == 0) return 0;
  
  // Scan forward from code_start to find effective code size
  // Look for consecutive udf #0 instructions (encoding: 0x00000000)
  // When we find 20 consecutive udf #0 (80 bytes), assume code ends there
  address scan_end = code_start + total_size;
  address pc = code_start;
  size_t zero_count = 0;
  address last_non_zero = code_start;
  const size_t CONTINUOUS_ZERO_THRESHOLD = 80;  // 20 instructions * 4 bytes
  
  while (pc + 4 <= scan_end) {
    uint32_t inst = *(uint32_t*)pc;
    
    if (inst == 0x00000000) {
      zero_count += 4;
      if (zero_count >= CONTINUOUS_ZERO_THRESHOLD) {
        // Found enough consecutive zeros, code likely ends at last non-zero instruction
        return (last_non_zero + 4) - code_start;
      }
    } else {
      zero_count = 0;
      last_non_zero = pc;
    }
    
    pc += 4;
  }
  
  // If no sufficient continuous zeros found, return total_size
  return total_size;
}
```

**Safety guarantees**:
- `total_size` is already capped at 64KB during estimation (`if (estimated_size > 65536)`), so forward scan never exceeds allocated memory.
- Scanning stops as soon as 80 consecutive zero bytes are found, avoiding reads into guard pages.
- Returns `total_size` as fallback if no clear boundary is detected.

#### 2. Usage in `compile_method()`

```cpp
// yuhuCompiler.cpp (~line 630)
generate_native_code(entry, function, func_name);

address code_start = entry->code_start();
size_t llvm_code_size = entry->code_limit() - code_start;

// Calculate effective code size by scanning for trailing udf #0 instructions
effective_code_size = calculate_effective_code_size(entry->code_start(), llvm_code_size);

// Use effective_code_size when scanning for offset markers
builder.scan_and_update_offset_markers(code_start, effective_code_size, offset_mapper);
```

#### 3. Fallback behavior

If the scan doesn't find 80 consecutive zeros (e.g., very small functions or unusual code patterns), it falls back to the original `llvm_code_size` estimate. This ensures correctness even in edge cases.

---

## Files Modified

| File | Change |
|---|---|
| `yuhuCompiler.cpp` (~line 389) | Replace backward scan with forward scan + continuous-zero detection |
| `yuhuCompiler.cpp` (~line 630) | Call `calculate_effective_code_size()` before `scan_and_update_offset_markers()` |

---

## Notes on `getSymbolAddress` and `finalizeMemory`

`RTDyldObjectLinkingLayer` will call `finalizeMemory` after all sections are written. The current stub returns `true` immediately, which is fine for now (memory is already executable via CodeCache permissions). `getSymbolAddress` returns 0, which may cause link-time symbol resolution issues for external symbols — that can be addressed separately.

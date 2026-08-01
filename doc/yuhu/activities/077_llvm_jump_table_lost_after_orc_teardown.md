# Activity 077: LLVM Jump Table Lost After ORC JIT Teardown — SIGBUS on `br xN`, and CodeBuffer Consts Section Sizing

## Date
2026-06-27

## Status
**In progress** — root cause confirmed by disassembly + lldb + LinkGraph inspection. Copy-and-patch path implemented; CodeBuffer sizing fix identified but **not yet runtime-verified**.

## Problem Description

Yuhu-compiled methods containing a `switch` crash with SIGBUS at an unmapped PC.
Reproducer: `jdk.internal.org.objectweb.asm.Frame::execute` (2252 bytecodes, three `switch`
statements, the largest with 200 cases).

```
siginfo: SIGBUS, si_addr: 0x000000012e85e729
pc  = 0x000000012e85e729     # not in CodeCache, not in any mapped region
```

Two false leads were eliminated first:

1. **Not a corrupted saved-LR slot.** The stack contents at `sp` were verified correct;
   the frame was intact. The thread branched to garbage, it did not *return* to garbage.
2. **Not a `_sigtramp` bug.** `_sigtramp` in `libsystem_kernel.dylib` is macOS's signal
   delivery trampoline, present in every crash stack — it is the reporting path, not the fault.

The faulting instruction sequence was a jump table dispatch:

```asm
0x...ec4:  adrp  x11, <page>          ; table base page
0x...ec8:  add   x11, x11, #0x0       ; table base
0x...ecc:  adr   x12, <label>         ; PC-relative anchor
0x...ed0:  ldrsw x13, [x11, x10, lsl #2]  ; load int32 offset from table
0x...ed4:  add   x12, x12, x13        ; target = anchor + offset
0x...ed8:  br    x12                  ; <-- branches to garbage
```

## Root Cause Analysis

### 1. LLVM places the jump table in a separate section, which Yuhu never copies

The IR (`_tmp_yuhu_ir_jdk.internal.org.objectweb.asm.Frame__execute.ll`) contains:

```llvm
switch i32 %2, label %bci_2236 [
    i32 0, label %bci_816
    i32 1, label %bci_819
    i32 2, label %bci_828
    ...            ; 200 cases
]
```

LLVM lowers this to an array of 200 `int32` PC-relative offsets, emitted into
`__TEXT,__const` (the macOS Mach-O equivalent of `.rodata`) — **not** into `__TEXT,__text`.

Yuhu copies only the text section into the CodeCache
(`yuhuCompiler.cpp:1247`, `memcpy(combined_base + adapter_size, entry->code_start(), effective_code_size)`),
then tears down the ORC JIT allocation with `RT->remove()` (`yuhuCompiler.cpp:1413`),
which frees **every** section including `__TEXT,__const`.

The failure chain:

1. Code is copied to CodeCache; `adrp x11` still encodes a page delta relative to its
   *ORC JIT* position, pointing at the now-freed const page.
2. `RT->remove()` frees that page.
3. `ldrsw x13, [x11, x10, lsl #2]` reads whatever now occupies that address.
4. `add x12, x12, x13` produces a garbage target; `br x12` jumps into unmapped memory → SIGBUS.

Note the crash is *address-dependent*, not deterministic: if the freed page happens to be
still mapped with plausible bytes, execution continues to a wrong bci instead of crashing.

### 2. Why the existing relocation scanner never noticed

`scan_and_generate_all_relocations` had a single ADRP matcher which required
**ADRP followed by LDR with immediate offset**:

```cpp
// yuhuVirtualAddressPatcher.hpp — formerly is_adrp_pattern
if ((instr[0] & 0x9F000000) == 0x90000000 &&      // adrp
    (instr[1] & 0xFFC00000) == 0xF9400000)        // ldr Xt, [Xn, #imm]
```

That is the GOT-style pattern: load a 64-bit *pointer* from a fixed address.
A jump table uses **ADRP followed by ADD** (`0x91000000`) to compute an *array base*,
which is then indexed by a register (`ldrsw x13, [x11, x10, lsl #2]`). The two encodings
are structurally disjoint, so the GOT matcher can never match a jump table, and the
scanner silently skipped these sites.

### 3. How C1 and C2 avoid the problem entirely

| Tier | Switch implementation | Why no relocation issue |
|---|---|---|
| C1 | Binary search over value ranges — `LIRGenerator::do_SwitchRanges` (`c1_LIRGenerator.cpp:2378-2404`) emits only `cmp` + `branch`; no table at all | No data reference to relocate |
| C2 | Real jump table, but built **directly in the CodeBuffer consts section** — `Compile::ConstantTable::fill_jump_table` (`compile.cpp:3757-3778`), and each entry gets `relocInfo::internal_word_type` | Table is inside the same blob as the code, and relocations survive the copy into the nmethod |
| Yuhu | Table produced by LLVM in ORC JIT memory, outside the CodeCache blob | Must be copied *and* relocated by hand |

C2's approach is the model: the table belongs to the same `CodeBuffer` as the code, and the
ADRP/table references are recorded as relocations.

## Fix

### Fix 1: Expose const symbols from the LinkGraph

`CallSiteExtractorPlugin::extractCallSites` (`yuhuORCPlugins.cpp:186-201`) now walks
sections whose name ends with `__const` and records every named symbol:

```cpp
YuhuDebugInformationRecorder::get()->register_const_symbol(
    Sym->getAddress().getValue(),
    Sym->getRange().Start.getValue(),
    Sym->getRange().End.getValue());
```

Verified output for `Frame::execute`:

```
section: __TEXT,__const   name: ltmp1
address: 4369858560  start: 4369858560  end: 4369859360     # 800 bytes = 200 x 4
```

`ltmp*` is LLVM's temporary label for anonymous data — expected, not a bug.
`ConstSymbolEntry {addr, start, end}` is stored in the recorder
(`yuhuDebugInformationRecorder.hpp:78-83`).

Table contents were cross-checked in lldb against the IR:

```
(lldb) memory read --size 4 --count 200 --format x 0x102ed0000
0x102ed0000: 0x00000010 0x00000180 0x00000248 0x00000248
0x102ed0010: 0x00000248 0x00000248 0x00000248 0x00000248
0x102ed0020: 0x00000248 0x00000f9c 0x00000f9c ...
```

Entries 2–8 share `0x248` (cases 2–8 → `bci_828`), entries 9–10 share `0xf9c` — matching the IR.
The values are offsets **relative to the `adr` anchor inside the code**, so the table content
is position-independent with respect to the table's own address and can be copied verbatim.

### Fix 2: New pattern matcher for jump tables

`is_adrp_pattern` renamed to `is_adrp_got_pattern` (`yuhuVirtualAddressPatcher.hpp:241`),
and `is_adrp_jump_table_pattern` added for ADRP+ADD (`:258`).
`is_unknown_adrp_pattern` (`:275`) keeps the `ShouldNotReachHere()` guard in
`yuhuORCPlugins.cpp:416-417`, so any third ADRP shape fails loudly instead of silently.

### Fix 3: Copy the table into the CodeBuffer consts section and relocate the ADRP

In `scan_and_generate_all_relocations`, the ADRP+ADD branch resolves
`target_address = adrp_target_page + add_imm12`, looks up the `ConstSymbolEntry`,
copies its bytes into `cb->consts()`, patches the ADRP+ADD to the new address, and — critically —
registers an `internal_word_type` relocation:

```cpp
RelocEntry reloc_entry{};
reloc_entry.offset     = i * 4 + adapter_size;
reloc_entry.reloc_type = relocInfo::relocType::internal_word_type;
reloc_entry.target     = (uint64_t)new_table_addr;
```

emitted as `cb->relocate((address)instr, internal_word_Relocation::spec((address)target))`.

**Why the relocation is mandatory, not cosmetic.** The code moves *twice*: ORC JIT → `CodeBuffer`,
then `CodeBuffer` → nmethod. Tracing `CodeBuffer::initialize_section_size`
(`codeBuffer.cpp:152-168`) shows the consts section is carved out of the **tail of insts**,
so the temporary buffer layout is `insts | consts | stubs`, whereas the final blob layout is
`consts | insts | stubs` (`codeBuffer.hpp:305-311`, "the order reflects the final layout").
The consts→insts distance therefore *changes sign* across `copy_code_to`. A PC-relative
ADRP+ADD patched against the buffer layout is wrong in the nmethod.

`internal_word_Relocation::pack_data_to` (`relocInfo.cpp:635-673`) auto-upgrades the record to
`section_word_type` when the target lies in another section, encoding it as
(section index, offset-in-section). `fix_relocation_after_move` (`:876-886`) then recomputes the
target from the destination blob and calls `Relocation::pd_set_data_value`
(`relocInfo_aarch64.cpp:35-59`) → `MacroAssembler::pd_patch_instruction_size`, whose
"PC-rel. addressing" branch (`macroAssembler_aarch64.cpp:94-103`) recognizes ADRP,
recomputes the page delta, **and rewrites the following ADD's imm12** — exactly the
instruction pair in question, including `ICache::invalidate_range`.

### Fix 4: Size the CodeBuffer for all three sections

Adding `combined_cb.initialize_consts_size(...)` immediately produced:

```
Internal Error (asm/codeBuffer.hpp:176)
assert(allocates2(pc)) failed: not in CodeBuffer memory:
  0x000000010d97ac00 <= 0x000000010d981710 <= 0x000000010d981500
Current CompileTask: yuhu: 66  6  jdk.internal.org.objectweb.asm.Frame::execute (2252 bytes)
V  [libjvm.dylib+0x321670]  CodeSection::set_end(unsigned char*)+0xa0
V  [libjvm.dylib+0xb1a3a4]  YuhuCompiler::compile_method(ciEnv*, ciMethod*, int)+0xcd8
```

The overflowing section is **insts**, not consts: capacity = `0x10d981500 - 0x10d97ac00`
= 26880 bytes, far larger than the 800-byte table; and `set_end` is called directly from
`compile_method` (the insts `set_end` calls at `yuhuCompiler.cpp:1249` / `:1262`).

`initialize_consts_size` does not allocate new memory — it **takes memory from insts**:

```cpp
// codeBuffer.cpp:157-163
address limit  = _insts._limit;
address middle = limit - size;
_insts._limit  = middle - slop;      // insts shrinks by size + slop
cs->initialize(middle, limit - middle);
```

The arithmetic closes exactly:

| Quantity | Value |
|---|---|
| insts capacity after the change | 26880 |
| insts content required | 27408 |
| **overflow** | **528** |
| consts steal (800 table + 64 slop) | 864 |
| insts capacity before the change | 27744 → only 336 bytes of headroom |

That 336-byte headroom came entirely from the ctor's incidental cushion at
`codeBuffer.cpp:98`, `code_size + (align+slop) * (SECT_LIMIT+1)` = `(64+64) * 4` = 512 bytes
(`CodeEntryAlignment = 64`, `globals_aarch64.hpp:52`). **The stubs section was already being
paid for out of alignment slop never intended for section content** — a pre-existing latent
defect that any method with a slightly larger exception/deopt handler would have hit.

The correct contract is C2's (`output.cpp:1129-1150`): every section's content is included in
the buffer size request.

```cpp
int stubs_size  = exc_handler_size + deopt_handler_size;
int consts_size = (int)align_size_up(
    YuhuDebugInformationRecorder::get()->total_const_symbol_size(), sizeof(jdouble));

CodeBuffer combined_cb("yuhu-normal-combined",
                       (int)combined_size + consts_size + stubs_size,
                       (int)(combined_size * 0.15));
if (consts_size > 0) combined_cb.initialize_consts_size(consts_size);
combined_cb.initialize_stubs_size(stubs_size);
```

`combined_size` itself must stay the insts-only content size, because it is also the bound
used by `insts()->set_end()` at `yuhuCompiler.cpp:1249` and `:1262`.

Section-size calls may be made in either order — all three in-tree callers
(`output.cpp:1149-1150`, `compile.cpp:622-623`, `c1_Compilation.cpp:419-425`) do consts first,
then stubs, despite the "call them in reverse section order" comment at `codeBuffer.hpp:527`.
Each call simply carves from the insts tail, and section identity is by index everywhere
(`total_offset_of`, `copy_relocations_to` iterate `SECT_FIRST..SECT_LIMIT`), so only the
temporary buffer's address layout differs.

### Fix 5: Copy each const symbol once

`total_const_symbol_size()` sums each registered entry once, but the scan loop copies
per ADRP+ADD *site*. Two sites resolving to the same symbol would memcpy the same table twice
into space reserved for one copy, silently overrunning the section boundary
(nothing checks `cb->consts()->remaining()`). A source→destination lookup was added so a
symbol is copied at most once and later sites reuse the first destination address.

## Open Items

1. **Runtime verification pending** — Fix 4 has not yet been exercised on `Frame::execute`.
2. **Truncated dedup comparison** in `register_const_symbol`
   (`yuhuDebugInformationRecorder.cpp:266-268`): `addr` is `uint64_t` but the token is
   dereferenced as `uint32_t*`, so only the low 32 bits participate. Two distinct symbols
   sharing low bits would falsely dedup, making `get_const_symbol_by_addr` return NULL and
   tripping `assert(const_symbol_entry != NULL)`. `get_const_symbol_by_addr`
   (`yuhuDebugInformationRecorder.hpp:326-328`) uses `uint64_t*` correctly.
3. **`guarantee(remaining() >= symbol_size)`** before the memcpy is still missing.
4. Non-jump-table `__TEXT,__const` data (float/double literals, if LLVM ever emits them for
   Yuhu functions) is reserved for and copied by the same path — correct but untargeted.
   Distinguishing jump tables would require checking for `AArch64::Delta32` edges into
   `__TEXT,__text` blocks in the LinkGraph.

## Modified Files

- `hotspot/src/share/vm/yuhu/yuhuORCPlugins.cpp` — register `__const` symbols from the LinkGraph; `ShouldNotReachHere()` guard for unknown ADRP shapes
- `hotspot/src/share/vm/yuhu/yuhuDebugInformationRecorder.{hpp,cpp}` — `ConstSymbolEntry`, `register_const_symbol`, `get_const_symbol_by_addr`, `total_const_symbol_size`
- `hotspot/src/share/vm/yuhu/yuhuVirtualAddressPatcher.hpp` — `is_adrp_pattern` → `is_adrp_got_pattern`; new `is_adrp_jump_table_pattern`, `is_unknown_adrp_pattern`
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` — copy const symbols into `cb->consts()`, patch ADRP+ADD, emit `internal_word_type` relocation; `RelocEntry.target` field
- `hotspot/src/share/vm/yuhu/yuhuCompiler.cpp` — CodeBuffer sizing for consts + stubs

## Impact

- Every Yuhu-compiled method containing a `switch` with enough dense cases for LLVM to choose
  a jump table. Sparse switches lowered to compare chains are unaffected.
- Same class of bug applies to **any** LLVM-emitted read-only data referenced from code
  (float/double literals, constant arrays): if ORC JIT places it outside `__TEXT,__text`,
  it dies at `RT->remove()`.

## Lessons Learned

1. **Copying only `.text` out of an ORC JIT allocation is not enough.** Any section the code
   references must be copied into the same CodeCache blob *and* have its references relocated,
   because `RT->remove()` frees everything.
2. **One ADRP matcher is not enough.** ADRP+LDR (GOT-style 64-bit pointer load) and ADRP+ADD
   (array base computation) are disjoint encodings with different meanings. Naming the matcher
   `is_adrp_pattern` hid the gap; `is_adrp_got_pattern` plus an explicit
   `is_unknown_adrp_pattern` → `ShouldNotReachHere()` turns future gaps into loud failures.
3. **A `CodeBuffer`'s size argument covers the insts section only.** `initialize_consts_size` /
   `initialize_stubs_size` redistribute, they do not grow. Relying on the constructor's
   alignment cushion to hold section content works until it doesn't.
4. **Cross-section PC-relative references must be relocations, not one-time patches.** The
   consts↔insts distance changes between the temporary buffer and the final nmethod;
   only `internal_word`/`section_word` records survive `copy_code_to`.
5. **"Jumped to a garbage address" and "returned to a garbage address" are different bugs.**
   Verifying the stack contents at `sp` first eliminated the entire saved-LR hypothesis and
   redirected the investigation to the branch-target computation.
6. **`_sigtramp` in a crash stack is never the bug.** It is macOS's signal-delivery
   trampoline in `libsystem_kernel.dylib`, part of the reporting path.

## Priority

**High** — correctness defect causing JVM crashes in any Yuhu-compiled method with a
table-lowered `switch`; the underlying CodeBuffer sizing defect affects all compilations.

# Activity 086: Copied `__const` Jump-Table Interior Pointers Stale After ORC Teardown — the Second Hop Fixed with Const-Anchored `internal_word` Relocations

## Date
2026-11-02

## Status
**Fixed and runtime-verified.** The ISO8601 reproducer (SIGSEGV on the first call before the
fix) now completes 1,000,000 calls across all 12 tableswitch indices with GC rounds between
them, printing correct values for every month slot, on the slowdebug image.

## Problem Description

Production Kafka JVM crashed ~4.6 s after installing a tier-6 Yuhu nmethod:

```
SIGSEGV, si_addr = x13 = 0x0000000104c24038
problematic frame: nmethod for org.apache.log4j.helpers.ISO8601DateFormat::format (457 bytes)
crash shape:  adrp x13, <consts page> ; add x13, x13, #off   (entry - 0x150)
              ldr  x13, [x13, w0, sxtw #3]                  ; table[index]
              ldr  x1,  [x13]                               ; fault on the dereference
```

A faithful minimizer was built (`test_yuhu/com/example/ISO8601JumpTableRepro.java` +
`test_yuhu/test_iso8601_jumptable.sh`, `YuhuCompileOnlyOf` scoped to the one method,
`-Xbatch`, all 12 switch indices cycled, `System.gc()` between rounds). It crashed on the
**first** call with the identical `si_addr = x13` shape, making the investigation deterministic.

Two early conclusions had to be disproven first:

1. **The jump table WAS copied.** The anchor `adrp+add` resolves to `entry - 0x150`, inside
   the installed blob's consts copy. The defect is not a missing copy — it is what lives
   *inside* the copied bytes.
2. **The consts data itself "looked correct"** in the dump. A correct-looking slot is no
   evidence that the pointer *reaching* it is correct.

## Root Cause Analysis

### 1. The reference chain has TWO hops; only hop 1 was modeled

LLVM lowered the merged 12-case switch (each case block just
`load ptr addrspace(1), ptr @__oop_slot_XXXX`) into a **pointer table** in `__DATA,__const`
whose elements are the **addresses** of `l___oop_slot_*` globals in `__DATA,__data`:

```
insts (adrp+add anchor)  ->  consts copy of the table  ->  oop/metadata slot
        hop 1                        hop 2
```

* **Hop 1** was fixed in Activity 077: the symbol is registered, memcpy'd into
  `cb->consts()`, and the anchor `adrp+add` carries `internal_word_Relocation::spec(...)`,
  which survives the CodeBuffer→nmethod move.
* **Hop 2** did not exist anywhere in the pipeline. Two structural gaps:
  1. `CallSiteExtractorPlugin::visitEdges` iterated `Section.blocks()` only inside the
     loop gated on section names ending in `__text`. Linker edges whose **source** is a
     const-data block (the `Data64` absolute pointers JITLink resolved into the array
     bytes during PostFixup) were never registered as anything.
  2. `scan_and_generate_all_relocations`' `const_symbol` branch memcpy'd the symbol
     **verbatim**. The words hold final arena addresses (legitimate inside the LLVM
     allocation, since array and slots share one JITLink graph), and `RT->remove()` frees
     that arena immediately after `register_method` — every interior word becomes a dangling
     pointer. The crash element `0x104c24038` was confirmed against the trace line
     registering `l___oop_slot_10048`.

### 2. How to detect the condition mechanically

The signal is the symbol's owning **`Block::edges()`**, never byte inspection:
zero edges = pure const data (memcpy safe); ≥1 in-range edges = interior pointers needing
fixup. The catch: LLVM 20's public headers expose **no** Mach-O arm64 pointer-edge
enumerators (`Data64` is absent from the generic `Edge::GenericEdgeKind` and from
`MachO_arm64.h`; `MachO64::PointerByOffset` is not public). Classification therefore became
**content-based**: after link, the word must equal `target + addend` (absolute64),
`target − place + addend` (relative64), or its low-32 projection (relative32). Non-matching
shapes emit a visible compile warning instead of being silently dropped.

### 3. Why a const-anchored relocation is the correct repair (the rejected detour)

An intermediate design proposed rewriting the interior words manually **after install**
(raw `*(uintptr_t*)dst_word = final_slot_addr` plus a hand-computed rebase). That approach
was rejected on review, for reasons now recorded as source-verified facts:

* It reinvents `fix_relocation_after_move` badly, and the recomputed offset arithmetic is a
  second source of truth that drifts from the CodeBuffer's own layout logic.
* The builder-time/scratch-buffer address space is a **moving base**: the temp layout is
  `insts | consts`, the nmethod layout is `consts | insts` (Activity 077), so any address
  recorded before the move without a relocation record is stale by construction.
* GC refresh: an oop-bearing consts slot is a *mirror* of the `_oops` table entry,
  re-written after **every** scavenge by `nmethod::fix_oop_relocations()`
  (`CodeCache::gc_epilogue` / `nmethod::oops_do_marking_epilogue`). A hand-pasted pointer
  fixes the value exactly once and rots at the first GC.

The HotSpot common machinery already supports the second hop natively — the key joints:

```
relocInfo.cpp:186-189   RelocIterator::addr_in_const() is computed PURELY from addr()
                        lying inside [consts.start, consts.end) — not a manually-set flag.
relocInfo.hpp:874-879   DataRelocation::set_value(x): if (addr_in_const()) *(address*)addr() = x;
                        else pd_set_data_value(x, o);   // raw word write for data sites
relocInfo.cpp:876-886   internal_word_Relocation::fix_relocation_after_move: if _target == NULL
                        and addr_in_const(), takes the old value FROM the data word, maps it
                        src→dest; always finishes with set_value(target).
relocInfo.cpp:635-671   internal_word_Relocation::pack_data_to: target in ANOTHER section →
                        record silently strengthened to section_word_type (absolute
                        section-index + offset, re-based against the destination base);
                        target in the SAME section → self-relative byte delta (aarch64
                        `offset_unit = 1`, relocInfo_aarch64.hpp), move-invariant because
                        site and target slide together inside the copied section.
codeBuffer.cpp:693-714  copy_code_to(CodeBlob*) → relocate_code_to: the scratch→nmethod move
                        runs the fix loop over ALL sections including consts.
```

So the same `internal_word_Relocation` type serves both hops, with behavior selected purely
by where its anchor site lives: instruction-patch semantics in insts (hop 1), raw data-word
semantics in consts (hop 2).

## Fix

All changes are in `hotspot/src/share/vm/yuhu/` (plus the recorder struct in the same dir):

### 1. Edge capture — `yuhuORCPlugins.cpp` (`extractCallSites`)

A second pass over the LinkGraph walks `__DATA,__const` / `__DATA,__data` section blocks and,
for every edge whose source lies inside a registered symbol, applies the content-based
classification above and records the edge:

```cpp
recorder->register_symbol_fixup(word_addr, target_addr, addend, fixup_kind);
```

### 2. Fixup storage — `yuhuDebugInformationRecorder.{hpp,cpp}`

`SymbolEntry` gains `GrowableArray<SymbolFixup*>* fixups`
(`SymbolFixup {word_addr, target_addr, addend, SymbolFixupKind kind}`);
`register_symbol_fixup()` lazily creates the arena list and dedups per word address.

### 3. Copy + relocation emission — `yuhuBuilder.cpp` (`copy_symbol_into_consts`)

The recursive copy helper (memoized src→dst, cycle-safe) remaps each `absolute64` interior
word exactly as before (code-label target → same rebase as the code copy; registered symbol
→ recursive copy; external/immortal → value kept), and now additionally appends:

```cpp
RelocEntry interior_entry{};
interior_entry.offset       = dst_word - ctx.code_start;   // absolute-address-keyed
interior_entry.reloc_type   = relocInfo::internal_word_type;
interior_entry.target       = new_value;                   // builder-time cb address
interior_entry.section_type = CodeBuffer::SECT_CONSTS;
reloc_entries.append(interior_entry);                        // only when remapped
```

Immortal values (metaspace pointers, sentinel `inttoptr` indices, runtime function
addresses) get no record — nothing may rebase them.

### 4. Registration — `scan_and_generate_all_relocations`

The `internal_word_type` branch of the reloc registration loop now accepts `SECT_CONSTS`:

```cpp
cb->consts()->relocate((address)instr, internal_word_Relocation::spec((address)reloc_entry.target));
```

The existing global sort by code_start-relative offset keeps every section's anchor chain
ascending (per-section relocate deltas are unsigned).

### 5. Data-slot materialization stays as-is

When a hop-2 target is a `__data` oop/metadata slot, the recursive copy materializes the
value in consts and attaches the pool-indexed `oop_Relocation` / `metadata_Relocation` at the
slot — that record is what `fix_oop_relocations` uses to mirror the slot after each GC. The
interior word points at the slot; the slot tracks the heap object. Both kinds are at
different addresses (word vs slot) and coexist in one ascending consts chain.

### 6. Debug safety net (`#ifdef ASSERT`)

After all copies and fixups, a sweep guarantees no copied word still holds a raw pointer
into the LLVM text blocks or any registered symbol range
("copied symbol retains a pointer into the LLVM arena").

## Verification

* `make hotspot` and `make images` (slowdebug): RC=0; image dylib ad-hoc re-signed.
* `test_yuhu/test_iso8601_jumptable.sh`: **exit 0, no hs_err, no warnings, no guarantee
  hits.** Driver output ends with all 12 slots correct (`month slot 5 -> 2023-06-01...`,
  slot 11 -> 2023-12) followed by `[repro] DONE without crash`. Value correctness matters:
  a silently wrong table would not crash but format the wrong month — the previous failure
  mode. The trace shows the one 96-byte = 12×8 `__DATA,__const` symbol registered and copied.
* Correctness argument at each joint was source-verified (see Root Cause §3), including
  `RelocIterator::next()` having no stale-value verification that could assert during the
  fix loop.

## Known Boundaries

1. `relative64`/`relative32` interior kinds are still repaired manually, no record. This is
   safe for intra-consts targets (byte delta is move-invariant inside one section) but would
   break for a relative word pointing into `__text` (section distance changes on the move).
   No such shape is produced by LLVM for Mach-O arm64 const tables today; a loud guarantee
   against remapped relative-kind targets across sections was offered but not yet added.
2. A copied symbol reached **only** through `__got` indirection still rides the got-branch
   handling; unchanged by this activity.
3. Activity 077 open item 2 (truncated 32-bit dedup comparison in `register_const_symbol`)
   remains separate and open.

## Modified Files

- `hotspot/src/share/vm/yuhu/yuhuORCPlugins.cpp` — const/data section block scan, content-based edge classification, `register_symbol_fixup` call
- `hotspot/src/share/vm/yuhu/yuhuDebugInformationRecorder.{hpp,cpp}` — `SymbolFixupKind`, `SymbolFixup`, `SymbolEntry::fixups`, `register_symbol_fixup`
- `hotspot/src/share/vm/yuhu/yuhuBuilder.cpp` — `copy_symbol_into_consts` (recursive memoized copy, remap tracking, hop-2 `RelocEntry` emission), consts branch in the reloc registration loop, ASSERT sweep
- `hotspot/src/share/vm/yuhu/yuhuBuilder.hpp` — `SymbolCopyContext` (ResourceObj), `RelocEntry` reused as-is
- `test_yuhu/com/example/ISO8601JumpTableRepro.java`, `test_yuhu/test_iso8601_jumptable.sh` — reproducer (from this investigation)

## Impact

- Every Yuhu-compiled method whose lowered switch pointer table (or any multi-word `__const`
  symbol) references other module symbols — the ISO8601/log4j case from the Kafka crash, and
  any dense `switch` returning/branching on merged oop loads.
- Completes Activity 077: hop 1 (anchor) and hop 2 (interior words) are now the same tested
  HotSpot mechanism instead of ad-hoc patching.

## Lessons Learned

1. **An anchor relocation does not make a copied multi-word symbol self-contained.** Copying
   the bytes and fixing the instruction that points at them says nothing about the pointers
   stored *inside* those bytes. Count reference hops explicitly (insts→const→const here) and
   model each one.
2. **`internal_word_Relocation` is site-polymorphic, not code-only.** `addr_in_const()` is a
   pure range test on `addr()`, so the same record type patches an adrp pair in insts and
   raw-rewrites a data word in consts. Doubting "can reloc X apply to a data word" is worth
   resolving by reading `set_value`/`fix_relocation_after_move` branches before designing a
   manual replacement.
3. **Any builder-time address must have a relocation record or it is stale by construction.**
   The temp buffer layout (`insts|consts`) differs from the nmethod layout (`consts|insts`);
   manual post-copy rewriting duplicates the layout logic and misses the post-GC refresh path
   (`fix_oop_relocations` only visits slots that carry records).
4. **Linker edges, not byte heuristics, are the machine-readable signal** for "this const
   symbol references other symbols": zero block edges = memcpy-safe. When the edge-kind
   enumerators are not public (LLVM 20 Mach-O arm64), content-based verification against
   `target+addend` is a sound fallback as long as mismatches fail loudly.
5. **"No crash" is not verification.** Cycle every table index and check the produced
   *values* (wrong month formatting would have exposed a mis-rebased entry that never
   faults); run across GCs because slot mirrors must survive moves.
6. **Grep for the actual names before claiming code does or doesn't exist.** An audit that
   searched for abandoned draft identifiers wrongly concluded the fixup pipeline was missing
   from the tree; it was on disk under `SymbolFixup`/`copy_symbol_into_consts`.

## Priority

**High** — was a deterministic SIGSEGV (Kafka production crash; first-call repro) for any
tier-6 method with a pointer-table switch; now closed with runtime verification.

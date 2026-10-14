# [AArch64][CodeGen] SIGSEGV (null deref) in `StackMaps::parseRegisterLiveOutMask` → `createLiveOutReg` when a live-out register has no minimal physical register class

**Upstream issue:** [llvm/llvm-project#231361](https://github.com/llvm/llvm-project/issues/231361)

## Summary

`llc` crashes with a segmentation fault during AArch64 machine-code emission (`AsmPrinter`) when a
stackmap-bearing instruction (`llvm.experimental.patchpoint` / lowered `llvm.experimental.gc.statepoint`)
has a **live-out register whose `TargetRegisterInfo::getMinimalPhysRegClass()` returns `nullptr`**.
`StackMaps::createLiveOutReg()` dereferences that null class via
`TRI->getSpillSize(*TRI->getMinimalPhysRegClass(Reg))` with no null check.

This fires on a valid, verifier-clean module produced by an LLVM-based JVM JIT that uses
`gc.statepoint` / RewriteStatepointsForGC (RS4GC) with floating-point values live across the
statepoints. It is reproducible entirely offline with `llc` — no JVM required — and a **27-line
standalone `.ll` reproducer is included below.**

## Version / environment

- LLVM **20.1.5** (Homebrew `llvm` on Apple Silicon). `llc --version` → `Optimized build` (assertions off).
- Also present (unfixed) on current `main` — see "Root cause" (the offending line has no null guard).
- Host OS: macOS 14.2.1, arm64 (Apple silicon).
- `llc` invocation used for the reproducer:

```
llc -mtriple=aarch64-apple-darwin --filetype=null repro.ll
```

The crash does **not** depend on `-mcpu`, on `-O` level, or on any `+reserve-*` register feature
(it also reproduces with `-mcpu=apple-m1`, `-mattr=+reserve-x20,...,+reserve-x28`, `-O0` and `-O3`).

## Actual result

```
PLEASE submit a bug report ...
Stack dump:
0.  Program arguments: llc -mtriple=aarch64-apple-darwin --filetype=null repro.ll
1.  Running pass 'Function Pass Manager' on module 'repro.ll'.
2.  Running pass 'AArch64 Assembly Printer' on function '@"<the method>"'
#0 llvm::sys::PrintStackTrace
#1 SignalHandler(int)
#2 (libsystem_platform)
#3 llvm::StackMaps::parseRegisterLiveOutMask(unsigned int const*) const + 364
#4 llvm::StackMaps::parseOperand(...)
#5 llvm::StackMaps::recordStackMapOpers(...)
#6 (anonymous namespace)::AArch64AsmPrinter::emitInstruction(llvm::MachineInstr const*)
#7 llvm::AsmPrinter::emitFunctionBody()
#8 (anonymous namespace)::AArch64AsmPrinter::runOnMachineFunction(llvm::MachineFunction&)
... (PassManager / ORC SimpleCompiler)
zsh: segmentation fault  llc ... ; exit=139
```

Faulting instruction (lldb on the local `llc`, `EXC_BAD_ACCESS code=1 address=0x0`):

```
libLLVM.dylib`llvm::StackMaps::parseRegisterLiveOutMask:
->  0x...+364: ldr    x8, [x8]          ; x8 == 0  (null TargetRegisterClass*)
    0x...+368: ldrh   w8, [x8, #0x18]   ; getSpillSize() reading RC field
    0x...+372: madd   w8, w10, w9, w8
    0x...+376: add    x8, x11, w8, uxtw #4     ; x11 = &llvm::RegClassInfos
```

Register state at the fault shows the register number being encoded in `x13 = 0x213 = 531`, with
`x11 = &llvm::RegClassInfos` and `x14 = &llvm::...::RegisterClasses` — i.e. it is resolving a
register class for live-out register number **531**.

## Root cause

`llvm/lib/CodeGen/StackMaps.cpp`:

```cpp
ErrorOr<LiveOutVec>
StackMaps::parseRegisterLiveOutMask(const uint32_t *Mask) const {
  assert(Mask && "No register mask specified");
  const TargetRegisterInfo *TRI = AP.MF->getSubtarget().getRegisterInfo();
  LiveOutVec LiveOuts;

  // Create a LiveOutReg for each bit that is set in the register mask.
  for (unsigned Reg = 0, NumRegs = TRI->getNumRegs(); Reg != NumRegs; ++Reg)
    if ((Mask[Reg / 32] >> (Reg % 32)) & 1)
      LiveOuts.push_back(createLiveOutReg(Reg, TRI));       // <-- no filter for class-less regs
  ...
}

StackMaps::LiveOutReg
StackMaps::createLiveOutReg(unsigned Reg, const TargetRegisterInfo *TRI) const {
  unsigned DwarfRegNum = getDwarfRegNum(Reg, TRI);
  unsigned Size = TRI->getSpillSize(*TRI->getMinimalPhysRegClass(Reg));  // <-- null deref
  return LiveOutReg(Reg, DwarfRegNum, Size);
}
```

`TargetRegisterInfo::getMinimalPhysRegClass(Reg)` returns `nullptr` for physical registers that belong
to no register class (e.g. registers that are only reachable as an alias/sub-register, or that the
subtarget does not assign a minimal class). `createLiveOutReg` unconditionally dereferences the result
through `getSpillSize(*RC)`, so any such register present as a set bit in the live-out mask crashes.

In this reproducer the offending live-out register (number 531) is in the **floating-point** register
space — confirmed empirically: recompiling with `-mattr=-fp-armv8` (which removes the FP registers from
the register file / live-out mask) makes `llc` exit `0`, while `-mattr=-sve`, `-mattr=-neon`, `-mattr=-fmv`
do **not** avoid it.

## Expected result

`llc` should compile the module without crashing — a stackmap live-out entry for a register with no
minimal physical register class should be skipped (or handled) rather than dereferencing a null class.

## Suggested fix

Guard the null class in `createLiveOutReg` (and/or skip class-less registers in the loop):

```cpp
StackMaps::LiveOutReg
StackMaps::createLiveOutReg(unsigned Reg, const TargetRegisterInfo *TRI) const {
  unsigned DwarfRegNum = getDwarfRegNum(Reg, TRI);
  const TargetRegisterClass *RC = TRI->getMinimalPhysRegClass(Reg);
  unsigned Size = RC ? TRI->getSpillSize(*RC) : 0;
  return LiveOutReg(Reg, DwarfRegNum, Size);
}
```

or, in `parseRegisterLiveOutMask`, do not emit an entry at all for a register without a class:

```cpp
  for (unsigned Reg = 0, NumRegs = TRI->getNumRegs(); Reg != NumRegs; ++Reg)
    if ((Mask[Reg / 32] >> (Reg % 32)) & 1 && TRI->getMinimalPhysRegClass(Reg))
      LiveOuts.push_back(createLiveOutReg(Reg, TRI));
```

A secondary question worth triaging: **why is a register with no minimal physical register class marked
live-out in the statepoint's register mask in the first place?** The defensive guard stops the crash
regardless, but the mask construction may also merit a look.

## How to reproduce

Save the following as `repro.ll` (27 lines, self-contained) and run `llc` on it:

```llvm
; Minimal reproducer: null deref in StackMaps::parseRegisterLiveOutMask -> createLiveOutReg
; on AArch64 when a floating-point value is live across a gc.statepoint under mild
; register pressure (a class-less FP register lands in the live-out mask and
; getMinimalPhysRegClass() returns null, and is dereferenced unchecked).
target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128-Fn32-ni:1"
target triple = "aarch64-apple-darwin"

define ptr addrspace(1) @f(ptr addrspace(1) %0) gc "statepoint-example" {
entry:
  %base = call ptr addrspace(1) (i64, i32, ptr, i32, ...) @llvm.experimental.patchpoint.p1(i64 10004, i32 16, ptr nonnull inttoptr (i64 10004 to ptr), i32 0)
  %addr = getelementptr i8, ptr addrspace(1) %base, i64 104
  %val = load double, ptr addrspace(1) %addr, align 8
  %c1 = fcmp uno double %val, 0.000000e+00
  br i1 %c1, label %gt, label %ord
ord:
  %c2 = fcmp ole double %val, 1.500000e+00
  %c3 = fcmp ult double %val, 1.500000e+00
  %and = and i1 %c2, %c3
  br i1 %and, label %gt, label %done
gt:
  br label %done
done:
  %ph = phi i1 [ true, %gt ], [ false, %ord ]
  %sp = call token (i64, i32, ptr, i32, i32, ...) @llvm.experimental.gc.statepoint.p0(i64 10005, i32 0, ptr elementtype(void (ptr)) inttoptr (i64 4560764544 to ptr), i32 1, i32 0, ptr null, i32 0, i32 0) [ "gc-live"(ptr addrspace(1) %0) ]
  %r = call coldcc ptr addrspace(1) @llvm.experimental.gc.relocate.p1(token %sp, i32 0, i32 0)
  %c4 = fcmp ole double %val, 2.000000e+00
  %cmp = and i1 %ph, %c4
  ret ptr addrspace(1) %r
}

declare token @llvm.experimental.gc.statepoint.p0(i64, i32, ptr, i32, i32, ...)
declare ptr addrspace(1) @llvm.experimental.gc.relocate.p1(token, i32, i32)
declare ptr addrspace(1) @llvm.experimental.patchpoint.p1(i64, i32, ptr, i32, ...)
```

```sh
llc -mtriple=aarch64-apple-darwin --filetype=null repro.ll                       # SIGSEGV, exit 139
llc -mtriple=aarch64-apple-darwin -mattr=-fp-armv8 --filetype=null repro.ll      # exit 0 (control)
```

Verified robustness: the crash is unaffected by `-O0/-O1/-O2/-O3`, by `-mcpu` (reproduces with both
the generic `cortex-a72` scheduling model and `apple-m1`), and by `-filetype=asm` vs `--filetype=null`.
The `--filetype=null` form is used above only to keep the reproducer focused on the backend pass.

### Reduction status

- `llvm-reduce` **cannot** shrink the original (large) input: it round-trips the module through its
  parser, which re-runs RA and perturbs the very live ranges that place a class-less FP register into
  the live-out mask (it reports "input isn't interesting"). The reduced case above was obtained by
  manual, crash-preserving bisection of the real JIT output.
- The crash is genuinely register-pressure / live-range sensitive: removing **either** the `patchpoint.p1`
  result feeding the FP load, **or** collapsing the two-way `fcmp` diamond into straight-line code,
  makes it stop reproducing. All of the following are required together:
  (a) a stackmap-bearing statepoint (`gc.statepoint` / `patchpoint`),
  (b) a `double` computed before and used after the statepoint (kept live across it),
  (c) enough surrounding FP liveness (the fcmp diamond) that a class-less FP register enters the live-out mask.

## Additional context

- Observed only on AArch64; the live-out mask enumerates registers up to `TargetRegisterInfo::getNumRegs()`
  (which includes FP/SVE-adjacent register numbers) without excluding those that have no minimal class.
- The same fault reproduces inside a `libjvm` that statically links this LLVM, on the JIT compiler thread,
  during `AArch64AsmPrinter::emitInstruction`, which is how it was first noticed.

---
*Attachments to include when filing:* `repro.ll` (the 27-line module above), and optionally the original
full JIT module and the `hs_err` register dump showing `x13 = 0x213` at the `ldr x8,[x8]` fault.

package com.example;

// M0 test for Yuhu "Path B" (LLVM-IR method inlining).
//
// Goal: prove that when -XX:+YuhuLLVMInlining is on, a caller compiled by Yuhu
// inlines its "branchy-but-quiescent" static callees as separate alwaysinline
// LLVM functions that the AlwaysInliner pass merges into the caller, WITHOUT
// changing program semantics.
//
// The three callees below are deliberately built to pass
// YuhuInliner::is_llvm_inlineable():
//   - only whitelisted opcodes (constants, loads/stores, non-trapping iadd/isub/
//     ineg, comparisons, forward branches, ireturn),
//   - acyclic (no loops -> no backedge safepoint poll inside the inlinee),
//   - at least one real branch (so they go through Path B, not the straight-line
//     bytecode inliner),
//   - tiny (well under YuhuMaxInlineSize).
// `run` is the Yuhu-compiled caller: it invokes the callees inside a loop and is
// made hot so the JIT compiles it and builds the inline tree.
public class LLVMInlineTest {

    // if + early return: exercises merging a two-exit CFG into the caller.
    static int signAdd(int x) {
        if (x < 0) {
            return -x;
        }
        return x + 1;
    }

    // two-path diamond (a < b ? a+1 : b-1): forces a control-flow join / phi.
    static int choose(int a, int b) {
        if (a < b) {
            return a + 1;
        }
        return b - 1;
    }

    // two sequential forward branches (clamp to [0,100] then +1).
    static int clamp(int x) {
        if (x < 0) {
            x = 0;
        }
        if (x > 100) {
            x = 100;
        }
        return x + 1;
    }

    // Caller Yuhu compiles; the callees above are its inline candidates.
    static long run(int lo, int hi) {
        long acc = 0;
        for (int x = lo; x <= hi; x++) {
            acc += signAdd(x);
            acc += choose(x, hi - x);
            acc += clamp(x);
        }
        return acc;
    }

    // Reference computation, run by the interpreter. Must equal run().
    static long ref(int lo, int hi) {
        long acc = 0;
        for (int x = lo; x <= hi; x++) {
            acc += (x < 0) ? -x : (x + 1);                 // signAdd
            int b = hi - x;
            acc += (x < b) ? (x + 1) : (b - 1);            // choose(x, b)
            int c = x;                                      // clamp
            if (c < 0) c = 0;
            if (c > 100) c = 100;
            acc += c + 1;
        }
        return acc;
    }

    public static void main(String[] args) {
        int lo = -50, hi = 50;

        // Call run() enough to make it hot so the Yuhu tier compiles it.
        long got = 0;
        for (int i = 0; i < 20000; i++) {
            got = run(lo, hi);
        }

        long want = ref(lo, hi);
        System.out.println("run(lo=" + lo + ", hi=" + hi + ") = " + got);
        System.out.println("ref(lo=" + lo + ", hi=" + hi + ") = " + want);

        if (got == want) {
            System.out.println("Yuhu M0 LLVM-inline: PASS");
        } else {
            System.out.println("Yuhu M0 LLVM-inline: FAIL");
            System.exit(1);
        }
    }
}

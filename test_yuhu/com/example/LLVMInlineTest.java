package com.example;

// M0 + M1 test for Yuhu "Path B" (LLVM-IR method inlining).
//
// Goal: prove that when -XX:+YuhuLLVMInlining is on, a caller compiled by Yuhu
// inlines its "branchy-but-quiescent" static callees as separate alwaysinline
// LLVM functions that the AlwaysInliner pass merges into the caller, WITHOUT
// changing program semantics.
//
// The six callees below are deliberately built to pass
// YuhuInliner::is_llvm_inlineable():
//   - only whitelisted opcodes (constants, loads/stores, non-trapping iadd/isub/
//     ineg, comparisons, branches, invokes including invokedynamic, ireturn),
//   - at least one real branch (so they go through Path B, not the straight-line
//     bytecode inliner),
//   - tiny (well under YuhuMaxInlineSize).
//
// signAdd / choose / clamp are acyclic (M0 shape).  sumTo has a for-loop
// (M1 loop shape): opening the loop gate in is_llvm_inlineable lets a
// backedge sit inside an inlinee.  When Yuhu parses sumTo as a Path B
// inlinee, the inlinee's own block hierarchy triggers
// maybe_add_backedge_safepoint, so the inlinee's IR contains a statepoint-
// id-attributed call to the safepoint-poll stub.  After AlwaysInlinerPass
// flattens sumTo into run, that call survives into run's merged IR with
// its statepoint-id intact -- verifiable by grep-ing the emitted
// _tmp_yuhu_ir_*.ll.
//
// maybeTriple has both a forward branch and an invokestatic (M1 invoke
// shape).  Its Path B inlinee body contains an invoke to triple, which
// itself is straight-line and gets Path A bytecode-inlined into
// maybeTriple's own llvm::Function.  The combined Function then gets
// flattened into run by AlwaysInlinerPass.  This exercises the invoke
// whitelist expansion and shows that depth-1 Path A inside a Path B
// inlinee composes correctly.
//
// lambdaApplyOrNeg has a branch AND an invokedynamic site produced by a
// Java lambda expression (M1 invokedynamic shape).  Bootstrap via
// LambdaMetafactory produces a CallSite whose target is a synthetic
// proxy class implementing IntUnaryOperator.  Yuhu's scan_for_traps
// treats _invokedynamic as its own case and does not apply the
// is_compiled_lambda_form / is_method_handle_intrinsic bail that the
// resolved-target invoke variants are subject to.  The subsequent
// invokeinterface on the proxy's applyAsInt dispatches through the
// proxy class's normal vtable entry (not a compiled lambda form), so
// that site clears the guard at yuhuTopLevelBlock.cpp:103 as well.
//
// `run` is the Yuhu-compiled caller: it invokes all six callees inside a
// loop and is made hot so the JIT compiles it and builds the inline
// tree.
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

    // M1: loop-shaped inlinee.  Contains a backedge but only whitelisted
    // opcodes (iconst/iload/istore/if_icmpge/iadd/iinc/goto/ireturn).  Bytecode
    // size is well under YuhuMaxInlineSize (32 bytes default).  For a negative
    // argument the loop body never runs and 0 is returned.
    static int sumTo(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            s = s + i;
        }
        return s;
    }

    // Straight-line helper target for the M1 invoke test.  Contains no
    // branches, so Path A bytecode-inlines it into maybeTriple (which is
    // itself a Path B inlinee of run).  Exercises the "Path A inside a
    // Path B inlinee" composition.
    static int triple(int x) {
        return x * 3;
    }

    // M1: invoke-shaped inlinee.  Has a forward branch (so Path A rejects
    // it) AND an invokestatic (allowed under M1's invoke whitelist).  The
    // callee's own do_invoke runs the same routing as a top-level block's
    // do_invoke: since triple is not marked for depth-2 LLVM-inline by
    // the planner, attempt_inline bytecode-inlines it into maybeTriple's
    // llvm::Function; the combined function then gets flattened into run
    // by AlwaysInlinerPass.
    static int maybeTriple(int x) {
        if (x > 0) {
            return triple(x);
        }
        return -x;
    }

    // M1: invokedynamic-shaped inlinee.  The lambda expression on the
    // first line compiles to an invokedynamic + astore_1 pair.  The
    // subsequent call site (f.applyAsInt(x)) compiles to an
    // invokeinterface against the IntUnaryOperator interface -- the
    // resolved target is the LambdaMetafactory-generated proxy class's
    // applyAsInt method, which is NOT a compiled lambda form, so the
    // scan_for_traps guard at yuhuTopLevelBlock.cpp:103 lets it through.
    // Bytecode is around 20-25 bytes, still under YuhuMaxInlineSize=32.
    static int lambdaApplyOrNeg(int x) {
        java.util.function.IntUnaryOperator f = y -> y + 1;
        if (x > 0) {
            return f.applyAsInt(x);
        }
        return -x;
    }

    // Caller Yuhu compiles; the callees above are its inline candidates.
    static long run(int lo, int hi) {
        long acc = 0;
        for (int x = lo; x <= hi; x++) {
            acc += signAdd(x);
            acc += choose(x, hi - x);
            acc += clamp(x);
            acc += sumTo(x);           // M1 loop-shaped inlinee
            acc += maybeTriple(x);     // M1 invoke-shaped inlinee
            acc += lambdaApplyOrNeg(x); // M1 invokedynamic-shaped inlinee
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
            int s = 0;                                      // sumTo
            for (int i = 0; i < x; i++) {
                s = s + i;
            }
            acc += s;
            acc += (x > 0) ? (x * 3) : (-x);               // maybeTriple
            acc += (x > 0) ? (x + 1) : (-x);               // lambdaApplyOrNeg
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
            System.out.println("Yuhu M0+M1 LLVM-inline: PASS");
        } else {
            System.out.println("Yuhu M0+M1 LLVM-inline: FAIL");
            System.exit(1);
        }
    }
}

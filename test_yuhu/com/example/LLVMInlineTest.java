package com.example;

// M0 + M1 + M2 test for Yuhu "Path B" (LLVM-IR method inlining).
//
// Goal: prove that when -XX:+YuhuLLVMInlining is on, a caller compiled by Yuhu
// inlines its "branchy-but-quiescent" static callees as separate alwaysinline
// LLVM functions that the AlwaysInliner pass merges into the caller, WITHOUT
// changing program semantics.
//
// The eight callees below are deliberately built to pass
// YuhuInliner::is_llvm_inlineable():
//   - only whitelisted opcodes (constants, loads/stores, non-trapping iadd/isub/
//     ineg, comparisons, branches, invokes including invokedynamic, allocation,
//     invokespecial <init>, athrow since M2, ireturn),
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
// throwOrNeg (M2) exercises Path B inlining of an athrow-bearing
// inlinee where the INLINEE has NO local try/catch.  Bytecode is
// iload_0 / iflt / (normal path: iload_0 ireturn) / (throw path:
// new IllegalArgumentException + dup + invokespecial <init> + athrow),
// ~14 bytes.  When x<0 the inlinee's do_athrow pops the exception,
// calls handle_exception with num_exceptions()==0, which falls through
// to handle_return(T_VOID, exception)'s inlinee branch: store excep-
// tion to Thread::pending_exception + branch to the inlinee's unified
// exit block.  Post AlwaysInlinerPass flattening, the caller's con-
// tinuation immediately after the inline call runs do_llvm_inline_call's
// newly-emitted check_pending_exception against the caller's try/catch
// -- the caller's own emit_handler_checks fires with the caller's own
// statepoint-id + deopt bundle (no multi-level bundle needed).  The
// caller catches IllegalArgumentException and adds 9999 to the acc.
//
// tryCatchNeg (M2) exercises Path B inlining of an inlinee that DOES
// have a local try/catch -- i.e., the inlinee's own ciMethod has an
// exception handler table entry.  Bytecode is iload_0 / ifle / (throw
// path: new RuntimeException + dup + invokespecial <init> + athrow)
// / (normal path: iload_0 ireturn) / handler (astore_1 iconst_m1
// ireturn), ~21 bytes, all under YuhuMaxInlineSize=32.  Yuhu's Path B
// inline eligibility check now passes has_exception_handlers() when
// may_be_inlinable is called with allow_exception_handlers=true (see
// yuhuInliner.cpp).  The inlinee's handle_exception consults the IN-
// LINEE's ciExceptionHandlerStream (num_exceptions() > 0), runs emit_
// handler_checks with the inlinee's catch klass, and dispatches to the
// inlinee's own handler block for x>0.  Post-inline that handler block
// lives inside the caller's llvm::Function; its statepoint-ids and
// inline-node references survive AlwaysInlinerPass unchanged.  Since
// the exception IS caught inside the inlinee, no pending_exception is
// set and the caller's post-inline check_pending_exception sees null
// and falls through cleanly.
//
// `run` is the Yuhu-compiled caller: it invokes all eight callees inside
// a loop and is made hot so the JIT compiles it and builds the inline
// tree.  The caller itself has a try/catch wrapping the throwOrNeg call
// site (so run's bytecode does have an exception handler table entry,
// which is fine at the top-level compile).
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

    // M2: Path B inlinee that throws uncaught.  Caller's try/catch
    // (added inside run() below) picks the exception up at compiled-
    // code level via do_llvm_inline_call's check_pending_exception
    // against the caller's own ciExceptionHandlerStream.
    static int throwOrNeg(int x) {
        if (x < 0) {
            throw new IllegalArgumentException();
        }
        return x;
    }

    // M2: Path B inlinee with an internal try/catch.  The inlinee's
    // own handle_exception fast dispatches to the inlinee's handler
    // block (post-inline lives inside the caller's llvm::Function),
    // so the exception never leaves the compiled frame.
    static int tryCatchNeg(int x) {
        try {
            if (x > 0) {
                throw new RuntimeException();
            }
            return x;
        } catch (RuntimeException e) {
            return -1;
        }
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
            // M2: Path B inlinee throws uncaught -> caller's try/catch
            // fires at compiled-code level via the caller's own
            // emit_handler_checks in do_llvm_inline_call's continuation.
            try {
                acc += throwOrNeg(x);
            } catch (IllegalArgumentException e) {
                acc += 9999;
            }
            // M2: Path B inlinee throws and catches internally -> the
            // inlinee's own emit_handler_checks runs; caller's post-
            // inline pending_exception check sees null and continues.
            acc += tryCatchNeg(x);
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
            acc += (x < 0) ? 9999 : x;                     // throwOrNeg (thrown + caught by caller)
            acc += (x > 0) ? -1 : x;                       // tryCatchNeg (thrown + caught inside inlinee)
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
            System.out.println("Yuhu M0+M1+M2 LLVM-inline: PASS");
        } else {
            System.out.println("Yuhu M0+M1+M2 LLVM-inline: FAIL");
            System.exit(1);
        }
    }
}

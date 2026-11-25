package com.example;

/**
 * Reproducer for Yuhu tier-6 BitData null_seen consumption at checkcast
 * and instanceof (flag: YuhuProfileNullCast, default OFF).
 *
 * Two speculation sites:
 *
 *   1. checkcast (downcast):  castToBarAndAccess(Object) — inside a loop
 *      the method casts Object→Bar and immediately calls Bar.fieldValue().
 *      The casted value flows through the getfield + invokevirtual, each of
 *      which normally emits a null check (do_zero_check → throw_NullPointer-
 *      Exception stub).  With null_seen speculation, the cast result carries
 *      zero_checked=true, so downstream accesses skip the null check.  The
 *      IR difference is: fewer call sites to throw_NullPointerException.
 *
 *   2. instanceof fused with if (maybe_do_instanceof_if): checkAndAccess
 *      (Object) — `if (o instanceof Bar) { ((Bar)o).fieldValue(); }`.  In
 *      the true branch, old_object gets set_zero_checked(true) before
 *      do_full_instance_check; new_object inherits the flag; replace_all
 *      propagates it into the branch.  Downstream getfield on the cast
 *      result skips its null check.
 *
 * Warm-up strategy: the test drives the method through tier-0 interpreter
 * (writes null_seen BitData only if null actually hits the checkcast),
 * then reaches tier-6.  As long as the loop NEVER stores null, null_seen
 * stays 0, the speculation gate fires, and the tier-6 IR reflects the
 * elision.
 *
 * The noise loop arithmetic (8 variables + 8 bitwise tests) inflates the
 * complexity score so run() reaches tier-6 naturally without any
 * YuhuComplexityThreshold override.
 *
 * Verification:
 *   Scenario [2] (-YuhuProfileNullCast, control):
 *     - throw_NullPointerException count in run()'s tier-6 IR >= 2
 *       (one from castToBarAndAccess's getfield, one from checkAndAccess's
 *       getfield in the true branch).
 *   Scenario [3] (+YuhuProfileNullCast, test):
 *     - throw_NullPointerException count in run()'s tier-6 IR < control's
 *       count (speculation elides the downstream checks).
 *     - Oracle values identical to control.
 *   Scenario [4] (+YuhuProfileNullCast +DeoptimizeALot):
 *     - Oracle values identical (speculation deopt is transparent).
 *   Scenario [5] tier-5 negative control:
 *     - throw_NullPointerException count equals control's (comp_level gate
 *       suppresses speculation below tier 6).
 */
public class NullCastProfileTest {

    static class Foo {
        int x;
        Foo(int v) { x = v; }
    }

    static class Bar extends Foo {
        int extra;
        Bar(int v) { super(v); extra = v * 3; }
        int fieldValue() { return x + extra; }
    }

    /**
     * Main driver: loops containing BOTH speculation patterns inline.
     * The checkcast and instanceof are directly in this method's
     * bytecode so tier-6 compilation applies speculation without
     * depending on inlining.
     *
     * Noise loop arithmetic (8 variables + 8 bitwise tests) inflates the
     * complexity score so run() reaches tier-6 naturally without any
     * YuhuComplexityThreshold override.
     */
    static long run(int N) {
        long acc = 0;
        int n0 = 0, n1 = 1, n2 = 2, n3 = 3;
        int n4 = 4, n5 = 5, n6 = 6, n7 = 7;
        for (int i = 0; i < N; i++) {
            // --- Speculation site 1: checkcast path (direct) ---
            Object obj = new Bar(i);
            Bar b = (Bar) obj;       // checkcast — null_seen profiled here
            acc += b.fieldValue();   // invokevirtual — null check elided

            // --- Speculation site 2: instanceof fused if path (direct) ---
            Object obj2 = new Bar(i + 1);
            if (obj2 instanceof Bar) {  // instanceof — null_seen profiled here
                Bar b2 = (Bar) obj2;
                acc += b2.x;            // getfield — null check elided
            }

            // --- Noise arithmetic + branches (inflate block count) ---
            if ((n0 & 1) != 0) n0 += i; else n0 ^= 0x5a;
            if ((n1 & 2) == 0) n1 += i; else n1 ^= 0x33;
            if ((n2 & 4) != 0) n2 += i; else n2 ^= 0x77;
            if ((n3 & 1) == 0) n3 += i; else n3 ^= 0x1f;
            if ((n4 & 8) != 0) n4 += i; else n4 ^= 0x2b;
            if ((n5 & 16) == 0) n5 += i; else n5 ^= 0x4d;
            if ((n6 & 32) != 0) n6 += i; else n6 ^= 0x6e;
            if ((n7 & 64) == 0) n7 += i; else n7 ^= 0x9a;

            acc += n0 + n1 + n2 + n3 + n4 + n5 + n6 + n7;
        }
        return acc;
    }

    /**
     * Independent oracle: computes the same result without casting.
     */
    static long runOracle(int N) {
        long acc = 0;
        int n0 = 0, n1 = 1, n2 = 2, n3 = 3;
        int n4 = 4, n5 = 5, n6 = 6, n7 = 7;
        for (int i = 0; i < N; i++) {
            Bar b = new Bar(i);
            acc += b.x + b.extra;  // same as Bar.fieldValue()

            Bar b2 = new Bar(i + 1);
            acc += b2.x;          // same as instanceof-true-branch path

            if ((n0 & 1) != 0) n0 += i; else n0 ^= 0x5a;
            if ((n1 & 2) == 0) n1 += i; else n1 ^= 0x33;
            if ((n2 & 4) != 0) n2 += i; else n2 ^= 0x77;
            if ((n3 & 1) == 0) n3 += i; else n3 ^= 0x1f;
            if ((n4 & 8) != 0) n4 += i; else n4 ^= 0x2b;
            if ((n5 & 16) == 0) n5 += i; else n5 ^= 0x4d;
            if ((n6 & 32) != 0) n6 += i; else n6 ^= 0x6e;
            if ((n7 & 64) == 0) n7 += i; else n7 ^= 0x9a;

            acc += n0 + n1 + n2 + n3 + n4 + n5 + n6 + n7;
        }
        return acc;
    }

    public static void main(String[] args) throws Exception {
        // Self-validation on any stock JVM: fixture must produce
        // identical results vs oracle in interpreter before JIT is tested.
        final int warmN = 32;
        final int warmIter = 200000;
        long gotWarm = 0;
        long expectWarm = 0;
        for (int w = 0; w < warmIter; w++) {
            gotWarm += run(warmN);
            expectWarm += runOracle(warmN);
        }
        if (gotWarm != expectWarm) {
            System.out.println("FAIL: warmup oracle mismatch: got "
                + gotWarm + " want " + expectWarm);
            System.exit(1);
        }

        // Post-tier-6 correctness: the profile from warmup is frozen
        // so tier-6 speculation is deterministically active.
        int bigN = 100000;
        long big_want = runOracle(bigN);
        for (int w = 0; w < 20; w++) {
            long got = run(bigN);
            if (got != big_want) {
                System.out.println("FAIL: big#" + w + ": got " + got
                    + " want " + big_want);
                System.exit(1);
            }
        }

        System.out.println("NullCastProfileTest: ALL OK");
    }
}

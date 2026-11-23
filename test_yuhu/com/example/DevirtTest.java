package com.example;

/**
 * Reproducer for Yuhu Increment 2: profile-driven devirtualization of
 * invokevirtual sites at the optimized tier (level 6).
 *
 * Scenarios covered:
 *   - runMono: monomorphic receiver (Mono only).  At tier-6 the planner
 *              reads the caller MDO's VirtualCallData row, sees a single
 *              hot Mono receiver, substitutes Mono.compute, records a
 *              Mono klass guard; do_call then emits the guard and
 *              LLVM-inlines Mono.compute.
 *   - runPoly: alternating Mono / Poly receivers.  Two non-null receiver
 *              rows, so virtual_call_target rejects; site stays virtual
 *              (vtable dispatch, no guard).
 *   - guard-fail trap: after tier-6 has inlined Mono.compute behind the
 *              guard, calling runMono with a Poly receiver takes the
 *              guard_fail edge.  do_trap(Reason_class_check,
 *              Action_reinterpret) fires and the interpreter reexecutes
 *              the invokevirtual normally against Poly.
 *
 * Complexity design (why runMono / runPoly are intentionally large):
 *   calculate_complexity_score = code_size * (num_blocks + 1) * has_loops
 *   The default YuhuComplexityThreshold is 5000.  We want the test methods
 *   to exceed 5000 naturally so the tier-6 promotion works WITHOUT
 *   lowering YuhuComplexityThreshold (which would send every trivial JDK
 *   helper into Yuhu and race C2 for compilation slots).
 *   Target: code_size ~400 bytes, num_blocks ~10, has_loops=true
 *         => 400 * 11 * 2 = 8800 >> 5000.  ✓
 *
 * Note: this test does NOT use CompileCommand=yuhuonly, because yuhuonly
 * forces a direct 0->6 compile that skips tier 3, and tier 3 is the sole
 * MDO profiler.  Instead the natural ladder 0 -> 3 -> 5 -> 6 is used.
 */
public class DevirtTest {

    // Base MUST be an abstract class, not an interface: the current
    // increment only wires _invokevirtual.  If we declared it as an
    // interface, javac emits _invokeinterface and the planner skips it.
    abstract static class Base {
        abstract int compute(int x);
    }

    // Deliberately non-final so virtual_call_target has to swap the
    // CP-cache statically resolved Base.compute (abstract) for Mono.compute
    // (concrete).  A final class would let CHA handle it and bypass our
    // new path.
    //
    // Keep these tiny (7 bytes each).  Mono.compute must fit inside
    // YuhuMaxInlineSize (default 32) so it can be LLVM-inlined into runMono.
    // Complexity belongs on the CALLER, not here.
    static class Mono extends Base {
        @Override public int compute(int x) { return x * 3 + 7; }
    }

    static class Poly extends Base {
        @Override public int compute(int x) { return x - 1; }
    }

    // -------------------------------------------------------------------------
    // Complex runMono: the outer semantics are still "sum of compute(i) for
    // i in [0, N)".  The n0..n7 noise variables and their branch chain exist
    // only to inflate code_size and num_blocks past the tier-6 complexity
    // threshold.  They never write to `acc`.
    // -------------------------------------------------------------------------
    static long runMono(Base c, int N) {
        long acc = 0;
        int n0 = 0, n1 = 1, n2 = 2, n3 = 3;
        int n4 = 4, n5 = 5, n6 = 6, n7 = 7;
        for (int i = 0; i < N; i++) {
            acc += c.compute(i);
            // Arithmetic noise (does not touch acc).
            n0 = (n0 ^ (int)acc) + i;
            n1 = (n1 * 3 - n0) ^ (i & 0xFF);
            n2 = (n2 + i) ^ (n1 >>> 1);
            n3 = (n3 ^ n2) + (n0 & n1);
            n4 = (n4 + n3) - (i * 2);
            n5 = (n5 ^ i) + (n4 << 1);
            n6 = (n6 + n5) ^ (n3 & 0x7F);
            n7 = (n7 ^ n6) + (n5 | n4);
            // Branch noise (inflates num_blocks).
            if ((n0 & 1) != 0) { n0 += 17; n1 -= 13; }
            if ((n1 & 1) != 0) { n2 += 19; n3 -= 23; }
            if ((n2 & 1) != 0) { n4 += 29; n5 -= 31; }
            if ((n3 & 1) != 0) { n6 += 37; n7 -= 41; }
            if ((n4 & 3) == 0) { n0 = (n0 + 7) ^ n5; }
            if ((n5 & 3) == 1) { n1 = (n1 + 11) ^ n6; }
            if ((n6 & 3) == 2) { n2 = (n2 + 13) ^ n7; }
            if ((n7 & 3) == 3) { n3 = (n3 + 17) ^ n4; }
        }
        // Impossible guard: prevents javac from eliminating the noise vars,
        // but is never true at runtime so `acc` reflects only the sum of
        // compute() calls.
        if (n0 + n1 + n2 + n3 + n4 + n5 + n6 + n7 == Integer.MIN_VALUE) {
            acc += 1;
        }
        return acc;
    }

    // -------------------------------------------------------------------------
    // Complex runPoly: same noise-accumulator pattern; the receiver alternates
    // between two klass types so the profile shows bimorphic rows and
    // virtual_call_target rejects.
    // -------------------------------------------------------------------------
    static long runPoly(Base a, Base b, int N) {
        long acc = 0;
        int n0 = 0, n1 = 1, n2 = 2, n3 = 3;
        int n4 = 4, n5 = 5, n6 = 6, n7 = 7;
        for (int i = 0; i < N; i++) {
            Base c = ((i & 1) == 0) ? a : b;
            acc += c.compute(i);
            n0 = (n0 ^ (int)acc) + i;
            n1 = (n1 * 5 - n0) ^ (i | 0x11);
            n2 = (n2 + i) ^ (n1 >>> 2);
            n3 = (n3 ^ n2) + (n0 & n1);
            n4 = (n4 + n3) - (i * 3);
            n5 = (n5 ^ i) + (n4 << 2);
            n6 = (n6 + n5) ^ (n3 | 0x5A);
            n7 = (n7 ^ n6) + (n5 ^ n4);
            if ((n0 & 1) == 0) { n0 += 11; n2 -= 7; }
            if ((n1 & 1) == 0) { n1 += 13; n3 -= 17; }
            if ((n2 & 1) == 0) { n4 += 19; n6 -= 23; }
            if ((n3 & 1) == 0) { n5 += 29; n7 -= 31; }
            if ((n4 & 3) == 1) { n0 = (n0 - 5) ^ n6; }
            if ((n5 & 3) == 2) { n1 = (n1 - 7) ^ n7; }
            if ((n6 & 3) == 3) { n2 = (n2 - 11) ^ n4; }
            if ((n7 & 3) == 0) { n3 = (n3 - 13) ^ n5; }
        }
        if (n0 + n1 + n2 + n3 + n4 + n5 + n6 + n7 == Integer.MAX_VALUE) {
            acc += 1;
        }
        return acc;
    }

    // ----- independent reference computations (always correct) -----

    private static long refMonoMono(int N) {
        long s = 0;
        for (int i = 0; i < N; i++) s += i * 3L + 7L;
        return s;
    }

    private static long refMonoPoly(int N) {
        // Same as runMono but receiver is Poly (guard-fail path).
        long s = 0;
        for (int i = 0; i < N; i++) s += (long) (i - 1);
        return s;
    }

    private static long refPoly(int N) {
        long s = 0;
        for (int i = 0; i < N; i++) {
            s += ((i & 1) == 0) ? (i * 3L + 7L) : (i - 1L);
        }
        return s;
    }

    private static void check(String tag, long got, long want) {
        if (got != want) {
            System.out.println("FAIL " + tag + ": got " + got + " want " + want);
            System.exit(1);
        }
    }

    public static void main(String[] args) {
        final Mono mono = new Mono();
        final Poly poly = new Poly();

        // ---- Warm runMono hard: ~200k invocations with small N.  The
        //      method is now complex enough that when cur_level reaches
        //      tier 3, should_compile_with_yuhu passes naturally (no
        //      YuhuComplexityThreshold override needed).  The invoke site
        //      only ever sees Mono receivers so MDO row 0 is Mono, row 1+
        //      is NULL. ----
        final int warmN = 32;
        final int warmIter = 200000;
        long expectWarm = 0;
        long gotWarm = 0;
        for (int w = 0; w < warmIter; w++) {
            gotWarm += runMono(mono, warmN);
            expectWarm += refMonoMono(warmN);
        }
        check("warmup-mono-aggregate", gotWarm, expectWarm);

        // ---- Warm runPoly: alternate Mono / Poly receivers so two rows
        //      in the MDO VirtualCallData are populated (bimorphic). ----
        long expectPoly = 0;
        long gotPoly = 0;
        for (int w = 0; w < warmIter / 2; w++) {
            gotPoly += runPoly(mono, poly, warmN);
            expectPoly += refPoly(warmN);
        }
        check("warmup-poly-aggregate", gotPoly, expectPoly);

        // ---- Post-tier6 correctness: keep calling runMono with Mono.
        //      This should execute the devirtualized inlined Mono.compute
        //      behind the receiver-klass guard (guard klass matches). ----
        for (int w = 0; w < 100; w++) {
            long s = runMono(mono, 1000);
            check("tier6-mono#" + w, s, refMonoMono(1000));
        }

        try { Thread.sleep(30000); } catch (Exception e) { }

        // ---- Force guard-fail: same compiled runMono now gets a Poly
        //      receiver.  The guard klass mismatches, do_trap fires, the
        //      interpreter reexecutes the invokevirtual normally against
        //      Poly.  Results must match a pure interpreted Poly workload. ----
        for (int w = 0; w < 10; w++) {
            long s = runMono(poly, 1000);
            check("guard-fail#" + w, s, refMonoPoly(1000));
        }

        // ---- Sanity: runPoly stays correct throughout (no devirt, still
        //      polymorphic dispatch via the vtable stub). ----
        for (int w = 0; w < 5; w++) {
            long s = runPoly(mono, poly, 1000);
            check("bimorphic-sanity#" + w, s, refPoly(1000));
        }

        System.out.println("DevirtTest: ALL OK");
    }
}

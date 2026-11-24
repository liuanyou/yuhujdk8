package com.example;

/**
 * Reproducer for Yuhu Increment 2 extended to _invokeinterface:
 * profile-driven devirtualization of interface call sites at the
 * optimized tier (level 6).
 *
 * This mirrors DevirtTest.java's structure, but the receiver is an
 * INTERFACE-typed local so javac emits invokeinterface (verified by the
 * runner via javap).  The whole point of following DevirtTest rather
 * than a Class.forName-driven load is that BOTH impls are present in
 * the class table BEFORE the tier-6 compile, so no dependency/class-load
 * invalidation ever fires; the substituted nmethod stays entrant and its
 * runtime receiver-klass guard is what actually runs the trap edge.
 *
 * Scenarios covered:
 *   - runShape: monomorphic receiver (Circle only).  At tier-6 the
 *              planner reads the caller MDO's VirtualCallData row, sees a
 *              single hot Circle receiver, substitutes Circle.area, records
 *              a Circle klass guard; do_call emits the guard and
 *              LLVM-inlines Circle.area.
 *   - runShapePoly: alternating Circle / Square receivers.  Two non-null
 *              receiver rows, so virtual_call_target rejects; site stays
 *              virtual (itable dispatch, no guard).
 *   - guard-fail trap: after tier-6 has inlined Circle.area behind the
 *              guard, calling runShape with a Square receiver takes the
 *              guard_fail edge.  do_trap(Reason_class_check,
 *              Action_reinterpret) fires and the interpreter reexecutes
 *              the invokeinterface normally against Square.  Because
 *              Square was loaded up front, runShape's nmethod was NEVER
 *              invalidated, so this is the original guarded code that
 *              takes the trap -- not a freshly recompiled vtable path.
 *
 * Complexity design (why runShape / runShapePoly are large): identical to
 * DevirtTest -- calculate_complexity_score = code_size * (num_blocks + 1)
 * * has_loops must exceed the default YuhuComplexityThreshold of 5000
 * naturally, so tier-6 promotion needs no flag override.  Noise vars n0..n7
 * never touch `acc`.
 */
public class InterfaceDevirtTest {

    // MUST be an interface so the call site is invokeinterface, exercising
    // the newly widened (_invokevirtual || _invokeinterface) devirt gate.
    interface Shape { int area(int x); }

    // Tiny (7 bytes) so Circle.area fits inside YuhuMaxInlineSize and can be
    // LLVM-inlined behind the guard.  Complexity lives on the caller.
    static class Circle implements Shape {
        @Override public int area(int x) { return x * 3 + 7; }
    }

    // Distinct body so guard-hit (Circle) and guard-fail (Square) results
    // differ, making a mis-executed guard unmissable.
    static class Square implements Shape {
        @Override public int area(int x) { return x - 1; }
    }

    // -------------------------------------------------------------------------
    // Monomorphic caller: sum of area(i) for i in [0, N).  Noise vars inflate
    // code_size / num_blocks past the tier-6 threshold without touching acc.
    // -------------------------------------------------------------------------
    static long runShape(Shape s, int N) {
        long acc = 0;
        int n0 = 0, n1 = 1, n2 = 2, n3 = 3;
        int n4 = 4, n5 = 5, n6 = 6, n7 = 7;
        for (int i = 0; i < N; i++) {
            acc += s.area(i);
            n0 = (n0 ^ (int)acc) + i;
            n1 = (n1 * 3 - n0) ^ (i & 0xFF);
            n2 = (n2 + i) ^ (n1 >>> 1);
            n3 = (n3 ^ n2) + (n0 & n1);
            n4 = (n4 + n3) - (i * 2);
            n5 = (n5 ^ i) + (n4 << 1);
            n6 = (n6 + n5) ^ (n3 & 0x7F);
            n7 = (n7 ^ n6) + (n5 | n4);
            if ((n0 & 1) != 0) { n0 += 17; n1 -= 13; }
            if ((n1 & 1) != 0) { n2 += 19; n3 -= 23; }
            if ((n2 & 1) != 0) { n4 += 29; n5 -= 31; }
            if ((n3 & 1) != 0) { n6 += 37; n7 -= 41; }
            if ((n4 & 3) == 0) { n0 = (n0 + 7) ^ n5; }
            if ((n5 & 3) == 1) { n1 = (n1 + 11) ^ n6; }
            if ((n6 & 3) == 2) { n2 = (n2 + 13) ^ n7; }
            if ((n7 & 3) == 3) { n3 = (n3 + 17) ^ n4; }
        }
        if (n0 + n1 + n2 + n3 + n4 + n5 + n6 + n7 == Integer.MIN_VALUE) {
            acc += 1;
        }
        return acc;
    }

    // -------------------------------------------------------------------------
    // Polymorphic caller: receiver alternates Circle / Square so two MDO
    // VirtualCallData rows are populated; virtual_call_target rejects.
    // -------------------------------------------------------------------------
    static long runShapePoly(Shape a, Shape b, int N) {
        long acc = 0;
        int n0 = 0, n1 = 1, n2 = 2, n3 = 3;
        int n4 = 4, n5 = 5, n6 = 6, n7 = 7;
        for (int i = 0; i < N; i++) {
            Shape s = ((i & 1) == 0) ? a : b;
            acc += s.area(i);
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

    private static long refCircle(int N) {         // guard-hit: Circle.area
        long s = 0;
        for (int i = 0; i < N; i++) s += i * 3L + 7L;
        return s;
    }

    private static long refSquare(int N) {         // guard-fail: Square.area
        long s = 0;
        for (int i = 0; i < N; i++) s += (long) (i - 1);
        return s;
    }

    private static long refPoly(int N) {           // alternating
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
        // BOTH impl instances constructed here, so Circle AND Square are in
        // the class table before any tier-6 compile.  No late class load,
        // hence no dependency invalidation -- the guarded nmethod stays
        // entrant for the whole run.
        final Circle circle = new Circle();
        final Square square = new Square();

        // ---- Warm runShape hard (monomorphic Circle) so it promotes to
        //      tier 6 naturally and the invoke site profile stays Mono:
        //      VirtualCallData row 0 = Circle, row 1+ = NULL. ----
        final int warmN = 32;
        final int warmIter = 200000;
        long expectWarm = 0;
        long gotWarm = 0;
        for (int w = 0; w < warmIter; w++) {
            gotWarm += runShape(circle, warmN);
            expectWarm += refCircle(warmN);
        }
        check("warmup-shape-aggregate", gotWarm, expectWarm);

        // ---- Warm runShapePoly: alternate Circle / Square so two rows are
        //      populated (bimorphic) and substitution is rejected there. ----
        long expectPoly = 0;
        long gotPoly = 0;
        for (int w = 0; w < warmIter / 2; w++) {
            gotPoly += runShapePoly(circle, square, warmN);
            expectPoly += refPoly(warmN);
        }
        check("warmup-poly-aggregate", gotPoly, expectPoly);

        // ---- Post-tier6 guard-HIT: keep calling runShape with Circle.
        //      Executes the devirtualized inlined Circle.area behind the
        //      matching receiver-klass guard. ----
        for (int w = 0; w < 100; w++) {
            long s = runShape(circle, 1000);
            check("tier6-guard-hit#" + w, s, refCircle(1000));
        }

        try { Thread.sleep(30000); } catch (Exception e) { }

        // ---- Force guard-FAIL: the SAME compiled runShape (never
        //      invalidated) now gets a Square receiver.  The guard klass
        //      mismatches, do_trap(Reason_class_check, Action_reinterpret)
        //      fires, the interpreter reexecutes the invokeinterface against
        //      Square.  Results must equal a pure Square workload. ----
        for (int w = 0; w < 10; w++) {
            long s = runShape(square, 1000);
            check("guard-fail#" + w, s, refSquare(1000));
        }

        // ---- Sanity: runShapePoly stays correct throughout (bimorphic,
        //      itable dispatch, no devirt). ----
        for (int w = 0; w < 5; w++) {
            long s = runShapePoly(circle, square, 1000);
            check("bimorphic-sanity#" + w, s, refPoly(1000));
        }

        System.out.println("InterfaceDevirtTest: ALL OK");
    }
}

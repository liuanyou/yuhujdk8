package com.example;

/**
 * Validation fixture for Yuhu's CHA-based devirtualization after the
 * "inhibit cast" block was removed from improve_virtual_call.
 *
 * Structure:
 *   Base   -- abstract receiver, declares abstract compute(int).
 *   Mono   -- the only concrete subclass loaded during phase 1, so
 *             Dependencies::find_unique_concrete_method(Base, compute)
 *             returns Mono.compute and improve_virtual_call binds the
 *             site directly behind an assert_unique_concrete_method dep.
 *   Tertius -- a second concrete subclass.  Loaded ONLY via
 *               Class.forName at the start of phase 2, so it is not
 *               present in the class table during tier-6 compilation of
 *               phase 1.  Its load should flush the tier-6 nmethod
 *               because the dep is violated, forcing re-execution via
 *               the interpreter and a subsequent CHA-ineligible tier-6
 *               recompile (Base.compute now has two impls).
 *
 * Reference sums are computed independently in Java (refSumMono,
 * refSumTertius) so the fixture is self-validating on a stock JVM.
 * The JIT-under-test must produce the same numbers.
 *
 * runCha is intentionally complex (8 noise locals + 8 in-loop
 * two-way branches + never-take tail guard) so its bytecode size
 * clears the default YuhuComplexityThreshold=5000 with a real loop,
 * triggering tier-6 naturally without -XX:YuhuComplexityThreshold=1.
 */
public class ChaDevirtTest {

    static abstract class Base { abstract int compute(int x); }

    static class Mono extends Base {
        public int compute(int x) { return x * 3 + 7; }
    }

    // Never referenced in main() bytecode; loaded lazily via Class.forName.
    static class Tertius extends Base {
        public int compute(int x) { return x + 1000; }
    }

    // Complexity-inflated caller. Body has ~300+ bytes of bytecode so the
    // tier-6 policy formula (code_size * (num_blocks+1) * has_loops?2:1)
    // exceeds 5000 by a wide margin without flag overrides.
    static int runCha(Base c, int N) {
        int acc = 0;
        int n0 = 0, n1 = 0, n2 = 0, n3 = 0, n4 = 0, n5 = 0, n6 = 0, n7 = 0;
        for (int i = 0; i < N; i++) {
            acc += c.compute(i);
            n0 = (n0 + i) ^ 3;
            n1 = (n1 * 5) + i;
            n2 = (n2 - i) | 1;
            n3 = (n3 + 17) & 0xFE;
            n4 = (n4 ^ i) + 3;
            n5 = (n5 * 3) - i;
            n6 = (n6 | i) + 7;
            n7 = (n7 & 0xF0) ^ i;
            if ((i & 1) == 0) n0 += 1; else n1 += 2;
            if ((i & 2) == 0) n2 += 3; else n3 += 4;
            if ((i & 4) == 0) n4 += 5; else n5 += 6;
            if ((i & 8) == 0) n6 += 7; else n7 += 8;
            if ((i & 16) == 0) n0 -= 1; else n1 -= 2;
            if ((i & 32) == 0) n2 -= 3; else n3 -= 4;
            if ((i & 64) == 0) n4 -= 5; else n5 -= 6;
            if ((i & 128) == 0) n6 -= 7; else n7 -= 8;
        }
        // Never-taken tail guard so the block count stays inflated.
        if ((n0 + n1 + n2 + n3 + n4 + n5 + n6 + n7) == Integer.MIN_VALUE) {
            acc += 1;
        }
        return acc;
    }

    // Straight-line oracles; must equal runCha for a given receiver type.
    static int refSumMono(int N)    { int s = 0; for (int i = 0; i < N; i++) s += i * 3 + 7;    return s; }
    static int refSumTertius(int N) { int s = 0; for (int i = 0; i < N; i++) s += i + 1000;     return s; }

    public static void main(String[] args) throws Exception {
        int N = 32;
        int warmup = 200_000;

        int refM = refSumMono(N);
        int refT = refSumTertius(N);

        // -------- Phase 1: only Mono is loaded. --------
        // Warm up hard enough for tier-6 to fire.  With -YuhuDevirtualize
        // the ONLY devirt available is CHA, and after the inhibit-cast
        // removal improve_virtual_call is allowed to substitute.  Any
        // result mismatch means the substitution either didn't happen or
        // was wrong.
        Base mono = new Mono();
        for (int k = 0; k < warmup; k++) {
            int v = runCha(mono, N);
            if (v != refM) {
                throw new AssertionError("phase1 iter " + k + ": got=" + v + " ref=" + refM);
            }
        }
        System.out.println("PHASE1_OK mono=" + refM);

        // -------- Phase 2: load Tertius. --------
        // Class.forName triggers a resolve at safepoint; HotSpot's
        // dependency tracker sees the new subclass of Base and marks
        // any nmethod whose assert_unique_concrete_method(Base, Mono.compute)
        // is now broken as not_entrant.  The subsequent calls MUST see
        // correct Tertius.compute behaviour via interpreter / vtable,
        // NOT the stale direct Mono.compute call.
        Class<?> cls = Class.forName("com.example.ChaDevirtTest$Tertius");
        Base tertius = (Base) cls.getDeclaredConstructor().newInstance();

        int v0 = runCha(tertius, N);
        if (v0 != refT) {
            throw new AssertionError("phase2 first: got=" + v0 + " ref=" + refT);
        }
        for (int k = 0; k < warmup; k++) {
            int v = runCha(tertius, N);
            if (v != refT) {
                throw new AssertionError("phase2 iter " + k + ": got=" + v + " ref=" + refT);
            }
        }
        // Sanity: after recompile, Mono receiver still dispatches correctly
        // (vtable path now that CHA sees two impls).
        for (int k = 0; k < 1000; k++) {
            int v = runCha(mono, N);
            if (v != refM) {
                throw new AssertionError("phase2 mono iter " + k + ": got=" + v + " ref=" + refM);
            }
        }
        System.out.println("PHASE2_OK tertius=" + refT + " mono=" + refM);

        System.out.println("ChaDevirtTest: ALL OK");
    }
}

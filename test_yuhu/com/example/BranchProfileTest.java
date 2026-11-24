package com.example;

/**
 * Reproducer for Yuhu tier-6 two-way branch profile consumption
 * (Option B: predicate-invert + successor-swap + !prof branch_weights).
 *
 * Design of the two asymmetric target branches (the ones the test cares
 * about) with warmN = 32:
 *   - HOT_TAKEN edge: `if (i < N - 1) { ... } else { ... }`.  For warmN=32
 *     this is taken on i=0..30 (31 iterations) and not-taken on i=31
 *     (1 iteration).  Per-invocation profile: 31 taken / 1 not-taken.
 *     BranchProfile::branch_profile returns have_profile=true, swap=false
 *     (taken_cnt 31 > not_taken_cnt 1).  The !prof metadata attaches with
 *     weights [31, 1]; the emission order is unchanged.
 *   - HOT_NOT_TAKEN edge: `if (i == 0) { ... }`.  For warmN=32 taken on
 *     i=0 only (1 iteration), not-taken on i=1..31 (31 iterations).
 *     Per-invocation profile: 1 taken / 31 not-taken.  swap=true:
 *     predicate inverted (ICMP_EQ -> ICMP_NE) and successors flipped so
 *     the not-taken (hot) block becomes the br's first successor.  !prof
 *     metadata attaches with weights [31, 1] in the new order.
 *
 * The 8 noise `if ((nX & K) ==|!= ...)` branches also emit BranchData
 * rows so they get !prof metadata too, but their ratios hover near 50/50
 * and are not the target of the test; only that the two hot-edge cases
 * fire swap vs no-swap deterministically matters for correctness.
 *
 * Complexity design (identical to DevirtTest/InterfaceDevirtTest): the
 * 8 arithmetic noise variables and the 8 in-loop noise branches inflate
 * code_size * (num_blocks + 1) * has_loops far above the default
 * YuhuComplexityThreshold=5000 so run() reaches tier-6 naturally with
 * the standard 0 -> 3 -> 6 ladder and without any threshold override.
 *
 * What the test verifies:
 *   1. run() reaches tier-6 with -YuhuBranchProbabilities.
 *   2. Dumped LLVM IR for run() at tier-6 contains ZERO
 *      !prof branch_weights nodes in the control run.
 *   3. Dumped LLVM IR for run() at tier-6 contains one or more
 *      !prof branch_weights nodes in the devirt run.
 *   4. The oracle sum is byte-for-byte identical across the two
 *      runs -- branch weights are a codegen hint, not a speculation,
 *      so swapping successor order cannot change results.
 *   5. DeoptimizeALot stress: hint-driven layout survives forced
 *      deopt churn.
 */
public class BranchProfileTest {

    static long run(int N) {
        long acc = 0;
        int n0 = 0, n1 = 1, n2 = 2, n3 = 3;
        int n4 = 4, n5 = 5, n6 = 6, n7 = 7;
        for (int i = 0; i < N; i++) {
            // HOT_TAKEN edge: taken 31/32 at warmN=32.
            if (i < N - 1) {
                acc += 1;
            } else {
                acc += 7;
            }
            // HOT_NOT_TAKEN edge: taken 1/32 at warmN=32 (only i==0).
            if (i == 0) {
                acc += 11;
            }
            // Arithmetic noise (does not touch acc).
            n0 = (n0 ^ (int)acc) + i;
            n1 = (n1 * 3 - n0) ^ (i & 0xFF);
            n2 = (n2 + i) ^ (n1 >>> 1);
            n3 = (n3 ^ n2) + (n0 & n1);
            n4 = (n4 + n3) - (i * 2);
            n5 = (n5 ^ i) + (n4 << 1);
            n6 = (n6 + n5) ^ (n3 & 0x7F);
            n7 = (n7 ^ n6) + (n5 | n4);
            // Branch noise (inflates num_blocks; these have ~50/50 profiles
            // but still get BranchData MDO slices, so they will show up as
            // additional !prof branch_weights metadata lines in the dump).
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

    // Independent straight-line oracle.  The noise variables are not
    // reproduced here because they never write to `acc`; the impossible
    // tail guard (== Integer.MIN_VALUE) is also assumed false at runtime
    // (same reasoning as DevirtTest's noise).
    private static long refRun(int N) {
        long acc = 0;
        for (int i = 0; i < N; i++) {
            acc += (i < N - 1) ? 1 : 7;
            if (i == 0) acc += 11;
        }
        return acc;
    }

    private static void check(String tag, long got, long want) {
        if (got != want) {
            System.out.println("FAIL " + tag + ": got " + got + " want " + want);
            System.exit(1);
        }
    }

    public static void main(String[] args) {
        // Warm run: 200K invocations of run(32).  Tier 3 profiling
        // accumulates: HOT_TAKEN ~6.2M taken / ~200K not-taken;
        // HOT_NOT_TAKEN ~200K taken / ~6.2M not-taken.  Once the method
        // promotes to tier 6 (which requires ~40+ invocations to satisfy
        // is_mature), the branch profile is well-populated.
        final int warmN = 32;
        final int warmIter = 200000;
        long gotWarm = 0;
        long expectWarm = 0;
        for (int w = 0; w < warmIter; w++) {
            gotWarm += run(warmN);
            expectWarm += refRun(warmN);
        }
        check("warmup-aggregate", gotWarm, expectWarm);

        // Post-tier-6 correctness at a bigger N to widen coverage; the
        // profile from warmup is already frozen so the tier-6 code path
        // does the same swap decisions on every call regardless of N.
        int bigN = 100000;
        long big_want = refRun(bigN);
        for (int w = 0; w < 20; w++) {
            check("big#" + w, run(bigN), big_want);
        }

        System.out.println("BranchProfileTest: ALL OK");
    }
}

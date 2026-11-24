package com.example;

/**
 * Reproducer for Yuhu tier-6 multi-way branch profile consumption
 * (Option R2, metadata-only: !prof branch_weights on SwitchInst built
 * from the caller's MultiBranchData).
 *
 * Design constraints that keep the IR assertion measuring ONLY the
 * switch produced by YuhuTopLevelBlock::do_switch:
 *
 *   - Each compiled method contains exactly ONE Java `switch`, and the
 *     switch selector is derived arithmetically (not from a chain of
 *     `if (x == const)`), so LLVM SimplifyCFG's convertIfChainToSwitch
 *     cannot synthesize a *second* switch and steal the metadata check.
 *   - No switch arm falls through (every case ends in `break`), so the
 *     javac bytecode has one distinct target per emitted case and the
 *     skipped-default fold never silently changes the arity.
 *   - A deliberately never-executed row exercises the clamp-to-1 path.
 *
 *   dense()  -- `switch (i % 5)`, cases 0..4 with distinct bodies and
 *               `break`; default body is unreachable (i%5 is always in
 *               0..4) so default_count stays 0 and is clamped to 1.
 *               At warmN=32, per-invocation counts:
 *                 case 0: 7   case 1: 7   case 2: 6
 *                 case 3: 6   case 4: 6   default: 0  (clamped)
 *               weight vector arity = 1 default + 5 cases = 6.
 *
 *   sparse() -- selector `s = (i & 3) * 99 + 1` in {1,100,199,298};
 *               a LOOKUPSWITCH (large gaps).  case 7777 is present in
 *               the bytecode (non-empty body) but never selected, so a
 *               real emitted row carries count 0 (clamped).  default
 *               also unreachable.
 *               At warmN=32, per-invocation: 1:8 100:8 199:8 298:8,
 *               7777:0 (clamp), default:0 (clamp).
 *
 * Post-tier-6 phases use different N to prove a cold/clamped hint never
 * changes results; every value is cross-checked against an independent
 * if/else-chain oracle.
 *
 * Complexity design (identical to BranchProfileTest): 8 arithmetic
 * noise locals + 8 in-loop noise branches push code_size *
 * (num_blocks + 1) * has_loops far above YuhuComplexityThreshold=5000 so
 * both methods reach tier 6 naturally under the 0 -> 3 -> 6 ladder.
 *
 * What the test verifies:
 *   1. dense() and sparse() both compile at tier 6.
 *   2. Their dumped IR contains exactly one switch instruction each.
 *   3. -YuhuSwitchProfile: no !prof on any switch (zero branch_weights).
 *   4. +YuhuSwitchProfile: every switch carries a branch_weights node of
 *      arity (cases + 1), with at least one clamped-to-1 entry.
 *   5. Numeric equality of the oracle sums across the two flag settings.
 *   6. DeoptimizeALot stress.
 */
public class SwitchProfileTest {

    static long dense(int N) {
        long acc = 0;
        int n0 = 0, n1 = 1, n2 = 2, n3 = 3;
        int n4 = 4, n5 = 5, n6 = 6, n7 = 7;
        for (int i = 0; i < N; i++) {
            switch (i % 5) {
                case 0: acc += 3;  break;
                case 1: acc += 5;  break;
                case 2: acc += 7;  break;
                case 3: acc += 11; break;
                case 4: acc += 13; break;
                default: acc += 17; // unreachable: i%5 in [0,4]
            }
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

    static long sparse(int N) {
        long acc = 0;
        int n0 = 0, n1 = 1, n2 = 2, n3 = 3;
        int n4 = 4, n5 = 5, n6 = 6, n7 = 7;
        for (int i = 0; i < N; i++) {
            int s = (i & 3) * 99 + 1;   // {1,100,199,298}
            switch (s) {
                case 1:    acc += 5;  break;
                case 100:  acc += 7;  break;
                case 199:  acc += 9;  break;
                case 298:  acc += 11; break;
                case 7777: acc += 13; break;   // never selected: clamp row
                default:   acc += 2;           // never selected: clamp default
            }
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

    // Independent if/else-chain oracles: same decision semantics without
    // a switch, so a codegen bug in the profile path shows up as a value
    // divergence, not a shared mistake.
    private static long refDense(int N) {
        long acc = 0;
        for (int i = 0; i < N; i++) {
            int m = i % 5;
            if      (m == 0) acc += 3;
            else if (m == 1) acc += 5;
            else if (m == 2) acc += 7;
            else if (m == 3) acc += 11;
            else if (m == 4) acc += 13;
            else               acc += 17;
        }
        if (false) acc += 1;
        return acc;
    }

    private static long refSparse(int N) {
        long acc = 0;
        for (int i = 0; i < N; i++) {
            int s = (i & 3) * 99 + 1;
            if      (s == 1)    acc += 5;
            else if (s == 100)  acc += 7;
            else if (s == 199)  acc += 9;
            else if (s == 298)  acc += 11;
            else if (s == 7777) acc += 13;
            else                acc += 2;
        }
        if (false) acc += 1;
        return acc;
    }

    private static void check(String tag, long got, long want) {
        if (got != want) {
            System.out.println("FAIL " + tag + ": got " + got + " want " + want);
            System.exit(1);
        }
    }

    public static void main(String[] args) {
        // Warm run: 200K invocations at warmN=32 so both MDOs mature
        // with the exact per-case counts documented above frozen in.
        final int warmN = 32;
        final int warmIter = 200000;
        long gotWarmD = 0, wantWarmD = 0, gotWarmS = 0, wantWarmS = 0;
        for (int w = 0; w < warmIter; w++) {
            gotWarmD += dense(warmN);   wantWarmD += refDense(warmN);
            gotWarmS += sparse(warmN);  wantWarmS += refSparse(warmN);
        }
        check("warmup-dense-aggregate", gotWarmD, wantWarmD);
        check("warmup-sparse-aggregate", gotWarmS, wantWarmS);

        // Post-tier-6: larger/different N re-exercise every arm; results
        // must be identical whether the profile hint was applied or not.
        int bigN = 100001;
        long wantBigD = refDense(bigN), wantBigS = refSparse(bigN);
        for (int w = 0; w < 20; w++) {
            check("big-dense#" + w, dense(bigN), wantBigD);
            check("big-sparse#" + w, sparse(bigN), wantBigS);
        }
        int oddN = 99999;
        check("odd-dense", dense(oddN), refDense(oddN));
        check("odd-sparse", sparse(oddN), refSparse(oddN));

        System.out.println("SwitchProfileTest: ALL OK");
    }
}

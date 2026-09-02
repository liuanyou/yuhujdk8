package com.example;

/**
 * Test for Yuhu OSR (On-Stack Replacement) compilation.
 *
 * OSR is triggered by backedge counter overflow in a hot loop.
 * The interpreter detects the overflow, creates an OSR buffer with all live locals,
 * and jumps to the OSR entry point of the compiled method.
 *
 * Tests cover:
 * - Basic int loop with parameter and non-parameter locals
 * - Long/double locals (2-word types, tests slot padding)
 * - Object reference locals (oop transfer through OSR buffer)
 * - Static and non-static methods
 * - Synchronized blocks inside loops (monitor transfer)
 * - Mixed-type locals in a single method
 */
public class OsrTest {

    // =====================================================================
    // Test 1: Basic int loop — parameter + non-parameter locals
    // =====================================================================

    /**
     * Simple loop: parameter 'n' is a parameter local, 'sum'/'i' are non-parameter locals.
     * The loop runs long enough to trigger OSR compilation mid-execution.
     */
    public static long basicIntLoop(int n) {
        long sum = 0;
        int counter = 0;
        for (int i = 0; i < n; i++) {
            sum += i;
            counter++;
        }
        // Verify: sum should be n*(n-1)/2, counter should equal n
        if (counter != n) {
            throw new RuntimeException("basicIntLoop: counter=" + counter + " expected=" + n);
        }
        return sum;
    }

    // =====================================================================
    // Test 2: Long locals — 2-word type, tests slot padding (T_LONG2)
    // =====================================================================

    /**
     * Loop with long arithmetic. On AArch64, long occupies one slot,
     * and the next slot (T_LONG2) is empty padding.
     */
    public static long longLoop(int iterations) {
        long acc = 0L;
        long step = 0x100000001L; // value that uses both halves of long
        for (int i = 0; i < iterations; i++) {
            acc += step;
            step++;
        }
        return acc;
    }

    // =====================================================================
    // Test 3: Double locals — 2-word type, tests T_DOUBLE2 padding
    // =====================================================================

    public static double doubleLoop(int iterations) {
        double acc = 0.0;
        double increment = 1.0 / 3.0;
        for (int i = 0; i < iterations; i++) {
            acc += increment;
        }
        return acc;
    }

    // =====================================================================
    // Test 4: Object reference locals — oop transfer through OSR buffer
    // =====================================================================

    /**
     * Loop that uses a StringBuilder periodically (object local).
     * Tests that oop references survive OSR transition.
     * We reset the builder periodically to avoid OOM.
     */
    public static int objectLoop(int iterations) {
        StringBuilder sb = new StringBuilder();
        String prefix = "item";
        int totalLen = 0;
        for (int i = 0; i < iterations; i++) {
            sb.append(prefix).append(i).append(",");
            // Reset periodically to avoid OOM, but keep accumulating length
            if (i % 10000 == 9999) {
                totalLen += sb.length();
                sb.setLength(0);
            }
        }
        totalLen += sb.length();
        if (totalLen == 0) {
            throw new RuntimeException("objectLoop: totalLen is 0");
        }
        return totalLen;
    }

    // =====================================================================
    // Test 5: Non-static method — tests 'this' as local[0] in OSR buffer
    // =====================================================================

    private int instanceField;

    public OsrTest(int initialValue) {
        this.instanceField = initialValue;
    }

    public long instanceLoop(int iterations) {
        long sum = instanceField; // capture 'this' into computation
        long multiplier = 2;
        for (int i = 0; i < iterations; i++) {
            sum += (long) i * multiplier;
            multiplier++;
        }
        return sum;
    }

    // =====================================================================
    // Test 6: Synchronized block inside loop — monitor transfer
    // =====================================================================

    /**
     * Loop with synchronized block. Tests that monitors are correctly
     * transferred through the OSR buffer (displaced header + obj pair).
     */
    public static long synchronizedLoop(int iterations) {
        Object lock = new Object();
        long sum = 0;
        for (int i = 0; i < iterations; i++) {
            synchronized (lock) {
                sum += i;
            }
        }
        return sum;
    }

    // =====================================================================
    // Test 7: Mixed types — int + long + double + Object in one loop
    // =====================================================================

    /**
     * Loop with mixed-type locals. Tests that the OSR buffer correctly
     * handles interleaved 1-word and 2-word slots.
     *
     * Layout on AArch64:
     *   local[0] = iterations (int, 1 word)
     *   local[1] = lock (Object, 1 word)
     *   local[2] = longAcc (long, 1 word)
     *   local[3] = <T_LONG2 padding, empty>
     *   local[4] = doubleAcc (double, 1 word)
     *   local[5] = <T_DOUBLE2 padding, empty>
     *   local[6] = intAcc (int, 1 word)
     *   local[7] = sb (Object/StringBuilder, 1 word)
     */
    public static String mixedTypeLoop(int iterations) {
        Object lock = new Object();
        long longAcc = 0L;
        double doubleAcc = 0.0;
        int intAcc = 0;
        StringBuilder sb = new StringBuilder();

        for (int i = 0; i < iterations; i++) {
            intAcc += i;
            longAcc += (long) i * 1000L;
            doubleAcc += (double) i * 0.5;
            synchronized (lock) {
                sb.append(i).append(";");
                // Reset periodically to avoid OOM
                if (i % 10000 == 9999) {
                    sb.setLength(0);
                }
            }
        }

        String result = "int=" + intAcc + " long=" + longAcc
                      + " double=" + doubleAcc + " sb_len=" + sb.length();
        return result;
    }

    // =====================================================================
    // Test 8: Nested loops — outer loop triggers OSR, inner loop runs fast
    // =====================================================================

    public static long nestedLoop(int outer, int inner) {
        long total = 0;
        for (int i = 0; i < outer; i++) {
            long partial = 0;
            for (int j = 0; j < inner; j++) {
                partial += j;
            }
            total += partial;
        }
        return total;
    }

    // =====================================================================
    // Test 9: Loop with conditional branches — tests OSR at loop header
    //         with non-trivial control flow inside the loop
    // =====================================================================

    public static long branchyLoop(int iterations) {
        long sum = 0;
        int evenCount = 0;
        int oddCount = 0;
        for (int i = 0; i < iterations; i++) {
            if (i % 2 == 0) {
                sum += i;
                evenCount++;
            } else {
                sum -= i;
                oddCount++;
            }
        }
        if (evenCount + oddCount != iterations) {
            throw new RuntimeException("branchyLoop: count mismatch");
        }
        return sum;
    }

    // =====================================================================
    // Verification helpers
    // =====================================================================

    static void check(String name, long actual, long expected) {
        if (actual != expected) {
            throw new RuntimeException(name + ": actual=" + actual + " expected=" + expected);
        }
        System.out.println("  PASS " + name + " = " + actual);
    }

    static void checkApprox(String name, double actual, double expected, double tolerance) {
        if (Math.abs(actual - expected) > tolerance) {
            throw new RuntimeException(name + ": actual=" + actual + " expected=" + expected);
        }
        System.out.println("  PASS " + name + " = " + actual);
    }

    // =====================================================================
    // Main
    // =====================================================================

    public static void main(String[] args) throws Exception {
        System.out.println("=== Yuhu OSR Compilation Test ===");
        System.out.println();

        // Iteration count: high enough to trigger backedge counter overflow
        // and OSR compilation mid-loop.
        final int HOT = 50_000_000;

//         // --- Test 1: Basic int loop ---
//         System.out.println("--- Test 1: basicIntLoop ---");
//         {
//             int n = 1000;
//             long expected = (long) n * (n - 1) / 2;
//             long result = basicIntLoop(n);
//             check("basicIntLoop(1000)", result, expected);
//         }
//         {
//             long result = basicIntLoop(HOT);
//             long expected = (long) HOT * (HOT - 1) / 2;
//             check("basicIntLoop(HOT)", result, expected);
//         }

//         // --- Test 2: Long loop ---
//         System.out.println("--- Test 2: longLoop ---");
//         {
//             long result = longLoop(HOT);
//             // step starts at 0x100000001, increments each iteration
//             // acc = sum of (0x100000001 + i) for i in [0, HOT)
//             //     = HOT * 0x100000001 + HOT*(HOT-1)/2
//             long expected = (long) HOT * 0x100000001L + (long) HOT * (HOT - 1) / 2;
//             check("longLoop(HOT)", result, expected);
//         }

//         // --- Test 3: Double loop ---
//         System.out.println("--- Test 3: doubleLoop ---");
//         {
//             double result = doubleLoop(HOT);
//             double expected = HOT * (1.0 / 3.0);
//             checkApprox("doubleLoop(HOT)", result, expected, 1.0);
//         }

//         // --- Test 4: Object loop ---
//         System.out.println("--- Test 4: objectLoop ---");
//         {
//             int result = objectLoop(100);
//             if (result == 0) {
//                 throw new RuntimeException("objectLoop(100): result is 0");
//             }
//             System.out.println("  PASS objectLoop(100) totalLen=" + result);
//         }
//         {
//             int result = objectLoop(HOT);
//             System.out.println("  PASS objectLoop(HOT) totalLen=" + result);
//         }

//         // --- Test 5: Non-static instance loop ---
//         System.out.println("--- Test 5: instanceLoop ---");
//         {
//             OsrTest obj = new OsrTest(100);
//             long result = obj.instanceLoop(HOT);
//             // sum = 100 + sum(i*2 + i*(i-1)/2 ... ) — compute expected
//             long expected = 100;
//             int multiplier = 2;
//             for (int i = 0; i < HOT; i++) {
//                 expected += (long) i * multiplier;
//                 multiplier++;
//             }
//             check("instanceLoop(HOT)", result, expected);
//         }

//         // --- Test 6: Synchronized loop ---
//         System.out.println("--- Test 6: synchronizedLoop ---");
//         {
//             long result = synchronizedLoop(HOT);
//             long expected = (long) HOT * (HOT - 1) / 2;
//             check("synchronizedLoop(HOT)", result, expected);
//         }

//         // --- Test 7: Mixed type loop ---
//         System.out.println("--- Test 7: mixedTypeLoop ---");
//         {
//             String result = mixedTypeLoop(HOT);
//             System.out.println("  PASS mixedTypeLoop(HOT): " + result);
//         }

//         // --- Test 8: Nested loop ---
//         System.out.println("--- Test 8: nestedLoop ---");
//         {
//             int outer = 1000;
//             int inner = 1000;
//             long result = nestedLoop(outer, inner);
//             // inner sum = inner*(inner-1)/2
//             // total = outer * inner*(inner-1)/2
//             long expected = (long) outer * inner * (inner - 1) / 2;
//             check("nestedLoop(1000,1000)", result, expected);
//         }

        // --- Test 9: Branchy loop ---
        System.out.println("--- Test 9: branchyLoop ---");
        {
            long result = branchyLoop(HOT);
            // even: 0+2+4+... = sum of even i
            // odd:  -(1+3+5+...) = negative sum of odd i
            // result = sum_even - sum_odd
            long sumEven = 0, sumOdd = 0;
            for (int i = 0; i < HOT; i++) {
                if (i % 2 == 0) sumEven += i;
                else sumOdd += i;
            }
            long expected = sumEven - sumOdd;
            check("branchyLoop(HOT)", result, expected);
        }

//         System.out.println();
//         System.out.println("=== All OSR Tests Passed ===");
    }
}

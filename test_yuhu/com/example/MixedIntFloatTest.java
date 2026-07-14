package com.example;

/**
 * Test class for verifying Yuhu compiler handles methods with mixed int and float/double parameters.
 *
 * Method 1 (largeFrameMixed): Large stack frame with both float and non-float parameters.
 *   - Tests different prologue patterns (pre-indexed vs unsigned offset stp).
 *   - Many local variables force a large frame allocation.
 *
 * Method 2 (overflowRegisters): 9 float/double + 9 int parameters.
 *   - Exceeds both the 8 FP register limit (v0-v7) and 8 GP register limit (x1-x7,x0).
 *   - Forces both FP and GP arguments onto the stack.
 */
public class MixedIntFloatTest {

    /**
     * Method with large stack space and mixed float/non-float parameters.
     * Many local variables force a large frame, triggering different prologue patterns.
     *
     * Parameters: 4 int + 4 double (all fit in registers)
     * Locals: ~20 local variables to force a large stack frame
     */
    public static double largeFrameMixed(int a, double b, int c, double d,
                                          float e, int f, double g, float h) {
        // Force a large stack frame with many locals
        double s1 = a + b;
        double s2 = c + d;
        double s3 = e + f;
        double s4 = g + h;
        double s5 = s1 * s2;
        double s6 = s3 * s4;
        double s7 = s5 + s6;
        int i1 = (int)(s1 + s2);
        int i2 = (int)(s3 + s4);
        int i3 = i1 * i2;
        float f1 = (float)(s5 - s6);
        float f2 = (float)(s7);
        double s8 = i3 + f1 + f2;
        double s9 = s8 * 0.5;
        double s10 = s9 + a * 0.1;
        double s11 = s10 - b * 0.2;
        double s12 = s11 + c * 0.3;
        double s13 = s12 * d;
        double s14 = s13 + e * 0.4;
        double s15 = s14 - f * 0.5;
        double s16 = s15 + g * 0.6;
        double result = s16 + h * 0.7;

        // Loop to trigger compilation
        for (int i = 0; i < 20; i++) {
            result += s1 * 0.01 + s2 * 0.02 + s3 * 0.03 + s4 * 0.04;
            result -= i1 * 0.001 + i2 * 0.002;
        }
        return result;
    }

    /**
     * Method with 9 float/double parameters and 9 int parameters.
     * Exceeds both register pools:
     *   - 9 ints: 8 in GP registers (x1-x7,x0), 9th on stack
     *   - 9 doubles: 8 in FP registers (v0-v7), 9th on stack
     *
     * Total: 18 parameters, 2 values on stack.
     */
    public static double overflowRegisters(int i1, double d1, int i2, double d2,
                                            int i3, double d3, int i4, double d4,
                                            int i5, double d5, int i6, double d6,
                                            int i7, double d7, int i8, double d8,
                                            int i9, double d9) {
        // Sum all integer parameters
        int intSum = i1 + i2 + i3 + i4 + i5 + i6 + i7 + i8 + i9;

        // Sum all double parameters
        double dblSum = d1 + d2 + d3 + d4 + d5 + d6 + d7 + d8 + d9;

        // Combine them
        double result = intSum * 0.1 + dblSum;

        // Loop to trigger compilation
        for (int iter = 0; iter < 20; iter++) {
            result += intSum * 0.001;
            result -= dblSum * 0.002;
            result += i1 * 0.1 + i2 * 0.2 + i3 * 0.3 + i4 * 0.4;
            result += d1 * 0.01 + d2 * 0.02 + d3 * 0.03 + d4 * 0.04;
        }
        return result;
    }

    /**
     * Main method to run the test.
     */
    public static void main(String[] args) {
        // Test large frame with mixed parameters
        double largeResult = 0;
        for (int i = 0; i < 1000000; i++) {
            largeResult = largeFrameMixed(10, 20.5, 30, 40.5, 1.5f, 50, 60.5, 2.5f);
        }
        System.out.println("largeFrameMixed result: " + largeResult);

        // Test register overflow with 9 int + 9 double
        double overflowResult = 0;
        for (int i = 0; i < 1000000; i++) {
            overflowResult = overflowRegisters(
                1, 1.0, 2, 2.0, 3, 3.0, 4, 4.0, 5, 5.0,
                6, 6.0, 7, 7.0, 8, 8.0, 9, 9.0);
        }
        System.out.println("overflowRegisters result: " + overflowResult);

        // Sanity checks
        boolean passed = true;
        if (Double.isNaN(largeResult) || Double.isInfinite(largeResult)) {
            System.out.println("FAIL: largeFrameMixed returned NaN or Infinity");
            passed = false;
        }
        if (Double.isNaN(overflowResult) || Double.isInfinite(overflowResult)) {
            System.out.println("FAIL: overflowRegisters returned NaN or Infinity");
            passed = false;
        }
        // overflowRegisters: intSum=45, dblSum=45.0, initial result = 45*0.1 + 45.0 = 49.5
        if (overflowResult < 40.0 || overflowResult > 60.0) {
            System.out.println("FAIL: overflowRegisters result out of range: " + overflowResult);
            passed = false;
        }

        if (passed) {
            System.out.println("SUCCESS: All mixed int/float tests passed!");
        } else {
            System.out.println("FAILURE: Some tests failed!");
            System.exit(1);
        }
    }
}

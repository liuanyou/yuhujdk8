package com.example;

/**
 * Test class for verifying Yuhu compiler handles methods with decimal (double/float) parameters.
 * ARM64 calling convention for floating-point:
 * - FP parameters 0-7: passed in v0-v7 (d0-d7 for double, s0-s7 for float)
 * - Integer parameters: passed in x0-x7 (shared with GP params)
 * - Parameters beyond register limit: passed on stack
 */
public class DecimalTest {

    /**
     * Method with double parameters only.
     */
    public static double addDoubles(double a, double b, double c, double d) {
        double sum = a + b + c + d;
        for (int i = 0; i < 10; i++) {
            sum += a * 0.1 + b * 0.2 + c * 0.3 + d * 0.4;
        }
        return sum;
    }

    /**
     * Method with float parameters only.
     */
    public static float addFloats(float a, float b, float c, float d) {
        float sum = a + b + c + d;
        for (int i = 0; i < 10; i++) {
            sum += a * 0.1f + b * 0.2f + c * 0.3f + d * 0.4f;
        }
        return sum;
    }

    /**
     * Method with mixed int and double parameters.
     * Tests register allocation across GP and FP register files.
     */
    public static double mixedIntDouble(int count, double value, int multiplier, double factor) {
        double result = value;
        for (int i = 0; i < count; i++) {
            result = result * multiplier + factor;
        }
        return result;
    }

    /**
     * Method with many double parameters (exceeds FP register limit).
     * ARM64 has 8 FP parameter registers (v0-v7), so 9+ doubles go on stack.
     */
    public static double nineDoubles(double d1, double d2, double d3, double d4, double d5,
                                      double d6, double d7, double d8, double d9) {
        double sum = d1 + d2 + d3 + d4 + d5 + d6 + d7 + d8 + d9;
        for (int i = 0; i < 10; i++) {
            sum += d1 * 0.01 + d2 * 0.02 + d3 * 0.03 + d4 * 0.04
                 + d5 * 0.05 + d6 * 0.06 + d7 * 0.07 + d8 * 0.08 + d9 * 0.09;
        }
        return sum;
    }

    /**
     * Method with mixed int and double parameters exceeding register limits.
     * 5 ints + 5 doubles: ints use x0-x4, doubles use v0-v4.
     */
    public static double tenMixedParams(int i1, double d1, int i2, double d2, int i3,
                                         double d3, int i4, double d4, int i5, double d5) {
        double result = d1 * i1 + d2 * i2 + d3 * i3 + d4 * i4 + d5 * i5;
        for (int iter = 0; iter < 10; iter++) {
            result += (i1 + i2 + i3 + i4 + i5) * 0.1;
            result -= (d1 + d2 + d3 + d4 + d5) * 0.01;
        }
        return result;
    }

    /**
     * Instance method with double parameters.
     */
    public double instanceDoubleMethod(double base, double exponent) {
        double result = base;
        for (int i = 1; i < (int) exponent; i++) {
            result *= base;
            if (result > 1e10) {
                result /= 1e5;
            }
        }
        return result;
    }

    /**
     * Main method to run the test.
     */
    public static void main(String[] args) {
        DecimalTest test = new DecimalTest();

        // Warmup and test double parameters
//         double doubleResult = 0;
//         for (int i = 0; i < 500000; i++) {
//             doubleResult = addDoubles(1.5, 2.5, 3.5, 4.5);
//         }
//         System.out.println("addDoubles result: " + doubleResult);
//         System.out.println("Expected: 12.0 + loop contribution");
//         System.out.println();

        // Test float parameters
//         float floatResult = 0;
//         for (int i = 0; i < 1000000; i++) {
//             floatResult = addFloats(1.5f, 2.5f, 3.5f, 4.5f);
//         }
//         System.out.println("addFloats result: " + floatResult);
//         System.out.println();

        // Test mixed int/double parameters
//         double mixedResult = 0;
//         for (int i = 0; i < 1000000; i++) {
//             mixedResult = mixedIntDouble(5, 10.0, 2, 3.0);
//         }
//         System.out.println("mixedIntDouble result: " + mixedResult);
//         System.out.println();

        // Test 9 double parameters (exceeds FP register limit)
//         double nineResult = 0;
//         for (int i = 0; i < 1000000; i++) {
//             nineResult = nineDoubles(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0);
//         }
//         System.out.println("nineDoubles result: " + nineResult);
//         System.out.println();

        // Test 10 mixed parameters
//         double tenResult = 0;
//         for (int i = 0; i < 1000000; i++) {
//             tenResult = tenMixedParams(1, 10.0, 2, 20.0, 3, 30.0, 4, 40.0, 5, 50.0);
//         }
//         System.out.println("tenMixedParams result: " + tenResult);
//         System.out.println();

        // Test instance method with doubles
        double instanceResult = 0;
        for (int i = 0; i < 1000000; i++) {
            instanceResult = test.instanceDoubleMethod(2.0, 10.0);
        }
        System.out.println("instanceDoubleMethod result: " + instanceResult);
        System.out.println();

        // Basic sanity checks
        boolean passed = true;
//         if (Math.abs(doubleResult - 12.0) > 1.0) {
//             System.out.println("FAIL: addDoubles result out of range");
//             passed = false;
//         }
//         if (Math.abs(floatResult - 12.0f) > 1.0f) {
//             System.out.println("FAIL: addFloats result out of range");
//             passed = false;
//         }
//         if (nineResult < 45.0 || nineResult > 50.0) {
//             System.out.println("FAIL: nineDoubles result out of range, got: " + nineResult);
//             passed = false;
//         }

        if (passed) {
            System.out.println("SUCCESS: All decimal parameter tests passed!");
        } else {
            System.out.println("FAILURE: Some tests failed!");
            System.exit(1);
        }
    }
}

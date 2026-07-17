package com.example;

/**
 * Test class for verifying Yuhu compiler handles methods with more than 7 parameters.
 * ARM64 calling convention:
 * - Parameters 0-7: passed in registers x0-x7
 * - Parameters 8+: passed on stack
 */
public class NineParameterTest {
    
    /**
     * Method with 9 parameters to test parameter passing beyond register limit.
     * 
     * @param p1 parameter 0 (will be in x1 for non-static, or x2 for static)
     * @param p2 parameter 1 (x2/x3)
     * @param p3 parameter 2 (x3/x4)
     * @param p4 parameter 3 (x4/x5)
     * @param p5 parameter 4 (x5/x6)
     * @param p6 parameter 5 (x6/x7)
     * @param p7 parameter 6 (x7/stack)
     * @param p8 parameter 7 (stack - first one on stack for non-static)
     * @param p9 parameter 8 (stack - second one on stack)
     * @return sum of all parameters
     */
    public int testNineParameters(int p1, int p2, int p3, int p4, 
                                   int p5, int p6, int p7, int p8, int p9) {
        System.out.println("=== Nine Parameter Test ===");
        System.out.println("p1 = " + p1);
        System.out.println("p2 = " + p2);
        System.out.println("p3 = " + p3);
        System.out.println("p4 = " + p4);
        System.out.println("p5 = " + p5);
        System.out.println("p6 = " + p6);
        System.out.println("p7 = " + p7);
        System.out.println("p8 = " + p8);
        System.out.println("p9 = " + p9);
        
        // Add a loop to make the method complex enough for Yuhu compilation
        int sum = 0;
        for (int i = 0; i < 10; i++) {
            sum += p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
            if (sum > 100) {
                sum -= 50;
            }
        }
        
        System.out.println("Sum = " + sum);
        System.out.println("===========================");
        
        return sum;
    }
    
    /**
     * Static method with 9 parameters for comparison.
     */
    public static int testNineParametersStatic(int p1, int p2, int p3, int p4, 
                                                int p5, int p6, int p7, int p8, int p9) {
//         System.out.println("=== Nine Parameter Test (Static) ===");
//         System.out.println("p1 = " + p1);
//         System.out.println("p2 = " + p2);
//         System.out.println("p3 = " + p3);
//         System.out.println("p4 = " + p4);
//         System.out.println("p5 = " + p5);
//         System.out.println("p6 = " + p6);
//         System.out.println("p7 = " + p7);
//         System.out.println("p8 = " + p8);
//         System.out.println("p9 = " + p9);
        
        // Add a loop to make the method complex enough for Yuhu compilation
        int sum = 0;
        for (int i = 0; i < 10; i++) {
            sum += p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
            if (sum > 500) {
                sum -= 250;
            }
        }
        
//         System.out.println("Sum = " + sum);
//         System.out.println("====================================");
        
        return sum;
    }

    public int testNineParamsCaller(boolean useCallee, int p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8, int p9) {
        int sum = p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
        if (useCallee) {
            return testNineParamsCallee(p1, p2, p3, p4, p5, p6, p7, p8, p9);
        }
        return sum;
    }

    public int testNineParamsCallee(int p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8, int p9) {
        int sum = 0;
        for (int i = 0; i < 10; i++) {
            sum += p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
            if (sum > 100) {
                sum -= 50;
            }
        }
        return sum;
    }

    public int testNineParamsCaller4(boolean useCallee, int p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8, int p9, Object obj4) {
        int sum = p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
        if (useCallee) {
            return testNineParamsCallee4(p1, p2, p3, p4, p5, p6, p7, p8, p9, obj4);
        }
        return sum;
    }

    public int testNineParamsCallee4(int p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8, int p9, Object obj4) {
        int sum = (obj4 != null ? 0 : -1);
        for (int i = 0; i < 10; i++) {
            sum += p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
            if (sum > 100) {
                sum -= 50;
            }
        }
        return sum;
    }

    public int testNineParamsCaller5(boolean useCallee, int p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8, int p9, Object obj5) {
        int sum = p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
        if (useCallee) {
            return testNineParamsCallee5(p1, p2, p3, p4, p5, p6, p7, p8, p9, obj5);
        }
        return sum;
    }

    public int testNineParamsCallee5(int p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8, int p9, Object obj5) {
        int sum = (obj5 != null ? 0 : -1);
        for (int i = 0; i < 10; i++) {
            sum += p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
            if (sum > 100) {
                sum -= 50;
            }
        }
        return sum;
    }

    public int testNineParamsCaller6(boolean useCallee, int p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8, int p9, Object obj6) {
        int sum = p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
        if (useCallee) {
            return testNineParamsCallee6(p1, p2, p3, p4, p5, p6, p7, p8, p9, obj6);
        }
        return sum;
    }

    public int testNineParamsCallee6(int p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8, int p9, Object obj6) {
        int sum = (obj6 != null ? 0 : -1);
        for (int i = 0; i < 10; i++) {
            sum += p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
            if (sum > 100) {
                sum -= 50;
            }
        }
        return sum;
    }

    public int testNineParamsCaller7(boolean useCallee, int p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8, int p9, Object obj7,
                                    boolean b1, byte b2, short b3, long l,
                                    float s1, float s2, float s3, float s4, float s5, float s6, float s7, float s8, float s9,
                                    double d1) {
        int sum = p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
        if (useCallee) {
            return testNineParamsCallee7(p1, p2, p3, p4, p5, p6, p7, p8, p9, obj7, b1, b2, b3, l, s1, s2, s3, s4, s5, s6, s7, s8, s9, d1);
        }
        return sum;
    }

    public int testNineParamsCallee7(int p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8, int p9, Object obj7,
                                     boolean b1, byte b2, short b3, long l,
                                     float s1, float s2, float s3, float s4, float s5, float s6, float s7, float s8, float s9,
                                     double d1) {
        int sum = (obj7 != null ? 0 : -1);
        for (int i = 0; i < 10; i++) {
            sum += p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
            if (sum > 100) {
                sum -= 50;
            }
        }
        System.out.println("p1=" + p1);
        System.out.println("p2=" + p2);
        System.out.println("p3=" + p3);
        System.out.println("p4=" + p4);
        System.out.println("p5=" + p5);
        System.out.println("p6=" + p6);
        System.out.println("p7=" + p7);
        System.out.println("p8=" + p8);
        System.out.println("p9=" + p9);
        System.out.println("obj7=" + obj7);
        System.out.println("b1=" + b1);
        System.out.println("b2=" + b2);
        System.out.println("b3=" + b3);
        System.out.println("l=" + l);
        System.out.println("s1=" + s1);
        System.out.println("s2=" + s2);
        System.out.println("s3=" + s3);
        System.out.println("s4=" + s4);
        System.out.println("s5=" + s5);
        System.out.println("s6=" + s6);
        System.out.println("s7=" + s7);
        System.out.println("s8=" + s8);
        System.out.println("s9=" + s9);
        System.out.println("d1=" + d1);
        return sum;
    }

    public int testNineParamsCaller8(boolean useCallee,
                                    float s1, float s2, float s3, float s4, float s5, float s6, float s7, float s8, float s9,
                                    double d1,
                                    int p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8, int p9, Object obj8,
                                    boolean b1, byte b2, short b3, long l) {
        int sum = p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
        if (useCallee) {
            return testNineParamsCallee8(s1, s2, s3, s4, s5, s6, s7, s8, s9, d1, p1, p2, p3, p4, p5, p6, p7, p8, p9, obj8, b1, b2, b3, l);
        }
        return sum;
    }

    public int testNineParamsCallee8(float s1, float s2, float s3, float s4, float s5, float s6, float s7, float s8, float s9,
                                    double d1,
                                    int p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8, int p9, Object obj8,
                                    boolean b1, byte b2, short b3, long l) {
        int sum = (obj8 != null ? 0 : -1);
        for (int i = 0; i < 10; i++) {
            sum += p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
            if (sum > 100) {
                sum -= 50;
            }
        }
        System.out.println("p1=" + p1);
        System.out.println("p2=" + p2);
        System.out.println("p3=" + p3);
        System.out.println("p4=" + p4);
        System.out.println("p5=" + p5);
        System.out.println("p6=" + p6);
        System.out.println("p7=" + p7);
        System.out.println("p8=" + p8);
        System.out.println("p9=" + p9);
        System.out.println("obj8=" + obj8);
        System.out.println("b1=" + b1);
        System.out.println("b2=" + b2);
        System.out.println("b3=" + b3);
        System.out.println("l=" + l);
        System.out.println("s1=" + s1);
        System.out.println("s2=" + s2);
        System.out.println("s3=" + s3);
        System.out.println("s4=" + s4);
        System.out.println("s5=" + s5);
        System.out.println("s6=" + s6);
        System.out.println("s7=" + s7);
        System.out.println("s8=" + s8);
        System.out.println("s9=" + s9);
        System.out.println("d1=" + d1);
        return sum;
    }

    /**
     * Main method to run the test.
     */
    public static void main(String[] args) {
        NineParameterTest test = new NineParameterTest();

        // 预热阶段
//         for (int i = 0; i < 100000; i++) {
//             test.testNineParameters(i, 2, 3, 4, 5, 6, 7, 8, 9);
// //             testNineParametersStatic(i, 20, 30, 40, 50, 60, 70, 80, 90);
//         }
//
//         try {
//             Thread.sleep(60000);
//         } catch(Exception e) {
//         }
        
        // Test instance method
//         int result1 = 0;
//         for (int i = 0; i < 100000; i++) {
//             result1 = test.testNineParameters(i, 2, 3, 4, 5, 6, 7, 8, 9);
//         }
// //         result1 = test.testNineParameters(99999, 2, 3, 4, 5, 6, 7, 8, 9);
//         System.out.println("Instance method result: " + result1);
//         System.out.println("Expected: 45");
//         System.out.println();
//
//         // Test static method
//         int result2 = 0;
//         for (int i = 0; i < 100000; i++) {
//             result2 = testNineParametersStatic(i, 20, 30, 40, 50, 60, 70, 80, 90);
//         }
//         System.out.println("Static method result: " + result2);
//         System.out.println("Expected: 450");
//         System.out.println();
//
//         // Verify results (adjusted for loop computation)
//         if (result1 == 400 && result2 == 4000) {
//             System.out.println("SUCCESS: All tests passed!");
//         } else {
//             System.out.println("FAILURE: Results do not match expected values!");
//             System.out.println("Expected result1=400, got: " + result1);
//             System.out.println("Expected result2=4000, got: " + result2);
//             System.exit(1);
//         }

//         for (int i = 0; i < 100000; i++) {
//             test.testNineParamsCaller(false, i, 2, 3, 4, 5, 6, 7, 8, 9);
//             test.testNineParamsCallee(i, 2, 3, 4, 5, 6, 7, 8, 9);
//         }
//
//         try {
//             Thread.sleep(15000);
//         } catch(Exception e) {
//
//         }
//
//         try {
//             Thread.sleep(15000);
//         } catch(Exception e) {
//
//         }
//
//         int result3 = test.testNineParamsCaller(true, 10000, 2, 3, 4, 5, 6, 7, 8, 9);
//         System.out.println("result3: " + result3);

//         Object obj4 = new Object();
//         for (int i = 0; i < 100000; i++) {
//             test.testNineParamsCaller4(false, i, 2, 3, 4, 5, 6, 7, 8, 9, obj4);
//         }
//
//         try {
//             Thread.sleep(15000);
//         } catch(Exception e) {
//
//         }
//
//         try {
//             Thread.sleep(15000);
//         } catch(Exception e) {
//
//         }
//
//         int result4 = test.testNineParamsCaller4(true, 10000, 2, 3, 4, 5, 6, 7, 8, 9, obj4);
//         assert(result4 == 99940);
//         System.out.println("result4: " + result4);

//         Object obj5 = new Object();
//         for (int i = 0; i < 100000; i++) {
//             test.testNineParamsCaller5(false, i, 2, 3, 4, 5, 6, 7, 8, 9, obj5);
//             test.testNineParamsCallee5(i, 2, 3, 4, 5, 6, 7, 8, 9, obj5);
//         }
//
//         try {
//             Thread.sleep(15000);
//         } catch(Exception e) {
//
//         }
//
//         try {
//             Thread.sleep(15000);
//         } catch(Exception e) {
//
//         }
//
//         int result5 = test.testNineParamsCaller5(true, 10000, 2, 3, 4, 5, 6, 7, 8, 9, obj5);
//         assert(result5 == 99940);
//         System.out.println("result5: " + result5);

//         Object obj6 = new Object();
//         for (int i = 0; i < 1000000; i++) {
//             test.testNineParamsCaller6(false, i, 2, 3, 4, 5, 6, 7, 8, 9, obj6);
//             test.testNineParamsCallee6(i, 2, 3, 4, 5, 6, 7, 8, 9, obj6);
//         }
//
//         try {
//             Thread.sleep(15000);
//         } catch(Exception e) {
//
//         }
//
//         try {
//             Thread.sleep(15000);
//         } catch(Exception e) {
//
//         }
//
//         int result6 = test.testNineParamsCaller6(true, 10000, 2, 3, 4, 5, 6, 7, 8, 9, obj6);
//         assert(result6 == 99940);
//         System.out.println("result6: " + result6);

//         Object obj7 = new Object();
//         for (int i = 0; i < 1000000; i++) {
//             test.testNineParamsCaller7(false, 10000, 2, 3, 4, 5, 6, 7, 8, 9, obj7,
//                                        true, (byte)1, (short)2, 4l,
//                                        1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f,
//                                        10.0d);
// //             test.testNineParamsCallee7(i, 2, 3, 4, 5, 6, 7, 8, 9, obj7);
//         }
//
//         try {
//             Thread.sleep(15000);
//         } catch(Exception e) {
//
//         }
//
//         try {
//             Thread.sleep(15000);
//         } catch(Exception e) {
//
//         }
//
//         int result7 = test.testNineParamsCaller7(true, 10000, 2, 3, 4, 5, 6, 7, 8, 9, obj7,
//                                                 true, (byte)1, (short)2, 4l,
//                                                 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f,
//                                                 10.0d);
//         assert(result7 == 99940);
//         System.out.println("result7: " + result7);

        Object obj8 = new Object();
        for (int i = 0; i < 1000000; i++) {
            test.testNineParamsCaller8(false, 1.1f, 2.2f, 3.3f, 4.4f, 5.5f, 6.6f, 7.7f, 8.8f, 9.9f, 10.10d,
                                        10000, 2, 3, 4, 5, 6, 7, 8, 9, obj8,
                                       true, (byte)1, (short)2, 4l);
        }

        try {
            Thread.sleep(15000);
        } catch(Exception e) {

        }

        try {
            Thread.sleep(15000);
        } catch(Exception e) {

        }

        int result8 = test.testNineParamsCaller8(true, 1.1f, 2.2f, 3.3f, 4.4f, 5.5f, 6.6f, 7.7f, 8.8f, 9.9f, 10.10d,
                                                10000, 2, 3, 4, 5, 6, 7, 8, 9, obj8,
                                                true, (byte)1, (short)2, 4l);
        assert(result8 == 99940);
        System.out.println("result8: " + result8);
    }
}

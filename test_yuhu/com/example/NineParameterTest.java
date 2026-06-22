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
        int result1 = 0;
        for (int i = 0; i < 100000; i++) {
            result1 = test.testNineParameters(i, 2, 3, 4, 5, 6, 7, 8, 9);
        }
//         result1 = test.testNineParameters(99999, 2, 3, 4, 5, 6, 7, 8, 9);
        System.out.println("Instance method result: " + result1);
        System.out.println("Expected: 45");
        System.out.println();
        
        // Test static method
        int result2 = 0;
//         for (int i = 0; i < 100000; i++) {
//             result2 = testNineParametersStatic(i, 20, 30, 40, 50, 60, 70, 80, 90);
//         }
        System.out.println("Static method result: " + result2);
        System.out.println("Expected: 450");
        System.out.println();
        
        // Verify results (adjusted for loop computation)
        if (result1 == 400 && result2 == 4000) {
            System.out.println("SUCCESS: All tests passed!");
        } else {
            System.out.println("FAILURE: Results do not match expected values!");
            System.out.println("Expected result1=400, got: " + result1);
            System.out.println("Expected result2=4000, got: " + result2);
            System.exit(1);
        }
    }
}

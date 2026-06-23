package com.example;

/**
 * Test Yuhu exception table handling (try-catch).
 * 
 * Covers:
 * 1. Simple try-catch
 * 2. Multiple catch blocks
 * 3. Nested try-catch
 * 4. Catch-all (finally not supported in same way, but catch Exception works)
 * 5. Exception re-throw
 * 6. Deoptimization through exception handler
 */
public class ExceptionTableTest {

    // 1. Simple try-catch
    public static int simpleTryCatch() {
        try {
            int x = 10 / 0; // Will throw ArithmeticException
            return x;
        } catch (ArithmeticException e) {
            return -1;
        }
    }

    // 2. Multiple catch blocks
    public static String multipleCatch(int type) {
        try {
            if (type % 3 == 0) {
                throw new ArithmeticException("div by zero");
            } else if (type % 3 == 1) {
                throw new NullPointerException("null ref");
            } else if (type % 3 == 2) {
                throw new IllegalArgumentException("bad arg");
            }
            return "no exception";
        } catch (ArithmeticException e) {
            return "caught ArithmeticException";
        } catch (NullPointerException e) {
            return "caught NullPointerException";
        } catch (IllegalArgumentException e) {
            return "caught IllegalArgumentException";
        }
    }

    // 3. Nested try-catch
    public static String nestedTryCatch() {
        try {
            try {
                int[] arr = new int[5];
                arr[10] = 42; // ArrayIndexOutOfBoundsException
            } catch (ArrayIndexOutOfBoundsException e) {
                throw new RuntimeException("wrapped", e); // Re-throw different type
            }
            return "no exception";
        } catch (RuntimeException e) {
            return "caught RuntimeException: " + e.getMessage();
        }
    }

    // 4. Catch-all with catch Exception
    public static String catchAll() {
        try {
            Object obj = null;
            obj.toString(); // NullPointerException
            return "success";
        } catch (Exception e) {
            return "caught: " + e.getClass().getSimpleName();
        }
    }

    // 5. Exception re-throw
    public static int rethrowTest() {
        try {
            throw new IllegalStateException("test");
        } catch (IllegalStateException e) {
            throw e; // Re-throw, should propagate up
        }
    }

    // 6. Try-catch with deoptimization trigger (implicit null check)
    public static int deoptWithCatch(String input) {
        try {
            // This will trigger implicit null check → exception → handler
            return input.length();
        } catch (NullPointerException e) {
            return -999;
        }
    }

    // 7. Multiple exceptions in sequence
    public static int sequentialExceptions() {
        int result = 0;
        try {
            result += 10 / 2; // No exception
        } catch (ArithmeticException e) {
            result += 100;
        }
        
        try {
            result += 10 / 0; // Exception
        } catch (ArithmeticException e) {
            result += 1000;
        }
        
        return result; // Should be 1005
    }

    public static void main(String[] args) {
        System.out.println("=== Exception Table Test ===");
        
        // Test 1
//         for (int i = 0; i < 50000; i++) {
//             int r1 = simpleTryCatch();
//             System.out.println("simpleTryCatch: " + r1 + " (expected: -1)");
//         }

        // Test 2
//         for (int i = 0; i < 50000; i++) {
//             String r2 = multipleCatch(i);
//             System.out.println("multipleCatch: " + r2);
//         }

        // Test 3
//         for (int i = 0; i < 50000; i++) {
//             String r3 = nestedTryCatch();
//             System.out.println("nestedTryCatch: " + r3);
//         }

        // Test 4
        for (int i = 0; i < 50000; i++) {
            String r4 = catchAll();
            System.out.println("catchAll: " + r4);
        }
//
//         // Test 5
//         try {
//             rethrowTest();
//             System.out.println("rethrowTest: FAILED (should have thrown)");
//         } catch (IllegalStateException e) {
//             System.out.println("rethrowTest: caught IllegalStateException (expected)");
//         }
//
//         // Test 6
//         int r6a = deoptWithCatch("hello");
//         int r6b = deoptWithCatch(null);
//         System.out.println("deoptWithCatch(hello): " + r6a + " (expected: 5)");
//         System.out.println("deoptWithCatch(null): " + r6b + " (expected: -999)");
//
//         // Test 7
//         int r7 = sequentialExceptions();
//         System.out.println("sequentialExceptions: " + r7 + " (expected: 1005)");
        
        System.out.println("=== Test Complete ===");
    }
}

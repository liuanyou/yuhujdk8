package com.example;

public class DeoptTest {
    
    // This test triggers deoptimization via exception handler trap
    // thrower() has no try-catch, so Yuhu generates a trap via handler_for_exception
    // When it throws, it deoptimizes back to callerWithHandler which has the catch block
    
    // Method that will be compiled by Yuhu
    // This method throws with NO try-catch - handler_for_exception will trigger trap
    static void thrower(int trigger) {
        if (trigger > 0) {
            throw new RuntimeException("Deopt trigger: " + trigger);
        }
    }
    
    // Caller with exception handler
    // This ensures there's a handler to return to after deopt
    static void callerWithHandler(int trigger) {
        try {
            thrower(trigger);
        } catch (RuntimeException e) {
            System.out.println("Caught in callerWithHandler: " + e.getMessage());
        }
    }
    
    // Complex method with multiple locals, expression stack operations, and monitors
    // This tests deoptimization state preservation for:
    // - Multiple local variables (int, long, double, Object references)
    // - Deep expression stack (multiple values on stack before throw)
    // - Monitored regions (synchronized blocks)
    // - T_LONG/T_DOUBLE 2-word value transfer to interpreter frame
    static void complexDeoptTarget(int trigger, Object obj1, Object obj2) {
        // Multiple locals - including T_LONG and T_DOUBLE for 2-word transfer test
        int localInt1 = 100;
        int localInt2 = 200;
        long localLong1 = 0xDEADBEEFL;
        double localDouble1 = 3.14159265358979;
        String localStr = "test-string";
        Object localObj = new Object();
        
        System.out.println("complexDeoptTarget: locals initialized");
        System.out.println("  localInt1=" + localInt1 + ", localInt2=" + localInt2);
        System.out.println("  localLong1=" + localLong1);
        System.out.println("  localDouble1=" + localDouble1);
        System.out.println("  localStr=" + localStr);
        
        // Synchronized block with monitor
        synchronized (obj1) {
            System.out.println("  Inside synchronized(obj1)");
            
            // More locals inside monitor - add more T_LONG/T_DOUBLE
            int monitorLocal1 = localInt1 + localInt2;
            long monitorLocalLong = localLong1 * 2;
            double monitorLocalDouble = localDouble1 * 2.0;
            
            System.out.println("  monitorLocal1=" + monitorLocal1);
            System.out.println("  monitorLocalLong=" + monitorLocalLong);
            System.out.println("  monitorLocalDouble=" + monitorLocalDouble);
            
            // Nested synchronized
            synchronized (obj2) {
                System.out.println("    Inside synchronized(obj2)");
                
                // Expression stack manipulation - push T_LONG and T_DOUBLE values onto stack
                // This tests if 2-word values on expression stack transfer correctly
                long stackLong = localLong1 + monitorLocalLong;
                double stackDouble = localDouble1 + monitorLocalDouble;
                int stackResult = (localInt1 + localInt2) * monitorLocal1 + (int)(monitorLocalLong % 1000);
                
                // Use the values to ensure they're on the expression stack at trap point
                String complexStr = "result=" + stackResult + 
                                   " stackLong=" + stackLong + 
                                   " stackDouble=" + stackDouble +
                                   " str=" + localStr;
                
                System.out.println("    stackResult=" + stackResult);
                System.out.println("    stackLong=" + stackLong);
                System.out.println("    stackDouble=" + stackDouble);
                System.out.println("    complexStr=" + complexStr);
                
                // This will trigger deopt if trigger > 0
                // At this point, locals and expression stack contain T_LONG/T_DOUBLE values
                if (trigger > 0) {
                    System.out.println("    Throwing exception with deep stack state (including T_LONG/T_DOUBLE)...");
                    throw new RuntimeException("Complex deopt trigger: " + trigger + 
                                             " stackResult=" + stackResult +
                                             " stackLong=" + stackLong +
                                             " stackDouble=" + stackDouble +
                                             " localInt1=" + localInt1 +
                                             " localInt2=" + localInt2 +
                                             " localLong1=" + localLong1 +
                                             " localDouble1=" + localDouble1 +
                                             " monitorLocal1=" + monitorLocal1 +
                                             " monitorLocalLong=" + monitorLocalLong +
                                             " monitorLocalDouble=" + monitorLocalDouble);
                }
                
                // More computation after potential trap point
                int finalResult = stackResult + localInt1 + localInt2;
                System.out.println("    finalResult=" + finalResult);
            }
        }
        
        System.out.println("complexDeoptTarget: completed normally");
    }
    
    // Caller for complex method with exception handler
    static void callComplexWithHandler(int trigger) {
        Object obj1 = new Object();
        Object obj2 = new Object();
        
        try {
            complexDeoptTarget(trigger, obj1, obj2);
        } catch (RuntimeException e) {
            System.out.println("Caught in callComplexWithHandler: " + e.getMessage());
        }
    }
    
    // Functional interface for lambda with long argument
    interface LongConsumer {
        long accept(long value);
    }

    // Functional interface for lambda with double argument
    interface DoubleConsumer {
        double accept(double value);
    }

    // Functional interface for lambda with mixed long + Object arguments
    interface MixedLambda {
        String accept(long lval, Object obj);
    }

    // Test lambda with long captured value - triggers invokedynamic with T_LONG slot
    static long lambdaWithLong(int trigger, long captured) {
        long local1 = captured * 2;
        long local2 = captured + 100;

        // Lambda captures 'captured' (long) - invokedynamic will have T_LONG on expression stack
        LongConsumer func = (long value) -> {
            long result = value + captured + local1;
            return result;
        };

        System.out.println("lambdaWithLong: captured=" + captured + " local1=" + local1 + " local2=" + local2);

        long lambdaResult = func.accept(local2);
        System.out.println("lambdaWithLong: lambdaResult=" + lambdaResult);

        // Deopt trigger after lambda usage
        if (trigger > 0) {
            throw new RuntimeException("lambdaWithLong deopt: trigger=" + trigger
                    + " captured=" + captured + " lambdaResult=" + lambdaResult);
        }

        return lambdaResult;
    }

    // Test lambda with double captured value - triggers invokedynamic with T_DOUBLE slot
    static double lambdaWithDouble(int trigger, double captured) {
        double local1 = captured * 1.5;

        DoubleConsumer func = (double value) -> {
            return value + captured + local1;
        };

        System.out.println("lambdaWithDouble: captured=" + captured + " local1=" + local1);

        double lambdaResult = func.accept(42.0);
        System.out.println("lambdaWithDouble: lambdaResult=" + lambdaResult);

        if (trigger > 0) {
            throw new RuntimeException("lambdaWithDouble deopt: trigger=" + trigger
                    + " captured=" + captured + " lambdaResult=" + lambdaResult);
        }

        return lambdaResult;
    }

    // Test lambda with mixed long + Object + int arguments - tests interleaved T_LONG and T_OBJECT slots
    static String lambdaWithMixed(int trigger, long longVal, Object obj) {
        int intVal = 42;
        long localLong = longVal + 1L;

        MixedLambda func = (long l, Object o) -> {
            return "l=" + l + " o=" + o + " longVal=" + longVal + " localLong=" + localLong;
        };

        System.out.println("lambdaWithMixed: longVal=" + longVal + " obj=" + obj + " intVal=" + intVal);

        String result = func.accept(localLong, obj);
        System.out.println("lambdaWithMixed: result=" + result);

        if (trigger > 0) {
            throw new RuntimeException("lambdaWithMixed deopt: trigger=" + trigger
                    + " longVal=" + longVal + " result=" + result);
        }

        return result;
    }

    // Caller with handler for lambda tests
    static void callLambdaWithLong(int trigger) {
        try {
            lambdaWithLong(trigger, 0xDEADBEEFCAFEL);
        } catch (RuntimeException e) {
            System.out.println("Caught in callLambdaWithLong: " + e.getMessage());
        }
    }

    static void callLambdaWithDouble(int trigger) {
        try {
            lambdaWithDouble(trigger, 3.14159265);
        } catch (RuntimeException e) {
            System.out.println("Caught in callLambdaWithDouble: " + e.getMessage());
        }
    }

    static void callLambdaWithMixed(int trigger) {
        try {
            lambdaWithMixed(trigger, 0x123456789ABCDEF0L, "hello");
        } catch (RuntimeException e) {
            System.out.println("Caught in callLambdaWithMixed: " + e.getMessage());
        }
    }

    public static void main(String[] args) throws Exception {
//         System.out.println("=== Deoptimization Test ===");
//
//         System.out.println("Warming up to trigger Yuhu compilation of thrower()...");
//
//         // Call many times to trigger Yuhu compilation of thrower()
//         // All calls with trigger=0 (no exception thrown)
//         for (int i = 0; i < 100000; i++) {
//             callerWithHandler(0);
//
//             // Print progress every 10,000 iterations
//             if (i % 10000 == 0) {
//                 System.out.println("Iteration " + i);
//             }
//         }
//
//         try {
//             Thread.sleep(30000);
//         } catch (Exception e) {
//             System.out.println("sleep failed");
//         }
//
//         System.out.println("\nWarm-up complete.");
//         System.out.println("If compilation happened, machine code should be in debug/yuhu_new_code.txt");
//         System.out.println("\nNow triggering deoptimization...");
//
//         // Call with trigger=1 - thrower() will throw
//         // thrower() has no handler, so handler_for_exception triggers trap
//         // Deoptimizes back to callerWithHandler's catch block
//         System.out.println("Calling callerWithHandler(1) - thrower() should trap and deoptimize...");
//         callerWithHandler(1);
//         System.out.println("Deoptimization should have been triggered!");
//
//         // Continue calling to verify deopt worked
//         System.out.println("\nContinuing with trigger=0 after deopt...");
//         callerWithHandler(0);
//         System.out.println("Success: callerWithHandler(0) worked after deopt");
        
//         // Test 2: Complex deoptimization with multiple locals, expression stack, and monitors
//         System.out.println("\n=== Test 2: Complex Deoptimization ===");
//         System.out.println("Warming up to trigger Yuhu compilation of complexDeoptTarget()...");
//
//         // Warm up to trigger Yuhu compilation
//         for (int i = 0; i < 50000; i++) {
//             callComplexWithHandler(0);
//
//             if (i % 10000 == 0) {
//                 System.out.println("Complex iteration " + i);
//             }
//         }
//
//         try {
//             Thread.sleep(30000);
//         } catch (Exception e) {
//             System.out.println("sleep failed");
//         }
//
//         System.out.println("\nWarm-up complete for complex method.");
//         System.out.println("Now triggering complex deoptimization...");
//
//         // Trigger deopt with complex state
//         System.out.println("Calling callComplexWithHandler(1) - complexDeoptTarget() should trap and deoptimize...");
//         callComplexWithHandler(1);
//         System.out.println("Complex deoptimization should have been triggered!");
//
//         // Verify it still works after deopt
//         System.out.println("\nContinuing with trigger=0 after complex deopt...");
//         callComplexWithHandler(0);
//         System.out.println("Success: callComplexWithHandler(0) worked after complex deopt");
        
//         // Test 3: Lambda with long argument (T_LONG slot test)
//         System.out.println("\n=== Test 3: Lambda with long (T_LONG) ===");
//         System.out.println("Warming up lambdaWithLong()...");
//         for (int i = 0; i < 50000; i++) {
//             callLambdaWithLong(0);
//             if (i % 10000 == 0) {
//                 System.out.println("LambdaLong iteration " + i);
//             }
//         }
//         try { Thread.sleep(30000); } catch (Exception e) { }
//         System.out.println("\nWarm-up complete. Triggering deopt...");
//         callLambdaWithLong(1);
//         System.out.println("Lambda with long deopt triggered!");
//         callLambdaWithLong(0);
//         System.out.println("Success: lambdaWithLong works after deopt");

//         // Test 4: Lambda with double argument (T_DOUBLE slot test)
//         System.out.println("\n=== Test 4: Lambda with double (T_DOUBLE) ===");
//         System.out.println("Warming up lambdaWithDouble()...");
//         for (int i = 0; i < 50000; i++) {
//             callLambdaWithDouble(0);
//             if (i % 10000 == 0) {
//                 System.out.println("LambdaDouble iteration " + i);
//             }
//         }
//         try { Thread.sleep(30000); } catch (Exception e) { }
//         System.out.println("\nWarm-up complete. Triggering deopt...");
//         callLambdaWithDouble(1);
//         System.out.println("Lambda with double deopt triggered!");
//         callLambdaWithDouble(0);
//         System.out.println("Success: lambdaWithDouble works after deopt");

        // Test 5: Lambda with mixed long + Object (interleaved T_LONG/T_OBJECT slots)
        System.out.println("\n=== Test 5: Lambda with mixed long+Object ===");
        System.out.println("Warming up lambdaWithMixed()...");
        for (int i = 0; i < 50000; i++) {
            callLambdaWithMixed(0);
            if (i % 10000 == 0) {
                System.out.println("LambdaMixed iteration " + i);
            }
        }
        try { Thread.sleep(30000); } catch (Exception e) { }
        System.out.println("\nWarm-up complete. Triggering deopt...");
        callLambdaWithMixed(1);
        System.out.println("Lambda with mixed deopt triggered!");
        callLambdaWithMixed(0);
        System.out.println("Success: lambdaWithMixed works after deopt");

        System.out.println("\n=== All Tests Complete ===");
        System.out.println("Check for hs_err_pid*.log or deoptimization messages above.");
    }
}

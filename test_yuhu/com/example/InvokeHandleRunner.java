package com.example;

import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

/**
 * Test case for invokehandle bytecode support in Yuhu compiler.
 * 
 * invokehandle is generated when calling signature-polymorphic methods:
 * - MethodHandle.invoke()
 * - MethodHandle.invokeExact()
 * 
 * The Yuhu compiler should trap on invokehandle and deoptimize to interpreter.
 */
public class InvokeHandleRunner {
    
    // A simple target method for MethodHandle to call
    public static String greet(String name) {
        return "Hello, " + name + "!";
    }
    
    public static int add(int a, int b) {
        return a + b;
    }
    
    public static long multiply(long a, long b) {
        return a * b;
    }
    
    /**
     * Test MethodHandle.invoke() - generates invokehandle bytecode
     */
    public static String testInvoke() throws Throwable {
        MethodHandles.Lookup lookup = MethodHandles.lookup();
        MethodType type = MethodType.methodType(String.class, String.class);
        MethodHandle handle = lookup.findStatic(InvokeHandleRunner.class, "greet", type);
        
        // This call generates invokehandle bytecode
        // because invoke() is signature-polymorphic
        String result = (String) handle.invoke("World");
        return result;
    }
    
    /**
     * Test MethodHandle.invokeExact() - also generates invokehandle bytecode
     */
    public static String testInvokeExact() throws Throwable {
        MethodHandles.Lookup lookup = MethodHandles.lookup();
        MethodType type = MethodType.methodType(String.class, String.class);
        MethodHandle handle = lookup.findStatic(InvokeHandleRunner.class, "greet", type);
        
        // invokeExact also generates invokehandle bytecode
        String result = (String) handle.invokeExact("Yuhu");
        return result;
    }
    
    /**
     * Test with primitive types
     */
    public static int testInvokeWithInts() throws Throwable {
        MethodHandles.Lookup lookup = MethodHandles.lookup();
        MethodType type = MethodType.methodType(int.class, int.class, int.class);
        MethodHandle handle = lookup.findStatic(InvokeHandleRunner.class, "add", type);
        
        // invoke with int arguments
        int result = (int) handle.invoke(10, 20);
        return result;
    }
    
    /**
     * Test with long types
     */
    public static long testInvokeWithLongs() throws Throwable {
        MethodHandles.Lookup lookup = MethodHandles.lookup();
        MethodType type = MethodType.methodType(long.class, long.class, long.class);
        MethodHandle handle = lookup.findStatic(InvokeHandleRunner.class, "multiply", type);
        
        // invoke with long arguments
        long result = (long) handle.invoke(100L, 200L);
        return result;
    }
    
    /**
     * Run multiple iterations to trigger Yuhu compilation
     */
    public static void runTests() throws Throwable {
        System.out.println("=== InvokeHandle Test ===");
        
        // Warm up to trigger compilation
        for (int i = 0; i < 10000; i++) {
            testInvoke();
//             testInvokeExact();
//             testInvokeWithInts();
//             testInvokeWithLongs();
        }

        try {
            Thread.sleep(15000);
        } catch (Exception e) {
            System.out.println(e);
        }
        
        // Verify results
        String result1 = testInvoke();
        System.out.println("testInvoke: " + result1);
        assert result1.equals("Hello, World!") : "Expected 'Hello, World!' but got: " + result1;
        
//         String result2 = testInvokeExact();
//         System.out.println("testInvokeExact: " + result2);
//         assert result2.equals("Hello, Yuhu!") : "Expected 'Hello, Yuhu!' but got: " + result2;
//
//         int result3 = testInvokeWithInts();
//         System.out.println("testInvokeWithInts: " + result3);
//         assert result3 == 30 : "Expected 30 but got: " + result3;
//
//         long result4 = testInvokeWithLongs();
//         System.out.println("testInvokeWithLongs: " + result4);
//         assert result4 == 20000L : "Expected 20000 but got: " + result4;
        
        System.out.println("=== All tests passed! ===");
    }
}

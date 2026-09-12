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
public class InvokeHandleTest {
    
    public static void main(String[] args) throws Throwable {
        InvokeHandleRunner.runTests();
    }
}

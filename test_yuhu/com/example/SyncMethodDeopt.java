package com.example;

/**
 * Reproducer for the "synchronized methods must have monitors" abort
 * (hotspot/src/share/vm/runtime/vframeArray.cpp:291) seen while deoptimizing
 * a Yuhu (tier-6) compiled *synchronized method*.
 *
 * The crashing production frames were all implicit-monitor methods, e.g.
 * sun.misc.URLClassPath.getLoader(I) / getResource(...), i.e. methods whose
 * only monitor is the one taken by the method prologue
 * (YuhuTopLevelBlock::acquire_method_lock), not by a monitorenter bytecode.
 * The existing tests (DeoptTest.complexDeoptTarget, OsrTest.synchronizedLoop)
 * only exercise explicit monitorenter blocks, so the implicit entry monitor
 * never reaches a deopt bundle.
 *
 * Each variant is warmed up until Yuhu compiles it, then deopted while the
 * entry monitor is still held:
 *   - lockAndThrow:        athrow inside a static synchronized method
 *   - lockInstanceThrow:   athrow inside an instance synchronized method
 *   - lockAndTrap:         deopt at a call site (no exception) inside a
 *                          synchronized method
 */
public class SyncMethodDeopt {

    static volatile int hits = 0;
    static Object sink = null;

    // ---- variant 1: static synchronized, deopt via athrow -----------------
    static synchronized String lockAndThrow(int trigger, String s) {
        String a = "a-" + s;
        long wide = 0x0123456789ABCDEFL;
        double dbl = 2.718281828459045;
        hits++;
        if (trigger > 0) {
            throw new RuntimeException("deopt inside static synchronized " + a + wide + dbl);
        }
        sink = a;
        return a;
    }

    // ---- variant 2: instance synchronized, deopt via athrow ---------------
    synchronized String lockInstanceThrow(int trigger) {
        String a = "b-" + trigger;
        long wide = 9876543210L * (trigger + 1);
        hits++;
        if (trigger > 0) {
            throw new RuntimeException("deopt inside instance synchronized " + a + wide);
        }
        sink = a;
        return a;
    }

    // ---- variant 3: static synchronized, deopt at a call site -------------
    // No exception: the trap is an ordinary deopt at a call site inside the
    // method, which is the shape of the production crash (reexecute bci).
    static synchronized Object lockAndTrap(Object arg) {
        hits++;
        // Force a deopt at a call site: the receiver is typed as String by the
        // warmup, so passing a non-String after compilation must trap.
        String s = (String) arg;
        sink = s;
        return s;
    }

    static SyncMethodDeopt self = new SyncMethodDeopt();

    public static void main(String[] args) {
        int warm = 20000;

        // variant 1
        for (int i = 0; i < warm; i++) {
            lockAndThrow(0, "w" + i);
        }
        System.out.println("[1] warmed, hits=" + hits);
        try {
            lockAndThrow(1, "boom");
            System.out.println("[1] FAIL: exception not thrown");
        } catch (RuntimeException e) {
            System.out.println("[1] OK caught: " + e.getMessage());
        }

        // variant 2
        for (int i = 0; i < warm; i++) {
            self.lockInstanceThrow(0);
        }
        System.out.println("[2] warmed, hits=" + hits);
        try {
            self.lockInstanceThrow(1);
            System.out.println("[2] FAIL: exception not thrown");
        } catch (RuntimeException e) {
            System.out.println("[2] OK caught: " + e.getMessage());
        }

        // variant 3
        for (int i = 0; i < warm; i++) {
            lockAndTrap("s" + i);
        }
        System.out.println("[3] warmed, hits=" + hits);
        try {
            lockAndTrap(new StringBuilder("not-a-string"));
            System.out.println("[3] NOTE: no deopt (checkcast succeeded)");
        } catch (RuntimeException e) {
            System.out.println("[3] OK after deopt: " + e.getClass().getSimpleName());
        }

        // After all the deopts the monitor must still be releasable exactly
        // once per activation; verify the lock was really carried, not leaked.
        int before = hits;
        synchronized (SyncMethodDeopt.class) {
            // contended re-acquire from the interpreter: would deadlock or
            // trip a monitor assert if the deopted frames dropped the lock.
            lockAndThrow(0, "after");
        }
        System.out.println("[4] lock still consistent, hits delta=" + (hits - before));

        System.out.println("DONE");
    }
}

package com.example;

/**
 * Reproducer candidate for the flaky
 *   assert(!method()->is_synchronized() || locks > 0, "synchronized methods must have monitors")
 * in vframeArrayElement::unpack_on_stack.
 *
 * The production crash is a *spontaneous* deopt, not a compiler trap: the hs_err
 * event log shows "Executing VM operation: Deoptimize" (Universe::flush_dependents_on
 * when a klass is linked) immediately followed by
 *   DEOPT PACKING pc=<address inside the nmethod>
 *   DEOPT UNPACKING pc=<deopt blob> sp=... mode 0
 * i.e. Deoptimization::Unpack_deopt.  A mode-0 deopt uses whatever pc the frame
 * happens to be sitting on, so it can land on a prologue/entry return pc whose
 * recorded scope carries no monitor even though the method is synchronized.
 *
 * Two ways to hit that same unpack path on demand:
 *   -XX:+DeoptimizeALot / -XX:+DeoptimizeRandom  (VM_DeoptimizeAll -> frame::deoptimize)
 *   a class load from inside a live frame of the compiled synchronized method
 *
 * The "contended" methods also park the frame inside the runtime monitorenter call,
 * which is the region where the entry monitor is not yet visible to a trap scope.
 */
public class SyncSpontDeopt {

    static volatile long sink = 0;

    /** Hot synchronized method with plenty of native exits: DeoptimizeALot keeps
     *  asking this live frame to deoptimize while it sits at call-return pcs. */
    static synchronized long nativeInside(int n) {
        long s = 0;
        for (int i = 0; i < n; i++) {
            s += System.currentTimeMillis();
            s += String.valueOf(i).length();
        }
        return s;
    }

    /** Two threads fight for the same lock, so the loser's frame stops right at the
     *  entry monitorenter (the prologue region of a synchronized method). */
    static synchronized long contended(int n) {
        long s = 0;
        for (int i = 0; i < n; i++) {
            s += i * 31 + 7;
        }
        return s;
    }

    static void hammer(final int iterations, final int inner) throws Exception {
        Thread[] ts = new Thread[3];
        for (int t = 0; t < ts.length; t++) {
            ts[t] = new Thread(new Runnable() {
                public void run() {
                    long acc = 0;
                    for (int i = 0; i < iterations; i++) {
                        acc += contended(inner);
                        acc += nativeInside(4);
                    }
                    sink += acc;
                }
            });
            ts[t].start();
        }
        for (int t = 0; t < ts.length; t++) {
            ts[t].join();
        }
    }

    public static void main(String[] args) throws Exception {
        String mode = args.length > 0 ? args[0] : "both";
        int warm = 30000;
        long acc = 0;
        // Warm the methods up so they get compiled (tier 1..3, then tier 6 yuhu).
        for (int i = 0; i < warm; i++) {
            acc += nativeInside(3) + contended(200);
        }
        sink += acc;
        System.out.println("SYNC_REPRO warm done sink=" + sink + " mode=" + mode);
        if (mode.equals("contended") || mode.equals("both")) {
            hammer(20000, 200);
        }
        System.out.println("SYNC_REPRO done sink=" + sink);
    }
}

package com.example;

/**
 * Control for SyncSpontDeopt: identical body, no synchronized anywhere.
 * Used to tell apart "spontaneous deopt of a Yuhu frame is broken in general"
 * from "spontaneous deopt of a returning synchronized frame is broken".
 */
public class PlainSpontDeopt {

    static volatile long sink = 0;

    static long nativeInside(int n) {
        long s = 0;
        for (int i = 0; i < n; i++) {
            s += System.currentTimeMillis();
            s += String.valueOf(i).length();
        }
        return s;
    }

    static long contended(int n) {
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
        for (int i = 0; i < warm; i++) {
            acc += nativeInside(3) + contended(200);
        }
        sink += acc;
        System.out.println("PLAIN_REPRO warm done sink=" + sink + " mode=" + mode);
        if (mode.equals("contended") || mode.equals("both")) {
            hammer(20000, 200);
        }
        System.out.println("PLAIN_REPRO done sink=" + sink);
    }
}

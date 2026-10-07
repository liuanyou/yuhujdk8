package com.example;

import java.util.concurrent.ConcurrentHashMap;

/**
 * Reproducer for the Yuhu monitorenter crash in ConcurrentHashMap.putVal
 * happening DURING CLASS LOADING.
 *
 * Crash signature (from hs_err_pid61732/61839/79304.log):
 *   Internal Error (synchronizer.cpp:240):
 *   assert(lock != mark->locker()) failed: must not re-lock the same lock
 *
 * Actual VM flags from the crash logs (NO DeoptimizeALot!):
 *   -XX:+UseYuhuInt
 *   -XX:+UseYuhuCompiler
 *   -XX:TieredStopAtLevel=6
 *   -XX:CompileCommand=yuhuonly,java/util/concurrent/ConcurrentHashMap.putVal
 *   -XX:YuhuCompileOnlyOf=java.util.concurrent.ConcurrentHashMap::putVal
 *   (plus various logging flags)
 *
 * Evidence from the logs:
 *   - "Deoptimization events (0 events)" — no deopt has occurred.
 *   - Yuhu's putVal nmethod is index 25 / 28 — the FIRST Yuhu compilation
 *     of putVal in the process.
 *   - Stack frames show the crash is inside class loading:
 *       ClassLoader.getClassLoadingLock / checkCerts / preDefineClass
 *         -> putIfAbsent (C1) -> Yuhu compiled putVal
 *           -> yuhu_monitorenter_stub -> YuhuRuntime::monitorenter
 *             -> ObjectSynchronizer::fast_enter -> slow_enter (assert)
 *   - Before compilation, Yuhu's INTERPRETER (-XX:+UseYuhuInt) executed
 *     putVal many times, taking the synchronized(f) path on bin collisions.
 *
 * Revised root cause hypothesis:
 *   Yuhu's interpreter OR the transition interpreter->compiled leaves a stale
 *   BasicLock pointer in a bin Node's mark word (either the interpreter did
 *   not release on some path, or a deopt/recompile boundary was crossed).
 *   Then the newly compiled nmethod's first execution finds the object's
 *   mark word already equal to its own frame's BasicObjectLock slot address.
 *
 * Reproduction strategy:
 *   Phase A: exercise ConcurrentHashMap.putIfAbsent on colliding keys enough
 *            times to (1) drive Yuhu's interpreter through the
 *            synchronized(f) path repeatedly, and (2) cross the invocation /
 *            backedge counter thresholds so Yuhu compiles putVal.
 *   Phase B: continue with class-loading-style putIfAbsent traffic
 *            (ClassLoader.classLoadingLocks) via Class.forName, so that
 *            AFTER compilation, putVal keeps getting called on possibly
 *            "poisoned" bin nodes.
 *
 * Run via test_reproducer_monitor_deopt.sh.
 */
public class Reproducer {

    // Separate map to drive putVal compilation.
    static final ConcurrentHashMap<String, String> warm = new ConcurrentHashMap<>();

    public static void main(String[] args) throws Exception {
        System.out.println("=== Yuhu putVal + class-loading reproducer ===");
        System.out.println("Flags: -XX:+UseYuhuInt -XX:+UseYuhuCompiler "
                           + "-XX:TieredStopAtLevel=6 "
                           + "-XX:CompileCommand=yuhuonly,java/util/concurrent/ConcurrentHashMap.putVal "
                           + "-XX:YuhuCompileOnlyOf=java.util.concurrent.ConcurrentHashMap::putVal");

        // ---------------------------------------------------------------
        // Phase A: force putVal through the synchronized(f) path many
        //          times, and cross Yuhu's compile threshold.
        //
        // CHM default table size is 16. Integer i has spread(i) with low
        // 4 bits varying; keys "0" and "16" both hash to bin 0 (because
        // spread("0".hashCode()="48")=... — we just need collisions and
        // can rely on generic String hashing producing them across 200 keys.
        // ---------------------------------------------------------------
        // Seed: put two keys that will surely collide after the map is
        // small, to force bin chain and synchronized(f).
        warm.put("seed-a", "A");
        warm.put("seed-b", "B");

        // Hammer the map — many putIfAbsent calls, mostly with new keys
        // (so a collision happens naturally as the map grows), plus a
        // remove to keep the bin reachable.
        for (int i = 0; i < 100000; i++) {
            String k = "warm-" + i;
            warm.putIfAbsent(k, Integer.toString(i));
            // Some collisions on purpose: alternate removing to keep map small
            // so bin count stays high relative to size.
            if ((i & 3) == 0) {
                warm.remove("warm-" + (i - 3));
            }
        }

        System.out.println("Phase A: putVal warmed up, Yuhu should have compiled it by now.");
        System.out.println("         warm map size = " + warm.size());

        // ---------------------------------------------------------------
        // Phase B: drive real class loading, which is where the crash
        //          actually manifests (ClassLoader.classLoadingLocks is
        //          a ConcurrentHashMap that Yuhu's compiled putVal will
        //          operate on).
        //
        // Each Class.forName triggers:
        //   ClassLoader.loadClass -> getClassLoadingLock -> classLoadingLocks.putIfAbsent
        // Which for a colliding class-name key hits Yuhu's compiled synchronized(f).
        // ---------------------------------------------------------------
        String[] classNames = {
            // Broad sweep over commonly loaded JDK classes.
            "java.util.HashMap", "java.util.LinkedHashMap", "java.util.IdentityHashMap",
            "java.util.WeakHashMap", "java.util.Hashtable", "java.util.Properties",
            "java.util.ArrayList", "java.util.LinkedList", "java.util.Vector",
            "java.util.HashSet", "java.util.LinkedHashSet", "java.util.TreeSet",
            "java.util.EnumMap", "java.util.EnumSet", "java.util.BitSet",
            "java.util.Scanner", "java.util.Formatter", "java.util.Random",
            "java.util.UUID", "java.util.Currency", "java.util.Locale",
            "java.util.TimeZone", "java.util.GregorianCalendar", "java.util.Calendar",
            "java.util.Date", "java.util.Stack", "java.util.PriorityQueue",
            "java.util.ArrayDeque", "java.util.Collections", "java.util.Arrays",
            "java.util.Objects", "java.util.Optional", "java.util.StringJoiner",
            "java.util.StringTokenizer", "java.util.ServiceLoader", "java.zip.ZipFile",
            // Reflection / MethodHandle classes to exercise parallel class loading paths.
            "java.lang.invoke.MethodHandle", "java.lang.invoke.MethodHandles",
            "java.lang.invoke.MethodType", "java.lang.invoke.CallSite",
            "java.lang.invoke.MutableCallSite", "java.lang.invoke.ConstantCallSite",
            "java.lang.invoke.VarHandle", "java.lang.invoke.MethodHandleProxies",
            "java.lang.reflect.Method", "java.lang.reflect.Field", "java.lang.reflect.Constructor",
            "java.lang.reflect.Proxy", "java.lang.reflect.Array", "java.lang.reflect.Modifier",
        };

        for (int round = 0; round < 40; round++) {
            for (String cn : classNames) {
                try {
                    Class.forName(cn, false, Reproducer.class.getClassLoader());
                } catch (Throwable t) {
                    // Ignore — we're triggering loadClass path, not verifying existence.
                }
            }
        }
        System.out.println("Phase B: class loading sweep complete.");

        // Also do a large burst of putIfAbsent on a fresh map to keep
        // hitting the compiled path after warm-up.
        ConcurrentHashMap<String, String> second = new ConcurrentHashMap<>();
        for (int i = 0; i < 200000; i++) {
            String k = "s-" + (i & 0x3F);  // 64 distinct keys, heavy collision
            second.putIfAbsent(k, Integer.toString(i));
        }

        System.out.println("Done. If no assert fired, the reproduction is incomplete.");
    }
}

package com.example;

import com.google.common.cache.CacheBuilder;
import com.google.common.cache.CacheLoader;
import com.google.common.cache.LoadingCache;

import java.util.concurrent.atomic.AtomicLong;

/**
 * Reproducer for the Yuhu tier-6 COMPILER-side assert
 *
 *   Internal Error (.../yuhu/yuhuTopLevelBlock.cpp:874):
 *   assert(slot_type == ciTypeFlow::StateVector::T_BOTTOM) failed:
 *       basic type should be T_CONFLICT
 *
 * observed in hs_err_pid62345.log while "Yuhu CompilerThread0" was compiling
 *
 *   com.google.common.cache.LocalCache$Segment::lockedGetOrLoad (447 bytes)
 *
 * during a Gradle (cpsms) build.  The exact flags in the log were just
 *   -XX:+UseYuhuInt -XX:+UseYuhuCompiler -XX:TieredStopAtLevel=6
 * (lockedGetOrLoad simply became hot and was picked up by tier 6 on its own).
 *
 * ------------------------------------------------------------------
 * Why this is a COMPILE-time crash
 * ------------------------------------------------------------------
 * The native stack in the log is entirely inside Yuhu's IR emission:
 *   YuhuFunction::initialize -> emit_IR -> parse_bytecode
 *     -> YuhuTopLevelBlock::do_monitorenter
 *       -> YuhuBlock::check_null -> zero_check -> do_zero_check
 *         -> YuhuTopLevelBlock::call_vm
 *           -> YuhuTopLevelBlock::register_deopt_bundle   <-- assert fires here
 *
 * register_deopt_bundle() walks every local slot i in 0..max_locals-1 of the
 * current JVM state.  When the materialized YuhuValue for a slot is NULL it
 * falls back to the slot's ciTypeFlow-derived type (_local_types[i]) and asserts
 * that this type is T_BOTTOM (yuhuTopLevelBlock.cpp:874).  lockedGetOrLoad has a
 * "synchronized (lock)" block; the objectref null check on that monitor emits a
 * deopt-bundle poll, and at that poll at least one slot is NULL-valued in Yuhu's
 * state while its ciTypeFlow type is NOT T_BOTTOM (it is the T_CONFLICT / merged
 * type the assert message names).  Because it happens purely while COMPILING the
 * method, we only have to get Yuhu to compile lockedGetOrLoad -- the cache's
 * runtime behaviour is irrelevant to the fault itself.
 *
 * ------------------------------------------------------------------
 * Reproduction strategy
 * ------------------------------------------------------------------
 *   1. Build a Guava LoadingCache and call cache.get(key) with distinct keys so
 *      every access is a segment miss routed through Segment::lockedGetOrLoad,
 *      driving it past the tier-6 (Yuhu) compilation threshold.
 *   2. Run with the log's flags plus, to make it fast and deterministic:
 *        -XX:YuhuCompileOnlyOf=com.google.common.cache.LocalCache$Segment::lockedGetOrLoad
 *      (YuhuCompileOnlyOf restricts Yuhu to compiling ONLY that method, so the
 *       crash is isolated and the run stays short.)  Each cache.get() with a
 *       distinct key is a segment miss that invokes lockedGetOrLoad once, so a
 *       few thousand misses already cross the tier-6 compilation threshold.
 *
 * Expected result on an assert/slowdebug build: a fresh hs_err_pid*.log carrying
 * the identical yuhuTopLevelBlock.cpp:874 assert and the same
 * register_deopt_bundle compiler stack.  See ../test_guava_lockedgetorload.sh.
 */
public class GuavaLockedGetOrLoadReproducer {

  static final AtomicLong LOADS = new AtomicLong();

  public static void main(String[] args) throws Exception {
    System.out.println("=== Yuhu LocalCache$Segment::lockedGetOrLoad deopt-bundle reproducer ===");

    final int n = args.length > 0 ? Integer.parseInt(args[0]) : 200000;

    // No maximumSize: every distinct key is a fresh segment miss that must run
    // the loading path (Segment::get -> lockedGetOrLoad).  Small heap-only
    // payloads keep memory modest.
    final LoadingCache<Integer, String> cache = CacheBuilder.newBuilder()
        .recordStats()
        .build(new CacheLoader<Integer, String>() {
          @Override public String load(Integer key) {
            LOADS.incrementAndGet();
            return "v" + key;
          }
        });

    for (int i = 0; i < n; i++) {
      cache.get(i);           // miss -> Segment.get -> lockedGetOrLoad (Yuhu tier-6)
    }

    System.out.println("Done: " + cache.size() + " entries, " + LOADS.get()
                       + " loads. If the compiler assert did not fire, raise n.");
  }
}

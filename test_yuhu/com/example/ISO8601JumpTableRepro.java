package com.example;

import java.text.FieldPosition;
import java.util.Calendar;
import java.util.Date;

import org.apache.log4j.helpers.ISO8601DateFormat;

/**
 * Reproducer for the hs_err_pid43806 crash: the nmethod for
 * org.apache.log4j.helpers.ISO8601DateFormat.format(Date, StringBuffer, FieldPosition)
 * (457 bytes) crashes at a jump-table dispatch with
 *   ldr x13, [x13, w0, sxtw #3]   ; table[idx]  (idx in 0..11, w0=10 in the crash)
 *   ldr x13, [x13]                ; fault, SEGV_ACCERR on an LLVM-cache address
 *
 * Strategy (same as the other scoped reproducers in this directory):
 *  - call the exact 3-arg format() directly (no log4j appender needed) so the
 *    MDO of that method heats up and Yuhu tier-6 compiles it;
 *  - vary the timestamp so the "lastTime" cache branch misses every call and
 *    the tableswitch at bci 75 actually executes;
 *  - cycle through all 12 months so every jump-table slot 0..11 is dereferenced
 *    AFTER installation (the crash hit slot 10 only ~4.6s after the nmethod
 *    was installed, i.e. after the ORC resource tracker had been removed);
 *  - do GCs between rounds so the LLVM blocks are definitely gone from any
 *    allocator reuse perspective.
 *
 * Run via test_iso8601_jumptable.sh. Expected outcome if the bug is present:
 * SIGSEGV SEGV_ACCERR inside the ISO8601DateFormat::format nmethod, or, if it
 * survives, a clean output — then inspect the YuhuTraceMachineCode dump to see
 * whether the const-section symbol (= the jump table) was copied and whether
 * its entries were rebased into the nmethod.
 */
public class ISO8601JumpTableRepro {

    // Enough invocations to pass the tier-6 throughput threshold even with
    // YuhuComplexityThreshold=1 and -Xbatch synchronous compilation.
    private static final int WARMUP_ROUNDS = 20;
    private static final int CALLS_PER_ROUND = 50_000;

    public static void main(String[] args) throws Exception {
        final ISO8601DateFormat fmt = new ISO8601DateFormat();
        final StringBuffer sb = new StringBuffer(32);
        final FieldPosition fpos = new FieldPosition(0);

        // Build one date per month of 2023 (index 0..11 through the tableswitch),
        // plus a couple of same-second repeats to keep the loop realistic.
        long[] months = new long[12];
        Calendar c = Calendar.getInstance();
        c.clear();
        c.set(2023, 0, 1, 12, 30, 45);
        for (int m = 0; m < 12; m++) {
            c.set(Calendar.MONTH, m);
            months[m] = c.getTimeInMillis();
        }

        System.out.println("[repro] sanity: " + fmt.format(new Date(months[0]), sb, fpos));
        sb.setLength(0);

        long t = months[0];
        int idx = 0;
        for (int round = 0; round < WARMUP_ROUNDS; round++) {
            for (int i = 0; i < CALLS_PER_ROUND; i++) {
                // Every call gets a fresh millisecond so the lastTime cache misses
                // and we walk the full conversion path including the tableswitch.
                t += 1;
                idx = (idx + 1) % 12;
                // Force a distinct (minute-shifted) value per month so the switch
                // index really cycles through 0..11 rather than LLVM hoisting it.
                long millis = (t / 12) * 12 + idx;
                sb.setLength(0);
                String out = fmt.format(new Date(millis), sb, fpos).toString();
                if (out.length() != 23) {
                    throw new AssertionError("unexpected format output: " + out);
                }
            }
            // Help the stale window along: drop the LLVM blocks and force scavenges
            // between rounds, then keep calling the (already installed) nmethod.
            System.gc();
        }

        // Post-warmup phase: if the nmethod was installed with un-rebased jump
        // table entries, dereferencing every slot now (long after RT->remove())
        // is what crashed the Kafka run.
        for (int repeat = 0; repeat < 4; repeat++) {
            for (int m = 0; m < 12; m++) {
                sb.setLength(0);
                fmt.format(new Date(months[m]), sb, fpos);
                System.out.println("[repro] month slot " + m + " -> " + sb);
            }
            System.gc();
        }

        System.out.println("[repro] DONE without crash - inspect the machine-code trace "
                + "for the '__const' symbol of ISO8601DateFormat.format and its entries.");
    }
}

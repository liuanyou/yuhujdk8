package com.example;

import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;

public class Reproducer {
    static final ConcurrentHashMap<String, String> map = new ConcurrentHashMap<>();
    static final AtomicInteger counter = new AtomicInteger(0);

    public static void main(String[] args) throws Exception {
        System.out.println("Starting ConcurrentHashMap.computeIfAbsent stress test...");

        // Pre-populate some keys to create bias
        for (int i = 0; i < 100; i++) {
            final int idx = i;
            map.computeIfAbsent("key-" + i, k -> "value-" + idx);
        }

        // Now hammer it with many threads to trigger bias revocation
        for (int round = 0; round < 1000; round++) {
            int numThreads = 16;
            CountDownLatch startLatch = new CountDownLatch(1);
            CountDownLatch doneLatch = new CountDownLatch(numThreads);
            Thread[] threads = new Thread[numThreads];

            for (int t = 0; t < numThreads; t++) {
                final int threadId = t;
                threads[t] = new Thread(() -> {
                    try {
                        startLatch.await();
                        for (int i = 0; i < 50; i++) {
                            String key = "key-" + ((threadId + i) % 100);
                            map.computeIfAbsent(key, k -> "new-value-" + counter.incrementAndGet());
                        }
                    } catch (Exception e) {
                        e.printStackTrace();
                    } finally {
                        doneLatch.countDown();
                    }
                });
                threads[t].start();
            }

            // Release all threads at once to maximize contention
            startLatch.countDown();
            doneLatch.await();

            if (round % 100 == 0) {
                System.out.println("Round " + round + " completed, counter=" + counter.get());
            }
        }

        System.out.println("Done! counter=" + counter.get());
    }
}

/**
 * Minimal test to reproduce the original C1 OSR SIGBUS crash on ARM64.
 *
 * Usage (from this directory):
 *   javac OSRCrashTest.java
 *   java -XX:CompileThreshold=100 -XX:+PrintCompilation OSRCrashTest
 */
public class OSRCrashTest {
    public static void main(String[] args) {
//         System.out.println("=== C1 OSR Crash Test ===");
//         System.out.println("Testing OSR compilation on ARM64...");
//         System.out.println("(Run multiple times to stress OSR transitions)");
//         System.out.println();

        for (int iter = 0; iter < 20; iter++) {
//             System.out.println("Iteration " + iter);
            int result = hotLoop(iter);
//             System.out.println("  Result: " + result);
        }

//         System.out.println();
//         System.out.println("✓ SUCCESS - No crash detected!");
//         System.out.println("If the bug is still present, re-run or tweak CompileThreshold.");
    }

    /** Hot loop designed to trigger OSR after enough backedges. */
    static int hotLoop(int seed) {
        int sum = 0;
        for (int i = 0; i < 50000; i++) {
            sum += i * seed;
            if (sum > 1_000_000) {
                sum %= 1000;
            }
        }
        return sum;
    }
}

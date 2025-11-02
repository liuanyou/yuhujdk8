# TTY Thread-Safety Bug - SIGSEGV at pc=0x0

**Date**: November 2, 2025  
**Status**: ✅ RESOLVED - Fixed with Option 2 (direct file writing)  
**Severity**: HIGH  
**Platform**: macOS ARM64 (applies to all platforms)  
**JDK Version**: OpenJDK 8 custom build  

---

## Summary

A thread-safety bug occurs when the global `tty` output stream is modified by one thread while another thread attempts to use it, resulting in `SIGSEGV` with `pc=0x0`. The crash happens when:

1. Compiler thread redirects `tty` to a stack-local `fileStream` for debugging output
2. Main (Java) thread hits OSR compilation and tries to print trace information via `tty`
3. Main thread dereferences a stale/invalid `fileStream` vtable → crash

---

## Symptoms

### Crash Signature
```
# SIGSEGV (0xb) at pc=0x0000000000000000, pid=XXXXX, tid=0xXXXX

Registers:
x8  = 0x0000000000000000   ← NULL function pointer
...

Stack: [0xXXXX,0xXXXX],  sp=0xXXXX,  free space=XXXk
Native frames:
(No native frames - pc is 0x0)

Instructions: (pc=0x0)
[error occurred during error reporting (printing registers, top of stack, instructions near pc), id 0xb]
```

### Assembly at Crash Point
```asm
0x104390424: blr x8    ← Branch through x8, which is NULL
```

This typically happens in:
- Interpreter call resolution stub
- During virtual method dispatch for `outputStream::print_cr()`
- When `TraceOnStackReplacement` flag is enabled

---

## Root Cause Analysis

### The Race Condition

#### Thread A (Compiler Thread)
```cpp
// In compileBroker or C1 compiler
{
    ResourceMark rm;
    FILE* f = fopen("/path/to/debug/file.txt", "a");
    fileStream fs(f, true);          // Stack-local fileStream
    
    outputStream* saved = tty;        // Save original tty
    tty = &fs;                        // ⚠️ GLOBAL STATE MODIFIED
    
    // Write disassembly to file
    Disassembler::decode(nm->code_begin(), nm->code_end(), tty);
    
    tty = saved;                      // Restore tty
}  // fs goes out of scope - fileStream destroyed
```

#### Thread B (Main Java Thread)
```cpp
// In InterpreterRuntime::frequency_counter_overflow
if (TraceOnStackReplacement) {
    tty->print("OSR bci: %d @ %d", ...);   // ← tty might be &fs!
    nm->print();                            // ← Internally calls tty->print_cr()
    //       ↑
    //       └─ Dereferences fs's vtable, which is:
    //          - Stack memory from Thread A
    //          - Already destroyed/invalid
    //          - Results in NULL function pointer
}
```

### Why NULL Check Didn't Help

```cpp
if (nm != NULL) {
    nm->print();  // ← The crash is NOT from nm being NULL
                  // It's from tty pointing to dead memory!
}
```

The `nmethod*` is valid. The problem is **`tty` (global variable) points to invalid memory**.

### Timeline of Events

```
Time    Thread A (Compiler)              Thread B (Main)
----    -----------------------          -----------------------
T0      tty = original_tty               
T1      tty = &fs (stack local)          
T2      Writing to fs...                 OSR triggers
T3      Writing to fs...                 frequency_counter_overflow()
T4      Writing to fs...                 if (TraceOnStackReplacement) {
T5      tty = original_tty                   tty->print(...) ← tty still = &fs!
T6      } // fs destroyed                    nm->print()
T7                                           → calls tty->print_cr()
T8                                           → dereferences fs vtable (GARBAGE)
T9                                           → blr x8 with x8=0
T10                                          ☠️ CRASH
```

---

## Reproduction

### Prerequisites
- Build with slowdebug variant
- Custom debugging code that modifies `tty` in compiler thread
- Enable `-XX:+TraceOnStackReplacement`
- Run OSR-triggering workload

### Test Case
```bash
cd test
./run_osr_test.sh
```

With the `tty` modification code present, crash occurs **reliably on every run**.  
Without the code, crash **never occurs**.

### Minimal Reproducer
```java
public class OSRCrashTest {
    public static void main(String[] args) {
        for (int iter = 0; iter < 20; iter++) {
            hotLoop(iter);
        }
    }
    
    static int hotLoop(int seed) {
        int sum = 0;
        for (int i = 0; i < 50000; i++) {  // Triggers OSR
            sum += i * seed;
            if (sum > 1_000_000) {
                sum %= 1000;
            }
        }
        return sum;
    }
}
```

Run with:
```bash
java -XX:-UseOnStackReplacement \
     -XX:CompileThreshold=100 \
     -XX:+TraceOnStackReplacement \
     OSRCrashTest
```

---

## Technical Details

### Why `tty` is Dangerous

`tty` is a **global output stream** defined in `hotspot/src/share/vm/utilities/ostream.hpp`:

```cpp
extern outputStream* tty;  // Global console output stream
```

Multiple threads access it:
- Compiler threads (C1, C2)
- GC threads
- Java threads (via runtime calls)
- VM thread

### The Unsafe Pattern

```cpp
// ❌ UNSAFE: Modifies global state without locking
outputStream* saved = tty;
tty = &my_local_stream;
// ... do work ...
tty = saved;
```

Problems:
1. Other threads see the modified `tty` during the window
2. `my_local_stream` may be stack-allocated → dangling pointer
3. No synchronization → data races
4. Vtable dereference on destroyed object → crash

### The Safe Pattern

#### Option 1: Use `ttyLocker`
```cpp
// ✅ SAFE: Lock tty for exclusive access
{
    ttyLocker ttyl;  // Acquires tty_lock
    
    outputStream* saved = tty;
    tty = &my_stream;
    // ... do work ...
    tty = saved;
}  // ttyl destructor releases lock
```

#### Option 2: Don't Modify `tty` (Best!)
```cpp
// ✅ BEST: Write directly to your stream
{
    fileStream fs(file, true);
    
    // Write directly to fs, don't touch tty
    fs.print_cr("Output: %s", ...);
    Disassembler::decode(code_begin, code_end, &fs);
}
```

---

## Fix

**Applied Solution**: Option 2 (Direct File Writing) ✅

### Solution 1: Add `ttyLocker` (Alternative)

**File**: `hotspot/src/share/vm/compiler/compileBroker.cpp` (or wherever the code is)

```cpp
// Before (UNSAFE):
{
    ResourceMark rm;
    FILE* f = fopen("/Users/liuanyou/CLionProjects/jdk8/debug/c1_new_code.txt", "a");
    fileStream fs(f, true);
    outputStream* saved = tty;
    tty = &fs;
    
    const char* klassName = method->method_holder()->name()->as_utf8();
    const char* methodName = method()->name()->as_utf8();
    const char* signature = method()->signature()->as_utf8();
    tty->print_cr("new klassName %s methodName %s signature %s", klassName, methodName, signature);
    tty->print_cr("code: ");
    Disassembler::decode(nm->code_begin(), nm->code_end(), tty);
    
    tty = saved;
}

// After (SAFE):
{
    ResourceMark rm;
    ttyLocker ttyl;  // ← ADD THIS
    
    FILE* f = fopen("/Users/liuanyou/CLionProjects/jdk8/debug/c1_new_code.txt", "a");
    fileStream fs(f, true);
    outputStream* saved = tty;
    tty = &fs;
    
    const char* klassName = method->method_holder()->name()->as_utf8();
    const char* methodName = method()->name()->as_utf8();
    const char* signature = method()->signature()->as_utf8();
    tty->print_cr("new klassName %s methodName %s signature %s", klassName, methodName, signature);
    tty->print_cr("code: ");
    Disassembler::decode(nm->code_begin(), nm->code_end(), tty);
    
    tty = saved;
}
```

### Solution 2: Write Directly to File (RECOMMENDED) ✅ APPLIED

```cpp
// Best approach - don't touch tty at all
{
    ResourceMark rm;
    
    FILE* f = fopen("/Users/liuanyou/CLionProjects/jdk8/debug/c1_new_code.txt", "a");
    fileStream fs(f, true);
    
    const char* klassName = method->method_holder()->name()->as_utf8();
    const char* methodName = method()->name()->as_utf8();
    const char* signature = method()->signature()->as_utf8();
    
    // Write directly to fs, not tty
    fs.print_cr("new klassName %s methodName %s signature %s", klassName, methodName, signature);
    fs.print_cr("code: ");
    Disassembler::decode(nm->code_begin(), nm->code_end(), &fs);
    
    // fileStream destructor closes file
}
```

**Why Solution 2 is better:**
- No global state modification
- No locking overhead
- No risk of other threads seeing redirected `tty`
- Cleaner and more maintainable

**Implementation Status**: ✅ **Applied and verified** - All 20 test runs complete successfully with 0% crash rate.

---

## Verification

### Before Fix
```bash
$ ./test/run_osr_test.sh
=== Run 1/20 ===
# A fatal error has been detected by the Java Runtime Environment:
# SIGSEGV (0xb) at pc=0x0000000000000000
```

**Result**: Crashes on first or second run, 100% reproduction rate

### After Fix
```bash
$ ./test/run_osr_test.sh
=== Run 1/20 ===
✓ Run 1 completed successfully
=== Run 2/20 ===
✓ Run 2 completed successfully
...
=== Run 20/20 ===
✓ Run 20 completed successfully

=== Test Summary ===
Total runs: 20
Crashes: 0
Success: 20
Crash rate: 0%

✓✓✓ All runs successful - bug likely fixed or not triggered
```

**Result**: All 20 runs complete successfully, 0% crash rate

---

## Related Issues

### Similar Bugs in HotSpot History
- [JDK-8XXX] Various `tty` race conditions in error reporting
- [JDK-8XXX] Crashes during compilation logging with `-XX:+PrintCompilation`
- [JDK-8XXX] Thread-safety issues with `ttyLocker`

### Related Code Sections
- `hotspot/src/share/vm/utilities/ostream.hpp` - `tty` declaration
- `hotspot/src/share/vm/utilities/ostream.cpp` - `ttyLocker` implementation
- `hotspot/src/share/vm/interpreter/interpreterRuntime.cpp` - OSR tracing code
- `hotspot/src/share/vm/compiler/compileBroker.cpp` - Compilation logging

---

## Lessons Learned

1. **Never modify `tty` without `ttyLocker`** - It's a global shared by all threads
2. **Prefer local streams over `tty` redirection** - Avoids global state modification
3. **Stack-local objects + global pointers = danger** - Classic dangling pointer bug
4. **Debug code can introduce production bugs** - Even conditional logging can cause races
5. **NULL checks don't prevent all crashes** - The pointer was valid, but pointed to garbage

---

## Action Items

- [x] Locate all instances of `tty = &local_stream` in codebase
- [x] Refactor to use local streams (Solution 2 applied)
- [x] Verify fix with comprehensive testing (20 runs, 0 crashes)
- [ ] Review all uses of `TraceOnStackReplacement` flag (for other potential issues)
- [ ] Consider adding static analysis to detect unsafe `tty` modifications
- [ ] Update coding guidelines to warn about `tty` thread-safety

---

## References

- **Test Script**: `doc/bugs/C1_OSR_SIGSEGV/run_osr_test.sh`
- **Test Case**: `doc/bugs/C1_OSR_SIGSEGV/OSRCrashTest.java`
- **Related Bug**: `doc/bugs/2025-10-30_C1_OSR_SIGBUS_Bug.md` (Original OSR frame bug)
- **HotSpot Wiki**: [Output Stream Guidelines](https://wiki.openjdk.java.net/display/HotSpot/OutputStreams)

---

## Timeline

- **2025-11-02**: Bug discovered during OSR debugging
- **2025-11-02**: Root cause identified as `tty` race condition
- **2025-11-02**: Fix strategy determined (use `ttyLocker` or local streams)
- **2025-11-02**: Solution 2 (direct file writing) implemented and verified
- **Status**: ✅ **RESOLVED** - Fix confirmed with 20 successful test runs

---

**Debugged by**: User (liuanyou)  
**Platform**: macOS ARM64  
**Build**: slowdebug variant


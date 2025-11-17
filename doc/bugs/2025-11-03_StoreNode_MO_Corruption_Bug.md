# StoreNode _mo Field Corruption in Node::clone()

**Date**: November 3, 2025  
**Status**: ✅ RESOLVED  
**Severity**: HIGH  
**Platform**: All platforms (architecture-independent)  
**JDK Version**: OpenJDK 8 custom build  
**Compiler**: C2 (Server Compiler)

---

## Summary

A critical bug in C2's loop optimization phase caused `StoreNode`'s `_mo` (memory ordering) field to be corrupted when nodes were cloned during loop transformations. The corruption occurred because `StoreNode` did not override `Node::size_of()`, causing `Node::clone()` to copy only `sizeof(Node)` bytes instead of `sizeof(StoreNode)`, leaving the `_mo` field uninitialized.

**Root Cause**: `StoreNode` lacks `size_of()` override, so `Node::clone()` only copies base `Node` size, missing `_mo` field.

**Resolution**: Added `StoreNode::size_of()` override returning `sizeof(StoreNode)` to ensure complete object copy during cloning.

---

## Symptoms

### Crash Signature
```
# A fatal error has been detected by the Java Runtime Environment:
#
#  Internal Error (/Users/.../hotspot/src/share/vm/opto/memnode.hpp:480), pid=XXXXX, tid=YYYYY
#  assert((_mo == unordered || _mo == release)) failed: unexpected
#
# JRE version: OpenJDK Runtime Environment (8.0)
# Java VM: OpenJDK 64-Bit Server VM (25.0-b70-debug mixed mode)
```

### When It Occurs
- During C2 compilation of methods with loops (e.g., `sun.nio.cs.UTF_8$Encoder.encode`)
- Specifically in `PhaseIdealLoop::partial_peel()` → `clone_loop()` → `Node::clone()`
- Only affects `StoreNode` and its subclasses (`StoreBNode`, `StoreINode`, etc.)
- Corruption happens immediately after `clone_loop()` completes

### Example Debug Output
```
[partial_peel before clone_loop] StoreB idx=747 ... [does not exist yet]
[partial_peel after clone_loop] StoreB idx=747 this=0x... mo=1373341840 [CORRUPTED]
```

The `_mo` value `1373341840` is clearly garbage (should be `0` for `unordered` or `1` for `release`).

---

## Root Cause Analysis

### The Problem: Incomplete Object Copy

#### Node::clone() Implementation
```cpp
// hotspot/src/share/vm/opto/node.cpp
Node *Node::clone() const {
  Compile* C = Compile::current();
  uint s = size_of();           // ← Gets sizeof(Node), not sizeof(StoreNode)!
  Node *n = (Node*)C->node_arena()->Amalloc_D(size_of() + _max*sizeof(Node*));
  Copy::conjoint_words_to_lower((HeapWord*)this, (HeapWord*)n, s);
  // Set the new input pointer array
  n->_in = (Node**)(((char*)n)+s);
  // ...
}
```

#### StoreNode Memory Layout
```cpp
// hotspot/src/share/vm/opto/memnode.hpp
class StoreNode : public MemNode {
private:
  const MemOrd _mo;  // ← This field is AFTER the base Node fields
  // ...
};
```

#### What Happens During Clone

1. **Original StoreNode** (valid):
   ```
   [Node base fields] [MemNode fields] [_mo = 0] [padding] [_in[] array]
   ```

2. **Node::clone() called on StoreNode**:
   - `s = size_of()` → returns `sizeof(Node)` (not `sizeof(StoreNode)`)
   - `Copy::conjoint_words_to_lower()` copies only `sizeof(Node)` bytes
   - `_mo` field is **NOT copied** (it's beyond the copied region)

3. **Cloned StoreNode** (corrupted):
   ```
   [Node base fields] [MemNode fields] [_mo = GARBAGE] [padding] [_in[] array]
                                                          ↑
                                              Uninitialized memory!
   ```

4. **When `_in[]` array is repositioned**:
   ```cpp
   n->_in = (Node**)(((char*)n)+s);  // s = sizeof(Node)
   ```
   The `_in[]` array is placed right after the base `Node`, potentially overlapping with `_mo` or leaving it in uninitialized memory.

### Why It Only Affects StoreNode

- `StoreNode` is the only `Node` subclass that adds a **non-pointer field** (`_mo`) after the base `Node` structure
- Other node types either:
  - Don't add fields (use base `Node::size_of()` correctly)
  - Add only pointer fields (which are handled separately)
  - Override `size_of()` properly (e.g., `LoadStoreNode`)

### When It Triggers

The bug manifests during loop optimizations that clone nodes:

1. **`PhaseIdealLoop::partial_peel()`** - Partially peels one iteration of a loop
2. **`PhaseIdealLoop::clone_loop()`** - Clones entire loop bodies
3. **`Node::clone()`** - Called for each node in the loop body
4. **`StoreNode::clone()`** - Falls back to `Node::clone()`, which doesn't copy `_mo`

### Investigation Timeline

1. **Initial Symptom**: `assert((_mo == unordered || _mo == release))` failure in `StoreNode::is_release()`
2. **Replay File Generation**: Used `-XX:+LogCompilation` to generate replay data for deterministic reproduction
3. **Debugging**: Added `walk_from_root_and_dump_all_storeb_mo()` to track `_mo` values throughout compilation
4. **Pinpointing**: Found `_mo` corruption occurs immediately after `clone_loop()` in `partial_peel()`
5. **Root Cause**: Identified that `StoreNode` doesn't override `size_of()`, causing incomplete copy
6. **Fix**: Added `StoreNode::size_of()` override returning `sizeof(StoreNode)`

---

## Using Replay Files for Investigation

Replay files were crucial for debugging this issue, providing deterministic reproduction and enabling precise breakpoint placement.

### What Are Replay Files?

Replay files (`.log` files generated by `-XX:+LogCompilation`) record compilation events in a deterministic format. They allow you to:
- **Reproduce the exact same compilation** multiple times
- **Set breakpoints at specific compilation phases** without waiting for warmup
- **Target specific methods** for compilation using `CompileCommand=compileonly,...`
- **Debug with LLDB/GDB** in a reproducible environment

### Generating Replay Files

#### Step 1: Enable Compilation Logging
```bash
java -XX:+LogCompilation \
     -XX:LogFile=/tmp/replay_pidXXXX.log \
     -XX:CompileCommand=compileonly,sun/nio/cs/UTF_8$Encoder::encode \
     YourApplication
```

This generates `/tmp/replay_pidXXXX.log` containing compilation metadata.

#### Step 2: Replay the Compilation
```bash
java -XX:+UnlockDiagnosticVMOptions \
     -XX:ReplayDataFile=/tmp/replay_pidXXXX.log \
     -XX:+ReplayCompiles \
     -XX:CompileCommand=compileonly,sun/nio/cs/UTF_8$Encoder::encode \
     -XX:-TieredCompilation \
     -XX:-BackgroundCompilation \
     -Xbatch \
     YourApplication
```

**Key Flags**:
- `-XX:+ReplayCompiles` - Enable replay mode
- `-XX:ReplayDataFile=...` - Path to replay log file
- `-XX:CompileCommand=compileonly,...` - Target specific method
- `-XX:-TieredCompilation` - Disable tiered compilation for simplicity
- `-XX:-BackgroundCompilation` - Compile synchronously
- `-Xbatch` - Compile all methods before execution

### Debugging Workflow with Replay

#### 1. Setting Up Debugger Breakpoints

Since replay files provide deterministic compilation, you can set breakpoints at specific phases:

```bash
lldb java
(lldb) settings set target.process.stop-on-sharedlibrary-events true
(lldb) break set -n StoreNode::is_release
(lldb) run -XX:+ReplayCompiles -XX:ReplayDataFile=/tmp/replay_pidXXXX.log ...
```

#### 2. Capturing the Corrupted Node

When the assert fires, capture the `this` pointer:

```cpp
// In StoreNode::is_release()
if (!(_mo == unordered || _mo == release)) {
  debug_compile_id = C->compile_id();
  debug_target = const_cast<StoreNode*>(this);  // ← Record this pointer
  os::breakpoint();  // Stop in debugger
}
```

#### 3. Using Watchpoints

With the `this` pointer from the breakpoint, set a watchpoint to catch when `_mo` gets corrupted:

```bash
(lldb) # After hitting the breakpoint, note the this pointer
(lldb) p debug_target
(StoreNode*) $0 = 0x0000000143047590

(lldb) # Set watchpoint on _mo field
(lldb) watchpoint set expression -w write -- &(((StoreNode*)0x0000000143047590)->_mo)
```

**Note**: The watchpoint address changes between runs, so you need to:
1. Run once to hit the breakpoint and get the current `this` pointer
2. Kill the process (`process kill`)
3. Set the watchpoint with the current address
4. Run again to catch the corruption

#### 4. Targeting Specific Compilation

Replay files allow targeting a specific `compile_id`:

```bash
java -XX:+ReplayCompiles \
     -XX:ReplayDataFile=/tmp/replay_pidXXXX.log \
     -XX:CompileCommand=compileonly,sun/nio/cs/UTF_8$Encoder::encode \
     -XX:CompileCommand=option,sun/nio/cs/UTF_8$Encoder::encode,PrintIdealGraphFile=/tmp/encode.xml \
     ...
```

This ensures:
- Only the target method is compiled
- Ideal Graph is dumped to XML for analysis
- Compilation is deterministic and reproducible

### Benefits of Replay Files

1. **Deterministic Reproduction**: Same compilation every time, making debugging predictable
2. **Fast Iteration**: No need to wait for warmup or trigger conditions
3. **Precise Targeting**: Compile only the problematic method
4. **Ideal Graph Analysis**: Generate XML dumps for specific compilations
5. **Watchpoint Feasibility**: With deterministic addresses, watchpoints become practical

### Limitations

1. **Address Changes**: Node addresses change between runs, requiring breakpoint → watchpoint workflow
2. **Arena Allocation**: Nodes are allocated from arenas, so addresses are not stable across runs
3. **Solution**: Use static `debug_target` pointer captured at breakpoint time

### Example Replay Command Used

```bash
java -XX:+UnlockDiagnosticVMOptions \
     -XX:ReplayDataFile=/var/folders/.../replay_pid28292.log \
     -XX:CompileCommand=compileonly,sun/nio/cs/UTF_8$Encoder.encode \
     -XX:PrintIdealGraphLevel=4 \
     -XX:PrintIdealGraphFile=/Users/.../encode.xml \
     -XX:+ReplayCompiles \
     -XX:-TieredCompilation \
     -XX:+PrintCompilation \
     -XX:-BackgroundCompilation \
     -Xbatch \
     YourApplication
```

This command:
- Replays the exact compilation that triggered the bug
- Targets only `UTF_8$Encoder.encode` method
- Generates Ideal Graph XML for analysis
- Compiles synchronously for easier debugging

### Using dump_spec to Track _mo Across Phases

The `dump_spec()` method was instrumental in identifying exactly which compilation phase corrupted the `_mo` field.

#### Adding _mo to dump_spec()

First, we modified `StoreNode::dump_spec()` to include the `_mo` value in the output:

**File**: `hotspot/src/share/vm/opto/memnode.cpp`
```cpp
void StoreNode::dump_spec(outputStream *st) const {
  // First print generic MemNode address/alias info
  MemNode::dump_spec(st);
  // Then append the MemOrd (_mo) value
  st->print(" mo=%d", (int)_mo);
}
```

This makes `_mo` visible in:
- `-XX:+PrintIdeal` text output
- Ideal Graph XML files (`-XX:+PrintIdealGraphFile`)
- IGV (Ideal Graph Visualizer) `dump_spec` property

#### Analyzing Ideal Graph XML

With `_mo` in `dump_spec`, we generated Ideal Graph XML files at different compilation phases:

```bash
java -XX:+PrintIdealGraphFile=/tmp/encode.xml \
     -XX:PrintIdealGraphLevel=4 \
     -XX:CompileCommand=compileonly,sun/nio/cs/UTF_8$Encoder::encode \
     ...
```

The XML files contain entries like:
```xml
<node id="747" name="StoreB">
  <property name="dump_spec" value="mo=0" />  <!-- Before corruption -->
  ...
</node>
```

#### Phase-by-Phase Analysis

By comparing `dump_spec` values across different phases in the XML files, we identified:

1. **Before PhaseIdealLoop 2**: All `StoreB` nodes show `mo=0` (correct)
2. **After PhaseIdealLoop 2**: Some `StoreB` nodes show abnormal values like `mo=1714882704`, `mo=1722495928` (corrupted)
3. **Before Matcher**: Corruption already present

This narrowed down the corruption to **PhaseIdealLoop 2**, specifically within the loop optimization phase.

#### Finding the Exact Sub-Phase

Using `PrintIdealGraphFile` with phase-specific instrumentation:

```bash
# Generate XML at specific phases
-XX:PrintIdealGraphFile=/tmp/encode_before_phase2.xml  # Before PhaseIdealLoop 2
-XX:PrintIdealGraphFile=/tmp/encode_after_phase2.xml   # After PhaseIdealLoop 2
```

Then comparing the XML files:
```bash
grep "StoreB.*mo=" encode_before_phase2.xml | head -5
# <node id="747" name="StoreB"><property name="dump_spec" value="mo=0" />

grep "StoreB.*mo=" encode_after_phase2.xml | head -5
# <node id="747" name="StoreB"><property name="dump_spec" value="mo=1373341840" />
```

This confirmed corruption occurs **during PhaseIdealLoop 2**.

#### Using IGV to Visualize

With Ideal Graph Visualizer (IGV), we could:
1. Open `encode.xml` files from different phases
2. Filter for `StoreB` nodes
3. View the `dump_spec` property showing `mo` values
4. Compare graphs before/after `PhaseIdealLoop 2` to see which nodes got corrupted

#### Combining with Instrumentation

We combined `dump_spec` analysis with runtime instrumentation:

```cpp
// In compile.cpp
C->walk_from_root_and_dump_all_storeb_mo("[PhaseIdealLoop 2 entry] ");
// ... PhaseIdealLoop 2 execution ...
C->walk_from_root_and_dump_all_storeb_mo("[PhaseIdealLoop 2 exit] ");
```

This provided:
- **Runtime verification**: Confirmed `_mo` values at specific points
- **XML correlation**: Cross-referenced with Ideal Graph XML `dump_spec` values
- **Phase narrowing**: Identified `partial_peel()` → `clone_loop()` as the exact corruption point

#### Key Insight from dump_spec

The `dump_spec` output revealed that:
- Corruption happens **after** `PhaseIdealLoop 2` starts
- Corruption happens **before** the Matcher phase
- The corrupted `_mo` values are large integers (memory addresses or uninitialized data), not valid `MemOrd` enum values

This led us to suspect memory copying issues, which eventually pointed to `Node::clone()`.

### How Replay Files Helped

1. **Isolated the Problem**: Replay allowed focusing on a single method compilation
2. **Deterministic Debugging**: Same compilation every run, making breakpoints reliable
3. **Ideal Graph Analysis**: Generated `encode.xml` to visualize when `_mo` corruption occurred
4. **Phase Identification**: By replaying and adding instrumentation, we identified `partial_peel()` → `clone_loop()` as the corruption point
5. **Verification**: After fix, replay confirmed `_mo` values remain correct throughout compilation

### How dump_spec Helped

1. **Phase Narrowing**: `dump_spec` in Ideal Graph XML showed corruption occurs during `PhaseIdealLoop 2`
2. **Visual Analysis**: IGV visualization made it easy to spot corrupted nodes across phases
3. **Correlation**: Combined with runtime instrumentation to verify exact corruption point
4. **Root Cause Clue**: Abnormal `_mo` values (large integers) suggested memory copying issues
5. **Verification**: After fix, `dump_spec` showed all `mo=0` values throughout compilation

---

## Resolution

### The Fix

**File**: `hotspot/src/share/vm/opto/memnode.hpp`
```cpp
class StoreNode : public MemNode {
  // ...
public:
  // Override size_of() to include _mo field in clone()
  virtual uint size_of() const;
  // ...
};
```

**File**: `hotspot/src/share/vm/opto/memnode.cpp`
```cpp
//------------------------------size_of-----------------------------------------
uint StoreNode::size_of() const {
  return sizeof(*this);
}
```

### Why This Works

1. **Before Fix**:
   - `StoreNode::clone()` → `Node::clone()` → `size_of()` → `sizeof(Node)`
   - Only base `Node` fields copied, `_mo` left uninitialized

2. **After Fix**:
   - `StoreNode::clone()` → `Node::clone()` → `size_of()` → `sizeof(StoreNode)` (virtual dispatch)
   - Entire `StoreNode` object copied, including `_mo` field

### Verification

**Before Fix**:
```
[partial_peel before clone_loop] StoreB idx=747 ... [does not exist]
[partial_peel after clone_loop] StoreB idx=747 this=0x... mo=1373341840 [CORRUPTED]
```

**After Fix**:
```
[partial_peel before clone_loop] StoreB idx=747 ... [does not exist]
[partial_peel after clone_loop] StoreB idx=747 this=0x... mo=0 [OK]
```

**Test Results**:
- ✅ `sun.nio.cs.UTF_8$Encoder.encode` compiles successfully
- ✅ All `StoreBNode` instances maintain correct `_mo` values throughout compilation
- ✅ No more `assert((_mo == unordered || _mo == release))` failures
- ✅ Program completes normally (`Process finished with exit code 0`)

---

## Technical Details

### Memory Ordering Semantics

The `_mo` field in `StoreNode` represents memory ordering semantics:
- `MemOrd::unordered` (0) - Normal store, can be reordered
- `MemOrd::release` (1) - Release semantics, ensures visibility to other threads

This field is critical for:
- Java memory model compliance
- Correct synchronization behavior
- Preventing data races in concurrent code

### Why It Matters

When `_mo` is corrupted:
1. `StoreNode::is_release()` may return incorrect values
2. Code generator may emit wrong memory barriers
3. JVM may violate Java memory model guarantees
4. Concurrent programs may exhibit data races or incorrect behavior

### Clone Usage in C2

`Node::clone()` is used extensively in C2 optimizations:
- Loop peeling/unrolling (`clone_loop`)
- Loop unswitching
- Partial loop peeling (`partial_peel`)
- Pre/main/post loop generation

Any `StoreNode` in a loop body that gets cloned will have its `_mo` corrupted without this fix.

---

## Lessons Learned

1. **Always override `size_of()` for subclasses that add fields**: If a `Node` subclass adds non-pointer fields, it must override `size_of()` to return the correct size.

2. **Virtual dispatch in `clone()`**: `Node::clone()` calls `size_of()` as a virtual method, so subclasses can override it. This is the correct mechanism to use.

3. **Memory layout matters**: When using `memcpy`-style copying (`Copy::conjoint_words_to_lower`), the size parameter is critical. Copying too little leaves fields uninitialized.

4. **Debugging with targeted instrumentation**: Adding `walk_from_root_and_dump_all_storeb_mo()` helped quickly pinpoint the exact phase where corruption occurred.

5. **Test with real workloads**: The bug was discovered when compiling `UTF_8$Encoder.encode`, a complex method with loops and memory operations.

---

## Related Code Locations

- **Bug Location**: `hotspot/src/share/vm/opto/node.cpp::Node::clone()`
- **Missing Override**: `hotspot/src/share/vm/opto/memnode.hpp::StoreNode` (no `size_of()`)
- **Fix Location**: 
  - `hotspot/src/share/vm/opto/memnode.hpp` (declaration)
  - `hotspot/src/share/vm/opto/memnode.cpp` (implementation)
- **Trigger Location**: `hotspot/src/share/vm/opto/loopopts.cpp::clone_loop()`

---

## References

- Similar pattern: `LoadStoreNode::size_of()` correctly overrides to return `sizeof(*this)`
- Related issue: C1 OSR frame construction bug (`2025-10-30_C1_OSR_SIGBUS_Bug.md`)
- Related issue: TTY thread-safety bug (`2025-11-02_TTY_THREAD_SAFETY_BUG.md`)

---

## Status

✅ **FIXED** - `StoreNode::size_of()` override added, `_mo` corruption eliminated.

**Verification**: Tested with `sun.nio.cs.UTF_8$Encoder.encode` compilation, all `StoreNode` instances maintain correct `_mo` values throughout the compilation pipeline.


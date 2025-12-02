# Yuhu JIT Compiler Implementation Plan (LLVM-Based)

**Objective**: Create a Yuhu JIT compiler using LLVM that can function as an alternative to C1 and C2 compilers, with the same integration points and capabilities.

**Architecture**: LLVM-based JIT compiler **based on Shark compiler implementation**, adapted for AArch64 platform and modern LLVM versions.

**Implementation Strategy**: **Direct migration from Shark** - Copy Shark's proven architecture and adapt it for Yuhu.

## 1. Architecture Overview

The Yuhu JIT compiler should integrate into the existing compilation framework as follows:

```
CompileBroker
├── _compilers[0] → C1 Compiler (slot 0)
├── _compilers[1] → C2 Compiler (slot 1) 
└── _compilers[2] → NEW: Yuhu JIT Compiler (slot 2)
```

### Key Integration Points

1. **Compiler Registration**: Add to `CompileBroker::compilation_init()`
2. **Compilation Levels**: Extend `CompLevel` enum for Yuhu compilation tiers
3. **Compiler Interface**: Implement `AbstractCompiler` interface
4. **Code Generation**: Leverage existing Yuhu infrastructure (MacroAssembler, TemplateTable)

## 2. Implementation Strategy: Based on Shark Compiler

### Decision: Use Shark's Proven Architecture

**We will directly migrate Shark's implementation to Yuhu**, adapting it for:
- AArch64 platform (instead of Zero)
- Modern LLVM versions (6.x - 10+)
- Yuhu naming conventions

### Why Shark?

1. **Proven Architecture**: Shark is a complete, working LLVM-based JIT compiler in HotSpot
2. **Full Implementation**: All components are implemented and tested
3. **Thread Safety**: Proper handling of LLVM/HotSpot lock interactions
4. **Memory Management**: Complete integration with HotSpot code cache
5. **OSR Support**: Full on-stack replacement support
6. **Error Handling**: Robust error handling and deoptimization

### Key Advantages of Using LLVM

1. **Proven Architecture**: Shark compiler demonstrates successful LLVM integration
2. **Optimization Pipeline**: Leverage LLVM's extensive optimization passes
3. **Cross-Platform**: Inherit platform support from LLVM
4. **Maintenance**: Reduce custom optimization code maintenance burden
5. **Unified IR**: LLVM has only one IR, simplifying the architecture compared to C1 (HIR+LIR) and C2 (Ideal Graph)

### Understanding IR Differences

**C1 Compiler IR**:
- **HIR (High-level IR)**: Instruction-based hierarchy (`Instruction`, `LoadField`, `StoreField`, etc.)
- **LIR (Low-level IR)**: LIR_Op-based hierarchy (`LIR_Op0`, `LIR_Op1`, `LIR_OpCall`, etc.)

**C2 Compiler IR**:
- **Ideal Graph IR**: Node-based graph structure (`Node`, `AddNode`, `LoadNode`, `CallNode`, etc.)
- SSA form, supports global optimizations

**LLVM IR (Shark/Yuhu)**:
- **Single unified IR**: SSA form, similar to C2's Ideal Graph but more general
- **Key insight**: We can use one LLVM IR with different optimization Pass configurations to achieve both C1 (fast) and C2 (optimized) behaviors

### Phase 1: Migrate Shark to Yuhu

#### 2.1 Migration Strategy

**Step 1: Copy Shark Files**

```bash
# Copy all Shark files to Yuhu directory
cp -r hotspot/src/share/vm/shark/* hotspot/src/share/vm/yuhu/

# Platform-specific files (if any)
cp -r hotspot/src/cpu/zero/vm/shark* hotspot/src/cpu/aarch64/vm/yuhu* 2>/dev/null || true
```

**Step 2: Rename Classes and Files**

```bash
# Batch rename: Shark → Yuhu
# Use sed or similar tool to replace:
# - SharkCompiler → YuhuCompiler
# - SharkContext → YuhuContext
# - SharkBuilder → YuhuBuilder
# - SharkFunction → YuhuFunction
# - SharkMemoryManager → YuhuMemoryManager
# - shark → yuhu (in file names)
# - SHARK → YUHU (in macros)
```

**Step 3: Create Yuhu Compiler Class (Based on Shark)**

**File**: `hotspot/src/share/vm/yuhu/yuhu_Compiler.hpp`
```cpp
#ifndef SHARE_VM_YUHU_YUHU_COMPILER_HPP
#define SHARE_VM_YUHU_YUHU_COMPILER_HPP

#include "ci/ciEnv.hpp"
#include "ci/ciMethod.hpp"
#include "compiler/abstractCompiler.hpp"
#include "compiler/compileBroker.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/yuhuMemoryManager.hpp"

class YuhuContext;

class YuhuCompiler : public AbstractCompiler {
 public:
  // Creation
  YuhuCompiler();

  // Name of this compiler
  const char *name() { return "Yuhu"; }

  // Compiler identification
  virtual bool is_yuhu() { return true; }
  virtual bool is_c1() { return false; }
  virtual bool is_c2() { return false; }
  virtual bool is_shark() { return false; }

  // Missing feature tests
  virtual bool supports_native() { return true; }
  virtual bool supports_osr() { return true; }
  virtual bool can_compile_method(methodHandle method) {
    return !(method->is_method_handle_intrinsic() || method->is_compiled_lambda_form());
  }

  // Initialization
  void initialize();

  // Compile a normal (bytecode) method and install it in the VM
  void compile_method(ciEnv* env, ciMethod* target, int entry_bci);

  // Generate a wrapper for a native (JNI) method
  nmethod* generate_native_wrapper(MacroAssembler* masm,
                                   methodHandle    target,
                                   int             compile_id,
                                   BasicType*      arg_types,
                                   BasicType       return_type);

  // Free compiled methods (and native wrappers)
  void free_compiled_method(address code);

  // Dual context pattern (from Shark)
 private:
  YuhuContext* _normal_context;  // For compiler threads
  YuhuContext* _native_context;   // For adapter generation

 public:
  YuhuContext* context() const {
    if (JavaThread::current()->is_Compiler_thread()) {
      return _normal_context;
    } else {
      assert(AdapterHandlerLibrary_lock->owned_by_self(), "should be");
      return _native_context;
    }
  }

  // LLVM execution engine (thread-safe with custom lock)
 private:
  Monitor*               _execution_engine_lock;
  YuhuMemoryManager*    _memory_manager;
  llvm::ExecutionEngine* _execution_engine;

 private:
  Monitor* execution_engine_lock() const {
    return _execution_engine_lock;
  }
  YuhuMemoryManager* memory_manager() const {
    assert(execution_engine_lock()->owned_by_self(), "should be");
    return _memory_manager;
  }
  llvm::ExecutionEngine* execution_engine() const {
    assert(execution_engine_lock()->owned_by_self(), "should be");
    return _execution_engine;
  }

  // Global access
 public:
  static YuhuCompiler* compiler() {
    AbstractCompiler *compiler =
      CompileBroker::compiler(CompLevel_full_optimization);
    assert(compiler->is_yuhu() && compiler->is_initialized(), "should be");
    return (YuhuCompiler *) compiler;
  }

  // Helpers
 private:
  static const char* methodname(const char* klass, const char* method);
  void generate_native_code(YuhuEntry*     entry,
                            llvm::Function* function,
                            const char*     name);
  void free_queued_methods();
};

#endif // SHARE_VM_YUHU_YUHU_COMPILER_HPP
```

**File**: `hotspot/src/share/vm/yuhu/yuhu_Compiler.cpp`
```cpp
#include "precompiled.hpp"
#include "ci/ciEnv.hpp"
#include "ci/ciMethod.hpp"
#include "code/debugInfoRec.hpp"
#include "code/dependencies.hpp"
#include "code/exceptionHandlerTable.hpp"
#include "code/oopRecorder.hpp"
#include "compiler/abstractCompiler.hpp"
#include "compiler/oopMap.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/yuhuBuilder.hpp"
#include "yuhu/yuhuCodeBuffer.hpp"
#include "yuhu/yuhuCompiler.hpp"
#include "yuhu/yuhuContext.hpp"
#include "yuhu/yuhuEntry.hpp"
#include "yuhu/yuhuFunction.hpp"
#include "yuhu/yuhuMemoryManager.hpp"
#include "yuhu/yuhu_globals.hpp"
#include "utilities/debug.hpp"

#include <fnmatch.h>

using namespace llvm;

namespace {
  cl::opt<std::string>
  MCPU("mcpu");

  cl::list<std::string>
  MAttrs("mattr",
         cl::CommaSeparated);
}

YuhuCompiler::YuhuCompiler()
  : AbstractCompiler() {
  // Create the lock to protect the memory manager and execution engine
  _execution_engine_lock = new Monitor(Mutex::leaf, "YuhuExecutionEngineLock");
  MutexLocker locker(execution_engine_lock());

  // Make LLVM safe for multithreading
  if (!llvm_start_multithreaded())
    fatal("llvm_start_multithreaded() failed");

  // Initialize the native target (AArch64)
#ifdef TARGET_ARCH_aarch64
  InitializeAArch64Target();
  InitializeAArch64TargetAsmPrinter();
#else
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
#endif

  // Create the two contexts which we'll use
  _normal_context = new YuhuContext("normal");
  _native_context = new YuhuContext("native");

  // Create the memory manager
  _memory_manager = new YuhuMemoryManager();

  // Finetune LLVM for the current host CPU
  StringMap<bool> Features;
  bool gotCpuFeatures = llvm::sys::getHostCPUFeatures(Features);
  std::string cpu("-mcpu=" + llvm::sys::getHostCPUName());

  std::vector<const char*> args;
  args.push_back(""); // program name
  args.push_back(cpu.c_str());

  std::string mattr("-mattr=");
  if(gotCpuFeatures){
    for(StringMap<bool>::iterator I = Features.begin(),
      E = Features.end(); I != E; ++I){
      if(I->second){
        std::string attr(I->first());
        mattr+="+"+attr+",";
      }
    }
    args.push_back(mattr.c_str());
  }

  args.push_back(0);  // terminator
  cl::ParseCommandLineOptions(args.size() - 1, (char **) &args[0]);

  // Create the JIT
  std::string ErrorMsg;

  EngineBuilder builder(_normal_context->module());
  builder.setMCPU(MCPU);
  builder.setMAttrs(MAttrs);
  builder.setJITMemoryManager(memory_manager());
  builder.setEngineKind(EngineKind::JIT);
  builder.setErrorStr(&ErrorMsg);
  
  // Configure optimization level (can be set via flag)
  if (! fnmatch(YuhuOptimizationLevel, "None", 0)) {
    builder.setOptLevel(llvm::CodeGenOpt::None);
  } else if (! fnmatch(YuhuOptimizationLevel, "Less", 0)) {
    builder.setOptLevel(llvm::CodeGenOpt::Less);
  } else if (! fnmatch(YuhuOptimizationLevel, "Aggressive", 0)) {
    builder.setOptLevel(llvm::CodeGenOpt::Aggressive);
  }
  
  _execution_engine = builder.create();

  if (!execution_engine()) {
    if (!ErrorMsg.empty())
      printf("Error while creating Yuhu JIT: %s\n",ErrorMsg.c_str());
    else
      printf("Unknown error while creating Yuhu JIT\n");
    exit(1);
  }

  execution_engine()->addModule(_native_context->module());

  // All done
  set_state(initialized);
}

void YuhuCompiler::initialize() {
  ShouldNotCallThis();
}

void YuhuCompiler::compile_method(ciEnv*    env,
                                   ciMethod* target,
                                   int       entry_bci) {
  assert(is_initialized(), "should be");
  ResourceMark rm;
  const char *name = methodname(
    target->holder()->name()->as_utf8(), target->name()->as_utf8());

  // Do the typeflow analysis
  ciTypeFlow *flow;
  if (entry_bci == InvocationEntryBci)
    flow = target->get_flow_analysis();
  else
    flow = target->get_osr_flow_analysis(entry_bci);
  if (flow->failing())
    return;

  // Create the recorders
  Arena arena;
  env->set_oop_recorder(new OopRecorder(&arena));
  OopMapSet oopmaps;
  env->set_debug_info(new DebugInformationRecorder(env->oop_recorder()));
  env->debug_info()->set_oopmaps(&oopmaps);
  env->set_dependencies(new Dependencies(env));

  // Create the code buffer and builder
  CodeBuffer hscb("Yuhu", 256 * K, 64 * K);
  hscb.initialize_oop_recorder(env->oop_recorder());
  MacroAssembler *masm = new MacroAssembler(&hscb);
  YuhuCodeBuffer cb(masm);
  YuhuBuilder builder(&cb);

  // Emit the entry point
  YuhuEntry *entry = (YuhuEntry *) cb.malloc(sizeof(YuhuEntry));

  // Build the LLVM IR for the method
  Function *function = YuhuFunction::build(env, &builder, flow, name);
  if (env->failing()) {
    return;
  }

  // Generate native code
  {
    ThreadInVMfromNative tiv(JavaThread::current());
    generate_native_code(entry, function, name);
  }

  // Install the method into the VM
  CodeOffsets offsets;
  offsets.set_value(CodeOffsets::Deopt, 0);
  offsets.set_value(CodeOffsets::Exceptions, 0);
  offsets.set_value(CodeOffsets::Verified_Entry,
                    target->is_static() ? 0 : wordSize);

  ExceptionHandlerTable handler_table;
  ImplicitExceptionTable inc_table;

  env->register_method(target,
                       entry_bci,
                       &offsets,
                       0,
                       &hscb,
                       0,
                       &oopmaps,
                       &handler_table,
                       &inc_table,
                       this,
                       env->comp_level(),
                       false,
                       false);
}

void YuhuCompiler::generate_native_code(YuhuEntry* entry,
                                         Function*   function,
                                         const char* name) {
  // Print the LLVM bitcode, if requested
  if (YuhuPrintBitcodeOf != NULL) {
    if (!fnmatch(YuhuPrintBitcodeOf, name, 0))
      function->dump();
  }

  if (YuhuVerifyFunction != NULL) {
    if (!fnmatch(YuhuVerifyFunction, name, 0)) {
      verifyFunction(*function);
    }
  }

  // Compile to native code
  address code = NULL;
  context()->add_function(function);
  {
    MutexLocker locker(execution_engine_lock());
    free_queued_methods();

    memory_manager()->set_entry_for_function(function, entry);
    code = (address) execution_engine()->getPointerToFunction(function);
  }
  assert(code != NULL, "code must be != NULL");
  entry->set_entry_point(code);
  entry->set_function(function);
  entry->set_context(context());
  address code_start = entry->code_start();
  address code_limit = entry->code_limit();

  // Register generated code for profiling, etc
  if (JvmtiExport::should_post_dynamic_code_generated())
    JvmtiExport::post_dynamic_code_generated(name, code_start, code_limit);
}
```

#### 2.2 Extend Compilation Levels

**File**: `hotspot/src/share/vm/utilities/globalDefinitions.hpp` (modify existing enum)

```cpp
enum CompLevel {
  CompLevel_any               = -1,
  CompLevel_all               = -1,
  CompLevel_none              = 0,         // Interpreter
  CompLevel_simple            = 1,         // C1
  CompLevel_limited_profile   = 2,         // C1, invocation & backedge counters
  CompLevel_full_profile      = 3,         // C1, invocation & backedge counters + mdo
  CompLevel_full_optimization = 4,         // C2 or Shark
  CompLevel_yuhu_fast        = 5,         // NEW: Yuhu fast compilation
  CompLevel_yuhu_optimized   = 6,         // NEW: Yuhu optimized compilation
  
  // ... existing code ...
};

// Add helper functions
inline bool is_yuhu_compile(int comp_level) {
  return comp_level == CompLevel_yuhu_fast || comp_level == CompLevel_yuhu_optimized;
}
```

#### 2.3 Integrate with CompileBroker

**File**: `hotspot/src/share/vm/compiler/compileBroker.cpp` (modify `compilation_init()`)

```cpp
void CompileBroker::compilation_init() {
  // ... existing code ...
  
  // Add Yuhu compiler support
  int yuhu_count = 0;
  if (UseYuhuCompiler) {
    yuhu_count = CompilationPolicy::policy()->compiler_count(CompLevel_yuhu_fast);
  }
  
#ifdef YUHU_COMPILER
  if (yuhu_count > 0) {
    _compilers[2] = new YuhuCompiler();  // Use slot 2
  }
#endif

  // Update thread initialization
  init_compiler_threads(c1_count, c2_count, yuhu_count);
}
```

**File**: `hotspot/src/share/vm/compiler/compileBroker.hpp` (modify compiler access)

```cpp
static AbstractCompiler* compiler(int comp_level) {
  if (is_yuhu_compile(comp_level)) return _compilers[2]; // Yuhu
  if (is_c2_compile(comp_level)) return _compilers[1];   // C2
  if (is_c1_compile(comp_level)) return _compilers[0];   // C1
  return NULL;
}
```

### Phase 2: Compilation Framework

#### 2.4 Create Yuhu Compilation Engine

**File**: `hotspot/src/share/vm/yuhu/yuhu_Compilation.hpp`
```cpp
#ifndef SHARE_VM_YUHU_YUHU_COMPILATION_HPP
#define SHARE_VM_YUHU_YUHU_COMPILATION_HPP

#include "ci/ciEnv.hpp"
#include "ci/ciMethod.hpp"

class YuhuCompilation {
private:
  ciEnv* _env;
  ciMethod* _target;
  int _entry_bci;
  
public:
  YuhuCompilation(ciEnv* env, ciMethod* target, int entry_bci)
    : _env(env), _target(target), _entry_bci(entry_bci) {}
    
  void compile();
  
private:
  void generate_code();
  void allocate_registers();
  void optimize_code();
};
```

#### 2.5 Leverage Existing Yuhu Infrastructure

The existing Yuhu interpreter provides excellent foundation components:

1. **YuhuMacroAssembler**: Already exists for AArch64 code generation
2. **YuhuTemplateTable**: Bytecode-to-assembly mapping logic
3. **YuhuInterpreter**: Method entry points and dispatch

**Key Reuse Strategy**:
- Use `YuhuMacroAssembler` for native code generation
- Adapt `YuhuTemplateTable` bytecode handlers for compilation
- Leverage the proven register allocation and instruction patterns

### Phase 3: Compilation Policies

#### 2.6 Create Yuhu Compilation Policy

**File**: `hotspot/src/share/vm/runtime/yuhu/yuhu_CompilationPolicy.hpp`
```cpp
class YuhuCompilationPolicy : public CompilationPolicy {
public:
  virtual int compiler_count(CompLevel comp_level) {
    if (comp_level == CompLevel_yuhu_fast) return 1;
    if (comp_level == CompLevel_yuhu_optimized && UseYuhuOptimizer) return 1;
    return 0;
  }
  
  virtual CompLevel task_level(methodHandle method, int osr_bci, bool blocking) {
    // Decision logic for when to use Yuhu compiler
    if (method->invocation_count() > YuhuCompileThreshold) {
      return UseYuhuOptimizer ? CompLevel_yuhu_optimized : CompLevel_yuhu_fast;
    }
    return CompLevel_none;
  }
};
```

## 3. Command Line Options

Add new JVM flags to control Yuhu compilation (based on Shark flags):

```cpp
// In hotspot/src/share/vm/runtime/globals.hpp
product(bool, UseYuhuCompiler, false, 
        "Use Yuhu JIT compiler as alternative to C1/C2")

product(int, YuhuCompileThreshold, 1500,
        "Invocation threshold for Yuhu compilation")

product(bool, UseYuhuOptimizer, false,
        "Use Yuhu optimized compilation tier")

product(int, YuhuCompileOnly, 0,
        "Compile only specified methods with Yuhu compiler")

// LLVM-specific flags (from Shark)
product(ccstr, YuhuOptimizationLevel, "Default",
        "Yuhu optimization level: None, Less, Default, or Aggressive")

product(ccstr, YuhuPrintBitcodeOf, NULL,
        "Print LLVM bitcode for methods matching this pattern")

product(ccstr, YuhuPrintAsmOf, NULL,
        "Print assembly code for methods matching this pattern")

product(ccstr, YuhuVerifyFunction, NULL,
        "Verify LLVM IR for methods matching this pattern")

product(bool, YuhuTraceInstalls, false,
        "Trace method installation in Yuhu compiler")
```

## 4. Implementation Phases

### Phase 1: Basic Compiler Registration (Week 1-2)
- [ ] Create `YuhuCompiler` class inheriting from `AbstractCompiler`
- [ ] Extend `CompLevel` enum for Yuhu compilation levels
- [ ] Integrate with `CompileBroker::compilation_init()`
- [ ] Add command line flags (`-XX:+UseYuhuCompiler`)

### Phase 2: Simple Code Generation (Week 3-4)
- [ ] Implement basic `compile_method()` that generates working native code
- [ ] Leverage existing `YuhuMacroAssembler` for AArch64 code generation
- [ ] Create simple frame layout and calling conventions
- [ ] Test with simple methods (getters/setters)

### Phase 3: Bytecode Translation (Week 5-6)
- [ ] Adapt `YuhuTemplateTable` bytecode handlers for compilation
- [ ] Implement basic register allocation
- [ ] Handle common bytecodes (load/store, arithmetic, method calls)
- [ ] Add exception handling support

### Phase 4: Optimization & Integration (Week 7-8)
- [ ] Implement basic optimizations (constant folding, dead code elimination)
- [ ] Add compilation policies for method selection
- [ ] Integrate with existing profiling and tiered compilation
- [ ] Performance testing and benchmarking

## 5. Technical Considerations

### 5.1 Code Generation Strategy

**Option A: Direct Bytecode Translation** (Recommended)
- Translate each bytecode to equivalent native instructions
- Use existing `YuhuTemplateTable` handlers as templates
- Leverage proven register allocation patterns from interpreter

**Option B: CFG-based Compilation**
- Build control flow graph from bytecodes
- Apply standard compiler optimizations
- More complex but potentially better performance

### 5.2 Integration with Existing Systems

1. **Profiling**: Integrate with existing invocation/backedge counters
2. **OSR Support**: Add on-stack replacement capability
3. **Deoptimization**: Support deoptimization back to interpreter
4. **Native Method Support**: Handle native method compilation

### 5.3 Testing Strategy

1. **Unit Tests**: Test each compilation component individually
2. **Integration Tests**: Use existing JDK test suites with Yuhu compiler
3. **Performance Tests**: Compare against C1/C2 for compilation speed and runtime performance
4. **Regression Tests**: Ensure existing functionality remains intact

## 6. File Structure (Based on Shark)

```
hotspot/src/share/vm/yuhu/
├── yuhu_Compiler.hpp/.cpp          # Main compiler class (from SharkCompiler)
├── yuhu_Context.hpp/.cpp           # LLVM context management (from SharkContext)
├── yuhu_Builder.hpp/.cpp           # LLVM IR builder (from SharkBuilder)
├── yuhu_Function.hpp/.cpp          # Function IR generation (from SharkFunction)
├── yuhu_TopLevelBlock.hpp/.cpp    # Bytecode block to IR (from SharkTopLevelBlock)
├── yuhu_MemoryManager.hpp/.cpp    # Code memory management (from SharkMemoryManager)
├── yuhu_Entry.hpp                  # Code entry point (from SharkEntry)
├── yuhu_CodeBuffer.hpp/.cpp        # Code buffer wrapper (from SharkCodeBuffer)
├── yuhu_State.hpp/.cpp             # Compilation state (from SharkState)
├── yuhu_Stack.hpp/.cpp             # Stack management (from SharkStack)
├── yuhu_Value.hpp/.cpp             # Value representation (from SharkValue)
├── yuhu_Constant.hpp/.cpp          # Constant handling (from SharkConstant)
├── yuhu_Runtime.hpp/.cpp           # Runtime calls (from SharkRuntime)
├── yuhu_NativeWrapper.hpp/.cpp    # Native method wrapper (from SharkNativeWrapper)
├── yuhu_Inliner.hpp/.cpp          # Method inlining (from SharkInliner)
├── yuhu_Intrinsics.hpp/.cpp       # Intrinsic methods (from SharkIntrinsics)
├── yuhu_CacheDecache.hpp/.cpp     # Cache/decache operations (from SharkCacheDecache)
├── yuhu_Invariants.hpp            # Target invariants (from SharkInvariants)
├── yuhu_StateScanner.hpp/.cpp     # State scanning (from SharkStateScanner)
├── llvmHeaders.hpp                # LLVM header includes
├── llvmValue.hpp                  # LLVM value utilities
├── yuhu_globals.hpp/.cpp          # Compiler flags and globals
└── yuhuType.hpp                    # Type definitions

hotspot/src/cpu/aarch64/vm/yuhu/
└── (Platform-specific files if needed)
```

**Note**: All files are migrated from Shark, with `Shark` → `Yuhu` renaming.

## 7. Expected Benefits

1. **Proven Architecture**: Direct migration from Shark, a working LLVM-based JIT compiler
2. **Fast Development**: No need to design from scratch - adapt existing code
3. **Complete Implementation**: All components already implemented (OSR, exception handling, etc.)
4. **Thread Safety**: Proper handling of LLVM/HotSpot interactions already solved
5. **Memory Management**: Complete integration with HotSpot code cache
6. **Extensibility**: Framework ready for future optimizations and improvements
7. **Low Risk**: Using battle-tested code reduces implementation risk

## 8. Migration Checklist

### Step 1: Copy Files
- [ ] Copy all Shark files to `hotspot/src/share/vm/yuhu/`
- [ ] Copy platform-specific files if any

### Step 2: Rename
- [ ] Batch replace `Shark` → `Yuhu` in all files
- [ ] Batch replace `shark` → `yuhu` in file names
- [ ] Batch replace `SHARK` → `YUHU` in macros
- [ ] Update include guards

### Step 3: Platform Adaptation
- [ ] Replace `InitializeNativeTarget()` with `InitializeAArch64Target()`
- [ ] Replace `InitializeNativeTargetAsmPrinter()` with `InitializeAArch64TargetAsmPrinter()`
- [ ] Remove Zero-specific code
- [ ] Update calling conventions for AArch64

### Step 4: LLVM Version
- [ ] Check LLVM version compatibility
- [ ] Update deprecated API calls if needed
- [ ] Test with target LLVM version

### Step 5: Integration
- [ ] Add to `CompileBroker::compilation_init()`
- [ ] Add command line flags
- [ ] Test compiler registration

### Step 6: Testing
- [ ] Compile simple methods
- [ ] Test OSR
- [ ] Test exception handling
- [ ] Performance testing

This approach leverages Shark's proven implementation and provides a clear, low-risk path to a functional Yuhu JIT compiler.

---

## 9. 补充建议：用 LLVM 重写 C1 和 C2

### 9.1 迁移策略：渐进式 vs 完全重写

**推荐：渐进式迁移**

#### 阶段 1：并行实现（推荐）
- 保持 C1/C2 作为默认编译器
- 实现 LLVM 版本的编译器作为替代选项
- 通过 `-XX:+UseYuhuCompiler` 启用
- 逐步迁移功能，确保每个功能都经过充分测试

#### 阶段 2：功能对等
- 实现 C1 的所有核心功能（快速编译、OSR、基本优化）
- 实现 C2 的核心功能（高级优化、逃逸分析、内联）
- 确保性能对等或更好

#### 阶段 3：完全替换
- 当 LLVM 版本稳定且性能达标后，可以完全替换 C1/C2
- 保留 C1/C2 代码作为回退选项（通过编译选项控制）

### 9.2 LLVM 版本选择和兼容性

#### 推荐的 LLVM 版本
- **JDK 8 兼容性**：LLVM 3.x - 6.x（Shark 使用的版本范围）
- **现代特性**：LLVM 10+ 提供更好的 JIT 支持（MCJIT → ORC JIT）
- **建议**：从 LLVM 6.x 开始，逐步升级到 LLVM 10+

#### 关键考虑
```cpp
// 检查 LLVM 版本兼容性
#if LLVM_VERSION_MAJOR >= 10
  // 使用 ORC JIT（推荐）
  #define USE_ORC_JIT
#else
  // 使用 MCJIT（向后兼容）
  #define USE_MCJIT
#endif
```

### 9.3 架构设计：C1 和 C2 的统一实现

#### 关键理解：IR 的差异

**C1 的 IR 结构**：
- **HIR (High-level IR)**: 基于 `Instruction` 类的层次结构
  - `Instruction`, `LoadField`, `StoreField`, `ArithmeticOp`, `Invoke` 等
  - 更接近 Java 字节码，保留更多 Java 语义
- **LIR (Low-level IR)**: 基于 `LIR_Op` 的层次结构
  - `LIR_Op0`, `LIR_Op1`, `LIR_Op2`, `LIR_OpCall` 等
  - 更接近机器码，寄存器分配在 LIR 阶段

**C2 的 IR 结构**：
- **Ideal Graph IR**: 基于 `Node` 类的图结构
  - `Node`, `AddNode`, `LoadNode`, `StoreNode`, `CallNode` 等
  - SSA 形式，支持全局优化
  - 图优化（逃逸分析、标量替换等）

**LLVM 的 IR**：
- **LLVM IR**: 统一的中间表示
  - SSA 形式，类似 C2 的 Ideal Graph
  - 但更通用，不特定于 Java

#### 方案 A：单一编译器，多级优化（推荐）

**核心思想**：LLVM 只有一套 IR，但可以通过不同的优化 Pass 配置来实现 C1 和 C2 的不同行为。

```cpp
class YuhuCompiler : public AbstractCompiler {
private:
  // 多级优化配置
  enum OptimizationLevel {
    Fast,      // C1 级别：快速编译，基本优化
    Balanced,  // 中等优化
    Aggressive // C2 级别：深度优化
  };
  
  // 共享的 ExecutionEngine（LLVM 只有一套）
  llvm::ExecutionEngine* _execution_engine;
  
public:
  void compile_method(ciEnv* env, ciMethod* target, int entry_bci) {
    OptimizationLevel level = select_optimization_level(target);
    compile_with_level(env, target, entry_bci, level);
  }
  
private:
  OptimizationLevel select_optimization_level(ciMethod* method) {
    // 根据方法特征选择优化级别
    if (method->invocation_count() < FastCompileThreshold) {
      return Fast;  // C1 级别
    } else if (method->is_hot()) {
      return Aggressive;  // C2 级别
    }
    return Balanced;
  }
  
  void compile_with_level(ciEnv* env, ciMethod* target, int entry_bci, 
                          OptimizationLevel level) {
    // 1. 生成 LLVM IR（统一的前端）
    llvm::Function* function = generate_llvm_ir(target);
    
    // 2. 根据级别配置优化 Pass
    llvm::PassManagerBuilder PMB;
    configure_optimization_passes(PMB, level);
    
    // 3. 应用优化
    llvm::FunctionPassManager FPM(function->getParent());
    PMB.populateFunctionPassManager(FPM);
    FPM.run(*function);
    
    // 4. 生成机器码（统一的后端）
    generate_native_code(function);
  }
  
  void configure_optimization_passes(llvm::PassManagerBuilder& PMB, 
                                     OptimizationLevel level) {
    switch (level) {
      case Fast:  // C1 级别
        PMB.OptLevel = 0;  // 无优化
        PMB.SizeLevel = 0;
        PMB.Inliner = nullptr;  // 禁用内联
        PMB.DisableUnrollLoops = true;
        PMB.DisableVectorization = true;
        break;
        
      case Aggressive:  // C2 级别
        PMB.OptLevel = 3;  // 最高优化
        PMB.SizeLevel = 0;
        PMB.Inliner = createFunctionInliningPass(275, 225, false);
        PMB.DisableUnrollLoops = false;
        PMB.DisableVectorization = false;
        // 添加 Java 特定优化
        PMB.addExtension(llvm::PassManagerBuilder::EP_LoopOptimizerEnd,
                         addJavaSpecificOptimizations);
        break;
    }
  }
};
```

**优势**：
- ✅ **代码复用**：共享 IR 生成逻辑（字节码 → LLVM IR）
- ✅ **统一维护**：单一代码库，减少维护成本
- ✅ **灵活切换**：可以根据运行时信息动态选择优化级别
- ✅ **LLVM 优势**：利用 LLVM 的统一优化框架

**关键点**：
- LLVM IR 是统一的，但优化 Pass 配置不同
- 前端（字节码 → LLVM IR）可以共享
- 后端（LLVM IR → 机器码）可以共享
- 只有优化 Pass 配置不同

#### 方案 B：分离的 C1 和 C2 实现（备选）

如果希望更清晰地分离 C1 和 C2 的职责：

```cpp
class YuhuC1Compiler : public AbstractCompiler {
  // 快速编译，基本优化
  // 使用 LLVM OptLevel = 0
};

class YuhuC2Compiler : public AbstractCompiler {
  // 深度优化，高级特性
  // 使用 LLVM OptLevel = 3 + 自定义 Pass
};
```

**优势**：
- 清晰的职责分离
- 可以独立优化和测试
- 更接近现有 HotSpot 架构

**劣势**：
- 代码重复：IR 生成逻辑需要重复实现
- 维护成本：需要维护两套代码

#### 推荐方案：方案 A（单一编译器 + 多级优化）

**理由**：
1. **LLVM 只有一套 IR**：不需要维护多套 IR
2. **优化级别是配置问题**：不是架构问题
3. **代码复用**：前端和后端都可以共享
4. **灵活性**：可以根据运行时信息动态调整

**实现策略**：
```cpp
// 统一的 IR 生成（前端）
llvm::Function* generate_llvm_ir(ciMethod* method) {
  // 字节码 → LLVM IR
  // 这部分逻辑对 C1 和 C2 都相同
}

// 根据编译级别配置优化（中端）
void configure_passes(llvm::PassManagerBuilder& PMB, CompLevel level) {
  if (is_c1_level(level)) {
    // C1 配置：快速编译
    PMB.OptLevel = 0;
  } else if (is_c2_level(level)) {
    // C2 配置：深度优化
    PMB.OptLevel = 3;
    add_java_specific_passes(PMB);
  }
}

// 统一的代码生成（后端）
void generate_native_code(llvm::Function* function) {
  // LLVM IR → 机器码
  // 这部分逻辑对 C1 和 C2 都相同
}
```

### 9.4 性能关键路径优化

#### 9.4.1 编译速度优化（C1 级别）

```cpp
// 快速编译配置
void YuhuCompiler::configure_fast_compilation(llvm::PassManagerBuilder& PMB) {
  PMB.OptLevel = 0;  // 无优化
  PMB.SizeLevel = 0;
  PMB.Inliner = nullptr;  // 禁用内联
  PMB.DisableUnrollLoops = true;
  PMB.DisableVectorization = true;
}
```

#### 9.4.2 代码质量优化（C2 级别）

```cpp
// 深度优化配置
void YuhuCompiler::configure_aggressive_optimization(llvm::PassManagerBuilder& PMB) {
  PMB.OptLevel = 3;  // 最高优化级别
  PMB.SizeLevel = 0;
  PMB.Inliner = createFunctionInliningPass(275, 225, false);
  PMB.DisableUnrollLoops = false;
  PMB.DisableVectorization = false;
  
  // 添加自定义优化 Pass
  PMB.addExtension(llvm::PassManagerBuilder::EP_LoopOptimizerEnd,
                   addCustomLoopOptimizations);
  PMB.addExtension(llvm::PassManagerBuilder::EP_ScalarOptimizerLate,
                   addCustomScalarOptimizations);
}
```

### 9.5 内存管理和代码缓存

#### 9.5.1 自定义内存管理器

```cpp
class YuhuMemoryManager : public llvm::JITMemoryManager {
private:
  CodeCache* _code_cache;  // HotSpot 代码缓存
  std::map<const llvm::Function*, nmethod*> _function_to_nmethod;
  
public:
  uint8_t* allocateCodeSection(uintptr_t Size, unsigned Alignment, 
                               unsigned SectionID, StringRef SectionName) {
    // 从 HotSpot 代码缓存分配
    return (uint8_t*) _code_cache->allocate(Size, Alignment);
  }
  
  void registerFunction(const llvm::Function* F, nmethod* nm) {
    _function_to_nmethod[F] = nm;
  }
};
```

#### 9.5.2 代码缓存管理

```cpp
// 与 HotSpot 代码缓存集成
void YuhuCompiler::generate_native_code(SharkEntry* entry, 
                                        Function* function, 
                                        const char* name) {
  // 1. 分配代码缓存空间
  int code_size = estimate_code_size(function);
  address code_start = _code_cache->allocate(code_size);
  
  // 2. 生成代码到缓存
  {
    MutexLocker locker(execution_engine_lock());
    memory_manager()->set_code_start(code_start);
    address code = (address) execution_engine()->getPointerToFunction(function);
  }
  
  // 3. 创建 nmethod
  nmethod* nm = new nmethod(method, code_start, code_size, ...);
  memory_manager()->registerFunction(function, nm);
}
```

### 9.6 OSR (On-Stack Replacement) 支持

#### 9.6.1 OSR 入口点生成

```cpp
void YuhuCompiler::compile_osr_method(ciEnv* env, ciMethod* target, int entry_bci) {
  // 1. 创建 OSR 入口点函数
  Function* osr_function = create_osr_function(target, entry_bci);
  
  // 2. 生成 OSR 状态转换代码
  // 从解释器栈帧转换为编译代码栈帧
  generate_osr_entry(osr_function, entry_bci);
  
  // 3. 生成正常入口点（如果需要）
  Function* normal_function = create_normal_function(target);
  
  // 4. 编译两个函数
  compile_function(osr_function);
  compile_function(normal_function);
}
```

#### 9.6.2 OSR 状态转换

```cpp
void YuhuBuilder::generate_osr_entry(llvm::Function* F, int entry_bci) {
  // 1. 获取解释器栈帧
  Value* interpreter_frame = get_interpreter_frame();
  
  // 2. 提取局部变量
  for (int i = 0; i < local_count(); i++) {
    Value* local = extract_local(interpreter_frame, i);
    set_local(i, local);
  }
  
  // 3. 提取操作数栈
  Value* stack_top = extract_stack_top(interpreter_frame);
  set_stack_top(stack_top);
  
  // 4. 跳转到目标字节码
  jump_to_bci(entry_bci);
}
```

### 9.7 Deoptimization 支持

#### 9.7.1 Deoptimization 点标记

```cpp
void YuhuBuilder::mark_deoptimization_point(int bci, DeoptReason reason) {
  // 1. 创建 deopt 点
  DeoptPoint* deopt_point = new DeoptPoint(bci, reason);
  
  // 2. 生成运行时检查
  Value* condition = generate_deopt_condition(reason);
  BasicBlock* deopt_block = create_deopt_block(deopt_point);
  BasicBlock* continue_block = create_continue_block();
  
  // 3. 插入条件分支
  builder()->CreateCondBr(condition, deopt_block, continue_block);
  
  // 4. 生成 deopt stub 调用
  setInsertPoint(deopt_block);
  generate_deopt_stub_call(deopt_point);
}
```

#### 9.7.2 Deoptimization 信息记录

```cpp
void YuhuCompiler::record_deoptimization_info(nmethod* nm, 
                                               const DeoptInfo& info) {
  // 记录 deopt 信息到 nmethod
  nm->add_deoptimization_info(info.bci, info.reason, info.debug_info);
}
```

### 9.8 内联和优化管道

#### 9.8.1 方法内联

```cpp
class YuhuInliner {
public:
  bool should_inline(ciMethod* caller, ciMethod* callee) {
    // 1. 检查大小限制
    if (callee->code_size() > MaxInlineSize) return false;
    
    // 2. 检查调用频率
    if (callee->invocation_count() < MinInlineFrequency) return false;
    
    // 3. 检查递归深度
    if (inline_depth() > MaxInlineDepth) return false;
    
    // 4. 检查特殊方法（构造函数、final 方法等）
    if (callee->is_constructor() && !should_inline_constructors()) {
      return false;
    }
    
    return true;
  }
  
  void inline_method(llvm::Function* caller, ciMethod* callee) {
    // 1. 生成被调用方法的 IR
    Function* callee_ir = generate_method_ir(callee);
    
    // 2. 内联到调用者
    InlineFunctionInfo IFI;
    InlineFunction(call_instruction, IFI);
    
    // 3. 更新调用图
    update_call_graph(caller, callee);
  }
};
```

#### 9.8.2 自定义优化 Pass

```cpp
// 添加 Java 特定的优化
class JavaSpecificOptimization : public llvm::FunctionPass {
public:
  bool runOnFunction(llvm::Function& F) override {
    bool changed = false;
    
    // 1. 空检查消除
    changed |= eliminate_null_checks(F);
    
    // 2. 边界检查消除
    changed |= eliminate_bounds_checks(F);
    
    // 3. 类型检查优化
    changed |= optimize_type_checks(F);
    
    return changed;
  }
};
```

### 9.9 线程安全和并发编译

#### 9.9.1 多线程编译支持

```cpp
class YuhuCompiler {
private:
  // 每个编译线程一个 LLVM Context
  static thread_local llvm::LLVMContext* _thread_context;
  
  // 共享的 ExecutionEngine（需要锁保护）
  llvm::ExecutionEngine* _execution_engine;
  Monitor* _execution_engine_lock;
  
public:
  llvm::LLVMContext& getContext() {
    if (_thread_context == nullptr) {
      _thread_context = new llvm::LLVMContext();
    }
    return *_thread_context;
  }
  
  void compile_method(ciEnv* env, ciMethod* target, int entry_bci) {
    // 1. 在本地 Context 中生成 IR
    llvm::LLVMContext& ctx = getContext();
    Function* function = generate_ir(ctx, target);
    
    // 2. 在锁保护下编译为机器码
    address code;
    {
      MutexLocker locker(_execution_engine_lock);
      code = (address) _execution_engine->getPointerToFunction(function);
    }
    
    // 3. 安装方法
    install_method(env, target, code);
  }
};
```

#### 9.9.2 编译队列管理

```cpp
class YuhuCompileQueue {
private:
  Monitor* _lock;
  GrowableArray<CompileTask*>* _queue;
  
public:
  void add_task(CompileTask* task) {
    MutexLocker locker(_lock);
    _queue->append(task);
    _lock->notify_all();
  }
  
  CompileTask* get_task() {
    MutexLocker locker(_lock);
    while (_queue->length() == 0) {
      _lock->wait();
    }
    return _queue->pop();
  }
};
```

### 9.10 错误处理和回退机制

#### 9.10.1 编译失败处理

```cpp
void YuhuCompiler::compile_method(ciEnv* env, ciMethod* target, int entry_bci) {
  try {
    // 尝试编译
    do_compile(env, target, entry_bci);
  } catch (const LLVMException& e) {
    // LLVM 编译失败，回退到解释器
    env->record_failure("LLVM compilation failed: " + e.message());
    target->set_not_compilable();
  } catch (const OutOfMemoryError& e) {
    // 内存不足，回退
    env->record_failure("Out of memory during compilation");
    target->set_not_compilable();
  }
}
```

#### 9.10.2 运行时错误处理

```cpp
void YuhuCompiler::generate_exception_handler(llvm::Function* F) {
  // 1. 创建异常处理基本块
  BasicBlock* exception_block = BasicBlock::Create(getContext(), "exception", F);
  
  // 2. 生成异常处理代码
  setInsertPoint(exception_block);
  Value* exception = get_current_exception();
  Value* handler = find_exception_handler(exception);
  builder()->CreateCall(handler, exception);
}
```

### 9.11 平台特定优化

#### 9.11.1 AArch64 特定优化

```cpp
void YuhuCompiler::configure_aarch64_optimizations(llvm::PassManagerBuilder& PMB) {
  // 1. 启用 AArch64 特定优化
  PMB.addExtension(llvm::PassManagerBuilder::EP_LoopOptimizerEnd,
                   [](llvm::FunctionPassManager& FPM, llvm::PassManagerBuilder::ExtensionPoint) {
                     FPM.add(createAArch64AddressTypePromotionPass());
                     FPM.add(createAArch64A57FPLoadBalancingPass());
                   });
  
  // 2. 配置目标特性
  std::vector<std::string> features = {
    "+neon", "+crc", "+crypto"  // 根据 CPU 特性启用
  };
  setTargetFeatures(features);
}
```

### 9.12 调试和测试策略

#### 9.12.1 IR 转储和验证

```cpp
void YuhuCompiler::compile_method(ciEnv* env, ciMethod* target, int entry_bci) {
  Function* function = generate_ir(target);
  
  // 1. 可选：转储 IR
  if (YuhuPrintIR) {
    function->print(llvm::errs());
  }
  
  // 2. 验证 IR
  if (YuhuVerifyIR) {
    std::string error;
    llvm::raw_string_ostream os(error);
    if (llvm::verifyFunction(*function, &os)) {
      fatal("IR verification failed: %s", error.c_str());
    }
  }
  
  // 3. 编译
  compile_function(function);
}
```

#### 9.12.2 性能分析

```cpp
class YuhuCompilationTimer {
private:
  elapsedTimer _ir_generation_time;
  elapsedTimer _optimization_time;
  elapsedTimer _code_generation_time;
  
public:
  void start_ir_generation() { _ir_generation_time.start(); }
  void stop_ir_generation() { _ir_generation_time.stop(); }
  
  void print_statistics() {
    tty->print_cr("IR Generation: %.3f ms", _ir_generation_time.milliseconds());
    tty->print_cr("Optimization: %.3f ms", _optimization_time.milliseconds());
    tty->print_cr("Code Generation: %.3f ms", _code_generation_time.milliseconds());
  }
};
```

### 9.13 实施优先级

#### 高优先级（必须实现）
1. ✅ 基本编译框架（Compiler 类、Compilation 类）
2. ✅ LLVM 集成（ExecutionEngine、MemoryManager）
3. ✅ 基本字节码翻译（load/store、算术运算）
4. ✅ 方法调用（invokevirtual、invokestatic 等）
5. ✅ 异常处理
6. ✅ OSR 支持

#### 中优先级（重要功能）
1. ⚠️ 内联优化
2. ⚠️ 循环优化
3. ⚠️ 死代码消除
4. ⚠️ 常量折叠
5. ⚠️ Deoptimization 支持

#### 低优先级（性能优化）
1. 🔄 逃逸分析
2. 🔄 标量替换
3. 🔄 向量化
4. 🔄 高级内联策略
5. 🔄 平台特定优化

### 9.14 与现有系统的集成检查清单

- [ ] **CompileBroker 集成**：正确注册编译器
- [ ] **编译级别**：支持所有 CompLevel
- [ ] **代码缓存**：正确使用 HotSpot 代码缓存
- [ ] **OopMap 生成**：GC 安全点支持
- [ ] **调试信息**：JVMTI 支持
- [ ] **性能计数**：编译统计和性能监控
- [ ] **方法安装**：正确安装到 Method*
- [ ] **适配器生成**：I2C/C2I 适配器
- [ ] **Native 方法**：Native 方法编译支持
- [ ] **同步方法**：synchronized 方法支持

### 9.15 关键命令选项

```cpp
// 在 globals.hpp 中添加
product(bool, UseYuhuCompiler, false,
        "Use Yuhu LLVM-based JIT compiler")
        
product(bool, UseYuhuC1, false,
        "Use Yuhu compiler for C1-level compilation")
        
product(bool, UseYuhuC2, false,
        "Use Yuhu compiler for C2-level compilation")
        
product(intx, YuhuCompileThreshold, 1500,
        "Invocation threshold for Yuhu compilation")
        
product(intx, YuhuBackEdgeThreshold, 10000,
        "Back edge threshold for Yuhu OSR compilation")
        
product(bool, YuhuPrintIR, false,
        "Print LLVM IR for compiled methods")
        
product(bool, YuhuVerifyIR, true,
        "Verify LLVM IR before code generation")
        
product(intx, YuhuOptLevel, 2,
        "LLVM optimization level (0-3)")
```

### 9.16 总结

用 LLVM 重写 C1 和 C2 是一个大型项目，建议：

1. **渐进式实施**：先实现基本功能，逐步添加高级特性
2. **充分测试**：每个功能都要经过充分测试
3. **性能对比**：确保性能至少与 C1/C2 相当
4. **保持兼容**：确保与现有 HotSpot 基础设施完全兼容
5. **文档完善**：详细记录设计决策和实现细节

**关键成功因素**：
- ✅ **Shark 已经解决了这些问题**：线程安全、代码缓存集成、错误处理都已实现
- ✅ **直接迁移**：复制 Shark 代码并适配平台即可
- ✅ **降低风险**：使用经过验证的实现，而不是从零开始
- ⚠️ **需要适配**：主要是平台（AArch64）和 LLVM 版本

## 11. 参考文档

- **`SHARK_TO_YUHU_MIGRATION_GUIDE.md`**: 详细的 Shark 到 Yuhu 迁移指南，包含完整的迁移步骤和代码示例
- **`SHARK_PATTERN_ANALYSIS.md`**: Shark 架构模式分析，深入理解 Shark 的设计
- **Shark 源代码**: `hotspot/src/share/vm/shark/` - 可以直接参考的完整实现
  - `sharkCompiler.cpp` - 主编译器实现
  - `sharkFunction.cpp` - 字节码到 LLVM IR 转换
  - `sharkTopLevelBlock.cpp` - 字节码块处理
  - `sharkBuilder.cpp` - LLVM IR 构建器
  - `sharkContext.cpp` - LLVM 上下文管理
  - `sharkMemoryManager.cpp` - 代码内存管理

## 12. 快速开始

### 第一步：复制 Shark 文件

```bash
cd /Users/liuanyou/CLionProjects/jdk8
cp -r hotspot/src/share/vm/shark hotspot/src/share/vm/yuhu
```

### 第二步：批量重命名

```bash
cd hotspot/src/share/vm/yuhu
# 重命名文件
for file in shark*; do mv "$file" "${file/shark/yuhu}"; done

# 替换文件内容（使用 sed 或类似工具）
find . -type f -name "*.cpp" -o -name "*.hpp" | xargs sed -i '' 's/Shark/Yuhu/g'
find . -type f -name "*.cpp" -o -name "*.hpp" | xargs sed -i '' 's/shark/yuhu/g'
find . -type f -name "*.cpp" -o -name "*.hpp" | xargs sed -i '' 's/SHARK/YUHU/g'
```

### 第三步：适配平台

修改 `yuhu_Compiler.cpp` 中的目标初始化：
```cpp
#ifdef TARGET_ARCH_aarch64
  InitializeAArch64Target();
  InitializeAArch64TargetAsmPrinter();
#endif
```

### 第四步：集成到 CompileBroker

在 `compileBroker.cpp` 的 `compilation_init()` 中添加：
```cpp
#ifdef YUHU_COMPILER
  if (UseYuhuCompiler) {
    _compilers[2] = new YuhuCompiler();
  }
#endif
```

### 第五步：编译和测试

```bash
# 编译
make

# 测试
java -XX:+UseYuhuCompiler YourTestClass
```

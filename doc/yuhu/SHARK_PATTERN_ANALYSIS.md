# Shark Pattern Analysis - LLVM Integration Architecture

## Overview

Shark is a successful LLVM-based JIT compiler integrated into HotSpot. It demonstrates the complete pattern for using LLVM as a backend for Java bytecode compilation.

## Core Architecture Components

### 1. **Compiler Class Structure** (`SharkCompiler`)

```cpp
class SharkCompiler : public AbstractCompiler {
private:
    // Dual context pattern for thread safety
    SharkContext* _normal_context;     // For compiler threads
    SharkContext* _native_context;     // For adapter generation
    
    // LLVM execution infrastructure
    Monitor* _execution_engine_lock;   // LLVM threading safety
    SharkMemoryManager* _memory_manager;
    llvm::ExecutionEngine* _execution_engine;

public:
    SharkContext* context() const {
        if (JavaThread::current()->is_Compiler_thread()) {
            return _normal_context;
        } else {
            assert(AdapterHandlerLibrary_lock->owned_by_self());
            return _native_context;
        }
    }
};
```

**Key Pattern Elements:**
- **Dual Contexts**: Separate LLVM contexts for different threading scenarios
- **Execution Engine Lock**: Custom locking to prevent LLVM/HotSpot lock deadlocks
- **Memory Manager Integration**: Custom memory management for generated code

### 2. **Initialization Sequence** (from `SharkCompiler()`)

```cpp
SharkCompiler::SharkCompiler() : AbstractCompiler() {
    // 1. Create execution lock
    _execution_engine_lock = new Monitor(Mutex::leaf, "SharkExecutionEngineLock");
    MutexLocker locker(execution_engine_lock());

    // 2. Initialize LLVM threading
    if (!llvm_start_multithreaded()) fatal("llvm_start_multithreaded() failed");

    // 3. Initialize targets
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();

    // 4. Create contexts
    _normal_context = new SharkContext("normal");
    _native_context = new SharkContext("native");

    // 5. Create memory manager
    _memory_manager = new SharkMemoryManager();

    // 6. CPU-specific configuration
    StringMap<bool> Features;
    bool gotCpuFeatures = llvm::sys::getHostCPUFeatures(Features);
    // Configure MCPU and MAttrs...

    // 7. Build execution engine
    EngineBuilder builder(_normal_context->module());
    builder.setMCPU(MCPU);
    builder.setMAttrs(MAttrs);
    builder.setJITMemoryManager(memory_manager());
    builder.setEngineKind(EngineKind::JIT);
    builder.setOptLevel(optimization_level);
    _execution_engine = builder.create();
}
```

**Pattern Principles:**
- **Thread-Safe Initialization**: Initialize LLVM for multithreading first
- **Dual Module Setup**: Separate modules for different compilation contexts
- **Host CPU Detection**: Automatic feature detection and configuration
- **Error Handling**: Proper error reporting for LLVM failures

### 3. **Compilation Flow** (`compile_method`)

```cpp
void SharkCompiler::compile_method(ciEnv* env, ciMethod* target, int entry_bci) {
    // 1. Type flow analysis (HotSpot standard)
    ciTypeFlow* flow = (entry_bci == InvocationEntryBci) 
                       ? target->get_flow_analysis()
                       : target->get_osr_flow_analysis(entry_bci);

    // 2. Setup HotSpot recorders
    env->set_oop_recorder(new OopRecorder(&arena));
    env->set_debug_info(new DebugInformationRecorder(...));
    env->set_dependencies(new Dependencies(env));

    // 3. Create code buffer and LLVM builder
    CodeBuffer hscb("Shark", 256 * K, 64 * K);
    MacroAssembler* masm = new MacroAssembler(&hscb);
    SharkCodeBuffer cb(masm);
    SharkBuilder builder(&cb);

    // 4. Build LLVM IR from bytecode
    Function* function = SharkFunction::build(env, &builder, flow, name);

    // 5. Generate native code (threading critical section)
    {
        ThreadInVMfromNative tiv(JavaThread::current());
        generate_native_code(entry, function, name);
    }

    // 6. Install method in HotSpot
    env->register_method(target, entry_bci, &offsets, 0, &hscb, 
                         0, &oopmaps, &handler_table, &inc_table, 
                         this, env->comp_level(), false, false);
}
```

**Pattern Elements:**
- **Standard HotSpot Integration**: Uses same ciTypeFlow, recorders as C1/C2
- **Thread State Management**: Critical `ThreadInVMfromNative` section
- **Code Buffer Integration**: Bridges LLVM code generation with HotSpot code cache

### 4. **LLVM IR Generation** (`SharkFunction::build`)

```cpp
static llvm::Function* SharkFunction::build(ciEnv* env, SharkBuilder* builder, 
                                           ciTypeFlow* flow, const char* name) {
    SharkFunction function(env, builder, flow, name);
    return function.function();
}

// Key IR building steps:
void SharkFunction::initialize(const char* name) {
    // 1. Create LLVM function with proper signature
    _function = Function::Create(entry_point_type(), 
                                GlobalVariable::InternalLinkage, name);

    // 2. Setup arguments (method, thread, etc.)
    Function::arg_iterator ai = function()->arg_begin();
    Argument* method = ai++; method->setName("method");
    Argument* thread = ai++; thread->setName("thread");

    // 3. Create basic blocks for each bytecode block
    for (int i = 0; i < block_count(); i++) {
        _blocks[i] = new SharkTopLevelBlock(this, flow()->block_at(i));
    }

    // 4. Generate IR for each block
    for (int i = 0; i < block_count(); i++) {
        if (block(i)->entered())
            block(i)->initialize();
    }
}
```

**Pattern Principles:**
- **Bytecode-to-IR Translation**: Direct translation from Java bytecodes to LLVM IR
- **Block-Based Structure**: Maps HotSpot's type flow blocks to LLVM basic blocks
- **Proper Function Signatures**: Matches HotSpot's calling conventions

### 5. **Native Code Generation** (`generate_native_code`)

```cpp
void SharkCompiler::generate_native_code(SharkEntry* entry, Function* function, const char* name) {
    // 1. Optional debugging: print LLVM IR
    if (SharkPrintBitcodeOf != NULL && !fnmatch(SharkPrintBitcodeOf, name, 0)) {
        function->dump();
    }

    // 2. Optional verification
    if (SharkVerifyFunction != NULL && !fnmatch(SharkVerifyFunction, name, 0)) {
        verifyFunction(*function);
    }

    // 3. Critical section: LLVM compilation
    address code = NULL;
    context()->add_function(function);
    {
        MutexLocker locker(execution_engine_lock());
        free_queued_methods();
        
        memory_manager()->set_entry_for_function(function, entry);
        code = (address) execution_engine()->getPointerToFunction(function);
    }

    // 4. Setup entry point and metadata
    assert(code != NULL, "code must be != NULL");
    entry->set_entry_point(code);
    entry->set_function(function);
    entry->set_context(context());

    // 5. Register with profiling/debugging systems
    if (JvmtiExport::should_post_dynamic_code_generated()) {
        JvmtiExport::post_dynamic_code_generated(name, code_start, code_limit);
    }
}
```

**Critical Pattern Elements:**
- **Thread Safety**: All LLVM operations protected by execution engine lock
- **Memory Management**: Custom memory manager tracks code addresses
- **Debug Integration**: Optional IR printing and verification
- **HotSpot Integration**: Proper registration with profiling systems

### 6. **Memory Management Pattern** (`SharkMemoryManager`)

```cpp
class SharkMemoryManager : public llvm::JITMemoryManager {
private:
    llvm::JITMemoryManager* _mm;  // Delegates to default manager
    std::map<const llvm::Function*, SharkEntry*> _entry_map;

public:
    void set_entry_for_function(const llvm::Function* function, SharkEntry* entry) {
        _entry_map[function] = entry;
    }
    
    // Override key methods to track code locations
    void* startFunctionBody(const llvm::Function* F, uintptr_t& ActualSize);
    void endFunctionBody(const llvm::Function* F, unsigned char* FunctionStart, unsigned char* FunctionEnd);
};
```

**Pattern Principles:**
- **Delegation**: Wraps LLVM's default memory manager
- **Code Tracking**: Maps LLVM functions to HotSpot entry points
- **Custom Allocation**: Can implement custom memory policies if needed

## Integration Points

### 1. **HotSpot CompileBroker Integration**

Shark integrates via the standard compiler interface:

```cpp
// In CompileBroker::compilation_init()
#ifdef SHARK
int c1_count = 0;
int c2_count = 1;  // Shark uses C2 slot
_compilers[1] = new SharkCompiler();
#endif

// Standard compiler dispatch
static AbstractCompiler* compiler(int comp_level) {
    if (is_c2_compile(comp_level)) return _compilers[1]; // Shark
    if (is_c1_compile(comp_level)) return _compilers[0]; // C1
    return NULL;
}
```

### 2. **Threading Model**

```cpp
// Per-thread context selection
SharkContext* context() const {
    if (JavaThread::current()->is_Compiler_thread()) {
        return _normal_context;  // Compiler threads
    } else {
        return _native_context;  // Adapter generation (requires lock)
    }
}

// Critical section for LLVM operations
{
    MutexLocker locker(execution_engine_lock());
    // All LLVM compilation happens here
}
```

## Key Takeaways for Yuhu Implementation

1. **Follow the Dual Context Pattern**: Separate contexts for different threading scenarios
2. **Implement Proper Locking**: Custom execution engine lock prevents deadlocks
3. **Use Standard HotSpot Integration**: Leverage ciTypeFlow, recorders, etc.
4. **Handle Thread State Correctly**: Critical `ThreadInVMfromNative` sections
5. **Implement Memory Management**: Custom memory manager for code tracking
6. **Debug Integration**: Optional IR printing and verification hooks

## Advantages of This Pattern

- **Proven Threading Safety**: Handles LLVM/HotSpot interaction correctly
- **HotSpot Integration**: Seamless integration with existing compilation pipeline
- **Debugging Support**: Full integration with HotSpot's debugging infrastructure
- **Performance**: Leverages LLVM's optimization pipeline
- **Maintainability**: Clean separation between IR generation and code generation

This pattern provides a complete, production-ready architecture for LLVM-based JIT compilation in HotSpot.










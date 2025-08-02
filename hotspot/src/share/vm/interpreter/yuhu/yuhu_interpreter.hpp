//
// Created by Anyou Liu on 2025/4/23.
//

#ifndef JDK8_YUHU_INTERPRETER_HPP
#define JDK8_YUHU_INTERPRETER_HPP

#include "code/stubs.hpp"
#include "interpreter/bytecodes.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/top.hpp"
#include "asm/yuhu/yuhu_macroAssembler.hpp"

class YuhuEntryPoint VALUE_OBJ_CLASS_SPEC {
private:
    address _entry[number_of_states];

public:
    // Construction
    YuhuEntryPoint();
    YuhuEntryPoint(address bentry, address centry, address sentry, address aentry, address ientry, address lentry, address fentry, address dentry, address ventry);

    // Attributes
    address entry(TosState state) const;                // return target address for a given tosca state
    void    set_entry(TosState state, address entry);   // set    target address for a given tosca state
    void    print();

    // Comparison
//    bool operator == (const EntryPoint& y);             // for debugging only
};

class YuhuDispatchTable VALUE_OBJ_CLASS_SPEC {
public:
    enum { length = 1 << BitsPerByte };                 // an entry point for each byte value (also for undefined bytecodes)

private:
    address _table[number_of_states][length];           // dispatch tables, indexed by tosca and bytecode

public:
    // Attributes
    YuhuEntryPoint entry(int i) const;                      // return entry point for a given bytecode i
    void       set_entry(int i, YuhuEntryPoint& entry);     // set    entry point for a given bytecode i
    address*   table_for(TosState state)          { return _table[state]; }
    address*   table_for()                        { return table_for((TosState)0); }
    int        distance_from(address *table)      { return table - table_for(); }
    int        distance_from(TosState state)      { return distance_from(table_for(state)); }

    // Comparison
//    bool operator == (DispatchTable& y);                // for debugging only
};

class YuhuInterpreter : AllStatic {
friend class YuhuInterpreterGenerator;
friend class YuhuTemplateTable;
public:
    enum MethodKind {
        zerolocals,                                                 // method needs locals initialization
        zerolocals_synchronized,                                    // method needs locals initialization & is synchronized
        native,                                                     // native method
        native_synchronized,                                        // native method & is synchronized
        empty,                                                      // empty method (code: _return)
        accessor,                                                   // accessor method (code: _aload_0, _getfield, _(a|i)return)
        abstract,                                                   // abstract method (throws an AbstractMethodException)
        method_handle_invoke_FIRST,                                 // java.lang.invoke.MethodHandles::invokeExact, etc.
        method_handle_invoke_LAST                                   = (method_handle_invoke_FIRST
                                                                       + (vmIntrinsics::LAST_MH_SIG_POLY
                                                                          - vmIntrinsics::FIRST_MH_SIG_POLY)),
        java_lang_math_sin,                                         // implementation of java.lang.Math.sin   (x)
        java_lang_math_cos,                                         // implementation of java.lang.Math.cos   (x)
        java_lang_math_tan,                                         // implementation of java.lang.Math.tan   (x)
        java_lang_math_abs,                                         // implementation of java.lang.Math.abs   (x)
        java_lang_math_sqrt,                                        // implementation of java.lang.Math.sqrt  (x)
        java_lang_math_log,                                         // implementation of java.lang.Math.log   (x)
        java_lang_math_log10,                                       // implementation of java.lang.Math.log10 (x)
        java_lang_math_pow,                                         // implementation of java.lang.Math.pow   (x,y)
        java_lang_math_exp,                                         // implementation of java.lang.Math.exp   (x)
        java_lang_ref_reference_get,                                // implementation of java.lang.ref.Reference.get()
        java_util_zip_CRC32_update,                                 // implementation of java.util.zip.CRC32.update()
        java_util_zip_CRC32_updateBytes,                            // implementation of java.util.zip.CRC32.updateBytes()
        java_util_zip_CRC32_updateByteBuffer,                       // implementation of java.util.zip.CRC32.updateByteBuffer()
        number_of_method_entries,
        invalid = -1
    };
    enum MoreConstants {
        number_of_return_entries  = number_of_states,               // number of return entry points
        number_of_deopt_entries   = number_of_states,               // number of deoptimization entry points
        number_of_return_addrs    = number_of_states                // number of return addresses
    };
    enum SomeConstants {
        number_of_result_handlers = 10                              // number of result handlers for native calls
    };
protected:
    static StubQueue* _code;
    static address    _native_entry_begin;                        // Region for native entry code
    static address    _native_entry_end;
    static YuhuEntryPoint _return_entry[number_of_return_entries];    // entry points to return to from a call
    static YuhuEntryPoint _deopt_entry[number_of_deopt_entries];      // entry points to return to from a deoptimization
    static YuhuEntryPoint _continuation_entry;
    static YuhuEntryPoint _safept_entry;

    static address _invoke_return_entry[number_of_return_addrs];           // for invokestatic, invokespecial, invokevirtual return entries
    static address _invokeinterface_return_entry[number_of_return_addrs];  // for invokeinterface return entries
    static address _invokedynamic_return_entry[number_of_return_addrs];    // for invokedynamic return entries

    static YuhuDispatchTable _active_table;                           // the active    dispatch table (used by the interpreter for dispatch)
    static YuhuDispatchTable _normal_table;                           // the normal    dispatch table (used to set the active table in normal mode)
    static YuhuDispatchTable _safept_table;                           // the safepoint dispatch table (used to set the active table for safepoints)

    static address       _wentry_point[YuhuDispatchTable::length];    // wide instructions only (vtos tosca always)
    // method entry points
    static address    _entry_table[number_of_method_entries];     // entry points for a given method
    static address    _native_abi_to_tosca[number_of_result_handlers];  // for native method result handlers

    static address    _rethrow_exception_entry;                   // rethrows an activation in previous frame
    static address    _throw_exception_entry;
#ifdef HOTSWAP
    static address    _remove_activation_preserving_args_entry;   // continuation address when current frame is being popped
#endif // HOTSWAP
    static address    _remove_activation_entry;                   // continuation address if an exception is not handled by current frame

    static address    _throw_ArrayIndexOutOfBoundsException_entry;
    static address    _throw_ArrayStoreException_entry;
    static address    _throw_ArithmeticException_entry;
    static address    _throw_ClassCastException_entry;
    static address    _throw_NullPointerException_entry;
    static address    _throw_StackOverflowError_entry;
public:
    static StubQueue* code() { return _code; }
    static void initialize();
    static address*   dispatch_table(TosState state)              { return _active_table.table_for(state); }
    static address*   dispatch_table()                            { return _active_table.table_for(); }
    static int        distance_from_dispatch_table(TosState state){ return _active_table.distance_from(state); }
    static int        BasicType_as_index(BasicType type);         // computes index into result_handler_by_index table

    const static int stackElementWords   = 1;
    const static int stackElementSize    = stackElementWords * wordSize;
    const static int logStackElementSize = LogBytesPerWord;

    // Local values relative to locals[n]
    static int  local_offset_in_bytes(int n) {
        return ((frame::interpreter_frame_expression_stack_direction() * n) * stackElementSize);
    }

    static address*   invoke_return_entry_table()                 { return _invoke_return_entry; }
    static address*   invokeinterface_return_entry_table()        { return _invokeinterface_return_entry; }
    static address*   invokedynamic_return_entry_table()          { return _invokedynamic_return_entry; }
    static address* invoke_return_entry_table_for(Bytecodes::Code code);

    static address    throw_exception_entry()                     { return _throw_exception_entry; }

#ifdef TARGET_ARCH_aarch64
# include "yuhu_interpreter_aarch64.hpp"
#endif
};

class YuhuInterpreterCodelet: public Stub {
    friend class VMStructs;
private:
    int         _size;                             // the size in bytes
    const char* _description;                      // a description of the codelet, for debugging & printing
    Bytecodes::Code _bytecode;                     // associated bytecode if any
    DEBUG_ONLY(CodeStrings _strings;)              // Comments for annotating assembler output.

public:
    // Initialization/finalization
    void    initialize(int size,
                       CodeStrings& strings)       { _size = size; DEBUG_ONLY(_strings.assign(strings);) }
    void    finalize()                             { ShouldNotCallThis(); }

    // General info/converters
    int     size() const                           { return _size; }
    static  int code_size_to_size(int code_size)   { return round_to(sizeof(YuhuInterpreterCodelet), CodeEntryAlignment) + code_size; }

    // Code info
    address code_begin() const                     { return (address)this + round_to(sizeof(YuhuInterpreterCodelet), CodeEntryAlignment); }
    address code_end() const                       { return (address)this + size(); }

    // Debugging
    void    verify();
    void    print_on(outputStream* st) const;
    void    print() const { print_on(tty); }

    // Interpreter-specific initialization
    void    initialize(const char* description, Bytecodes::Code bytecode);

    // Interpreter-specific attributes
    int         code_size() const                  { return code_end() - code_begin(); }
    const char* description() const                { return _description; }
    Bytecodes::Code bytecode() const               { return _bytecode; }
};

// Define a prototype interface
DEF_STUB_INTERFACE(YuhuInterpreterCodelet);

class YuhuCodeletMark: ResourceMark {
private:
    YuhuInterpreterCodelet*         _clet;
    YuhuMacroAssembler** _masm;
    CodeBuffer                  _cb;

    int codelet_size() {
        // Request the whole code buffer (minus a little for alignment).
        // The commit call below trims it back for each codelet.
        int codelet_size = YuhuInterpreter::code()->available_space() - 2*K;

        // Guarantee there's a little bit of code space left.
        guarantee (codelet_size > 0 && (size_t)codelet_size >  2*K,
                   "not enough space for interpreter generation");

        return codelet_size;
    }

public:
    YuhuCodeletMark(
            YuhuMacroAssembler*& masm,
            const char* description,
            Bytecodes::Code bytecode = Bytecodes::_illegal):
            _clet((YuhuInterpreterCodelet*)YuhuInterpreter::code()->request(codelet_size())),
            _cb(_clet->code_begin(), _clet->code_size())

    { // request all space (add some slack for Codelet data)
        assert (_clet != NULL, "we checked not enough space already");

        // initialize Codelet attributes
        _clet->initialize(description, bytecode);
        // create assembler for code generation
        masm  = new YuhuMacroAssembler(&_cb);
        _masm = &masm;
    }

    ~YuhuCodeletMark() {
        // align so printing shows nop's instead of random code at the end (Codelets are aligned)
        (*_masm)->align(wordSize);
        // make sure all code is in code buffer
        (*_masm)->flush();


        // commit Codelet
        YuhuInterpreter::code()->commit((*_masm)->code()->pure_insts_size(), (*_masm)->code()->strings());
        // make sure nobody can use _masm outside a CodeletMark lifespan
        *_masm = NULL;
    }
};

#endif //JDK8_YUHU_INTERPRETER_HPP

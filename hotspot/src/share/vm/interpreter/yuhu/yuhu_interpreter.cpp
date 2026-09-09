//
// Created by Anyou Liu on 2025/4/23.
//
#include "precompiled.hpp"
#include "interpreter/yuhu/yuhu_interpreter.hpp"
#include "interpreter/yuhu/yuhu_interpreterGenerator.hpp"

StubQueue* YuhuInterpreter::_code = NULL;
bool       YuhuInterpreter::_notice_safepoints                          = false;
address YuhuInterpreter::_native_entry_begin = NULL;
address YuhuInterpreter::_native_entry_end = NULL;
YuhuEntryPoint YuhuInterpreter::_return_entry[number_of_return_entries];
YuhuEntryPoint YuhuInterpreter::_deopt_entry[number_of_deopt_entries];
YuhuEntryPoint YuhuInterpreter::_continuation_entry;
YuhuEntryPoint YuhuInterpreter::_safept_entry;

address YuhuInterpreter::_invoke_return_entry[number_of_return_addrs];
address YuhuInterpreter::_invokeinterface_return_entry[number_of_return_addrs];
address YuhuInterpreter::_invokedynamic_return_entry[number_of_return_addrs];

YuhuDispatchTable YuhuInterpreter::_active_table;
YuhuDispatchTable YuhuInterpreter::_safept_table;
YuhuDispatchTable YuhuInterpreter::_normal_table;
address    YuhuInterpreter::_wentry_point[YuhuDispatchTable::length];

address    YuhuInterpreter::_entry_table            [YuhuInterpreter::number_of_method_entries];
address    YuhuInterpreter::_native_abi_to_tosca[number_of_result_handlers];
address    YuhuInterpreter::_slow_signature_handler = NULL;

address YuhuInterpreter::_rethrow_exception_entry = NULL;
address YuhuInterpreter::_throw_exception_entry = NULL;
address YuhuInterpreter::_remove_activation_preserving_args_entry    = NULL;
address YuhuInterpreter::_remove_activation_entry = NULL;

address    YuhuInterpreter::_throw_ArrayIndexOutOfBoundsException_entry = NULL;
address    YuhuInterpreter::_throw_ArrayStoreException_entry = NULL;
address    YuhuInterpreter::_throw_ArithmeticException_entry = NULL;
address    YuhuInterpreter::_throw_ClassCastException_entry = NULL;
address    YuhuInterpreter::_throw_NullPointerException_entry = NULL;
address    YuhuInterpreter::_throw_StackOverflowError_entry = NULL;

YuhuEntryPoint::YuhuEntryPoint() {
    assert(number_of_states == 9, "check the code below");
    _entry[btos] = NULL;
    _entry[ctos] = NULL;
    _entry[stos] = NULL;
    _entry[atos] = NULL;
    _entry[itos] = NULL;
    _entry[ltos] = NULL;
    _entry[ftos] = NULL;
    _entry[dtos] = NULL;
    _entry[vtos] = NULL;
}

YuhuEntryPoint::YuhuEntryPoint(address bentry, address centry, address sentry, address aentry, address ientry, address lentry, address fentry, address dentry, address ventry) {
    assert(number_of_states == 9, "check the code below");
    _entry[btos] = bentry;
    _entry[ctos] = centry;
    _entry[stos] = sentry;
    _entry[atos] = aentry;
    _entry[itos] = ientry;
    _entry[ltos] = lentry;
    _entry[ftos] = fentry;
    _entry[dtos] = dentry;
    _entry[vtos] = ventry;
}

void YuhuEntryPoint::set_entry(TosState state, address entry) {
    assert(0 <= state && state < number_of_states, "state out of bounds");
    _entry[state] = entry;
}


address YuhuEntryPoint::entry(TosState state) const {
    assert(0 <= state && state < number_of_states, "state out of bounds");
    return _entry[state];
}


void YuhuEntryPoint::print() {
    tty->print("[");
    for (int i = 0; i < number_of_states; i++) {
        if (i > 0) tty->print(", ");
        tty->print(INTPTR_FORMAT, _entry[i]);
    }
    tty->print("]");
}

YuhuEntryPoint YuhuDispatchTable::entry(int i) const {
    assert(0 <= i && i < length, "index out of bounds");
    return
            YuhuEntryPoint(
                    _table[btos][i],
                    _table[ctos][i],
                    _table[stos][i],
                    _table[atos][i],
                    _table[itos][i],
                    _table[ltos][i],
                    _table[ftos][i],
                    _table[dtos][i],
                    _table[vtos][i]
            );
}

void YuhuDispatchTable::set_entry(int i, YuhuEntryPoint& entry) {
    assert(0 <= i && i < length, "index out of bounds");
    assert(number_of_states == 9, "check the code below");
    _table[btos][i] = entry.entry(btos);
    _table[ctos][i] = entry.entry(ctos);
    _table[stos][i] = entry.entry(stos);
    _table[atos][i] = entry.entry(atos);
    _table[itos][i] = entry.entry(itos);
    _table[ltos][i] = entry.entry(ltos);
    _table[ftos][i] = entry.entry(ftos);
    _table[dtos][i] = entry.entry(dtos);
    _table[vtos][i] = entry.entry(vtos);
}

void YuhuInterpreterCodelet::verify() {
}

void YuhuInterpreterCodelet::print_on(outputStream* st) const {
}

void YuhuInterpreterCodelet::initialize(const char* description, Bytecodes::Code bytecode) {
    _description       = description;
    _bytecode          = bytecode;
}

void yuhuInterpreter_init() {
    YuhuInterpreter::initialize();
}

void YuhuInterpreter::initialize() {
    if (_code != NULL) return;

    YuhuTemplateTable::initialize();

    {
        ResourceMark rm;
        TraceTime timer("yuhuInterpreter generation", TraceStartupTime);
        int code_size = InterpreterCodeSize;
        NOT_PRODUCT(code_size *= 4;)  // debug uses extra interpreter code space
        _code = new StubQueue(new YuhuInterpreterCodeletInterface, code_size, NULL,
                              "yuhInterpreter");
        YuhuInterpreterGenerator g;
    }

    // initialize dispatch table
    _active_table = _normal_table;
}

int YuhuInterpreter::BasicType_as_index(BasicType type) {
    int i = 0;
    switch (type) {
        case T_BOOLEAN: i = 0; break;
        case T_CHAR   : i = 1; break;
        case T_BYTE   : i = 2; break;
        case T_SHORT  : i = 3; break;
        case T_INT    : i = 4; break;
        case T_LONG   : i = 5; break;
        case T_VOID   : i = 6; break;
        case T_FLOAT  : i = 7; break;
        case T_DOUBLE : i = 8; break;
        case T_OBJECT : i = 9; break;
        case T_ARRAY  : i = 9; break;
        default       : ShouldNotReachHere();
    }
    assert(0 <= i && i < YuhuInterpreter::number_of_result_handlers,
           "index out of bounds");
    return i;
}

address* YuhuInterpreter::invoke_return_entry_table_for(Bytecodes::Code code) {
    switch (code) {
        case Bytecodes::_invokestatic:
        case Bytecodes::_invokespecial:
        case Bytecodes::_invokevirtual:
        case Bytecodes::_invokehandle:
            return YuhuInterpreter::invoke_return_entry_table();
        case Bytecodes::_invokeinterface:
            return YuhuInterpreter::invokeinterface_return_entry_table();
        case Bytecodes::_invokedynamic:
            return YuhuInterpreter::invokedynamic_return_entry_table();
        default:
            fatal(err_msg("invalid bytecode: %s", Bytecodes::name(code)));
            return NULL;
    }
}

int YuhuInterpreter::TosState_as_index(TosState state) {
    assert( state < number_of_states , "Invalid state in TosState_as_index");
    assert(0 <= (int)state && (int)state < YuhuInterpreter::number_of_return_addrs, "index out of bounds");
    return (int)state;
}

/**
 * If a deoptimization happens, this function returns the point of next bytecode to continue execution.
 */
address YuhuInterpreter::deopt_continue_after_entry(Method* method, address bcp, int callee_parameters, bool is_top_frame) {
    assert(method->contains(bcp), "just checkin'");

    // Get the original and rewritten bytecode.
    Bytecodes::Code code = Bytecodes::java_code_at(method, bcp);
    assert(!YuhuInterpreter::bytecode_should_reexecute(code), "should not reexecute");

    const int bci = method->bci_from(bcp);

    // compute continuation length
    const int length = Bytecodes::length_at(method, bcp);

    // compute result type
    BasicType type = T_ILLEGAL;

    switch (code) {
        case Bytecodes::_invokevirtual  :
        case Bytecodes::_invokespecial  :
        case Bytecodes::_invokestatic   :
        case Bytecodes::_invokeinterface: {
            Thread *thread = Thread::current();
            ResourceMark rm(thread);
            methodHandle mh(thread, method);
            type = Bytecode_invoke(mh, bci).result_type();
            // since the cache entry might not be initialized:
            // (NOT needed for the old calling convension)
            if (!is_top_frame) {
                int index = Bytes::get_native_u2(bcp+1);
                method->constants()->cache()->entry_at(index)->set_parameter_size(callee_parameters);
            }
            break;
        }

        case Bytecodes::_invokedynamic: {
            Thread *thread = Thread::current();
            ResourceMark rm(thread);
            methodHandle mh(thread, method);
            type = Bytecode_invoke(mh, bci).result_type();
            // since the cache entry might not be initialized:
            // (NOT needed for the old calling convension)
            if (!is_top_frame) {
                int index = Bytes::get_native_u4(bcp+1);
                method->constants()->invokedynamic_cp_cache_entry_at(index)->set_parameter_size(callee_parameters);
            }
            break;
        }

        case Bytecodes::_ldc   :
        case Bytecodes::_ldc_w : // fall through
        case Bytecodes::_ldc2_w:
        {
            Thread *thread = Thread::current();
            ResourceMark rm(thread);
            methodHandle mh(thread, method);
            type = Bytecode_loadconstant(mh, bci).result_type();
            break;
        }

        default:
            type = Bytecodes::result_type(code);
            break;
    }

    // return entry point for computed continuation state & bytecode length
    return
            is_top_frame
            ? YuhuInterpreter::deopt_entry (as_TosState(type), length)
            : YuhuInterpreter::return_entry(as_TosState(type), length, code);
}

address YuhuInterpreter::deopt_entry(TosState state, int length) {
    guarantee(0 <= length && length < YuhuInterpreter::number_of_deopt_entries, "illegal length");
    return _deopt_entry[length].entry(state);
}

/**
 * Returns the return entry address for the given top-of-stack state and bytecode.
 */
address YuhuInterpreter::return_entry(TosState state, int length, Bytecodes::Code code) {
    guarantee(0 <= length && length < Interpreter::number_of_return_entries, "illegal length");
    const int index = TosState_as_index(state);
    switch (code) {
        case Bytecodes::_invokestatic:
        case Bytecodes::_invokespecial:
        case Bytecodes::_invokevirtual:
        case Bytecodes::_invokehandle:
            return _invoke_return_entry[index];
        case Bytecodes::_invokeinterface:
            return _invokeinterface_return_entry[index];
        case Bytecodes::_invokedynamic:
            return _invokedynamic_return_entry[index];
        default:
            assert(!Bytecodes::is_invoke(code), err_msg("invoke instructions should be handled separately: %s", Bytecodes::name(code)));
            return _return_entry[length].entry(state);
    }
}

YuhuInterpreter::MethodKind YuhuInterpreter::method_kind(methodHandle m) {
    // Abstract method?
    if (m->is_abstract()) return abstract;

    // Method handle primitive?
    if (m->is_method_handle_intrinsic()) {
        vmIntrinsics::ID id = m->intrinsic_id();
        assert(MethodHandles::is_signature_polymorphic(id), "must match an intrinsic");
        MethodKind kind = (MethodKind)( method_handle_invoke_FIRST +
                                        ((int)id - vmIntrinsics::FIRST_MH_SIG_POLY) );
        assert(kind <= method_handle_invoke_LAST, "parallel enum ranges");
        return kind;
    }

#ifndef CC_INTERP
    if (UseCRC32Intrinsics && m->is_native()) {
        // Use optimized stub code for CRC32 native methods.
        switch (m->intrinsic_id()) {
            case vmIntrinsics::_updateCRC32            : return java_util_zip_CRC32_update;
            case vmIntrinsics::_updateBytesCRC32       : return java_util_zip_CRC32_updateBytes;
            case vmIntrinsics::_updateByteBufferCRC32  : return java_util_zip_CRC32_updateByteBuffer;
        }
    }
#endif

    // Native method?
    // Note: This test must come _before_ the test for intrinsic
    //       methods. See also comments below.
    if (m->is_native()) {
        assert(!m->is_method_handle_intrinsic(), "overlapping bits here, watch out");
        return m->is_synchronized() ? native_synchronized : native;
    }

    // Synchronized?
    if (m->is_synchronized()) {
        return zerolocals_synchronized;
    }

    if (RegisterFinalizersAtInit && m->code_size() == 1 &&
        m->intrinsic_id() == vmIntrinsics::_Object_init) {
        // We need to execute the special return bytecode to check for
        // finalizer registration so create a normal frame.
        return zerolocals;
    }

    // Empty method?
    if (m->is_empty_method()) {
        return empty;
    }

    // Special intrinsic method?
    // Note: This test must come _after_ the test for native methods,
    //       otherwise we will run into problems with JDK 1.2, see also
    //       AbstractInterpreterGenerator::generate_method_entry() for
    //       for details.
    switch (m->intrinsic_id()) {
        case vmIntrinsics::_dsin  : return java_lang_math_sin  ;
        case vmIntrinsics::_dcos  : return java_lang_math_cos  ;
        case vmIntrinsics::_dtan  : return java_lang_math_tan  ;
        case vmIntrinsics::_dabs  : return java_lang_math_abs  ;
        case vmIntrinsics::_dsqrt : return java_lang_math_sqrt ;
        case vmIntrinsics::_dlog  : return java_lang_math_log  ;
        case vmIntrinsics::_dlog10: return java_lang_math_log10;
        case vmIntrinsics::_dpow  : return java_lang_math_pow  ;
        case vmIntrinsics::_dexp  : return java_lang_math_exp  ;

        case vmIntrinsics::_Reference_get:
            return java_lang_ref_reference_get;
    }

    // Accessor method?
    if (m->is_accessor()) {
        assert(m->size_of_parameters() == 1, "fast code for accessors assumes parameter size = 1");
        return accessor;
    }

    // Note: for now: zero locals for all non-empty methods
    return zerolocals;
}

bool YuhuInterpreter::contains(address pc) {
    // Check if PC is within YuhuInterpreter's generated code
    return _code != NULL && _code->contains(pc);
}

void YuhuInterpreter::set_entry_for_kind(YuhuInterpreter::MethodKind kind, address entry) {
    assert(kind >= method_handle_invoke_FIRST &&
           kind <= method_handle_invoke_LAST, "late initialization only for MH entry points");
    assert(_entry_table[kind] == _entry_table[abstract], "previous value must be AME entry");
    _entry_table[kind] = entry;
}

// If deoptimization happens, the interpreter should reexecute this bytecode.
// This function mainly helps the compilers to set up the reexecute bit.
bool YuhuInterpreter::bytecode_should_reexecute(Bytecodes::Code code) {
    if (code == Bytecodes::_return) {
        //Yes, we consider Bytecodes::_return as a special case of reexecution
        return true;
    } else {
        return AbstractInterpreter::bytecode_should_reexecute(code);
    }
}

// If deoptimization happens, this function returns the point where the interpreter reexecutes
// the bytecode.
// Note: Bytecodes::_athrow (C1 only) and Bytecodes::_return are the special cases
//       that do not return "Interpreter::deopt_entry(vtos, 0)"
address YuhuInterpreter::deopt_reexecute_entry(Method* method, address bcp) {
    assert(method->contains(bcp), "just checkin'");
    Bytecodes::Code code   = Bytecodes::java_code_at(method, bcp);
    if (code == Bytecodes::_return) {
        // This is used for deopt during registration of finalizers
        // during Object.<init>.  We simply need to resume execution at
        // the standard return vtos bytecode to pop the frame normally.
        // reexecuting the real bytecode would cause double registration
        // of the finalizable object.
        return _normal_table.entry(Bytecodes::_return).entry(vtos);
    } else {
        assert(method->contains(bcp), "just checkin'");
        Bytecodes::Code code   = Bytecodes::java_code_at(method, bcp);
#ifdef COMPILER1
        if(code == Bytecodes::_athrow ) {
            return YuhuInterpreter::rethrow_exception_entry();
        }
#endif /* COMPILER1 */
        return YuhuInterpreter::deopt_entry(vtos, 0);
    }
}

// These should never be compiled since the interpreter will prefer
// the compiled version to the intrinsic version.
bool YuhuInterpreter::can_be_compiled(methodHandle m) {
    switch (method_kind(m)) {
        case YuhuInterpreter::java_lang_math_sin     : // fall thru
        case YuhuInterpreter::java_lang_math_cos     : // fall thru
        case YuhuInterpreter::java_lang_math_tan     : // fall thru
        case YuhuInterpreter::java_lang_math_abs     : // fall thru
        case YuhuInterpreter::java_lang_math_log     : // fall thru
        case YuhuInterpreter::java_lang_math_log10   : // fall thru
        case YuhuInterpreter::java_lang_math_sqrt    : // fall thru
        case YuhuInterpreter::java_lang_math_pow     : // fall thru
        case YuhuInterpreter::java_lang_math_exp     :
            return false;
        default:
            return true;
    }
}

// Safepoint suppport

static inline void copy_table(address* from, address* to, int size) {
    // Copy non-overlapping tables. The copy has to occur word wise for MT safety.
    while (size-- > 0) *to++ = *from++;
}

void YuhuInterpreter::notice_safepoints() {
    if (!_notice_safepoints) {
        // switch to safepoint dispatch table
        _notice_safepoints = true;
        copy_table((address*)&_safept_table, (address*)&_active_table, sizeof(_active_table) / sizeof(address));
    }
}

// switch from the dispatch table which notices safepoints back to the
// normal dispatch table.  So that we can notice single stepping points,
// keep the safepoint dispatch table if we are single stepping in JVMTI.
// Note that the should_post_single_step test is exactly as fast as the
// JvmtiExport::_enabled test and covers both cases.
void YuhuInterpreter::ignore_safepoints() {
    if (_notice_safepoints) {
        if (!JvmtiExport::should_post_single_step()) {
            // switch to normal dispatch table
            _notice_safepoints = false;
            copy_table((address*)&_normal_table, (address*)&_active_table, sizeof(_active_table) / sizeof(address));
        }
    }
}
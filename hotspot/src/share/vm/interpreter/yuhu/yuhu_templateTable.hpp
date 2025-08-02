//
// Created by Anyou Liu on 2025/5/4.
//

#ifndef JDK8_YUHU_TEMPLATETABLE_HPP
#define JDK8_YUHU_TEMPLATETABLE_HPP

#include "interpreter/bytecodes.hpp"
#include "memory/allocation.hpp"
#include "runtime/frame.hpp"
#include "asm/yuhu/yuhu_macroAssembler.hpp"

class YuhuTemplate VALUE_OBJ_CLASS_SPEC {
private:
    enum Flags {
        uses_bcp_bit,                                // set if template needs the bcp pointing to bytecode
        does_dispatch_bit,                           // set if template dispatches on its own
        calls_vm_bit,                                // set if template calls the vm
        wide_bit                                     // set if template belongs to a wide instruction
    };

    typedef void (*generator)(int arg);

    int       _flags;                              // describes interpreter template properties (bcp unknown)
    TosState  _tos_in;                             // tos cache state before template execution
    TosState  _tos_out;                            // tos cache state after  template execution
    generator _gen;                                // template code generator
    int       _arg;                                // argument for template code generator

    void      initialize(int flags, TosState tos_in, TosState tos_out, generator gen, int arg);

    friend class YuhuTemplateTable;
public:
    Bytecodes::Code bytecode() const;
    bool      is_valid() const                     { return _gen != NULL; }
    bool      uses_bcp() const                     { return (_flags & (1 << uses_bcp_bit     )) != 0; }
    bool      does_dispatch() const                { return (_flags & (1 << does_dispatch_bit)) != 0; }
    bool      calls_vm() const                     { return (_flags & (1 << calls_vm_bit     )) != 0; }
    bool      is_wide() const                      { return (_flags & (1 << wide_bit         )) != 0; }
    TosState  tos_in() const                       { return _tos_in; }
    TosState  tos_out() const                      { return _tos_out; }
    void      generate(YuhuMacroAssembler* masm);
};

class YuhuTemplateTable: AllStatic {
friend class YuhuTemplate;
public:
    enum Operation {
        add, sub, mul, div, rem, _and, _or, _xor, shl, shr, ushr
    };
    enum Condition {
        equal, not_equal, less, less_equal, greater, greater_equal
    };
    enum CacheByte {
        f1_byte = 1, f2_byte = 2
    };  // byte_no codes
private:
    static bool _is_initialized;        // true if TemplateTable has been initialized
    static YuhuTemplate _template_table[Bytecodes::number_of_codes];
    static YuhuTemplate _template_table_wide[Bytecodes::number_of_codes];
    static YuhuTemplate *_desc;                  // the current template to be generated
    static Bytecodes::Code bytecode() { return _desc->bytecode(); }

    static BarrierSet *_bs;                    // Cache the barrier set.
public:
    static YuhuMacroAssembler *_masm;       // the assembler used when generating templates
private:
    // debugging of TemplateGenerator
    static void transition(TosState tos_in, TosState tos_out);// checks if in/out states expected by template generator correspond to table entries

    // special registers
    static inline YuhuAddress at_bcp(int offset);
    static void patch_bytecode(Bytecodes::Code bc, YuhuMacroAssembler::YuhuRegister bc_reg,
                               YuhuMacroAssembler::YuhuRegister temp_reg, bool load_bc_into_bc_reg = true, int byte_no = -1);
    static void locals_index(YuhuMacroAssembler::YuhuRegister reg, int offset = 1);

    // initialization helpers
    static void def(Bytecodes::Code code, int flags, TosState in, TosState out, void (*gen)(            ), char filler );
    static void def(Bytecodes::Code code, int flags, TosState in, TosState out, void (*gen)(int arg     ), int arg     );
    static void def(Bytecodes::Code code, int flags, TosState in, TosState out, void (*gen)(bool arg    ), bool arg    );
    static void def(Bytecodes::Code code, int flags, TosState in, TosState out, void (*gen)(Operation op), Operation op);
    static void def(Bytecodes::Code code, int flags, TosState in, TosState out, void (*gen)(Condition cc), Condition cc);
    static void def(Bytecodes::Code code, int flags, TosState in, TosState out, void (*gen)(TosState tos), TosState tos);

    // bytecodes
    static void nop();

    static void aconst_null();
    static void iconst(int value);
    static void lconst(int value);
    static void fconst(int value);
    static void dconst(int value);

    static void bipush();
    static void sipush();
    static void ldc(bool wide);
    static void ldc2_w();

    static void iload();
    static void lload();
    static void fload();
    static void dload();
    static void aload();

    static void iload(int n);
    static void lload(int n);
    static void fload(int n);
    static void dload(int n);
    static void aload_0();
    static void aload(int n);

    static void iaload();
    static void laload();
    static void faload();
    static void daload();
    static void aaload();
    static void baload();
    static void caload();
    static void saload();

    static void istore();
    static void lstore();
    static void fstore();
    static void dstore();
    static void astore();

    static void istore(int n);
    static void lstore(int n);
    static void fstore(int n);
    static void dstore(int n);
    static void astore(int n);

    static void iastore();
    static void lastore();
    static void fastore();
    static void dastore();
    static void aastore();
    static void bastore();
    static void castore();
    static void sastore();

    static void pop();
    static void pop2();
    static void dup();
    static void dup_x1();
    static void dup_x2();
    static void dup2();
    static void dup2_x1();
    static void dup2_x2();
    static void swap();

    static void iop2(Operation op);
    static void lop2(Operation op);
    static void fop2(Operation op);
    static void dop2(Operation op);

    static void lmul();
    static void idiv();
    static void ldiv();

    static void irem();
    static void lrem();

    static void ineg();
    static void lneg();
    static void fneg();
    static void dneg();

    static void lshl();
    static void lshr();
    static void lushr();

    static void iinc();
    static void convert();

    static void lcmp();
    static void float_cmp (bool is_float, int unordered_result);
    static void float_cmp (int unordered_result);
    static void double_cmp(int unordered_result);

    static void branch(bool is_jsr, bool is_wide);
    static void if_0cmp   (Condition cc);
    static void if_icmp   (Condition cc);
    static void if_acmp   (Condition cc);

    static void _goto();
    static void jsr();
    static void ret();

    static void tableswitch();
    static void lookupswitch();

    static void _return(TosState state);

    static void resolve_cache_and_index(int byte_no,       // one of 1,2,11
                                        YuhuMacroAssembler::YuhuRegister cache,    // output for CP cache
                                        YuhuMacroAssembler::YuhuRegister index,    // output for CP index
                                        size_t index_size); // one of 1,2,4
    static void load_invoke_cp_cache_entry(int byte_no,
                                           YuhuMacroAssembler::YuhuRegister method,
                                           YuhuMacroAssembler::YuhuRegister itable_index,
                                           YuhuMacroAssembler::YuhuRegister flags,
                                           bool is_invokevirtual,
                                           bool is_virtual_final,
                                           bool is_invokedynamic);
    static void load_field_cp_cache_entry(YuhuMacroAssembler::YuhuRegister obj,
                                          YuhuMacroAssembler::YuhuRegister cache,
                                          YuhuMacroAssembler::YuhuRegister index,
                                          YuhuMacroAssembler::YuhuRegister offset,
                                          YuhuMacroAssembler::YuhuRegister flags,
                                          bool is_static);
    static void pop_and_check_object(YuhuMacroAssembler::YuhuRegister obj);

    static void getfield_or_static(int byte_no, bool is_static);
    static void getstatic(int byte_no);
    static void putfield_or_static(int byte_no, bool is_static);
    static void putstatic(int byte_no);
    static void getfield(int byte_no);
    static void putfield(int byte_no);

    static void invokevirtual(int byte_no);
    static void invokespecial(int byte_no);
    static void invokestatic(int byte_no);
    static void invokeinterface(int byte_no);
    static void invokedynamic(int byte_no);

    static void _new();
    static void newarray();
    static void anewarray();

    static void arraylength();
    static void athrow();
    static void checkcast();
    static void instanceof();
public:
    static void initialize();

    static YuhuTemplate* template_for     (Bytecodes::Code code)  { Bytecodes::check     (code); return &_template_table     [code]; }
    static YuhuTemplate* template_for_wide(Bytecodes::Code code)  { Bytecodes::wide_check(code); return &_template_table_wide[code]; }

#ifdef TARGET_ARCH_MODEL_aarch64
# include "yuhu_templateTable_aarch64.hpp"
#endif
};
#endif //JDK8_YUHU_TEMPLATETABLE_HPP

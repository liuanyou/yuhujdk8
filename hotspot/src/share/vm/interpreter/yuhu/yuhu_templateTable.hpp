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
public:
    static void initialize();

    static YuhuTemplate* template_for     (Bytecodes::Code code)  { Bytecodes::check     (code); return &_template_table     [code]; }
    static YuhuTemplate* template_for_wide(Bytecodes::Code code)  { Bytecodes::wide_check(code); return &_template_table_wide[code]; }

#ifdef TARGET_ARCH_MODEL_aarch64
# include "yuhu_templateTable_aarch64.hpp"
#endif
};
#endif //JDK8_YUHU_TEMPLATETABLE_HPP

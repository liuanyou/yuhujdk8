//
// Created by Anyou Liu on 2025/4/24.
//

#ifndef JDK8_YUHU_INTERPRETERGENERATOR_HPP
#define JDK8_YUHU_INTERPRETERGENERATOR_HPP

#include "code/stubs.hpp"
#include "interpreter/bytecodes.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/top.hpp"
#include "asm/yuhu/yuhu_macroAssembler.hpp"

class YuhuInterpreterGenerator: public StackObj {
protected:
    YuhuMacroAssembler* _masm;
    address _unimplemented_bytecode;
    address _illegal_bytecode_sequence;
    void generate_all();
    address generate_error_exit(const char* msg);
    address generate_return_entry_for(TosState state, int step, size_t index_size);
    address generate_deopt_entry_for(TosState state, int step);
    address generate_result_handler_for(BasicType type);
    address generate_continuation_for(TosState state);
    address generate_safept_entry_for(TosState state, address runtime_entry);

    void generate_and_dispatch (YuhuTemplate* t, TosState tos_out = ilgl);
    void set_vtos_entry_points (YuhuTemplate* t, address& bep, address& cep, address& sep, address& aep, address& iep, address& lep, address& fep, address& dep, address& vep);
    void set_short_entry_points(YuhuTemplate* t, address& bep, address& cep, address& sep, address& aep, address& iep, address& lep, address& fep, address& dep, address& vep);
    void set_wide_entry_point  (YuhuTemplate* t, address& wep);
    void set_entry_points(Bytecodes::Code code);
    void set_unimplemented(int i);
    void set_entry_points_for_all_bytes();
    void set_safepoints_for_all_bytes();

    // entry point generator
    address generate_method_entry(YuhuInterpreter::MethodKind kind);

    void bang_stack_shadow_pages(bool native_call);

    void initialize_method_handle_entries();
public:
    YuhuInterpreterGenerator();

#ifdef TARGET_ARCH_aarch64
# include "yuhu_interpreterGenerator_aarch64.hpp"
#endif
};

#endif //JDK8_YUHU_INTERPRETERGENERATOR_HPP

//
// Created by Anyou Liu on 2025/5/6.
//

#ifndef JDK8_YUHU_INTERPRETERGENERATOR_AARCH64_HPP
#define JDK8_YUHU_INTERPRETERGENERATOR_AARCH64_HPP

protected:

    void bang_stack_shadow_pages(bool native_call);
    void generate_fixed_frame(bool native_call);
private:

    address generate_normal_entry(bool synchronized);
    address generate_native_entry(bool synchronized);
    address generate_abstract_entry(void);
    address generate_math_entry(YuhuInterpreter::MethodKind kind);
    void generate_transcendental_entry(YuhuInterpreter::MethodKind kind, int fpargs);
    address generate_empty_entry(void);
    address generate_accessor_entry(void);
    address generate_Reference_get_entry();
    address generate_CRC32_update_entry();
    address generate_CRC32_updateBytes_entry(YuhuInterpreter::MethodKind kind);
    void lock_method(void);
    void generate_stack_overflow_check(void);

//    void generate_counter_incr(Label* overflow, Label* profile_method, Label* profile_method_continue);
//    void generate_counter_overflow(Label* do_continue);

#endif //JDK8_YUHU_INTERPRETERGENERATOR_AARCH64_HPP

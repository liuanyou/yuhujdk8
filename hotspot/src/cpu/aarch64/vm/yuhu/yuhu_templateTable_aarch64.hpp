//
// Created by Anyou Liu on 2025/7/25.
//

#ifndef JDK8_YUHU_TEMPLATETABLE_AARCH64_HPP
#define JDK8_YUHU_TEMPLATETABLE_AARCH64_HPP

static void prepare_invoke(int byte_no,
                           YuhuMacroAssembler::YuhuRegister method,         // linked method (or i-klass)
                           YuhuMacroAssembler::YuhuRegister index = YuhuMacroAssembler::noreg,  // itable index, MethodType, etc.
                           YuhuMacroAssembler::YuhuRegister recv  = YuhuMacroAssembler::noreg,  // if caller wants to see it
                           YuhuMacroAssembler::YuhuRegister flags = YuhuMacroAssembler::noreg   // if caller wants to test it
);
static void invokevirtual_helper(YuhuMacroAssembler::YuhuRegister index, YuhuMacroAssembler::YuhuRegister recv,
                                 YuhuMacroAssembler::YuhuRegister flags);

// Helpers
static void index_check(YuhuMacroAssembler::YuhuRegister array, YuhuMacroAssembler::YuhuRegister index);

#endif //JDK8_YUHU_TEMPLATETABLE_AARCH64_HPP

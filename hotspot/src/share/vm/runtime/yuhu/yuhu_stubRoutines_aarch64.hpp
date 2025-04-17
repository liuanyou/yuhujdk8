//
// Created by Anyou Liu on 2025/4/16.
//

#ifndef JDK8_YUHU_STUBROUTINES_AARCH64_HPP
#define JDK8_YUHU_STUBROUTINES_AARCH64_HPP

static bool returns_to_call_stub(address return_pc)   {
    return return_pc == YuhuStubRoutines::_call_stub_return_address;
}

#endif //JDK8_YUHU_STUBROUTINES_AARCH64_HPP

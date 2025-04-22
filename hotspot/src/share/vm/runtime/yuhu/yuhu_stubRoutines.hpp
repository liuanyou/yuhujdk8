//
// Created by Anyou Liu on 2025/4/2.
//

#ifndef JDK8_YUHU_STUBROUTINES_HPP
#define JDK8_YUHU_STUBROUTINES_HPP

#include "code/codeBlob.hpp"
#include "memory/allocation.hpp"
#include "runtime/frame.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/stubCodeGenerator.hpp"
#include "utilities/top.hpp"

enum platform_dependent_constants {
    code_size1 = 19000,          // simply increase if too small (assembler will crash if too small)
    code_size2 = 22000           // simply increase if too small (assembler will crash if too small)
};

class YuhuStubRoutines : AllStatic {
public:
    static address _call_stub_entry;
    static address _call_stub_return_address;

    static address _catch_exception_entry;

    static BufferBlob* _code1;
public:
    // Initialization/Testing
    static void    initialize1();                            // must happen before universe::genesis
    static void    initialize2();                            // must happen after  universe::genesis

    // Calls to Java
    typedef void (*CallStub)(
            address   link,
            intptr_t* result,
            BasicType result_type,
            Method* method,
            address   entry_point,
            intptr_t* parameters,
            int       size_of_parameters,
            TRAPS
    );

    static CallStub call_stub()                              { return CAST_TO_FN_PTR(CallStub, _call_stub_entry); }

    static address catch_exception_entry()                   { return _catch_exception_entry; }

#ifdef TARGET_ARCH_aarch64
#include "yuhu_stubRoutines_aarch64.hpp"
#endif
};

#endif //JDK8_YUHU_STUBROUTINES_HPP

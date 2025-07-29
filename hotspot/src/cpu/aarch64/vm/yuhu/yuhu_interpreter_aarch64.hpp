//
// Created by Anyou Liu on 2025/4/24.
//

#ifndef JDK8_YUHU_INTERPRETER_AARCH64_HPP
#define JDK8_YUHU_INTERPRETER_AARCH64_HPP

public:
    // Size of interpreter code.  Increase if too small.  Interpreter will
    // fail with a guarantee ("not enough space for interpreter generation");
    // if too small.
    // Run with +PrintInterpreter to get the VM to print out the size.
    // Max size with JVMTI
    const static int InterpreterCodeSize = 200 * 1024;

    // Offset from rsp (which points to the last stack element)
    static int expr_offset_in_bytes(int i) { return stackElementSize * i; }

#endif //JDK8_YUHU_INTERPRETER_AARCH64_HPP

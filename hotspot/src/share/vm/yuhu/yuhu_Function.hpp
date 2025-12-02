#ifndef SHARE_VM_YUHU_YUHU_FUNCTION_HPP
#define SHARE_VM_YUHU_YUHU_FUNCTION_HPP

#include "yuhu/yuhu_llvmHeaders.hpp"

class ciEnv;
class ciTypeFlow;
class YuhuBuilder;

class YuhuFunction {
 public:
  // Main entry point for building LLVM IR from Java bytecode
  static llvm::Function* build(ciEnv* env, 
                               YuhuBuilder* builder,
                               ciTypeFlow* flow,
                               const char* name);

 private:
  // Bytecode translation helpers
  static void translate_bytecode(ciEnv* env, YuhuBuilder* builder, 
                                ciTypeFlow* flow, llvm::Function* function);

  // Leverage existing Yuhu interpreter bytecode handlers
  static void translate_arithmetic(ciEnv* env, YuhuBuilder* builder, 
                                  int bci, Bytecodes::Code code);
  static void translate_load_store(ciEnv* env, YuhuBuilder* builder,
                                  int bci, Bytecodes::Code code);
  static void translate_control_flow(ciEnv* env, YuhuBuilder* builder,
                                    int bci, Bytecodes::Code code);
};











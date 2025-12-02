#ifndef SHARE_VM_YUHU_YUHU_LLVM_HEADERS_HPP
#define SHARE_VM_YUHU_YUHU_LLVM_HEADERS_HPP

#include "utilities/globals.hpp"

// LLVM headers - following Yuhu pattern
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/Function.h"
#include "llvm/Type.h"
#include "llvm/DerivedTypes.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JITMemoryManager.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/PassManager.h"

// LLVM namespaces
using namespace llvm;

#endif // SHARE_VM_YUHU_YUHU_LLVM_HEADERS_HPP











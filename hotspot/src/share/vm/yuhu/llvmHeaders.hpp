/*
 * Copyright (c) 1999, 2011, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2008, 2009, 2010 Red Hat, Inc.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_VM_YUHU_LLVMHEADERS_HPP
#define SHARE_VM_YUHU_LLVMHEADERS_HPP

#ifdef assert
  #undef assert
#endif

#ifdef DEBUG
  #define YUHU_DEBUG
  #undef DEBUG
#endif

// CRITICAL: Undefine HotSpot macros BEFORE including ANY LLVM headers
// HotSpot defines max() and min() macros, but LLVM uses std::numeric_limits<T>::max() and min()
// These macros must be undefined before llvm-config.h or any other LLVM header is included
#ifdef max
  #undef max
#endif
#ifdef min
  #undef min
#endif

// HotSpot defines AARCH64 as 1, but LLVM uses AARCH64 as an enum identifier
// We need to undefine it before including LLVM headers, then redefine it after
#ifdef AARCH64
  #define YUHU_AARCH64_WAS_DEFINED
  #undef AARCH64
#endif

// Include LLVM config to get version information
// This must be after undefining macros, but before other LLVM headers
#include <llvm/Config/llvm-config.h>

// Define YUHU_LLVM_VERSION for compatibility with old code
// Format: major * 10 + minor (e.g., 31 for 3.1, 40 for 4.0, 200 for 20.0)
#ifndef YUHU_LLVM_VERSION
#define YUHU_LLVM_VERSION (LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR)
#endif

// Verifier header location changed in LLVM 3.8+
// LLVM 3.8+ uses llvm/IR/Verifier.h, older versions use llvm/Analysis/Verifier.h
// LLVM 20.1.5 (and all modern versions) use llvm/IR/Verifier.h
//
// IMPORTANT: This file requires LLVM to be installed and configured!
// If you don't have LLVM installed, you should NOT define YUHU macro during build.
// All Yuhu code is protected by #ifdef YUHU, so it won't be compiled if YUHU is not defined.
//
// To use Yuhu compiler:
// 1. Install LLVM development libraries (see doc/yuhu/BUILD_INSTRUCTIONS.md)
// 2. Configure LLVM paths in hotspot/make/bsd/makefiles/yuhu.make
//    (yuhu.make uses llvm-config for automatic configuration)
// 3. Build with TYPE=YUHU
//
#ifdef YUHU_USE_OLD_LLVM_VERIFIER
  // Use old location for LLVM < 3.8 (deprecated, not recommended)
  #include <llvm/Analysis/Verifier.h>
#else
  // Use new location (LLVM 3.8+, including LLVM 20.1.5)
  // If compilation fails here, see BUILD_INSTRUCTIONS.md for setup help
  #include <llvm/IR/Verifier.h>
#endif

// LLVM 4.0+ moved many headers to llvm/IR/ directory
// LLVM 20 uses the new structure
#if LLVM_VERSION_MAJOR >= 4
  // LLVM 4.0+ (including LLVM 20)
  #include <llvm/IR/Argument.h>
  #include <llvm/IR/Constants.h>
  #include <llvm/IR/DerivedTypes.h>
  #include <llvm/IR/Instructions.h>
  #include <llvm/IR/LLVMContext.h>
  #include <llvm/IR/Module.h>
  #include <llvm/IR/Type.h>
  #include <llvm/IR/IRBuilder.h>
#else
  // LLVM 3.x (old structure)
  #include <llvm/Argument.h>
  #include <llvm/Constants.h>
  #include <llvm/DerivedTypes.h>
  #include <llvm/Instructions.h>
  #include <llvm/LLVMContext.h>
  #include <llvm/Module.h>
  #include <llvm/Type.h>
  #if YUHU_LLVM_VERSION <= 31
  #include <llvm/Support/IRBuilder.h>
  #else
  #include <llvm/IRBuilder.h>
  #endif
#endif

// ORC JIT (LLVM 11+) - recommended for LLVM 20
// Note: ORC JIT requires LLVM 11 or later. LLVM 20 is recommended.
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/Transforms/Utils/Cloning.h>  // For CloneModule
#include <llvm/Support/Error.h>  // For handleAllErrors, ErrorInfoBase
// Keep ExecutionEngine.h for forward declarations if needed
#include <llvm/ExecutionEngine/ExecutionEngine.h>

#include <llvm/Support/Threading.h>
#include <llvm/Support/TargetSelect.h>
#if LLVM_VERSION_MAJOR >= 20
#include <llvm/Target/TargetMachine.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/TargetParser/Triple.h>
#endif

// JITMemoryManager was deprecated in LLVM 3.7 and removed in LLVM 3.9+
// For LLVM 4.0+, we need to use RTDyldMemoryManager or SectionMemoryManager
#if LLVM_VERSION_MAJOR >= 4
  // LLVM 4.0+ (including LLVM 20) - JITMemoryManager removed
  // Include new memory manager headers
  #include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
  #include <llvm/ExecutionEngine/SectionMemoryManager.h>
  // Forward declare JITMemoryManager for compatibility
  // Note: YuhuMemoryManager needs to be refactored to use RTDyldMemoryManager
  namespace llvm {
    // JITMemoryManager was removed in LLVM 4.0+
    // For now, we create a forward declaration to allow compilation
    // TODO: Refactor YuhuMemoryManager to inherit from RTDyldMemoryManager
    class JITMemoryManager;
  }
#else
  // LLVM 3.x - old JITMemoryManager API
  #include <llvm/ExecutionEngine/JITMemoryManager.h>
#endif
#include <llvm/Support/CommandLine.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
// LLVM 15+ moved Host.h to TargetParser directory
#if LLVM_VERSION_MAJOR >= 15
  #include <llvm/TargetParser/Host.h>
#else
  #include <llvm/Support/Host.h>
#endif

// Restore AARCH64 macro if it was defined before
// This must be done after all LLVM headers are included
#ifdef YUHU_AARCH64_WAS_DEFINED
  #undef YUHU_AARCH64_WAS_DEFINED
  #define AARCH64 1
#endif

#include <map>

#ifdef assert
  #undef assert
#endif

// from hotspot/src/share/vm/utilities/debug.hpp
#ifdef ASSERT
#ifndef USE_REPEATED_ASSERTS
#define assert(p, msg)                                                       \
do {                                                                         \
  if (!(p)) {                                                                \
    report_vm_error(__FILE__, __LINE__, "assert(" #p ") failed", msg);       \
    BREAKPOINT;                                                              \
  }                                                                          \
} while (0)
#else // #ifndef USE_REPEATED_ASSERTS
#define assert(p, msg)
do {                                                                         \
  for (int __i = 0; __i < AssertRepeat; __i++) {                             \
    if (!(p)) {                                                              \
      report_vm_error(__FILE__, __LINE__, "assert(" #p ") failed", msg);     \
      BREAKPOINT;                                                            \
    }                                                                        \
  }                                                                          \
} while (0)
#endif // #ifndef USE_REPEATED_ASSERTS
#else
  #define assert(p, msg)
#endif

#ifdef DEBUG
  #undef DEBUG
#endif
#ifdef YUHU_DEBUG
  #define DEBUG
  #undef YUHU_DEBUG
#endif

#endif // SHARE_VM_YUHU_LLVMHEADERS_HPP

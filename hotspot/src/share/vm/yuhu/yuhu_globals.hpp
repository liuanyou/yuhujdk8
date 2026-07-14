/*
 * Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_YUHU_YUHU_GLOBALS_HPP
#define SHARE_VM_YUHU_YUHU_GLOBALS_HPP

#include "runtime/globals.hpp"
#ifdef TARGET_ARCH_zero
# include "yuhu_globals_zero.hpp"
#endif
// Note: AArch64 does not need platform-specific globals

#define YUHU_FLAGS(develop, develop_pd, product, product_pd, diagnostic, notproduct) \
                                                                              \
  /* inlining */                                                              \
  product(intx, YuhuMaxInlineSize, 32,                                       \
          "Maximum bytecode size of methods to inline when using Yuhu")      \
                                                                              \
  product(ccstr, YuhuOptimizationLevel, "Default",                           \
          "The optimization level passed to LLVM, possible values: None, Less, Default and Agressive") \
                                                                              \
  /* compiler debugging */                                                    \
  develop(ccstr, YuhuPrintTypeflowOf, NULL,                                  \
          "Print the typeflow of the specified method")                       \
                                                                              \
  develop(ccstr, YuhuCompileOnlyOf, NULL,                                  \
          "Compile the specified method only")                       \
                                                                              \
  diagnostic(ccstr, YuhuPrintBitcodeOf, NULL,                                \
          "Print the LLVM bitcode of the specified method")                   \
                                                                              \
  diagnostic(ccstr, YuhuPrintAsmOf, NULL,                                    \
          "Print the asm of the specified method")                            \
                                                                              \
  diagnostic(bool, YuhuTraceMachineCode, false,                               \
          "Trace machine code generation from LinkGraph (AArch64 disassembly)") \
                                                                                     \
  diagnostic(ccstr, YuhuStackMapFile, NULL,                                    \
          "Dump stack map to file (default file is yuhu_stack_map.txt)")                            \
                                                                              \
  diagnostic(bool, YuhuTraceIRCompilation, false,                             \
          "Trace IR to object file compilation (print IR and disassemble object file)") \
                                                                              \
  develop(bool, YuhuTraceBytecodes, false,                                   \
          "Trace bytecode compilation")                                       \
                                                                              \
  diagnostic(bool, YuhuTraceInstalls, false,                                 \
          "Trace method installation")                                        \
                                                                              \
  diagnostic(bool, YuhuPerformanceWarnings, false,                           \
          "Warn about things that could be made faster")                      \
                                                                              \
  develop(ccstr, YuhuVerifyFunction, NULL,                                   \
          "Runs LLVM verify over LLVM IR")                                    \
                                                                              \
  diagnostic(bool, YuhuDumpIRToFile, false,                                   \
          "Dump LLVM IR to _tmp_yuhu_ir_*.ll files for debugging")            \
                                                                              \
  diagnostic(bool, YuhuTraceOsrCompilation, false,                            \
          "Trace osr compilation")                                            \
                                                                              \
  diagnostic(bool, YuhuTracePHI, false,                                       \
          "Trace phi state")                                                  \
                                                                              \
  diagnostic(bool, YuhuTraceFunction, false,                                  \
          "Trace function")                                                   \
                                                                              \
  diagnostic(bool, YuhuTraceOffset, false,                                    \
          "Trace offset")                                                     \

// Yuhu frame layout constants
const int YUHU_FRAME_HEADER_WORDS = 6;  // Frame header size in words

// AArch64 instruction encodings
const uint32_t BLR_MASK = 0xFFFFFC1F;
const uint32_t BLR_PATTERN = 0xD63F0000;
const uint32_t B_MASK = 0xFC000000;
const uint32_t B_PATTERN = 0x14000000;

// Virtual address magic numbers for placeholder identification
const uint64_t LAST_JAVA_PC_MAGIC = 0xDEAD0000;
const uint64_t CALL_TARGET_MAGIC = 0xBEEF;

// deopt statepoint id
const uint64_t DEOPT_STATEPOINT_ID = 4096;

const uint64_t EXTENDED_SP_ALLOCA_STATEPOINT_ID = 1024;

const uint64_t UNIFIED_EXIT_BLOCK_START_STATEPOINT_ID = 1026;

YUHU_FLAGS(DECLARE_DEVELOPER_FLAG, DECLARE_PD_DEVELOPER_FLAG, DECLARE_PRODUCT_FLAG, DECLARE_PD_PRODUCT_FLAG, DECLARE_DIAGNOSTIC_FLAG, DECLARE_NOTPRODUCT_FLAG)

#endif // SHARE_VM_YUHU_YUHU_GLOBALS_HPP

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
  diagnostic(ccstr, YuhuPrintBitcodeOf, NULL,                                \
          "Print the LLVM bitcode of the specified method")                   \
                                                                              \
  diagnostic(ccstr, YuhuPrintAsmOf, NULL,                                    \
          "Print the asm of the specified method")                            \
                                                                              \
  diagnostic(bool, YuhuTraceMachineCode, false,                               \
          "Trace machine code generation from LinkGraph (AArch64 disassembly)") \
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
          "Dump LLVM IR to /tmp/yuhu_ir_*.ll files for debugging")            \
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
const int yuhu_frame_header_words = 6;  // Frame header size in words

// LLVM register spill reservation (AArch64)
// Reserve space for all callee-saved registers (x19-x28 = 10 regs = 80 bytes)
// This is the MAXIMUM LLVM could possibly need for register spills.
// Both yuhuFunction.cpp (alloca) and yuhuStack.cpp (frame size) must use this value.
const int yuhu_llvm_spill_slots = 10;  // 80 bytes - absolute maximum for AArch64

// Callee-saved register save area (AArch64)
// Reserve 6 words (48 bytes) right after LLVM spill slots for saving callee-saved registers
// across Java method calls. Location: [sp, #80] (fixed offset after 80-byte spill area).
// Saves: x19, x20, x23, x25, x27 (5 regs = 40 bytes) + 8 bytes padding for 16-byte alignment.
const int yuhu_callee_saved_save_area = 6;  // 48 bytes

YUHU_FLAGS(DECLARE_DEVELOPER_FLAG, DECLARE_PD_DEVELOPER_FLAG, DECLARE_PRODUCT_FLAG, DECLARE_PD_PRODUCT_FLAG, DECLARE_DIAGNOSTIC_FLAG, DECLARE_NOTPRODUCT_FLAG)

#endif // SHARE_VM_YUHU_YUHU_GLOBALS_HPP

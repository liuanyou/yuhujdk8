#
# Copyright (c) 1999, 2010, Oracle and/or its affiliates. All rights reserved.
# Copyright 2008, 2010 Red Hat, Inc.
# DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
#
# This code is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License version 2 only, as
# published by the Free Software Foundation.
#
# This code is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# version 2 for more details (a copy is included in the LICENSE file that
# accompanied this code).
#
# You should have received a copy of the GNU General Public License version
# 2 along with this work; if not, write to the Free Software Foundation,
# Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
#
# Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
# or visit www.oracle.com if you need additional information or have any
# questions.
#  
#

# Sets make macros for making Yuhu version of VM
# Note: Yuhu compiler requires LLVM to be installed and configured
# This Makefile is used when --enable-yuhu-compiler=yes is specified during configure

VM_SUBDIR = server

# Only define YUHU macro if USE_YUHU_COMPILER is true
# USE_YUHU_COMPILER is set by configure script based on --enable-yuhu-compiler
ifeq ($(USE_YUHU_COMPILER), true)
  CFLAGS += -DYUHU

  # LLVM configuration from configure script
  # These variables are set by configure based on:
  #   --with-llvm=PREFIX (sets both include and lib paths)
  #   --with-llvm-include=DIR (overrides include path)
  #   --with-llvm-lib=DIR (overrides lib path)
  ifneq ($(LLVM_INCLUDE_PATH),)
    CFLAGS += -I$(LLVM_INCLUDE_PATH)
  endif

  ifneq ($(LLVM_LIB_PATH),)
    LDFLAGS += -L$(LLVM_LIB_PATH)
  endif

  # Try to use llvm-config for additional flags if available
  # First try common installation paths, then fall back to PATH
  # Check if LLVM_CONFIG was set by configure script, otherwise try common paths
  ifeq ($(LLVM_CONFIG),)
    LLVM_CONFIG := $(shell \
      if [ -x "/opt/homebrew/opt/llvm/bin/llvm-config" ]; then \
        echo "/opt/homebrew/opt/llvm/bin/llvm-config"; \
      elif [ -x "/usr/local/opt/llvm/bin/llvm-config" ]; then \
        echo "/usr/local/opt/llvm/bin/llvm-config"; \
      elif [ -x "/opt/homebrew/bin/llvm-config" ]; then \
        echo "/opt/homebrew/bin/llvm-config"; \
      elif command -v llvm-config >/dev/null 2>&1; then \
        echo "llvm-config"; \
      else \
        echo ""; \
      fi)
  endif
  
  ifneq ($(LLVM_CONFIG),)
    ifneq ($(shell test -x "$(LLVM_CONFIG)" 2>/dev/null && echo "ok"),)
      LLVM_CXXFLAGS_RAW := $(shell $(LLVM_CONFIG) --cxxflags 2>/dev/null)
      LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags 2>/dev/null)
      # Get core LLVM libraries
      LLVM_LIBS_CORE := $(shell $(LLVM_CONFIG) --libs core executionengine jit native 2>/dev/null)
      # Get AArch64 backend libraries (needed for LLVMInitializeAArch64Target)
      LLVM_LIBS_AARCH64 := $(shell $(LLVM_CONFIG) --libs aarch64 2>/dev/null)
      # Combine all LLVM libraries
      LLVM_LIBS := $(LLVM_LIBS_CORE) $(LLVM_LIBS_AARCH64)
      # Also get system libraries that LLVM depends on
      LLVM_SYSTEM_LIBS := $(shell $(LLVM_CONFIG) --system-libs 2>/dev/null)

      # Filter out C++ standard flags from LLVM (JDK uses C++11, not C++17)
      # Remove -std=c++17, -std=c++14, -std=c++11, -std=c++98, etc.
      LLVM_CXXFLAGS := $(filter-out -std=c++%,$(LLVM_CXXFLAGS_RAW))

      # Add LLVM flags (without C++ standard)
      CFLAGS += $(LLVM_CXXFLAGS)
      # Export LLVM_LIBS and LLVM_LDFLAGS for vm.make to use
      # Note: These variables will be used in vm.make similar to JVM_VARIANT_ZEROSHARK
    else
      # llvm-config not found or not executable
      $(warning llvm-config not found at $(LLVM_CONFIG), LLVM linking may fail)
      # Fallback: try to use LLVM 20's unified library
      # LLVM 20 uses a single large library libLLVM-20.dylib
      LLVM_LIBS := -lLLVM-20
      LLVM_LDFLAGS :=
      LLVM_SYSTEM_LIBS :=
    endif
  else
    # Fallback: try to use LLVM 20's unified library
    # LLVM 20 uses a single large library libLLVM-20.dylib
    # Note: This may not work for all LLVM installations
    LLVM_LIBS := -lLLVM-20
    LLVM_LDFLAGS :=
    LLVM_SYSTEM_LIBS :=
    $(warning llvm-config not found, using fallback -lLLVM-20. This may not work correctly.)
  endif

  # If using LLVM < 3.8, uncomment this (not needed for LLVM 15+):
  # CFLAGS += -DYUHU_USE_OLD_LLVM_VERIFIER

  # Note: LLVM 16.0+ requires C++17, but JDK 8 uses C++11
  # Solution: Use forward declarations in header files to avoid including LLVM headers
  # Only Yuhu .cpp files need C++17 (they include llvmHeaders.hpp)
  # Header files (yuhuCompiler.hpp) use forward declarations, so compileBroker.cpp can use C++11
  #
  # Set C++17 only for Yuhu .cpp files that include LLVM headers
  # List all Yuhu source files that need C++17
  # Also disable the reserved-user-defined-literal warning for C++17 compatibility
  # This warning occurs because some HotSpot code uses string literal concatenation
  # without spaces (e.g., "string"MACRO"string"), which is valid in C++11 but
  # triggers a warning in C++17. We disable it for Yuhu files only.
  CXXFLAGS/yuhuCompiler.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuContext.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuBuilder.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuStack.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuRuntime.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuType.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuFunction.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuEntry.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuCodeBuffer.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuMemoryManager.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuNativeWrapper.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuValue.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhu_globals.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuCacheDecache.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuInvariants.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuIntrinsics.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuConstant.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuInliner.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuTopLevelBlock.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuStateScanner.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuBlock.o += -std=c++17 -Wno-reserved-user-defined-literal
  CXXFLAGS/yuhuState.o += -std=c++17 -Wno-reserved-user-defined-literal
  # Note: compileBroker.cpp does NOT need C++17 because yuhuCompiler.hpp uses forward declarations

  # Disable PCH (Precompiled Headers) for all Yuhu .cpp files
  # PCH is compiled with C++11, but Yuhu files need C++17
  # Use the same pattern as gcc.make for other files that disable PCH
  # This must be set after gcc.make is included (which defines PCH_FLAG/NO_PCH)
  ifdef PCH_FLAG/NO_PCH
    PCH_FLAG/yuhuCompiler.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuContext.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuBuilder.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuStack.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuRuntime.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuType.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuFunction.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuEntry.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuCodeBuffer.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuMemoryManager.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuNativeWrapper.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuValue.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhu_globals.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuCacheDecache.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuInvariants.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuIntrinsics.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuConstant.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuInliner.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuTopLevelBlock.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuStateScanner.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuBlock.o = $(PCH_FLAG/NO_PCH)
    PCH_FLAG/yuhuState.o = $(PCH_FLAG/NO_PCH)
  endif
else
  # YUHU compiler is disabled, do not define YUHU macro
  # All YUHU code will be excluded by #ifdef YUHU guards
endif


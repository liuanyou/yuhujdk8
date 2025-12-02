/*
 * Copyright (c) 1999, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2009 Red Hat, Inc.
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

#ifndef SHARE_VM_YUHU_YUHUMEMORYMANAGER_HPP
#define SHARE_VM_YUHU_YUHUMEMORYMANAGER_HPP

#include "yuhu/llvmHeaders.hpp"
#include "yuhu/yuhuEntry.hpp"

// YuhuMemoryManager wraps the LLVM JIT Memory Manager.  We could use
// this to run our own memory allocation policies, but for now all we
// use it for is figuring out where the resulting native code ended up.

// Note: JITMemoryManager was removed in LLVM 4.0+
// For LLVM 20, we need to adapt this to use RTDyldMemoryManager
// For now, we use conditional compilation to support both old and new APIs
#if LLVM_VERSION_MAJOR >= 4
  // LLVM 4.0+ (including LLVM 20) - JITMemoryManager removed
  // TODO: Refactor YuhuMemoryManager to inherit from RTDyldMemoryManager
  // For now, we create a minimal implementation that can compile
  class YuhuMemoryManager : public llvm::RTDyldMemoryManager {
   private:
    std::map<const llvm::Function*, YuhuEntry*> _entry_map;

   public:
    YuhuMemoryManager() : llvm::RTDyldMemoryManager() {}

    void set_entry_for_function(const llvm::Function* function,
                                YuhuEntry*           entry) {
      _entry_map[function] = entry;
    }
    YuhuEntry* get_entry_for_function(const llvm::Function* function) {
      auto it = _entry_map.find(function);
      return (it != _entry_map.end()) ? it->second : NULL;
    }

    // RTDyldMemoryManager interface methods
    // Note: These need to be implemented properly for LLVM 20
    uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment, unsigned SectionID, llvm::StringRef SectionName) override;
    uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment, unsigned SectionID, llvm::StringRef SectionName, bool IsReadOnly) override;
    void registerEHFrames(uint8_t *Addr, uint64_t LoadAddr, size_t Size) override;
    void deregisterEHFrames() override;
    uint64_t getSymbolAddress(const std::string &Name) override;
    void *getPointerToNamedFunction(const std::string &Name, bool AbortOnFailure = true) override;
    bool finalizeMemory(std::string *ErrMsg = nullptr) override;
  };
#else
  // LLVM 3.x - old JITMemoryManager API
  class YuhuMemoryManager : public llvm::JITMemoryManager {
   public:
    YuhuMemoryManager()
      : _mm(llvm::JITMemoryManager::CreateDefaultMemManager()) {}

   private:
    llvm::JITMemoryManager* _mm;

   private:
    llvm::JITMemoryManager* mm() const {
      return _mm;
    }

   private:
    std::map<const llvm::Function*, YuhuEntry*> _entry_map;

   public:
    void set_entry_for_function(const llvm::Function* function,
                                YuhuEntry*           entry) {
      _entry_map[function] = entry;
    }
    YuhuEntry* get_entry_for_function(const llvm::Function* function) {
      return _entry_map[function];
    }

   public:
    void AllocateGOT();
    unsigned char* getGOTBase() const;
    unsigned char* allocateStub(const llvm::GlobalValue* F,
                                unsigned StubSize,
                                unsigned Alignment);
    unsigned char* startFunctionBody(const llvm::Function* F,
                                     uintptr_t& ActualSize);
    void endFunctionBody(const llvm::Function* F,
                         unsigned char* FunctionStart,
                         unsigned char* FunctionEnd);
    unsigned char* startExceptionTable(const llvm::Function* F,
                                       uintptr_t& ActualSize);
    void endExceptionTable(const llvm::Function* F,
                           unsigned char* TableStart,
                           unsigned char* TableEnd,
                           unsigned char* FrameRegister);
    void *getPointerToNamedFunction(const std::string &Name, bool AbortOnFailure = true);
    uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment, unsigned SectionID);
    uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment, unsigned SectionID);
    void setPoisonMemory(bool);
    uint8_t* allocateGlobal(uintptr_t, unsigned int);
    void setMemoryWritable();
    void setMemoryExecutable();
    void deallocateExceptionTable(void *ptr);
    void deallocateFunctionBody(void *ptr);
    unsigned char *allocateSpace(intptr_t Size,
                                 unsigned int Alignment);
  };
#endif

#endif // SHARE_VM_YUHU_YUHUMEMORYMANAGER_HPP

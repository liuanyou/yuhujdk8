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

#include "precompiled.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/yuhuEntry.hpp"
#include "yuhu/yuhuMemoryManager.hpp"
#include "memory/allocation.hpp"
#include "runtime/os.hpp"

using namespace llvm;

// LLVM 3.x (old JITMemoryManager API) - these methods only exist for old LLVM versions
#if LLVM_VERSION_MAJOR < 4
void YuhuMemoryManager::AllocateGOT() {
  mm()->AllocateGOT();
}

unsigned char* YuhuMemoryManager::getGOTBase() const {
  return mm()->getGOTBase();
}

unsigned char* YuhuMemoryManager::allocateStub(const GlobalValue* F,
                                                unsigned StubSize,
                                                unsigned Alignment) {
  return mm()->allocateStub(F, StubSize, Alignment);
}

unsigned char* YuhuMemoryManager::startFunctionBody(const Function* F,
                                                     uintptr_t& ActualSize) {
  return mm()->startFunctionBody(F, ActualSize);
}

void YuhuMemoryManager::endFunctionBody(const Function* F,
                                         unsigned char* FunctionStart,
                                         unsigned char* FunctionEnd) {
  mm()->endFunctionBody(F, FunctionStart, FunctionEnd);

  YuhuEntry *entry = get_entry_for_function(F);
  if (entry != NULL)
    entry->set_code_limit(FunctionEnd);
}

unsigned char* YuhuMemoryManager::startExceptionTable(const Function* F,
                                                       uintptr_t& ActualSize) {
  return mm()->startExceptionTable(F, ActualSize);
}

void YuhuMemoryManager::endExceptionTable(const Function* F,
                                           unsigned char* TableStart,
                                           unsigned char* TableEnd,
                                           unsigned char* FrameRegister) {
  mm()->endExceptionTable(F, TableStart, TableEnd, FrameRegister);
}

void YuhuMemoryManager::setMemoryWritable() {
  mm()->setMemoryWritable();
}

void YuhuMemoryManager::setMemoryExecutable() {
  mm()->setMemoryExecutable();
}

void YuhuMemoryManager::deallocateExceptionTable(void *ptr) {
  mm()->deallocateExceptionTable(ptr);
}

void YuhuMemoryManager::deallocateFunctionBody(void *ptr) {
  mm()->deallocateFunctionBody(ptr);
}

uint8_t* YuhuMemoryManager::allocateGlobal(uintptr_t Size,
                                            unsigned int Alignment) {
  return mm()->allocateGlobal(Size, Alignment);
}

void* YuhuMemoryManager::getPointerToNamedFunction(const std::string &Name, bool AbortOnFailure) {
  return mm()->getPointerToNamedFunction(Name, AbortOnFailure);
}

uint8_t* YuhuMemoryManager::allocateCodeSection(uintptr_t Size, unsigned Alignment, unsigned SectionID) {
  return mm()->allocateCodeSection(Size, Alignment, SectionID);
}

uint8_t* YuhuMemoryManager::allocateDataSection(uintptr_t Size, unsigned Alignment, unsigned SectionID) {
  return mm()->allocateDataSection(Size, Alignment, SectionID);
}

void YuhuMemoryManager::setPoisonMemory(bool poison) {
  mm()->setPoisonMemory(poison);
}

unsigned char *YuhuMemoryManager::allocateSpace(intptr_t Size,
                                                 unsigned int Alignment) {
  return mm()->allocateSpace(Size, Alignment);
}
#endif // LLVM_VERSION_MAJOR < 4

// LLVM 4.0+ (including LLVM 20) - RTDyldMemoryManager implementation
#if LLVM_VERSION_MAJOR >= 4
uint8_t *YuhuMemoryManager::allocateCodeSection(uintptr_t Size, unsigned Alignment, unsigned SectionID, llvm::StringRef SectionName) {
  // Use SectionMemoryManager as a base implementation
  // For now, we use a simple malloc-based allocation
  // TODO: Implement proper memory management
  return (uint8_t*)os::malloc(Size, mtCode);
}

uint8_t *YuhuMemoryManager::allocateDataSection(uintptr_t Size, unsigned Alignment, unsigned SectionID, llvm::StringRef SectionName, bool IsReadOnly) {
  // Use SectionMemoryManager as a base implementation
  // For now, we use a simple malloc-based allocation
  // TODO: Implement proper memory management
  return (uint8_t*)os::malloc(Size, mtCode);
}

void YuhuMemoryManager::registerEHFrames(uint8_t *Addr, uint64_t LoadAddr, size_t Size) {
  // Register exception handling frames
  // TODO: Implement proper EH frame registration
}

void YuhuMemoryManager::deregisterEHFrames() {
  // Deregister exception handling frames
  // TODO: Implement proper EH frame deregistration
}

uint64_t YuhuMemoryManager::getSymbolAddress(const std::string &Name) {
  // Get symbol address by name
  // TODO: Implement symbol lookup
  return 0;
}

void *YuhuMemoryManager::getPointerToNamedFunction(const std::string &Name, bool AbortOnFailure) {
  // Get pointer to named function
  // TODO: Implement function lookup
  if (AbortOnFailure) {
    char msg[256];
    snprintf(msg, sizeof(msg), "YuhuMemoryManager::getPointerToNamedFunction: function %s not found", Name.c_str());
    fatal(msg);
  }
  return NULL;
}

bool YuhuMemoryManager::finalizeMemory(std::string *ErrMsg) {
  // Finalize memory - make code sections executable, data sections writable, etc.
  // This is called after all sections have been allocated but before code execution
  // For now, we just return true (success)
  // TODO: Implement proper memory finalization
  return true;
}
#endif

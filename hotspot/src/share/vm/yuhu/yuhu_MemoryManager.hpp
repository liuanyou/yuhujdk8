#ifndef SHARE_VM_YUHU_YUHU_MEMORYMANAGER_HPP
#define SHARE_VM_YUHU_YUHU_MEMORYMANAGER_HPP

#include "yuhu/yuhu_llvmHeaders.hpp"

class YuhuEntry;

class YuhuMemoryManager : public JITMemoryManager {
 private:
  typedef std::map<const llvm::Function*, YuhuEntry*> EntryMap;
  EntryMap _entry_map;

 public:
  // LLVM JITMemoryManager interface
  virtual void* allocateCodeSection(uintptr_t Size, unsigned Alignment,
                                   unsigned SectionID, llvm::StringRef SectionName);

  virtual void* allocateDataSection(uintptr_t Size, unsigned Alignment,
                                   unsigned SectionID, llvm::StringRef SectionName,
                                   bool IsReadOnly);

  virtual void registerEHFrames(uint8_t* Addr, uint64_t LoadAddr, size_t Size);
  virtual void deregisterEHFrames(uint8_t* Addr, uint64_t LoadAddr, size_t Size);

  virtual bool needsExactSize();

  virtual void* startExceptionTable(const llvm::Function* F, uintptr_t& ActualSize);
  virtual void endExceptionTable(const llvm::Function* F, uintptr_t ActualSize,
                                 uint8_t* TableStart, uint8_t* TableEnd,
                                 uint8_t* FrameRegister);

  // Yuhu-specific methods
  void set_entry_for_function(const llvm::Function* function, YuhuEntry* entry) {
    _entry_map[function] = entry;
  }

  YuhuEntry* entry_for_function(const llvm::Function* function) {
    EntryMap::iterator iter = _entry_map.find(function);
    return (iter != _entry_map.end()) ? iter->second : NULL;
  }
};

#endif // SHARE_VM_YUHU_YUHU_MEMORYMANAGER_HPP











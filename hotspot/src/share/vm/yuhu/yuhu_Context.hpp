#ifndef SHARE_VM_YUHU_YUHU_CONTEXT_HPP
#define SHARE_VM_YUHU_YUHU_CONTEXT_HPP

#include "yuhu/yuhu_llvmHeaders.hpp"
#include "yuhu/yuhu_Compiler.hpp"

// Forward declarations
class YuhuFreeQueueItem;

class YuhuContext : public llvm::LLVMContext {
 public:
  YuhuContext(const char* name);

 private:
  llvm::Module* _module;

 public:
  llvm::Module* module() const {
    return _module;
  }

  // Get this thread's YuhuContext
 public:
  static YuhuContext& current() {
    return *YuhuCompiler::compiler()->context();
  }

  // Module accessors
 public:
  void add_function(llvm::Function* function) const {
    module()->getFunctionList().push_back(function);
  }
  llvm::Constant* get_external(const char* name, llvm::FunctionType* sig) {
    return module()->getOrInsertFunction(name, sig);
  }

  // Basic types - following Yuhu pattern
 private:
  llvm::Type*        _void_type;
  llvm::IntegerType* _bit_type;
  llvm::IntegerType* _jbyte_type;
  llvm::IntegerType* _jshort_type;
  llvm::IntegerType* _jint_type;
  llvm::IntegerType* _jlong_type;
  llvm::Type*        _jfloat_type;
  llvm::Type*        _jdouble_type;

 public:
  llvm::Type* void_type() const { return _void_type; }
  llvm::IntegerType* bit_type() const { return _bit_type; }
  llvm::IntegerType* jbyte_type() const { return _jbyte_type; }
  llvm::IntegerType* jshort_type() const { return _jshort_type; }
  llvm::IntegerType* jint_type() const { return _jint_type; }
  llvm::IntegerType* jlong_type() const { return _jlong_type; }
  llvm::Type* jfloat_type() const { return _jfloat_type; }
  llvm::Type* jdouble_type() const { return _jdouble_type; }
  llvm::IntegerType* intptr_type() const {
    return LP64_ONLY(jlong_type()) NOT_LP64(jint_type());
  }

  // Compound types for Java interoperability
 private:
  llvm::PointerType*  _oop_type;
  llvm::PointerType*  _klass_type;
  llvm::PointerType*  _Method_type;
  llvm::PointerType*  _Metadata_type;
  llvm::FunctionType* _entry_point_type;
  llvm::FunctionType* _osr_entry_point_type;

 public:
  llvm::PointerType* oop_type() const { return _oop_type; }
  llvm::PointerType* klass_type() const { return _klass_type; }
  llvm::PointerType* Method_type() const { return _Method_type; }
  llvm::PointerType* Metadata_type() const { return _Metadata_type; }
  llvm::FunctionType* entry_point_type() const { return _entry_point_type; }
  llvm::FunctionType* osr_entry_point_type() const { return _osr_entry_point_type; }

  // Type mappings for Java types to LLVM types
 private:
  llvm::Type* _to_stackType[T_CONFLICT];
  llvm::Type* _to_arrayType[T_CONFLICT];

 private:
  llvm::Type* map_type(llvm::Type* const* table, BasicType type) const {
    assert(type >= 0 && type < T_CONFLICT, "unhandled type");
    llvm::Type* result = table[type];
    assert(result != NULL, "unhandled type");
    return result;
  }

 public:
  llvm::Type* to_stackType(BasicType type) const {
    return map_type(_to_stackType, type);
  }
  llvm::Type* to_arrayType(BasicType type) const {
    return map_type(_to_arrayType, type);
  }

  // Functions queued for freeing
 private:
  YuhuFreeQueueItem* _free_queue;

 public:
  void push_to_free_queue(llvm::Function* function);
  llvm::Function* pop_from_free_queue();
};

#endif // SHARE_VM_YUHU_YUHU_CONTEXT_HPP











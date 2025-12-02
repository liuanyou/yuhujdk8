#ifndef SHARE_VM_YUHU_YUHU_COMPILATION_HPP
#define SHARE_VM_YUHU_YUHU_COMPILATION_HPP

#include "ci/ciEnv.hpp"
#include "ci/ciMethod.hpp"

// Forward declarations
class YuhuCodeGenerator;
class YuhuRegisterAllocator;

class YuhuCompilation : StackObj {
 private:
  ciEnv* _env;
  ciMethod* _method;
  int _entry_bci;
  
  // Compilation components
  YuhuCodeGenerator* _code_generator;
  YuhuRegisterAllocator* _register_allocator;
  
  // Compilation state
  bool _compiled;
  nmethod* _nmethod;
  
 public:
  YuhuCompilation(ciEnv* env, ciMethod* method, int entry_bci)
    : _env(env), _method(method), _entry_bci(entry_bci),
      _code_generator(NULL), _register_allocator(NULL),
      _compiled(false), _nmethod(NULL) {
  }

  ~YuhuCompilation();

  // Main compilation driver
  void compile();

  // Accessors
  ciEnv* env() const { return _env; }
  ciMethod* method() const { return _method; }
  int entry_bci() const { return _entry_bci; }
  
  YuhuCodeGenerator* code_generator() { return _code_generator; }
  YuhuRegisterAllocator* register_allocator() { return _register_allocator; }
  
  bool is_compiled() const { return _compiled; }
  nmethod* nmethod() const { return _nmethod; }

 private:
  void build_hir();      // Build high-level intermediate representation
  void allocate_registers();  // Perform register allocation
  void generate_code();  // Generate native code
  void install_code();   // Install compiled method in code cache
};


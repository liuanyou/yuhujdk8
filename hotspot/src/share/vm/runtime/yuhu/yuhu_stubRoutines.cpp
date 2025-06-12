//
// Created by Anyou Liu on 2025/4/8.
//
#include "precompiled.hpp"
#include "asm/codeBuffer.hpp"
#include "memory/resourceArea.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/interfaceSupport.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/timer.hpp"
#include "utilities/copy.hpp"

address YuhuStubRoutines::_call_stub_entry = NULL;
address YuhuStubRoutines::_call_stub_return_address = NULL;
address YuhuStubRoutines::_catch_exception_entry = NULL;
address YuhuStubRoutines::_forward_exception_entry = NULL;
address YuhuStubRoutines::_throw_StackOverflowError_entry = NULL;
address YuhuStubRoutines::_verify_oop_subroutine_entry = NULL;
jint YuhuStubRoutines::_verify_oop_count = 0;
BufferBlob* YuhuStubRoutines::_code1 = NULL;

extern void YuhuStubGenerator_generate(CodeBuffer* code, bool all);

void YuhuStubRoutines::initialize1() {
    if (_code1 == NULL) {
        ResourceMark rm;
        TraceTime timer("YuhuStubRoutines generation 1", TraceStartupTime);
        _code1 = BufferBlob::create("YuhuStubRoutines (1)", code_size1);
        if (_code1 == NULL) {
            vm_exit_out_of_memory(code_size1, OOM_MALLOC_ERROR, "CodeCache: no room for StubRoutines (1)");
        }
        CodeBuffer buffer(_code1);
        YuhuStubGenerator_generate(&buffer, false);
    }
}

void YuhuStubRoutines::initialize2() {
//    if (_code2 == NULL) {
//        ResourceMark rm;
//        TraceTime timer("StubRoutines generation 2", TraceStartupTime);
//        _code2 = BufferBlob::create("StubRoutines (2)", code_size2);
//        if (_code2 == NULL) {
//            vm_exit_out_of_memory(code_size2, OOM_MALLOC_ERROR, "CodeCache: no room for StubRoutines (2)");
//        }
//        CodeBuffer buffer(_code2);
//        StubGenerator_generate(&buffer, true);
//    }
//
//#ifdef ASSERT
//
//    os::current_thread_enable_wx(WXExec);
//
//#define TEST_ARRAYCOPY(type)                                                    \
//  test_arraycopy_func(          type##_arraycopy(),          sizeof(type));     \
//  test_arraycopy_func(          type##_disjoint_arraycopy(), sizeof(type));     \
//  test_arraycopy_func(arrayof_##type##_arraycopy(),          sizeof(HeapWord)); \
//  test_arraycopy_func(arrayof_##type##_disjoint_arraycopy(), sizeof(HeapWord))
//
//  // Make sure all the arraycopy stubs properly handle zero count
//  TEST_ARRAYCOPY(jbyte);
//  TEST_ARRAYCOPY(jshort);
//  TEST_ARRAYCOPY(jint);
//  TEST_ARRAYCOPY(jlong);
//
//#undef TEST_ARRAYCOPY
//
//#define TEST_FILL(type)                                                                      \
//  if (_##type##_fill != NULL) {                                                              \
//    union {                                                                                  \
//      double d;                                                                              \
//      type body[96];                                                                         \
//    } s;                                                                                     \
//                                                                                             \
//    int v = 32;                                                                              \
//    for (int offset = -2; offset <= 2; offset++) {                                           \
//      for (int i = 0; i < 96; i++) {                                                         \
//        s.body[i] = 1;                                                                       \
//      }                                                                                      \
//      type* start = s.body + 8 + offset;                                                     \
//      for (int aligned = 0; aligned < 2; aligned++) {                                        \
//        if (aligned) {                                                                       \
//          if (((intptr_t)start) % HeapWordSize == 0) {                                       \
//            ((void (*)(type*, int, int))StubRoutines::_arrayof_##type##_fill)(start, v, 80); \
//          } else {                                                                           \
//            continue;                                                                        \
//          }                                                                                  \
//        } else {                                                                             \
//          ((void (*)(type*, int, int))StubRoutines::_##type##_fill)(start, v, 80);           \
//        }                                                                                    \
//        for (int i = 0; i < 96; i++) {                                                       \
//          if (i < (8 + offset) || i >= (88 + offset)) {                                      \
//            assert(s.body[i] == 1, "what?");                                                 \
//          } else {                                                                           \
//            assert(s.body[i] == 32, "what?");                                                \
//          }                                                                                  \
//        }                                                                                    \
//      }                                                                                      \
//    }                                                                                        \
//  }                                                                                          \
//
//  TEST_FILL(jbyte);
//  TEST_FILL(jshort);
//  TEST_FILL(jint);
//
//#undef TEST_FILL
//
//#define TEST_COPYRTN(type) \
//  test_arraycopy_func(CAST_FROM_FN_PTR(address, Copy::conjoint_##type##s_atomic),  sizeof(type)); \
//  test_arraycopy_func(CAST_FROM_FN_PTR(address, Copy::arrayof_conjoint_##type##s), (int)MAX2(sizeof(HeapWord), sizeof(type)))
//
//  // Make sure all the copy runtime routines properly handle zero count
//  TEST_COPYRTN(jbyte);
//  TEST_COPYRTN(jshort);
//  TEST_COPYRTN(jint);
//  TEST_COPYRTN(jlong);
//
//#undef TEST_COPYRTN
//
//  test_arraycopy_func(CAST_FROM_FN_PTR(address, Copy::conjoint_words), sizeof(HeapWord));
//  test_arraycopy_func(CAST_FROM_FN_PTR(address, Copy::disjoint_words), sizeof(HeapWord));
//  test_arraycopy_func(CAST_FROM_FN_PTR(address, Copy::disjoint_words_atomic), sizeof(HeapWord));
//  // Aligned to BytesPerLong
//  test_arraycopy_func(CAST_FROM_FN_PTR(address, Copy::aligned_conjoint_words), sizeof(jlong));
//  test_arraycopy_func(CAST_FROM_FN_PTR(address, Copy::aligned_disjoint_words), sizeof(jlong));
//  os::current_thread_enable_wx(WXWrite);
//#endif
}

void yuhuStubRoutines_init1() { YuhuStubRoutines::initialize1(); }
void yuhuStubRoutines_init2() { YuhuStubRoutines::initialize2(); }
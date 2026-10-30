/*
 * Copyright (c) 1999, 2010, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_YUHU_YUHUBUILDER_HPP
#define SHARE_VM_YUHU_YUHUBUILDER_HPP

#include "ci/ciType.hpp"
#include "memory/barrierSet.hpp"
#include "memory/cardTableModRefBS.hpp"
#include "yuhu/llvmHeaders.hpp"
#include "yuhu/llvmValue.hpp"
#include "yuhu/yuhuCodeBuffer.hpp"
#include "yuhu/yuhuEntry.hpp"
#include "yuhu/yuhuType.hpp"
#include "yuhu/yuhuValue.hpp"
#include "utilities/debug.hpp"
#include "utilities/sizes.hpp"
#include "yuhu/yuhuStack.hpp"
#include "yuhu/yuhuFunction.hpp"

// Forward declarations
class YuhuFunction;

class RelocEntry : public ResourceObj {
public:
    relocInfo::relocType reloc_type;
    size_t offset;
    int spec_index;
    uint64_t target;
    int section_type;
};

class YuhuBuilder : public llvm::IRBuilder<> {
  friend class YuhuCompileInvariants;
  friend class YuhuFunction;
  friend class YuhuBlock;
  friend class YuhuCompiler;

 public:
  YuhuBuilder(YuhuCodeBuffer* code_buffer, YuhuFunction* function);

  // The code buffer we are building into.
 private:
  YuhuCodeBuffer* _code_buffer;
  YuhuFunction* _function;  // Current function being compiled

 protected:
  YuhuCodeBuffer* code_buffer() const {
    return _code_buffer;
  }
  YuhuFunction* function() const {
    return _function;
  }

  // Set the function pointer (called by YuhuFunction during construction)
  void set_function(YuhuFunction* function) {
    _function = function;
  }

 public:
  llvm::LoadInst* CreateAtomicLoad(llvm::Value* ptr,
                                   llvm::Type* load_type,
                                   unsigned align = HeapWordSize,
                                   llvm::AtomicOrdering ordering = llvm::AtomicOrdering::SequentiallyConsistent,
                                   bool isVolatile = true,
                                   const char *name = "");
  llvm::StoreInst* CreateAtomicStore(llvm::Value *val,
                                     llvm::Value *ptr,
                                     unsigned align = HeapWordSize,
                                     llvm::AtomicOrdering ordering = llvm::AtomicOrdering::SequentiallyConsistent,
                                     bool isVolatile = true,
                                     const char *name = "");

  // Helpers for accessing structures.
 public:
  llvm::Value* CreateAddressOfStructEntry(llvm::Value* base,
                                          ByteSize offset,
                                          llvm::Type* type,
                                          const char *name = "");
  llvm::LoadInst* CreateValueOfStructEntry(llvm::Value* base,
                                           ByteSize offset,
                                           llvm::Type* type,
                                           const char *name = "");

  // Helpers for accessing arrays.
 public:
  llvm::LoadInst* CreateArrayLength(llvm::Value* arrayoop);
  llvm::Value* CreateArrayAddress(llvm::Value*      arrayoop,
                                  llvm::Type* element_type,
                                  int               element_bytes,
                                  ByteSize          base_offset,
                                  llvm::Value*      index,
                                  const char*       name = "");
  llvm::Value* CreateArrayAddress(llvm::Value* arrayoop,
                                  BasicType    basic_type,
                                  ByteSize     base_offset,
                                  llvm::Value* index,
                                  const char*  name = "");
  llvm::Value* CreateArrayAddress(llvm::Value* arrayoop,
                                  BasicType    basic_type,
                                  llvm::Value* index,
                                  const char*  name = "");

  // Helpers for creating intrinsics and external functions.
 private:
  static llvm::Type* make_type(char type, bool void_ok);
 public:
  static llvm::FunctionType* make_ftype(const char* params,
                                              const char* ret);
  llvm::Value* make_function(const char* name,
                             const char* params,
                             const char* ret);
  llvm::Value* make_function(address     func,
                             const char* params,
                             const char* ret);

  // Intrinsics and external functions, part 1: VM calls.
  //   These are functions declared with JRT_ENTRY and JRT_EXIT,
  //   macros which flip the thread from _thread_in_Java to
  //   _thread_in_vm and back.  VM calls always safepoint, and can
  //   therefore throw exceptions.  VM calls require of setup and
  //   teardown, and must be called with YuhuTopLevelBlock::call_vm.
 public:
  llvm::Value* monitorenter();
  llvm::Value* monitorexit();
  llvm::Value* new_instance();
  llvm::Value* newarray();
  llvm::Value* anewarray();
  llvm::Value* multianewarray();
  llvm::Value* register_finalizer();
  llvm::Value* safepoint();
  llvm::Value* throw_ArithmeticException();
  llvm::Value* throw_ArrayIndexOutOfBoundsException();
  llvm::Value* throw_ClassCastException();
  llvm::Value* throw_NullPointerException();

  // Klass pointer encoding/decoding (compressed class pointers support)
 public:
  llvm::Value* encode_klass_not_null(llvm::Value* full_klass);
  llvm::Value* decode_klass_not_null(llvm::Value* compressed_klass);
  void store_klass_to_object(llvm::Value* object, llvm::Value* klass);
  llvm::Value* load_klass_from_object(llvm::Value* object);

  // Intrinsics and external functions, part 2: High-level non-VM calls.
  //   These are called like normal functions.  The stack is not set
  //   up for walking so they must not safepoint or throw exceptions,
  //   or call anything that might.
 public:
  llvm::Value* f2i();
  llvm::Value* f2l();
  llvm::Value* d2i();
  llvm::Value* d2l();
  llvm::Value* is_subtype_of();
  // VerifyOops leaf helper.  Reached by CreateVerifyOops; not called
  // directly by callers -- use CreateVerifyOops which handles the
  // VerifyOops-off early return and the msg-string materialisation.
  llvm::Value* verify_oop_wrapper();
  llvm::Value* current_time_millis();
  llvm::Value* sin();
  llvm::Value* cos();
  llvm::Value* tan();
  llvm::Value* atan2();
  llvm::Value* sqrt();
  llvm::Value* log();
  llvm::Value* log10();
  llvm::Value* pow();
  llvm::Value* exp();
  llvm::Value* fabs();
  llvm::Value* unsafe_field_offset_to_byte_offset();
  llvm::Value* osr_migration_end();

  // Intrinsics and external functions, part 3: semi-VM calls.
  //   These are special cases that do VM call stuff but are invoked
  //   as though they were normal calls.  This is acceptable so long
  //   as the method that calls them returns to its immediately that
  //   the semi VM call returns.
 public:
  llvm::Value* throw_StackOverflowError();
  llvm::Value* debug_stack_overflow_check();
  
  // LLVM intrinsics for deoptimization
  llvm::CallInst* CreateExperimentalDeoptimize(uint64_t statepoint_id);

  llvm::CallInst* CreateStackMap(uint64_t id, uint32_t num_shadow_bytes, llvm::ArrayRef<llvm::Value*> live_values);

  // Intrinsics and external functions, part 4: Native-Java transition.
  //   This is a special case in that it is invoked during a thread
  //   state transition.  The stack must be set up for walking, and it
  //   may throw exceptions, but the state is _thread_in_native_trans.
 public:
  llvm::Value* check_special_condition_for_native_trans();

  // Intrinsics and external functions, part 5: Low-level non-VM calls.
  //   These have the same caveats as the high-level non-VM calls
  //   above.  They are not accessed directly; rather, you should
  //   access them via the various Create* methods below.
 private:
  llvm::Value* cmpxchg_int();
  llvm::Value* cmpxchg_ptr();
  llvm::Value* frame_address();
  llvm::Value* unimplemented();
  llvm::Value* should_not_reach_here();
  llvm::Value* dump();

  // Public interface to low-level non-VM calls.
 public:
  llvm::CallInst* CreateReadFramePointer();
  llvm::CallInst* CreateReadStackPointer();  // Read actual SP register (x31) on AArch64
  llvm::CallInst* CreateReadLinkRegister();   // Read LR register (x30) on AArch64
  // CreateReadMethodRegister() removed: the compiled nmethod's Method* is now a
  // compile-time constant (see YuhuFunction::initialize), not the x12 register.
  llvm::CallInst* CreateReadThreadRegister(); // Read rthread register (x28) on AArch64
  llvm::CallInst* CreateReadCurrentPC(); // Read current pc on AArch64
  llvm::CallInst* CreateReadX0Register();  // Read x0 register (holds saved p7 parameter)
  llvm::LoadInst* CreateLoadX0Slot();
  llvm::CallInst* CreateReadRegister(const char* reg_name); // Generic register reader
  llvm::CallInst* CreateMemset(llvm::Value* dst,
                               llvm::Value* value,
                               llvm::Value* len,
                               llvm::Value* align);
  llvm::CallInst* CreateUnimplemented(const char* file, int line);
  llvm::CallInst* CreateShouldNotReachHere(const char* file, int line);
  NOT_PRODUCT(llvm::CallInst* CreateDump(llvm::Value* value));

  // HotSpot memory barriers
 public:
  // The pre/post pair around a reference store.  Which barrier set is in play
  // is answered by dispatching on its kind, exactly like
  // GraphKit::pre_barrier() and GraphKit::post_barrier(): a card-table heap
  // (Serial, Parallel) needs only the dirty-card write afterwards, while G1
  // needs a SATB pre barrier before the store plus a remembered-set post
  // barrier after it.
  //
  // CreatePreBarrierSet must run BEFORE the field is overwritten because it
  // logs the value that was in the field; CreateUpdateBarrierSet runs after.
  //   field   : address of the reference field being written
  //   new_val : the oop being stored, decoded (uncompressed) or null.  G1's
  //             region-crossing test needs the real address, which is why
  //             TemplateTable::do_oop_store copies the value aside before
  //             store_heap_oop smashes it with the encoded bits.  Ignored by
  //             the card-table barrier.
  void CreatePreBarrierSet(BarrierSet* bs, llvm::Value* thread, llvm::Value* field);
  void CreateUpdateBarrierSet(BarrierSet* bs, llvm::Value* thread,
                              llvm::Value* field, llvm::Value* new_val);

  // True when the barrier set wants the G1 pair.  Public so the store sites
  // can say why they hand over a decoded value.
  static bool needs_g1_write_bars(BarrierSet* bs);

  // Barrier internals, one kind each; called through the two entry points
  // above.  They create their own basic blocks and leave the insertion point
  // in the join block, so the caller keeps emitting into straight-line code.
  void CreateCardTablePostBarrier(BarrierSet* bs, llvm::Value* field);
  void CreateG1PreBarrier(llvm::Value* thread, llvm::Value* field);
  void CreateG1PostBarrier(BarrierSet* bs, llvm::Value* thread,
                           llvm::Value* field, llvm::Value* new_val);

  // Plain call to a JRT_LEAF C function, same convention as the VerifyOops
  // sites: no statepoint, no OopMap, no gc-live contribution, and the site is
  // recorded as CallSiteType::leaf_call.  LLVM's allocator spills whatever is
  // live across it, so unlike the machine-code barriers these need no push/pop.
  void CreateBarrierLeafCall(llvm::Value* callee, llvm::FunctionType* func_type,
                             llvm::Value* arg0, llvm::Value* arg1);

  // Emit a VerifyOops leaf call validating that the given oop-typed
  // (ptr addrspace(1)) value currently holds a plausible heap reference.
  // No-op unless VerifyOops is on.  Callers guard the argument with an
  // is_jobject() check on the corresponding YuhuValue, or pass a freshly
  // decoded ptr addrspace(1) produced by CreateDecodeHeapOop.  Emitted
  // sites are decache (locals / expression stack) and every decode_heap_oop
  // result.  Leaf call -- does not register a statepoint and does not
  // produce an OopMap entry.
  //
  // `holder` (a ptr addrspace(1) base object, or NULL at decache slots) and
  // `field_off` (field byte offset, or frame slot offset at decache sites)
  // form a stable location anchor logged alongside the pointer value, so a
  // stale read stays greppable even when the decoded address is garbage.
  void CreateVerifyOops(llvm::Value* target, const char* msg,
                        llvm::Value* holder = NULL, int64_t field_off = 0);

  // Compressed oops helpers
 public:
  // Internal helper for the two functions below; not for direct use.
  //
  // Emit the narrow-oop shift as a single opaque AArch64 lsl/lsr instruction
  // wrapped in inline asm, and mark it sideeffect so the optimizer can neither
  // fold it away nor move it.  This is load bearing for GC correctness:
  // with narrow_oop_base == NULL and narrow_oop_shift == 3 the encode/decode
  // pair composes to an identity, so plain IR lets InstCombine collapse a
  // putfield's encode all the way back to the narrowOop read by the matching
  // getfield.  A narrowOop is an i32, RS4GC only tracks pointer-typed values,
  // so that value is spilled to a slot which is (correctly) absent from every
  // OopMap and is never relocated; using it as the source of a reference store
  // then writes a pre-GC address into a live object.  Passing the oop pointer
  // directly to the asm also keeps LLVM from materialising a ptrtoint of it,
  // which would be the same hazard in a different shape.
  //
  //   left == true:  i64 zero-extended narrowOop -> i64 full-width heap address
  //   left == false: ptr addrspace(1) oop        -> i64 narrowOop
  llvm::Value* CreateOpaqueOopShift(llvm::Value* value, bool left, int shift);

  // Companion of CreateOpaqueOopShift for the base-relative compressed-oop
  // modes:  returns  (oop - base)  as intptr, again as one opaque, sideeffect
  // pinned instruction.  The oop is passed as a ptr addrspace(1) operand rather
  // than through ptrtoint for the same reason given above -- an integer form of
  // the pointer must never exist as an IR node, because RS4GC cannot relocate an
  // integer and it would then survive a safepoint unchanged.  Used only when
  // narrow_oop_base != NULL; the shift is applied by the caller in IR, which is
  // safe because it consumes this opaque, pinned result.
  llvm::Value* CreateOpaqueBaseSub(llvm::Value* oop, intptr_t base);

  llvm::Value* CreateDecodeHeapOop(llvm::Value* compressed_oop,
                                   llvm::Value* verify_holder = NULL,
                                   int64_t verify_field_off = 0);
  llvm::Value* CreateEncodeHeapOop(llvm::Value* oop);

  // Helpers for accessing the code buffer.
 public:
  llvm::Value* code_buffer_address(int offset);

  public:
  llvm::Value* CreateInlineOop(ciObject* object, const char* name = "");

  public:

  // Static field access using CP index (like C1)
  llvm::Value* CreateInlineOopForStaticField(ciField* field, const char* name = "oop");

  // Emits a value-producing patch point whose target operand is the Metadata*
  // address itself. scan_and_generate_all_relocations() later rewrites the
  // patch point's "blr xN" as "mov x0, xN" and attaches a metadata_Relocation,
  // so the nmethod keeps the Metadata* in its metadata pool instead of relying
  // only on the address baked into the instruction stream.
  llvm::Value* CreateInlineMetadata(::Metadata* metadata, llvm::PointerType* type, const char* name = "");
  llvm::Value* CreateInlineMetadata(ciMetadata* metadata, llvm::PointerType* type, const char* name = "") {
    return CreateInlineMetadata(metadata->constant_encoding(), type, name);
  }

  // hotspot stores relocation points in unsigned short.
  // Layout:
  // Bits [15:12] = relocType (4 bits)
  // Bits [11:1]  = offset (11 bits)
  // Bit [0]      = format
  // it requires offset to be positive, otherwise locs_point assertion fails.
  // Handle all relocations in one method, including oop_relocation, poll_relocation and so on.
  void scan_and_generate_all_relocations(address llvm_code_start, size_t llvm_code_size, CodeBuffer* cb, address code_start, size_t adapter_size);

  // Helpers for creating basic blocks.
  // NB don't use unless YuhuFunction::CreateBlock is unavailable.
  // XXX these are hacky and should be removed.
 public:
  llvm::BasicBlock* GetBlockInsertionPoint() const;
  void InsertStackMapAtBlockStart(llvm::BasicBlock* block, uint64_t id, llvm::ArrayRef<llvm::Value*> live_values);
  llvm::BasicBlock* CreateBlock(llvm::BasicBlock* ip,
                                const char*       name="") const;
};
  #endif // SHARE_VM_YUHU_YUHUBUILDER_HPP

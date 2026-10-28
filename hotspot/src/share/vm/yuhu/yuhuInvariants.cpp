/*
 * Copyright (c) 1999, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2008, 2009 Red Hat, Inc.
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
#include "yuhu/yuhuInvariants.hpp"
#include "yuhu/yuhuBuilder.hpp"
#include "yuhu/yuhu_globals.hpp"
// The raw LLVM headers must stay after the HotSpot ones: they pull <cassert>,
// which replaces the two-argument HotSpot assert() macro that llvmHeaders.hpp
// takes care to restore, and ci/*.hpp (reached through yuhuInvariants.hpp) is
// full of assert(e, "msg") uses that then look like over-evaluated calls.
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Cloning.h"

int YuhuTargetInvariants::count_monitors() {
  int result = 0;
  if (is_synchronized() || target()->has_monitor_bytecodes()) {
    for (int i = 0; i < flow()->block_count(); i++) {
      result = MAX2(result, flow()->pre_order_at(i)->monitor_count());
    }
  }
  return result;
}

YuhuCodeBuffer* YuhuCompileInvariants::code_buffer() const {
    return builder()->code_buffer();
}


// ===========================================================================
// YuhuNarrowOopInvariant -- see the class comment in yuhuInvariants.hpp for
// what is guaranteed and why a reviewer cannot supply that guarantee.
// ===========================================================================

// Heap references live in addrspace(1); the same value is used by
// YuhuIRTransformer's escape inventory.
static const unsigned NARROW_GC_ADDRSPACE = 1;

const char* YuhuNarrowOopInvariant::store_md_name() {
  return "yuhu.narrow_oop_store";
}

const char* YuhuNarrowOopInvariant::encode_md_name() {
  return "yuhu.narrow_oop_encode";
}

void YuhuNarrowOopInvariant::mark_encode(llvm::Value* encoded) {
  llvm::Instruction* I = llvm::dyn_cast<llvm::Instruction>(encoded);
  if (I == NULL) {
    return;
  }
  I->setMetadata(encode_md_name(), llvm::MDNode::get(I->getContext(), {}));
}

void YuhuNarrowOopInvariant::mark_store(llvm::Value* store) {
  llvm::Instruction* I = llvm::dyn_cast<llvm::Instruction>(store);
  if (I == NULL) {
    return;
  }
  I->setMetadata(store_md_name(), llvm::MDNode::get(I->getContext(), {}));
}

namespace {

// The bug's signature: a narrowOop (i32) read straight out of the heap.  Such a
// value is not a GC pointer, so RS4GC does not track it and a collection cannot
// relocate it.
bool is_narrow_heap_load(llvm::Value* v, llvm::LoadInst** found) {
  llvm::LoadInst* ld = llvm::dyn_cast<llvm::LoadInst>(v);
  if (ld == NULL) {
    return false;
  }
  if (!ld->getType()->isIntegerTy(32)) {
    return false;
  }
  llvm::PointerType* pt = llvm::dyn_cast<llvm::PointerType>(ld->getPointerOperand()->getType());
  if (pt == NULL || pt->getAddressSpace() != NARROW_GC_ADDRSPACE) {
    return false;
  }
  *found = ld;
  return true;
}

bool is_marked_encode(llvm::Value* v) {
  llvm::Instruction* I = llvm::dyn_cast<llvm::Instruction>(v);
  return I != NULL && I->getMetadata(YuhuNarrowOopInvariant::encode_md_name()) != NULL;
}

// Walk the def chain of the value a marked store writes, and report the first
// narrow heap load that no marked encode sits in front of.
//
// What stops the walk is what makes the check free of false positives:
//
//   - a marked encode ends the branch as safe.  This is the normal outcome: the
//     chain of a correct putfield is  store <- trunc <- encode-asm <- pointer,
//     and the pointer's own history (which does lead back to a narrow load via
//     the decode) is deliberately not walked, because a pointer is exactly the
//     form that GC can relocate;
//   - a Constant is safe (a null or pre-encoded value written without any read);
//   - any other CallBase is opaque: its result is produced by a helper or
//     intrinsic, not by arithmetic on one of its operands;
//   - an Argument is safe: the caller handed the pointer over;
//   - only arithmetic, casts, GEPs, selects and phis are transparent, because
//     those are the instructions the middle-end uses to cancel an encode.
//
// So the walk reaches a narrow load only if the encode really is gone, which is
// also why a plain int-field copy -- indistinguishable from a reference store by
// type alone -- cannot be reported: it is never marked.
bool reaches_undecoded_narrow_load(llvm::Value* value, llvm::LoadInst** narrow) {
  llvm::SmallVector<llvm::Value*, 16> worklist;
  llvm::SmallPtrSet<llvm::Value*, 16> seen;
  worklist.push_back(value);
  // Def chains here are short arithmetic runs.  The budget only guards against
  // a pathological chain spinning forever; it is never reached in practice.
  int budget = 4096;
  while (!worklist.empty() && budget-- > 0) {
    llvm::Value* v = worklist.pop_back_val();
    if (v == NULL || !seen.insert(v).second) {
      continue;
    }
    if (is_narrow_heap_load(v, narrow)) {
      return true;
    }
    if (is_marked_encode(v) || llvm::isa<llvm::Constant>(v) || llvm::isa<llvm::Argument>(v)) {
      continue;
    }
    if (llvm::dyn_cast<llvm::LoadInst>(v) != NULL || llvm::dyn_cast<llvm::CallBase>(v) != NULL ||
        llvm::dyn_cast<llvm::AllocaInst>(v) != NULL || llvm::dyn_cast<llvm::StoreInst>(v) != NULL) {
      continue;
    }
    if (llvm::PHINode* phi = llvm::dyn_cast<llvm::PHINode>(v)) {
      for (llvm::Value* incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
      continue;
    }
    // Anything left is an instruction that defines its value in terms of its
    // operands (cast, binary op, GEP, select, ...), so keep walking up.
    llvm::User* user = llvm::dyn_cast<llvm::User>(v);
    if (user == NULL) {
      continue;
    }
    for (llvm::Use& use : user->operands()) {
      llvm::Value* op = use.get();
      if (llvm::isa<llvm::BasicBlock>(op)) {
        continue;
      }
      worklist.push_back(op);
    }
  }
  return false;
}

void report_violation(llvm::StoreInst* store, llvm::LoadInst* narrow) {
  std::string store_text, narrow_text;
  {
    llvm::raw_string_ostream os(store_text);
    store->print(os);
  }
  {
    llvm::raw_string_ostream os(narrow_text);
    narrow->print(os);
  }
  llvm::errs() << "Yuhu narrowOop invariant: a reference-field store lost its encode in "
               << (store->getFunction() != NULL ? store->getFunction()->getName().str() : std::string("?"))
               << "\n    store: " << store_text
               << "\n    fed by narrow heap read: " << narrow_text
               << "\n    the value stored is a pre-collection narrowOop: it is not a GC pointer,"
                  " so no OopMap tracks it and no collector can relocate it."
                  "  Failing the compilation; see doc/yuhu/activities/082_*.\n";
  llvm::errs().flush();
}

unsigned count_marked(llvm::Module& M, const char* md_name) {
  unsigned marked = 0;
  for (llvm::Function& F : M) {
    if (F.isDeclaration()) {
      continue;
    }
    for (llvm::BasicBlock& BB : F) {
      for (llvm::Instruction& I : BB) {
        if (I.getMetadata(md_name) != NULL) {
          marked++;
        }
      }
    }
  }
  return marked;
}

} // anonymous namespace

unsigned YuhuNarrowOopInvariant::count_marked_stores(llvm::Module& M) {
  return count_marked(M, store_md_name());
}

unsigned YuhuNarrowOopInvariant::count_marked_encodes(llvm::Module& M) {
  return count_marked(M, encode_md_name());
}

unsigned YuhuNarrowOopInvariant::scan(llvm::Module& M, bool report) {
  unsigned violations = 0;
  for (llvm::Function& F : M) {
    if (F.isDeclaration()) {
      continue;
    }
    for (llvm::BasicBlock& BB : F) {
      for (llvm::Instruction& I : BB) {
        llvm::StoreInst* store = llvm::dyn_cast<llvm::StoreInst>(&I);
        if (store == NULL || store->getMetadata(store_md_name()) == NULL) {
          continue;
        }
        // Marked stores write a narrowOop, which is i32.  Anything else means
        // the builder changed shape and this check's premise no longer holds.
        if (!store->getValueOperand()->getType()->isIntegerTy(32)) {
          continue;
        }
        llvm::LoadInst* narrow = NULL;
        if (reaches_undecoded_narrow_load(store->getValueOperand(), &narrow)) {
          violations++;
          if (report) {
            report_violation(store, narrow);
          }
        }
      }
    }
  }
  return violations;
}

unsigned YuhuNarrowOopInvariant::check_after_middle_end(llvm::Function* function) {
  llvm::Module* M = function->getParent();
  if (M == NULL) {
    return 0;
  }
  // Cheap exit: methods that never store a reference under compressed oops have
  // nothing to verify, and they are the majority of what Yuhu compiles.
  unsigned marked = count_marked_stores(*M);
  unsigned encodes_before = count_marked_encodes(*M);
  if (marked == 0) {
    if (YuhuTraceIRCompilation) {
      llvm::errs() << "Yuhu narrowOop invariant: no reference-field store in module "
                   << M->getName() << ", nothing to check\n";
    }
    return 0;
  }

  // Clone rather than check the real module: the pipeline below canonicalizes in
  // place, and the code that is actually installed must not be optimized twice.
  std::unique_ptr<llvm::Module> clone(llvm::CloneModule(*M).release());

  // Exactly the function-level prefix YuhuIRTransformer::runGCPasses() runs, in
  // the same order, so the fold either happens here or happens there.  RS4GC is
  // deliberately not part of the prefix: it cannot remove an encode (it only
  // inserts statepoints and relocates pointers), it needs the thread-local
  // YuhuDebugInformationRecorder that a throwaway module must not disturb, and it
  // rewrites the marked stores and drops their mark -- which is why
  // runGCPasses() scans the IR at that point itself, before RS4GC runs, rather
  // than after the whole pipeline.
  llvm::PassBuilder PB;
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  PB.registerLoopAnalyses(LAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  llvm::FunctionPassManager FPM;
  FPM.addPass(llvm::InstCombinePass());
  FPM.addPass(llvm::SimplifyCFGPass());
  llvm::ModulePassManager MPM;
  MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
  MPM.run(*clone, MAM);

  unsigned violations = scan(*clone, /*report=*/true);

  // The check can go blind: a pass that rebuilds a store instead of editing it
  // drops the mark, and then the def-chain scan simply has nothing to say about
  // that store any more -- and reports the module as clean.  This is not theory;
  // it is what YuhuRewriteStatepointsForGC does to stores, which is why this scan
  // runs before it.  A decrease here therefore does not fail the compilation (a
  // pass may legitimately delete a store that turned out to be dead), but it must
  // not be silent either.
  unsigned marked_after = count_marked_stores(*clone);
  if (marked_after < marked) {
    llvm::errs() << "Yuhu narrowOop invariant: " << (marked - marked_after)
                 << " of " << marked << " reference-field store mark(s) vanished while module "
                 << M->getName() << " was being optimized.  The def-chain scan can no longer"
                    " see those stores, so a clean result for this module is not proof: find"
                    " which pass dropped the mark.\n";
    llvm::errs().flush();
  }

  // Second, weaker signal: the encodes themselves.  An encode is a sideeffect
  // inline asm, so nothing but a fold may make it disappear -- but deleting a
  // block that turns out to be unreachable takes the encodes inside it with it,
  // which is not the bug, so a decrease is reported for a human to look at and
  // does not fail the compilation.  The def-chain scan above is what decides.
  unsigned encodes_after = count_marked_encodes(*clone);
  if (encodes_after < encodes_before) {
    llvm::errs() << "Yuhu narrowOop invariant: " << (encodes_before - encodes_after)
                 << " of " << encodes_before << " compressed-oop encode(s) are gone from module "
                 << M->getName() << " after the middle-end.  Either they were in a block"
                    " that proved unreachable, or the encode/decode pair folded.  Check"
                    " the dumped IR by hand; see doc/yuhu/activities/082_*.\n";
    llvm::errs().flush();
  }

  if (YuhuTraceIRCompilation) {
    // Also proves the check ran on this compilation, which a silent pass cannot.
    llvm::errs() << "Yuhu narrowOop invariant: " << marked << " reference-field store(s) ("
                 << marked_after << " still marked) and " << encodes_before << " encode(s) ("
                 << encodes_after << " still marked) checked after InstCombine+SimplifyCFG in module "
                 << M->getName() << ", " << violations << " violation(s)\n";
  }
  return violations;
}

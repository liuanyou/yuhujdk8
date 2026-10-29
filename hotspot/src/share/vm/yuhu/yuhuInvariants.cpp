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

// A mark the builder put on the IR before optimization, remembered well enough
// to be followed through the pass pipeline.  The count alone cannot answer the
// only question that matters when a mark disappears: a store the passes
// *deleted* is harmless for this invariant (nothing writes the field any more,
// so nothing can write a stale value -- whatever it means for the program is a
// different bug), while a store the passes *rebuilt* is still live and still
// writes, and the check has just gone blind on it.  Those two look identical in
// a count, and only the second one is dangerous.
//
// The site keeps only a pointer into the original module, which outlives the
// clone, so nothing is formatted unless a mark is actually reported lost -- the
// common case costs one pointer per marked instruction.  The clone is found again
// through CloneModule's value map, whose entries are WeakTrackingVHs: they null
// out when the cloned instruction is destroyed.  So a null entry is a deletion,
// a live entry without the metadata is a rebuild.
struct MarkedSite {
  llvm::Instruction* original;
};

void collect_marked(llvm::Module& M, const char* md_name, llvm::SmallVectorImpl<MarkedSite>& out) {
  for (llvm::Function& F : M) {
    if (F.isDeclaration()) {
      continue;
    }
    for (llvm::BasicBlock& BB : F) {
      for (llvm::Instruction& I : BB) {
        if (I.getMetadata(md_name) == NULL) {
          continue;
        }
        MarkedSite site;
        site.original = &I;
        out.push_back(site);
      }
    }
  }
}

// Report-time only: name the function and block a lost site lived in, and the
// instruction itself, all read from the original module.
void print_lost_site(llvm::Instruction* original) {
  llvm::Function* F = original->getFunction();
  llvm::BasicBlock* BB = original->getParent();
  llvm::errs() << "    in " << (F != NULL ? F->getName().str() : std::string("?"));
  if (BB != NULL && !BB->getName().empty()) {
    llvm::errs() << " block " << BB->getName().str();
  }
  llvm::errs() << ": ";
  original->print(llvm::errs());
  llvm::errs().flush();
}

enum class MarkFate {
  still_marked,      // the instruction survived with its mark
  rebuilt_unmarked,  // the instruction is still there, the mark is not
  deleted,           // the instruction is gone
  lost_mapping       // never found in the clone's value map
};

// Is the mark this site carried still on the instruction the clone ended up
// with?  `md_name` is the mark that selected the site, so the question is a
// single metadata lookup.
MarkFate classify(const MarkedSite& site, const char* md_name,
                  llvm::ValueToValueMapTy& vmap, llvm::Instruction** clone_out) {
  *clone_out = NULL;
  llvm::ValueToValueMapTy::iterator it = vmap.find(site.original);
  if (it == vmap.end()) {
    return MarkFate::lost_mapping;
  }
  llvm::Value* v = it->second;   // a WeakTrackingVH: null once the clone dies
  if (v == NULL) {
    return MarkFate::deleted;
  }
  llvm::Instruction* I = llvm::dyn_cast<llvm::Instruction>(v);
  if (I == NULL) {
    return MarkFate::deleted;
  }
  *clone_out = I;
  return (I->getMetadata(md_name) != NULL) ? MarkFate::still_marked
                                          : MarkFate::rebuilt_unmarked;
}

// Account for the marks that did not survive, and close the blind spot they open.
struct Lost {
  unsigned still_marked;
  unsigned deleted;
  unsigned rewritten;
  unsigned violations_from_rewritten;
  Lost() : still_marked(0), deleted(0), rewritten(0), violations_from_rewritten(0) {}
};

// Walk every site the builder marked and classify what the passes did to it.
// `stores` selects the store behaviour: besides classifying, a store that is
// still present but no longer marked is invisible to scan(), so its def chain is
// checked here directly -- that is the case in which this check would otherwise
// report clean without having looked.  The classification therefore always runs;
// only the printing is gated on YuhuTraceIRCompilation, because a mark lost on a
// block that proved unreachable is routine optimizer behaviour and a real
// workload reports it several times per start-up.  A violation found by the
// re-scan is never gated: it refuses the compilation, so it has to be visible.
// Per-category printing is capped so a pathological method cannot flood the log.
void classify_sites(const llvm::SmallVectorImpl<MarkedSite>& sites, const char* md_name,
                    llvm::ValueToValueMapTy& vmap, bool stores, Lost& lost) {
  const unsigned max_lines = 8;
  unsigned printed_deleted = 0, printed_rewritten = 0;
  for (unsigned i = 0; i < sites.size(); i++) {
    const MarkedSite& site = sites[i];
    llvm::Instruction* clone_inst = NULL;
    MarkFate fate = classify(site, md_name, vmap, &clone_inst);
    if (fate == MarkFate::still_marked) {
      lost.still_marked++;
      continue;
    }
    if (fate == MarkFate::deleted || fate == MarkFate::lost_mapping) {
      lost.deleted++;
      if (YuhuTraceIRCompilation && printed_deleted < max_lines) {
        printed_deleted++;
        llvm::errs() << "    deleted";
        print_lost_site(site.original);
      }
      continue;
    }
    lost.rewritten++;
    if (stores && clone_inst != NULL) {
      llvm::StoreInst* st = llvm::dyn_cast<llvm::StoreInst>(clone_inst);
      if (st != NULL && st->getValueOperand()->getType()->isIntegerTy(32)) {
        llvm::LoadInst* narrow = NULL;
        if (reaches_undecoded_narrow_load(st->getValueOperand(), &narrow)) {
          lost.violations_from_rewritten++;
          report_violation(st, narrow);
        }
      }
    }
    if (YuhuTraceIRCompilation && printed_rewritten < max_lines) {
      printed_rewritten++;
      llvm::errs() << "    still present, mark gone";
      print_lost_site(site.original);
    }
  }
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
  llvm::SmallVector<MarkedSite, 16> store_sites;
  llvm::SmallVector<MarkedSite, 16> encode_sites;
  collect_marked(*M, store_md_name(), store_sites);
  collect_marked(*M, encode_md_name(), encode_sites);
  const unsigned marked = (unsigned)store_sites.size();
  const unsigned encodes_before = (unsigned)encode_sites.size();
  if (marked == 0) {
    if (YuhuTraceIRCompilation) {
      llvm::errs() << "Yuhu narrowOop invariant: no reference-field store in module "
                   << M->getName() << ", nothing to check\n";
    }
    return 0;
  }

  // Clone rather than check the real module: the pipeline below canonicalizes in
  // place, and the code that is actually installed must not be optimized twice.
  // The value map is what lets each marked site be followed into the clone; its
  // entries are WeakTrackingVHs, so a site the passes destroyed reads as null
  // rather than as a dangling pointer.
  llvm::ValueToValueMapTy vmap;
  std::unique_ptr<llvm::Module> clone(llvm::CloneModule(*M, vmap).release());

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

  // A disappearing mark has two causes and they mean opposite things, which a
  // count cannot tell apart:
  //
  //   deleted  -- the store is gone, so nothing writes that field any more.  It
  //     cannot write a stale narrowOop, so this invariant is not the issue.  The
  //     write vanishing is a question of its own (see the erased-region hazard in
  //     doc/yuhu/activities/076_* and 082 §9.8), and the dumped IR answers it;
  //   rewritten -- the store is still there and still writes, but it no longer
  //     carries the mark, so scan() above skipped it.  That is the one way this
  //     check can go blind while a real fold sits in the module, so each such
  //     store is walked by def chain here, and a violation fails the compilation.
  //
  // Either way the count decrease is accounted for, and reported only under
  // -XX:+YuhuTraceIRCompilation: losing the mark on a block that proved
  // unreachable is normal, and on a real workload it happens several times per
  // start-up.  What is not gated is the re-scan above -- it is what makes the
  // decrease visible as a violation when it is not a deletion.
  Lost store_loss;
  classify_sites(store_sites, store_md_name(), vmap, /*stores=*/true, store_loss);
  violations += store_loss.violations_from_rewritten;
  if (YuhuTraceIRCompilation && (store_loss.deleted > 0 || store_loss.rewritten > 0)) {
    llvm::errs() << "Yuhu narrowOop invariant: reference-field store marks in module "
                 << M->getName() << " after the middle-end: " << store_loss.still_marked
                 << " kept, " << store_loss.deleted << " deleted, " << store_loss.rewritten
                 << " rewritten without a mark\n";
    if (store_loss.rewritten > 0) {
      llvm::errs() << "    rewritten stores were re-checked by def chain without the mark: "
                   << store_loss.violations_from_rewritten << " violation(s)\n";
    }
    if (store_loss.deleted > 0) {
      llvm::errs() << "    deleted stores write nothing, so they cannot store a stale narrowOop;"
                      " but if the field write is missing from the installed code that is a"
                      " separate bug -- check this method in the IR dump (-XX:+YuhuDumpIRToFile)\n";
    }
    llvm::errs().flush();
  }

  // Same accounting for the encodes.  An encode is a sideeffect inline asm, so
  // nothing but a fold may make it disappear -- but deleting a block that proved
  // unreachable takes the encodes inside it with it, which is not the bug.  So a
  // decrease here is reported and not acted on: the def-chain walk stops at any
  // call, marked or not, and is what decides.  Trace-gated like the store
  // reporting, for the same reason: a deleted unreachable block is not the bug.
  Lost encode_loss;
  classify_sites(encode_sites, encode_md_name(), vmap, /*stores=*/false, encode_loss);
  if (YuhuTraceIRCompilation && (encode_loss.deleted > 0 || encode_loss.rewritten > 0)) {
    llvm::errs() << "Yuhu narrowOop invariant: compressed-oop encode marks in module "
                 << M->getName() << " after the middle-end: " << encode_loss.still_marked
                 << " kept, " << encode_loss.deleted << " deleted, " << encode_loss.rewritten
                 << " rewritten without a mark\n";
    llvm::errs().flush();
  }

  if (YuhuTraceIRCompilation) {
    // Also proves the check ran on this compilation, which a silent pass cannot.
    llvm::errs() << "Yuhu narrowOop invariant: " << marked << " reference-field store(s) ("
                 << store_loss.still_marked << " still marked) and " << encodes_before
                 << " encode(s) (" << encode_loss.still_marked
                 << " still marked) checked after InstCombine+SimplifyCFG in module "
                 << M->getName() << ", " << violations << " violation(s)\n";
  }
  return violations;
}

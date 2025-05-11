//
// Created by Anyou Liu on 2025/5/10.
//
#include "precompiled.hpp"
#include "asm/yuhu/yuhu_macroAssembler.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "asm/codeBuffer.hpp"
#include "runtime/atomic.hpp"
#include "runtime/atomic.inline.hpp"
#include "runtime/icache.hpp"
#include "runtime/os.hpp"

void YuhuLabel::add_patch_at(CodeBuffer* cb, int branch_loc) {
    assert(_loc == -1, "YuhuLabel is unbound");
    if (_patch_index < PatchCacheSize) {
        _patches[_patch_index] = branch_loc;
    } else {
        if (_patch_overflow == NULL) {
            _patch_overflow = cb->create_patch_overflow();
        }
        _patch_overflow->push(branch_loc);
    }
    ++_patch_index;
}

void YuhuLabel::patch_instructions(YuhuMacroAssembler* masm) {
  assert(is_bound(), "YuhuLabel is bound");
  CodeBuffer* cb = masm->code();
  int target_sect = CodeBuffer::locator_sect(loc());
  address target = cb->locator_address(loc());
  while (_patch_index > 0) {
    --_patch_index;
    int branch_loc;
    if (_patch_index >= PatchCacheSize) {
      branch_loc = _patch_overflow->pop();
    } else {
      branch_loc = _patches[_patch_index];
    }
    int branch_sect = CodeBuffer::locator_sect(branch_loc);
    address branch = cb->locator_address(branch_loc);
    if (branch_sect == CodeBuffer::SECT_CONSTS) {
      // The thing to patch is a constant word.
      *(address*)branch = target;
      continue;
    }

#ifdef ASSERT
    // Cross-section branches only work if the
    // intermediate section boundaries are frozen.
    if (target_sect != branch_sect) {
      for (int n = MIN2(target_sect, branch_sect),
               nlimit = (target_sect + branch_sect) - n;
           n < nlimit; n++) {
        CodeSection* cs = cb->code_section(n);
        assert(cs->is_frozen(), "cross-section branch needs stable offsets");
      }
    }
#endif //ASSERT

    // Push the target offset into the branch instruction.
    masm->pd_patch_instruction(branch, target);
  }
}
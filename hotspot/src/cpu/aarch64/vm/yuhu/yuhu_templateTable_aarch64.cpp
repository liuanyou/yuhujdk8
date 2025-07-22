//
// Created by Anyou Liu on 2025/7/22.
//
#include "precompiled.hpp"
#include "asm/yuhu/yuhu_macroAssembler.hpp"
#include "interpreter/yuhu/yuhu_interpreter.hpp"
#include "interpreter/interpreterRuntime.hpp"
#include "interpreter/yuhu/yuhu_templateTable.hpp"
#include "memory/universe.inline.hpp"
#include "oops/methodData.hpp"
#include "oops/method.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/oop.inline.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/synchronizer.hpp"

#define __ _masm->

// Individual instructions

void YuhuTemplateTable::nop() {
    transition(vtos, vtos);
    // nothing to do
}
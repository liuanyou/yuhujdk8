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

#ifndef SHARE_VM_YUHU_YUHUCACHEDECACHE_HPP
#define SHARE_VM_YUHU_YUHUCACHEDECACHE_HPP

#include "ci/ciMethod.hpp"
#include "code/debugInfoRec.hpp"
#include "yuhu/yuhuBuilder.hpp"
#include "yuhu/yuhuFunction.hpp"
#include "yuhu/yuhuStateScanner.hpp"

// Class hierarchy:
// - YuhuStateScanner
//   - YuhuCacherDecacher
//     - YuhuDecacher
//       - YuhuJavaCallDecacher
//       - YuhuVMCallDecacher
//       - YuhuTrapDecacher
//     - YuhuCacher
//       - YuhuJavaCallCacher
//       - YuhuVMCallCacher
//       - YuhuFunctionEntryCacher
//         - YuhuNormalEntryCacher
//         - YuhuOSREntryCacher

class YuhuCacherDecacher : public YuhuStateScanner {
 protected:
  YuhuCacherDecacher(YuhuFunction* function)
    : YuhuStateScanner(function) {}

  // Helper
 protected:
  static int adjusted_offset(YuhuValue* value, int offset) {
    if (value->is_two_word())
      offset--;
    return offset;
  }
};

class YuhuDecacher : public YuhuCacherDecacher {
 protected:
  YuhuDecacher(YuhuFunction* function, int bci)
    : YuhuCacherDecacher(function), _bci(bci) {}

 private:
  int _bci;

 protected:
  int bci() const {
    return _bci;
  }

 private:
  int                           _pc_offset;
  OopMap*                       _oopmap;
  GrowableArray<ScopeValue*>*   _exparray;
  GrowableArray<MonitorValue*>* _monarray;
  GrowableArray<ScopeValue*>*   _locarray;

 private:
  int pc_offset() const {
    return _pc_offset;
  }
  OopMap* oopmap() const {
    return _oopmap;
  }
  GrowableArray<ScopeValue*>* exparray() const {
    return _exparray;
  }
  GrowableArray<MonitorValue*>* monarray() const {
    return _monarray;
  }
  GrowableArray<ScopeValue*>* locarray() const {
    return _locarray;
  }

  // Callbacks
 protected:
  void start_frame();

  void start_stack(int stack_depth);
  void process_stack_slot(int index, YuhuValue** value, int offset);

  void start_monitors(int num_monitors);
  void process_monitor(int index, int box_offset, int obj_offset);

  void process_oop_tmp_slot(llvm::Value** value, int offset);
  void process_method_slot(llvm::Value** value, int offset);
  void process_pc_slot(int offset);

  void start_locals();
  void process_local_slot(int index, YuhuValue** value, int offset);

  void end_frame();

  // oopmap and debuginfo helpers
 private:
  static int oopmap_slot_munge(int offset) {
    return YuhuStack::oopmap_slot_munge(offset);
  }
  static VMReg slot2reg(int offset) {
    return YuhuStack::slot2reg(offset);
  }
  static Location slot2loc(int offset, Location::Type type) {
    return Location::new_stk_loc(type, offset * wordSize);
  }
  static LocationValue* slot2lv(int offset, Location::Type type) {
    return new LocationValue(slot2loc(offset, type));
  }
  static Location::Type location_type(YuhuValue** addr, bool maybe_two_word) {
    // low addresses this end
    //                           Type       32-bit    64-bit
    //   ----------------------------------------------------
    //   stack[0]    local[3]    jobject    oop       oop
    //   stack[1]    local[2]    NULL       normal    lng
    //   stack[2]    local[1]    jlong      normal    invalid
    //   stack[3]    local[0]    jint       normal    normal
    //
    // high addresses this end

    YuhuValue *value = *addr;
    if (value) {
      if (value->is_jobject())
        return Location::oop;
#ifdef _LP64
      if (value->is_two_word())
        return Location::invalid;
#endif // _LP64
      return Location::normal;
    }
    else {
      if (maybe_two_word) {
        value = *(addr - 1);
        if (value && value->is_two_word()) {
#ifdef _LP64
          if (value->is_jlong())
            return Location::lng;
          if (value->is_jdouble())
            return Location::dbl;
          ShouldNotReachHere();
#else
          return Location::normal;
#endif // _LP64
        }
      }
      return Location::invalid;
    }
  }

  // Stack slot helpers
 protected:
  virtual bool stack_slot_needs_write(int index, YuhuValue* value) = 0;
  virtual bool stack_slot_needs_oopmap(int index, YuhuValue* value) = 0;
  virtual bool stack_slot_needs_debuginfo(int index, YuhuValue* value) = 0;

  static Location::Type stack_location_type(int index, YuhuValue** addr) {
    return location_type(addr, *addr == NULL);
  }

  // Local slot helpers
 protected:
  virtual bool local_slot_needs_write(int index, YuhuValue* value) = 0;
  virtual bool local_slot_needs_oopmap(int index, YuhuValue* value) = 0;
  virtual bool local_slot_needs_debuginfo(int index, YuhuValue* value) = 0;

  static Location::Type local_location_type(int index, YuhuValue** addr) {
    return location_type(addr, index > 0);
  }

  // Writer helper
 protected:
  void write_value_to_frame(llvm::Type* type,
                            llvm::Value*      value,
                            int               offset);
};

class YuhuJavaCallDecacher : public YuhuDecacher {
 public:
  YuhuJavaCallDecacher(YuhuFunction* function, int bci, ciMethod* callee)
    : YuhuDecacher(function, bci), _callee(callee) {}

 private:
  ciMethod* _callee;

 protected:
  ciMethod* callee() const {
    return _callee;
  }

  // Stack slot helpers
 protected:
  bool stack_slot_needs_write(int index, YuhuValue* value) {
    return value && (index < callee()->arg_size() || value->is_jobject());
  }
  bool stack_slot_needs_oopmap(int index, YuhuValue* value) {
    return value && value->is_jobject() && index >= callee()->arg_size();
  }
  bool stack_slot_needs_debuginfo(int index, YuhuValue* value) {
    return index >= callee()->arg_size();
  }

  // Local slot helpers
 protected:
  bool local_slot_needs_write(int index, YuhuValue* value) {
    return value && value->is_jobject();
  }
  bool local_slot_needs_oopmap(int index, YuhuValue* value) {
    return value && value->is_jobject();
  }
  bool local_slot_needs_debuginfo(int index, YuhuValue* value) {
    return true;
  }
};

class YuhuVMCallDecacher : public YuhuDecacher {
 public:
  YuhuVMCallDecacher(YuhuFunction* function, int bci)
    : YuhuDecacher(function, bci) {}

  // Stack slot helpers
 protected:
  bool stack_slot_needs_write(int index, YuhuValue* value) {
    return value && value->is_jobject();
  }
  bool stack_slot_needs_oopmap(int index, YuhuValue* value) {
    return value && value->is_jobject();
  }
  bool stack_slot_needs_debuginfo(int index, YuhuValue* value) {
    return true;
  }

  // Local slot helpers
 protected:
  bool local_slot_needs_write(int index, YuhuValue* value) {
    return value && value->is_jobject();
  }
  bool local_slot_needs_oopmap(int index, YuhuValue* value) {
    return value && value->is_jobject();
  }
  bool local_slot_needs_debuginfo(int index, YuhuValue* value) {
    return true;
  }
};

class YuhuTrapDecacher : public YuhuDecacher {
 public:
  YuhuTrapDecacher(YuhuFunction* function, int bci)
    : YuhuDecacher(function, bci) {}

  // Stack slot helpers
 protected:
  bool stack_slot_needs_write(int index, YuhuValue* value) {
    return value != NULL;
  }
  bool stack_slot_needs_oopmap(int index, YuhuValue* value) {
    return value && value->is_jobject();
  }
  bool stack_slot_needs_debuginfo(int index, YuhuValue* value) {
    return true;
  }

  // Local slot helpers
 protected:
  bool local_slot_needs_write(int index, YuhuValue* value) {
    return value != NULL;
  }
  bool local_slot_needs_oopmap(int index, YuhuValue* value) {
    return value && value->is_jobject();
  }
  bool local_slot_needs_debuginfo(int index, YuhuValue* value) {
    return true;
  }
};

class YuhuCacher : public YuhuCacherDecacher {
 protected:
  YuhuCacher(YuhuFunction* function)
    : YuhuCacherDecacher(function) {}

  // Callbacks
 protected:
  void process_stack_slot(int index, YuhuValue** value, int offset);

  void process_oop_tmp_slot(llvm::Value** value, int offset);
  virtual void process_method_slot(llvm::Value** value, int offset);

  virtual void process_local_slot(int index, YuhuValue** value, int offset);

  // Stack slot helper
 protected:
  virtual bool stack_slot_needs_read(int index, YuhuValue* value) = 0;

  // Local slot helper
 protected:
  virtual bool local_slot_needs_read(int index, YuhuValue* value) {
    return value && value->is_jobject();
  }

  // Writer helper
 protected:
  llvm::Value* read_value_from_frame(llvm::Type* type, int offset);
};

class YuhuJavaCallCacher : public YuhuCacher {
 public:
  YuhuJavaCallCacher(YuhuFunction* function, ciMethod* callee)
    : YuhuCacher(function), _callee(callee) {}

 private:
  ciMethod* _callee;

 protected:
  ciMethod* callee() const {
    return _callee;
  }

  // Stack slot helper
 protected:
  bool stack_slot_needs_read(int index, YuhuValue* value) {
    return value && (index < callee()->return_type()->size() ||
                     value->is_jobject());
  }
};

class YuhuVMCallCacher : public YuhuCacher {
 public:
  YuhuVMCallCacher(YuhuFunction* function)
    : YuhuCacher(function) {}

  // Stack slot helper
 protected:
  bool stack_slot_needs_read(int index, YuhuValue* value) {
    return value && value->is_jobject();
  }
};

class YuhuFunctionEntryCacher : public YuhuCacher {
 public:
  YuhuFunctionEntryCacher(YuhuFunction* function, llvm::Value* method)
    : YuhuCacher(function), _method(method) {}

 private:
  llvm::Value* _method;

 private:
  llvm::Value* method() const {
    return _method;
  }

  // Method slot callback
 protected:
  void process_method_slot(llvm::Value** value, int offset);

  // Stack slot helper
 protected:
  bool stack_slot_needs_read(int index, YuhuValue* value) {
    ShouldNotReachHere(); // entry block shouldn't have stack
    return false; // Never reached, but required for non-void return
  }

  // Local slot helper
 protected:
  bool local_slot_needs_read(int index, YuhuValue* value) {
    return value != NULL;
  }
};

class YuhuNormalEntryCacher : public YuhuFunctionEntryCacher {
 public:
  YuhuNormalEntryCacher(YuhuFunction* function,
                        llvm::Value*   method,
                        llvm::Value*   arg_base,
                        llvm::Value*   arg_count)
    : YuhuFunctionEntryCacher(function, method),
      _arg_base(arg_base),
      _arg_count(arg_count) {}

 protected:
  void process_local_slot(int index, YuhuValue** addr, int offset);

 private:
  llvm::Value* _arg_base;   // intptr pointing to packed Java args
  llvm::Value* _arg_count;  // jint count of args (currently informational)
};

class YuhuOSREntryCacher : public YuhuFunctionEntryCacher {
 public:
  YuhuOSREntryCacher(YuhuFunction* function,
                      llvm::Value*   method,
                      llvm::Value*   osr_buf)
    : YuhuFunctionEntryCacher(function, method),
      _osr_buf(
        builder()->CreateBitCast(
          osr_buf,
          llvm::PointerType::getUnqual(
            llvm::ArrayType::get(
              YuhuType::intptr_type(),
              max_locals() + max_monitors() * 2)))) {}

 private:
  llvm::Value* _osr_buf;

 private:
  llvm::Value* osr_buf() const {
    return _osr_buf;
  }

  // Callbacks
 protected:
  void process_monitor(int index, int box_offset, int obj_offset);
  void process_local_slot(int index, YuhuValue** value, int offset);

  // Helper
 private:
  llvm::Value* CreateAddressOfOSRBufEntry(int offset, llvm::Type* type);
};

#endif // SHARE_VM_YUHU_YUHUCACHEDECACHE_HPP

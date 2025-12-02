# Yuhu 编译器开发文档

## 目录

- [开发日志](DEVELOPMENT_LOG.md) - 完整的开发过程记录
- [迁移步骤](MIGRATION_STEPS.md) - Shark 到 Yuhu 的详细迁移步骤
- [集成详情](INTEGRATION_DETAILS.md) - CompileBroker 集成详细说明
- [问题和解决方案](ISSUES_AND_SOLUTIONS.md) - 开发过程中遇到的问题和解决方案
- [并存实现](COEXISTENCE_IMPLEMENTATION.md) - YUHU 与 C1、C2 并存实现详情
- [构建说明](BUILD_INSTRUCTIONS.md) - LLVM 安装和构建配置
- [配置选项](CONFIGURE_OPTIONS.md) - configure 脚本选项说明
- [LLVM 20 配置](LLVM_20_CONFIG.md) - LLVM 20.1.5 专用配置指南

## 快速开始

### 项目概述

Yuhu 是一个基于 LLVM 的 JIT 编译器，用于 HotSpot JVM。它基于 Shark 编译器的实现，适配了 AArch64 平台。

### 当前状态

- ✅ **Phase 1**: 文件迁移和重命名 - 完成
- ✅ **Phase 2**: 平台适配 - 完成（ZeroStack 已适配为 AArch64）
- ✅ **Phase 3**: CompileBroker 集成 - 完成（YUHU 与 C1/C2 并存）
- ⚠️ **Phase 4**: 编译错误修复 - 待处理（需要 LLVM）
- 📋 **Phase 5**: 测试和验证 - 待处理

### 关键发现

1. **aarch64.ad 不需要**: 使用 LLVM 时，所有架构相关的代码生成都由 LLVM 处理
2. **ZeroStack 适配**: 已完成，采用 Option B（实现 AArch64 等效）
3. **并存模式**: YUHU 与 C1、C2 并存，使用独立的槽位和队列
4. **编译级别**: YUHU 使用 `CompLevel_yuhu_fast` (5) 和 `CompLevel_yuhu_optimized` (6)

## 相关文档

- `doc/YUHU_VS_C2_ARCHITECTURE.md` - Yuhu vs C2 架构对比
- `doc/YUHU_JIT_COMPILER_DESIGN.md` - 设计文档（如果存在）

## 开发团队

- 开发者: [Your Name]
- 开始日期: 2024-12-07

## 更新日志

- **2024-12-07**: 初始文档创建
  - 创建开发日志
  - 记录迁移步骤
  - 记录集成详情
  - 记录问题和解决方案

- **2024-12-07**: 更新文档
  - 更新集成详情：反映 YUHU 与 C1/C2 并存
  - 更新开发日志：记录 ZeroStack 适配完成和并存实现
  - 添加并存实现文档
  - 添加构建说明文档


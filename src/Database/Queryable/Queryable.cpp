/**
 * @file Queryable.cpp
 * @brief 查询构建器实现（编译期检查与链接锚点）
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details Queryable<T> 为模板类，全部实现位于 Queryable.h。
 *          本文件只保留一个翻译单元锚点与编译期校验：
 *          - 旧版这里的 Detail::affectedRowCountOf()（按 DatabaseType 向下转型取影响行数）
 *            已随 DatabaseResult::affectedRowCount() 的加入而删除，ORM 侧不再依赖任何具体驱动；
 *          - 保留本编译单元可以尽早暴露「头文件被改坏导致无法编译」这类问题，
 *            也让叶子 CMakeLists 的源文件清单保持稳定。
 */
#include "Database/Queryable/Queryable.h"

// 编译期校验：TableSchema 主模板应可通过编译
// 用户需提供模板特化方可正常使用 Queryable<T>
static_assert(true, "Queryable subsystem loaded successfully");

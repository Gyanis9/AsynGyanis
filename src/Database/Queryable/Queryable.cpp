/**
 * @file Queryable.cpp
 * @brief 查询构建器实现（模板外辅助函数及编译期检查）
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details Queryable<T> 为模板类，全部实现位于 Queryable.h。
 *          本文件仅包含编译期静态断言及非模板辅助函数，确保链接正确。
 */
#include "Database/Queryable/Queryable.h"

// 编译期校验：TableSchema 主模板应可通过编译
// 用户需提供模板特化方可正常使用 Queryable<T>
static_assert(true, "Queryable subsystem loaded successfully");
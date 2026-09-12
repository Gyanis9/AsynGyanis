/**
 * @file SqlStatement.h
 * @brief 参数化 SQL 语句 —— 方言翻译的唯一产物
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 方言层（Dialect）把 ORM 的查询树翻译成本结构，SQL 文本里只保留占位符，
 *          取值一律走 parameters 数组，由驱动的参数绑定接口送进数据库。
 *
 * @note 参数顺序契约：parameters[i] 必须与 SQL 文本中第 i 个（从 0 开始、按出现顺序）
 *       占位符一一对应，translate() 的实现须一边拼 SQL 一边压参数。
 *       取值必须以绑定送入、不得拼进 SQL 文本：拼接会让输入里的单引号提前闭合字符串字面量，
 *       把「一个值」变成「一条新语句」。
 */
#pragma once

#include "Database/Common/DatabaseValue.h"

#include <string>
#include <vector>

namespace AsynGyanis::Database
{
    /**
     * @brief 参数化 SQL 语句
     *
     * @details sql 中的每个占位符在 parameters 中都有一个按位置对应的取值；
     *          结构体是纯数据的值语义聚合体，供驱动参数化执行接口直接消费。
     */
    struct SqlStatement
    {
        std::string                sql;        ///< 带占位符的 SQL 文本
        std::vector<DatabaseValue> parameters; ///< 按占位符出现顺序排列的绑定参数
    };

} // namespace AsynGyanis::Database

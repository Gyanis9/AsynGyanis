/**
 * @file SqlStatement.h
 * @brief 参数化 SQL 语句 —— 方言翻译的唯一产物
 * @author Gyanis
 * @date 2026-09-16
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 方言层（Dialect）把 ORM 的查询树翻译成本结构，SQL 文本里只保留占位符，
 *          真正的取值一律走 parameters 数组，由驱动的参数绑定接口送进数据库。
 *
 * ## 为什么必须用参数绑定而不是拼接字符串
 * 拼接 SQL 会把数据文本直接塞进语句，输入里的单引号可以提前闭合字符串字面量，
 * 从而把「一个值」变成「一条新语句」（典型的 SQL 注入）；即使不考虑恶意输入，
 * 反斜杠、换行、中文编码差异也会让拼接结果与预期不符。参数绑定把「语句结构」与
 * 「数据」在协议层彻底分开：占位符只描述位置，值以二进制/文本形式单独传递，
 * 数据库不会把参数内容当作 SQL 语法解析，因此单引号、"--" 注释、分号都只是普通字符。
 * 附带收益是同类语句可以复用预编译计划，省掉重复的语法分析与查询规划。
 *
 * ## 参数顺序契约
 * parameters[i] 必须与 SQL 文本中第 i 个（从 0 开始，按出现顺序）占位符一一对应。
 * translate() 的实现必须一边拼 SQL 一边压参数，二者顺序天然对齐。
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
     * @details sql 中的每个占位符在 parameters 中都有一个按位置对应的取值。
     *          结构体是纯数据的值语义聚合体，供驱动参数化执行接口直接消费。
     *
     * @code
     *   SqlStatement statement = dialect->translate(query);
     *   // statement.sql        => "SELECT \"id\" FROM \"users\" WHERE \"age\" >= ?"
     *   // statement.parameters => { std::int64_t{18} }
     *   auto result = connection->execute(statement.sql, statement.parameters);
     * @endcode
     */
    struct SqlStatement
    {
        std::string                sql;        ///< 带占位符的 SQL 文本
        std::vector<DatabaseValue> parameters; ///< 按占位符出现顺序排列的绑定参数
    };

} // namespace AsynGyanis::Database

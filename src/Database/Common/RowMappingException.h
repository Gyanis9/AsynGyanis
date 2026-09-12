/**
 * @file RowMappingException.h
 * @brief 结果集行映射失败异常
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Database/Common/DatabaseException.h"

#include <source_location>
#include <string>

namespace AsynGyanis::Database
{
    /**
     * @brief 结果集行映射失败异常
     *
     * @details 结果集的某一列无法按结构体成员的声明转换时抛出：列缺失、类型不符、
     *          整型越界、NULL 落到非 optional 成员。这类失败说明**表结构与 C++ 结构体的
     *          声明已经不一致**（schema 漂移），是契约问题而不是容量或语句问题——
     *          重试、换连接、改 SQL 都不会好，只能对齐表结构与结构体定义。
     */
    class RowMappingException : public DatabaseException
    {
    public:
        /**
         * @brief 构造行映射失败异常
         * @param message 异常描述消息（含列名、期望类型与实际类型）
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit RowMappingException(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());
    };
} // namespace AsynGyanis::Database

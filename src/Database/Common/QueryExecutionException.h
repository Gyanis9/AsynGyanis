/**
 * @file QueryExecutionException.h
 * @brief 数据库语句执行失败异常
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
     * @brief 数据库语句执行失败异常
     *
     * @details 语句被服务端或驱动拒绝时抛出（语法错误、约束冲突、表不存在、权限不足、
     *          事务控制语句失败等），异常消息里带上驱动的原始错误文本。
     *          与取连接失败分开：本类说明**这次操作本身有问题**，重试同一条语句不会变好，
     *          正确的处置是记日志、把原因回给调用方并让本次请求失败。
     */
    class QueryExecutionException : public DatabaseException
    {
    public:
        /**
         * @brief 构造语句执行失败异常
         * @param message 异常描述消息（通常含驱动的原始错误文本）
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit QueryExecutionException(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());
    };
} // namespace AsynGyanis::Database

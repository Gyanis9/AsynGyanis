/**
 * @file ConnectionUnavailableException.h
 * @brief 取数据库连接失败异常
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
     * @brief 取数据库连接失败异常
     *
     * @details 连接池已达上限且等待超时、或连接工厂创建连接失败时抛出。
     *          与「语句执行失败」分开，是因为调用方的处置完全不同：本类是**容量/可用性问题**，
     *          数据与语句本身没错，可以做退避重试、降级到只读缓存、或给上游回一个
     *          「服务繁忙」；把它和 SQL 错误混在一起会让重试逻辑去重试一条注定失败的语句。
     */
    class ConnectionUnavailableException : public DatabaseException
    {
    public:
        /**
         * @brief 构造取连接失败异常
         * @param message 异常描述消息
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit ConnectionUnavailableException(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());
    };
} // namespace AsynGyanis::Database

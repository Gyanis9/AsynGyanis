/**
 * @file TransactionTimeoutException.h
 * @brief 整笔事务超时异常
 * @author Gyanis
 * @date 2026-10-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Database/Common/DatabaseException.h"

#include <source_location>
#include <string>

namespace AsynGyanis::Database
{
    /**
     * @brief 整笔事务超时异常
     *
     * @details `Transaction::setTimeLimit()` 设的额度用满之后，走这笔事务的语句在发出前被拒时抛本类；
     *          `commit()` 与三条保存点语句改为返回 false 并把原因写进 lastError()。它与 `QueryExecutionException`
     *          分开，是因为处置完全不同：
     *          本类要说的是「这笔事务不该再往下走」——收尾（`rollback()` 或让它随作用域结束）另起
     *          一笔，或把额度放宽到实际需要的时长；而语句执行失败是「这一条 SQL 有问题」，重试同一条语句
     *          有意义。把两者混成一个类，调用方就会拿一条注定被拒的语句去做退避重试，把占着的连接与
     *          行锁一直留到作用域结束。
     * @note `rollback()` 永不被本类拒绝：时限管的是「别再往前走」，不管「把手里的改动放掉」——
     *       拦住回滚等于让超时反而把事务钉得更死
     */
    class ASYN_DATABASE_API TransactionTimeoutException : public DatabaseException
    {
    public:
        /**
         * @brief 构造整笔事务超时异常
         * @param message 异常描述消息
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit TransactionTimeoutException(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());
    };
} // namespace AsynGyanis::Database

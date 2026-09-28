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

#include <cstdint>
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
     * @note 「不会变好」只对约束/语法那批成立。死锁与锁等待同样会走到本类，而它们正是
     *       该重试的形状——所以本类额外带出驱动原生码与 isRetryable()，让调用方能分开判。
     *       在这之前原生码只拼在消息文本末尾（见 ErrorText.h），要判就得匹配中文。
     */
    class QueryExecutionException : public DatabaseException
    {
    public:
        /// 驱动未给出可与本次失败配对的错误码时的取值
        static constexpr std::int64_t kUnknownNativeErrorCode = -1;

        /**
         * @brief 构造语句执行失败异常（不带原生码，即码未知）
         * @param message 异常描述消息（通常含驱动的原始错误文本）
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit QueryExecutionException(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 构造语句执行失败异常并带上驱动原生码
         * @param message 异常描述消息（通常含驱动的原始错误文本）
         * @param nativeErrorCode 驱动给出的原生错误码（MySQL 的 mysql_stmt_errno、
         *        SQLite 的 sqlite3_errcode 之类），未知时传 kUnknownNativeErrorCode
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        QueryExecutionException(const std::string &message, std::int64_t nativeErrorCode, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 取驱动原生错误码
         * @return std::int64_t 原生码；未知时为 kUnknownNativeErrorCode
         */
        [[nodiscard]] std::int64_t nativeErrorCode() const noexcept;

        /**
         * @brief 重试同一条语句有没有可能变好
         * @details 判定表刻意只收「服务端主动让这条语句重来」这一类，其余一律 false：
         *          - MySQL 1205（锁等待超时）、1213（死锁被选为牺牲者）→ true；
         *          - SQLite 5（SQLITE_BUSY）、6（SQLITE_LOCKED）→ true；
         *          - 约束冲突（MySQL 1062 / SQLite 19）、语法与对象不存在、权限不足 → false；
         *          - 语句超时被打断（MySQL 3024 / SQLite 9 INTERRUPT）→ **false**：写语句可能
         *            已经在服务端生效，重试就是二次写入，其代价远大于多一趟往返；
         *          - 码未知（-1）→ false：宁可少重试一次，也不要把一条注定失败的语句打进循环。
         * @note 未列出 2006/2013（连接中断类）：那条语句同样可能已在服务端生效，
         *       跨连接重放属于「换一个连接重做整笔业务」的决策，不是本函数能替调用方拍的。
         * @return true 属于可重试的冲突类失败
         */
        [[nodiscard]] bool isRetryable() const noexcept;

    private:
        std::int64_t m_nativeErrorCode{kUnknownNativeErrorCode}; ///< 驱动原生码，与消息文本出自同一次失败
    };
} // namespace AsynGyanis::Database

/**
 * @file OptimisticLockException.h
 * @brief ORM 乐观锁冲突异常
 * @author Gyanis
 * @date 2026-10-15
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
     * @brief 乐观锁冲突：这条行在读出之后被别人改过了
     *
     * @details 表在 `TableSchema<T>` 里声明了 `kVersionColumn` 之后，`update()` 的 WHERE 会带上
     *          「主键 = ? AND 版本 = ?」两个条件，SET 里把版本加一。更新到 0 行即说明那句 WHERE
     *          什么都没匹配上——行被删了，或者别人先改过、版本已经不是手里这个值。
     *          单独成一个类型而不是并进 `QueryExecutionException`：驱动没有报错，服务端执行得
     *          好端端的，这是业务侧并发的信号，处置方式也完全不同——典型的处置是
     *          「重新读出、合并、再试一次」，照着语句执行失败那一族去记日志或换连接都不对症。
     * @note 消息里点名了表、版本列、期望的版本值与主键值，两者都在的消息才够调用方定位是哪一行、
     *       以及手里那份数据有多旧（见 Queryable::buildUpdateStatement）
     */
    class ASYN_DATABASE_API OptimisticLockException : public DatabaseException
    {
    public:
        /**
         * @brief 构造乐观锁冲突异常
         * @param message 异常描述消息（含表名、版本列、期望版本与主键值）
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit OptimisticLockException(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());
    };
} // namespace AsynGyanis::Database

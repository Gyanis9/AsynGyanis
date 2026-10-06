/**
 * @file PoolConfig.h
 * @brief 连接池配置项
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include "AsynGyanisExport.h"

#include <cstddef>

namespace AsynGyanis::Database
{

    /// 后台健康检查间隔的上限（秒，7 天）：ConnectionPool 在构造时按这条拒掉超出范围的取值。
    /// 存在的理由不是「没人要一千年一次」，而是那条线程把秒换算成毫秒后拿去当睡眠片长——
    /// 换算溢出会先变成负数，wait_for 对负时长立即返回，于是线程每秒空转一轮：
    /// 既不睡觉，也不按配置的节奏干活，而现场只看得到 CPU 在动
    inline constexpr std::size_t kMaximumHealthCheckIntervalSeconds = 7U * 24U * 60U * 60U;

    /**
     * @brief 连接池配置
     *
     * @details 所有超时/间隔类字段均使用 size_t 以避免符号转换警告，
     *          实际取值不应超过 uint64_t 上限（默认值为通用服务场景取值）。
     * @warning 本结构有**四个** 0 值，含义各不相同，且都写在取值那一行而不是靠猜：
     *          idleTimeoutSeconds 为 0 = 该项不生效（不因空闲被驱逐）；
     *          maximumLifetimeSeconds 为 0 = 每条连接一归还就过期（等于禁用复用）——这是刻意留出来的
     *          （微基准的 churn 形态与若干用例都靠它构造「每次归还都丢弃」），想「不限存活期」请给一个
     *          足够大的值，别填 0；maximumPoolSize 为 0 = 不允许创建任何连接（每条借出都拿空）；
     *          acquireTimeoutMilliseconds 为 0 = 不排队，两次乐观尝试后立即返回空。
     *          最后这一条与 `Database` 里其余超时字段的读法**相反**（MySQL/Redis 的 connectTimeout 与
     *          queryTimeout 都把非正值读成「不超时」），因为这里的 0 是「等不起」而不是「不限等待」——
     *          一个无限的借用时限会把调用方的协程永久挂住，那不是任何配置想要的意思。
     */
    struct ASYN_DATABASE_API PoolConfig
    {
        std::size_t maximumPoolSize        = 32;   ///< 连接数上限（含空闲与活跃），0 表示不允许创建任何连接
        std::size_t idleTimeoutSeconds     = 300;  ///< 空闲连接超时（秒）：取出时与后台定期检查时判定，0 表示不因空闲被驱逐
        std::size_t maximumLifetimeSeconds = 1800; ///< 连接最大存活时间（秒），从建立到丢弃的总时长上限；0 表示一归还就过期（等于禁用复用）
        /// 后台健康检查间隔（秒），周期性遍历空闲列表并驱逐过期连接。实际下限被压到 1 秒；上限是 7 天，
        /// 超出即在 ConnectionPool 构造时被拒绝——后台要把这份秒数换算成毫秒，不设上限的话一个荒谬的
        /// 取值会先溢出成负数，让那条线程退化成每秒空转一轮（既不睡觉，也不按配置的节奏干活）
        std::size_t healthCheckIntervalSeconds = 60;
        /// 阻塞获取连接的超时（毫秒），超时未取到返回空 PooledConnection；0 表示不排队——做两次乐观
        /// 尝试就返回空，而不是「不限等待」。上限是本平台把毫秒折进时钟刻度所能表达的量
        /// （`steady_clock` 的 duration 折成毫秒再取一半，另一半让给「现在这一刻」），
        /// 超出即在 ConnectionPool 构造时被拒绝——这条时长要直接加到 `steady_clock::now()` 上，
        /// 越过 2^63 会绕成负时长，两条等待路径算出的截止时刻都落在「现在之前」：配得越大反倒一条都不等
        std::size_t acquireTimeoutMilliseconds = 5000;
    };

} // namespace AsynGyanis::Database

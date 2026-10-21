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
     * @warning 本结构有五个 0 值，含义各不相同，且都写在取值那一行而不是靠猜：
     *          idleTimeoutSeconds 为 0 = 该项不生效（不因空闲被驱逐）；
     *          maximumLifetimeSeconds 为 0 = 每条连接一归还就过期（等于禁用复用）——这是刻意留出来的
     *          （微基准的 churn 形态与若干用例都靠它构造「每次归还都丢弃」），想「不限存活期」请给一个
     *          足够大的值，别填 0；maximumPoolSize 为 0 = 不允许创建任何连接（每条借出都拿空）；
     *          acquireTimeoutMilliseconds 为 0 = 不排队，两次乐观尝试后立即返回空；
     *          maximumWaitingRequests 为 0 = 同样不排队——等待名额判据是「合计等待数 >= 上限」，
     *          0 让该式恒真，两条取出路径都直接空手返回。
     *          最后这两条与 `Database` 里其余超时字段的读法相反（MySQL/Redis 的 connectTimeout 与
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
        /// 阻塞获取连接的超时（毫秒），超时未取到返回空 PooledConnection；0 表示不排队——截止时刻就是
        /// 当下，几次乐观尝试之后立即返回空，而不是「不限等待」。上限是本平台把毫秒折进时钟刻度所能表达的量
        /// （`steady_clock` 的 duration 折成毫秒再取一半，另一半让给「现在这一刻」），
        /// 超出即在 ConnectionPool 构造时被拒绝——这条时长要直接加到 `steady_clock::now()` 上，
        /// 越过 2^63 会绕成负时长，两条等待路径算出的截止时刻都落在「现在之前」：配得越大反倒一条都不等
        std::size_t acquireTimeoutMilliseconds = 5000;
        /// 等待获取的连接数上限（同步等待与协程等待的合计）：到顶后新来的借用请求不再排队、
        /// 立即空手返回（含异步路径：不挂起）。队列的深度由请求到达速率与 acquireTimeoutMilliseconds
        /// 相乘决定，坏日子里它是一条不受池容量约束的内存放大路径——这条上限把它封在「默认池容量
        /// （32）的两倍」这一档：至多两轮轮换的排队深度，再深说明请求方该降级而不是继续等。
        /// 0 表示不排队（见上面 0 值清单）。命中它意味着池容量或下游耗时出了问题，
        /// 要修的是那一头，不该靠调大这个数绕开
        std::size_t maximumWaitingRequests = 64;
        /// 借出前是否先发一次有效性探针（`DatabaseConnection::probeAlive()`）。默认关闭：
        /// 打开后每条从空闲栈交出的连接多一次网络往返（MySQL 的 mysql_ping、Redis 的 PING 都是），
        /// 这是性能契约的变化，因此投产前要在 docs/数据库访问.md 的连接池那条里过一眼。刚建好的那条与归还时直接交接给
        /// 等待者的那条都不探——前者几毫秒前才握完手，后者刚从上一位手里出来，等于刚刚被证明可用。
        /// 它换到的是「不要把手里的死人交出去」：服务端重启过、而池里还躺着一条状态读起来仍然
        /// 「已连接」的连接时，不开探针就只能等下一个借用者在第一条语句上报错、丢弃、再重建
        bool probeBeforeAcquire = false;
        /// 一次借用里建连最多试几次（含第一次）。默认 1 ＝ 不重试，与这项存在之前的行为一致：
        /// 失败当场空手交回调用方。大于 1 时只重 `isTransientConnectFailure()` 认的那一类失败
        /// （服务端正在重启／连接被拒这类「等一会再来」的形状），鉴权错、库名错这类重试也不会中，
        /// 一律直接空手。重试的间隔是 5 毫秒起、每次翻倍、单次封顶 100 毫秒，总时长不超过
        /// acquireTimeoutMilliseconds —— 后一项是这次借用的截止时刻，重试不许越过它
        std::size_t maximumConnectAttempts = 1;
    };

} // namespace AsynGyanis::Database

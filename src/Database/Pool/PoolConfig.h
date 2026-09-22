/**
 * @file PoolConfig.h
 * @brief 连接池配置项
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include <cstddef>

namespace AsynGyanis::Database
{

    /**
     * @brief 连接池配置
     *
     * @details 所有超时/间隔类字段均使用 size_t 以避免符号转换警告，
     *          实际取值不应超过 uint64_t 上限（默认值为通用服务场景取值）。
     * @warning 两个 0 的含义是**相反**的，且都写在取值那一行而不是靠猜：
     *          idleTimeoutSeconds 为 0 = 该项不生效；maximumLifetimeSeconds 为 0 = 每条连接一归还就过期。
     *          后者是刻意留出来的（微基准的 churn 形态与若干用例都靠它构造「每次归还都丢弃」），
     *          想「不限存活期」请给一个足够大的值，别填 0。
     */
    struct PoolConfig
    {
        std::size_t maximumPoolSize            = 32;   ///< 连接数上限（含空闲与活跃），0 表示不允许创建任何连接
        std::size_t idleTimeoutSeconds         = 300;  ///< 空闲连接超时（秒）：取出时与后台定期检查时判定，0 表示不因空闲被驱逐
        std::size_t maximumLifetimeSeconds     = 1800; ///< 连接最大存活时间（秒），从建立到丢弃的总时长上限；0 表示一归还就过期（等于禁用复用）
        std::size_t healthCheckIntervalSeconds = 60;   ///< 后台健康检查间隔（秒），周期性遍历空闲列表并驱逐过期连接；实际下限被压到 1 秒
        std::size_t acquireTimeoutMilliseconds = 5000; ///< 阻塞获取连接的超时（毫秒），超时未取到返回空 PooledConnection
    };

} // namespace AsynGyanis::Database

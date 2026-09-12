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
     *          实际取值不应超过 uint64_t 上限。各默认值按通用服务场景
     *          给出，线上部署应根据业务负载与数据库规格调整。
     */
    struct PoolConfig
    {
        std::size_t maximumPoolSize              = 32;    ///< 连接数上限（含空闲与活跃），0 表示不允许创建任何连接
        std::size_t idleTimeoutSeconds            = 300;   ///< 空闲连接超时（秒），超过该时长未使用的连接在归还时或定期检查时被关闭
        std::size_t maximumLifetimeSeconds        = 1800;  ///< 连接最大存活时间（秒），连接从创建到销毁的总时长上限
        std::size_t healthCheckIntervalSeconds    = 60;    ///< 后台健康检查间隔（秒），周期性遍历空闲列表并驱逐过期连接
        std::size_t acquireTimeoutMilliseconds    = 5000;  ///< 阻塞获取连接的超时（毫秒），超时未取到返回空 PooledConnection
        bool        useAsyncAcquire              = false; ///< 是否启用协程异步获取（需要 Core::EventLoop 环境），默认关闭
    };

} // namespace AsynGyanis::Database
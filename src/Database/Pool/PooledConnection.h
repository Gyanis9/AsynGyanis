/**
 * @file PooledConnection.h
 * @brief RAII 连接包装器 — 析构时自动归还连接池
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 这是连接池向调用方交出连接的唯一接口形态。析构或 release() 时自动归还，移动后源对象
 *          为空、不会重复归还，重复 release() 也安全（由标志位防护）。
 *          上层（Queryable、事务）直接使用本类，因此接口在此冻结。
 */
#pragma once

#include "Database/Common/DatabaseConnection.h"

#include <memory>

namespace AsynGyanis::Database
{

    // 前置声明：ConnectionPool 在 ConnectionPool.h 中定义
    class ConnectionPool;

    /**
     * @brief RAII 连接包装器
     *
     * @details 析构时自动归还连接池，手工调用 release() 可提前归还。公共路径（析构与 release）
     *          应尽可能短，归还后的过期检查与健康检查这类重活留给后台线程，不健康的连接被丢弃。
     */
    class PooledConnection
    {
    public:
        /**
         * @brief 构造一个空的 PooledConnection，不持有任何连接
         */
        PooledConnection() noexcept = default;

        /**
         * @brief 构造一个持有有效连接的 PooledConnection
         * @param connection 数据库连接的所有权
         * @param pool       归属的连接池指针，不可为空
         */
        explicit PooledConnection(std::unique_ptr<DatabaseConnection> connection, ConnectionPool *pool) noexcept;

        /**
         * @brief 析构函数，自动归还连接（若尚未 release）
         */
        ~PooledConnection();

        // 禁止拷贝：每个连接同时只能有一个 RAII 包装持有
        PooledConnection(const PooledConnection &) = delete;

        PooledConnection &operator=(const PooledConnection &) = delete;

        /**
         * @brief 移动构造函数
         * @param other 源对象；移动后源对象为空，析构不归还
         */
        PooledConnection(PooledConnection &&other) noexcept;

        /**
         * @brief 移动赋值运算符
         * @param other 源对象；移动后源对象为空，析构不归还
         * @return 当前对象的引用
         */
        PooledConnection &operator=(PooledConnection &&other) noexcept;

        /**
         * @brief 通过箭头运算符访问底层连接
         * @return DatabaseConnection* 底层连接指针；不持有时返回 nullptr
         */
        DatabaseConnection *operator->() const;

        /**
         * @brief 通过解引用运算符访问底层连接
         * @return DatabaseConnection& 底层连接引用
         * @warning 不持有时解引用导致未定义行为，调用前应检查 operator bool
         */
        DatabaseConnection &operator*() const;

        /**
         * @brief 检查是否持有有效连接
         * @return true 持有有效连接；false 为空
         */
        explicit operator bool() const noexcept;

        /**
         * @brief 提前归还连接至池
         *
         * @details 调用后连接被归还到 ConnectionPool，池会执行过期与健康检查。
         *          本对象被标记为空，后续析构不会重复归还。
         *          支持多次调用：第二次及以后的调用被忽略（double-release 防护）。
         */
        void release();

    private:
        /**
         * @brief 归还连接的内部实现
         * @details 由析构函数和 release() 共用，确保只执行一次归还逻辑。
         *          归还后清空 m_connection 与 m_pool，防止重复归还。
         */
        void doReturnToPool();

        std::unique_ptr<DatabaseConnection> m_connection;     ///< 底层数据库连接的所有权
        ConnectionPool *                    m_pool = nullptr; ///< 归属的连接池，析构时据此归还
    };

} // namespace AsynGyanis::Database

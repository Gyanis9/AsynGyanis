/**
 * @file TestConnectionPool.h
 * @brief 连接池单元测试辅助：MockConnection 与便利工厂
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details MockConnection 是一个打桩的 DatabaseConnection 实现，不依赖任何真实数据库。
 *          每个实例拥有唯一 ID，工厂可以追踪哪些连接被创建、被销毁、被检查健康状态，
 *          使测试可在不触碰真实数据库的前提下断言池的行为。
 */
#pragma once

#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseType.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Database::TestPoolSupport
{

    /**
     * @brief 全局连接计数器，用于生成唯一 ID 并追踪创建/销毁总数
     */
    struct ConnectionCounter
    {
        std::atomic<std::int64_t> totalCreated{0};   ///< 累计创建的连接数
        std::atomic<std::int64_t> totalDestroyed{0};  ///< 累计销毁的连接数
        std::atomic<std::int64_t> healthCheckCount{0}; ///< isConnected() 调用次数
    };

    /**
     * @brief 打桩的数据库连接，用于测试连接池行为
     *
     * @details 不连接任何真实数据库。提供以下可配置行为：
     *          - m_healthOk：isConnected() 的返回值，可设为 false 模拟断连
     *          - m_connectOk：connect() 的返回值
     *          - 每个实例有唯一 ID，方便断言连接复用
     */
    class MockConnection : public DatabaseConnection
    {
    public:
        /**
         * @brief 构造一个打桩连接
         * @param counter 全局计数器（非空），用于统计
         * @param id      连接唯一 ID
         */
        explicit MockConnection(ConnectionCounter &counter, std::int64_t id)
            : m_counter(&counter)
            , m_id(id)
        {
        }

        /**
         * @brief 析构时更新销毁计数
         */
        ~MockConnection() override
        {
            m_counter->totalDestroyed.fetch_add(1);
        }

        MockConnection(const MockConnection &)            = delete;
        MockConnection &operator=(const MockConnection &) = delete;

        /**
         * @brief 模拟连接建立
         * @return m_connectOk 的值
         */
        bool connect() override
        {
            m_isConnected = m_connectOk;
            return m_connectOk;
        }

        /**
         * @brief 模拟断开连接
         */
        void disconnect() override
        {
            m_isConnected = false;
        }

        /**
         * @brief 模拟健康检查
         * @return m_healthOk 的值
         */
        [[nodiscard]] bool isConnected() const override
        {
            m_counter->healthCheckCount.fetch_add(1);
            return m_healthOk && m_isConnected;
        }

        /**
         * @brief 模拟执行命令
         * @return 固定返回 nullptr（测试中不使用结果集）
         */
        [[nodiscard]] std::unique_ptr<DatabaseResult> execute(std::string_view /*command*/) override
        {
            return nullptr;
        }

        /**
         * @brief 获取数据库类型
         * @return DatabaseType::Sqlite（任意值，测试不重要）
         */
        [[nodiscard]] DatabaseType databaseType() const override
        {
            return DatabaseType::Sqlite;
        }

        /**
         * @brief 获取连接唯一 ID
         * @return std::int64_t 连接 ID
         */
        [[nodiscard]] std::int64_t id() const noexcept { return m_id; }

        /**
         * @brief 设置健康检查的返回值
         * @param ok true 表示健康
         */
        void setHealthOk(bool ok) noexcept { m_healthOk = ok; }

    private:
        ConnectionCounter *m_counter;   ///< 全局计数器
        std::int64_t       m_id;        ///< 连接唯一 ID
        bool               m_connectOk{true};   ///< connect() 的返回值
        bool               m_healthOk{true};  ///< isConnected() 的返回值
        bool               m_isConnected{true}; ///< 连接状态（由 connect/disconnect 控制）
    };

    /**
     * @brief 创建一个 MockConnection 的工厂
     * @param counter 全局计数器
     * @return 返回可传递给 ConnectionPool 的工厂函数
     */
    inline std::function<std::unique_ptr<DatabaseConnection>()>
    makeMockFactory(ConnectionCounter &counter)
    {
        return [&counter]() -> std::unique_ptr<DatabaseConnection>
        {
            const std::int64_t id = counter.totalCreated.fetch_add(1);
            return std::make_unique<MockConnection>(counter, id);
        };
    }

} // namespace AsynGyanis::Database::TestPoolSupport
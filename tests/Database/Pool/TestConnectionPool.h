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
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Database::TestPoolSupport
{

    /**
     * @brief 把一次「到达」停在原地的门闩
     * @details 要判定「某段代码不握着池的锁」或「某次通知正好落在等待者还没睡下的空档」，
     *          就得能把那一次调用变成可观察、可无限延长的状态，再从别的线程安排对手方动作。
     *          布防期间到达的调用会等着解除布防；未布防时到达只记一次到达，不改变行为。
     *          现有两处用法：把一次 disconnect() 停在锁外，和把一次工厂建连停在取出与睡下之间。
     */
    class ArrivalGate
    {
    public:
        /**
         * @brief 布防并清掉上一次的到达记录，使随后的到达一定属于本用例要量的那一次
         */
        void arm()
        {
            const std::lock_guard lock(m_mutex);
            m_hasArrived = false;
            m_isArmed    = true;
        }

        /**
         * @brief 解除布防并放行所有停在门闩里的到达
         */
        void release()
        {
            {
                const std::lock_guard lock(m_mutex);
                m_isArmed = false;
            }
            m_condition.notify_all();
        }

        /**
         * @brief 被调用时进入：已布防就在此等到解除布防
         */
        void arrive()
        {
            std::unique_lock lock(m_mutex);
            m_hasArrived = true;
            // 谓词判定与 m_isArmed 的写在同一把锁里，因此不会漏掉 release()
            m_condition.wait(lock, [this] { return !m_isArmed; });
        }

        /**
         * @brief 是否已经有一次到达发生在布防之后
         * @return true 至少到达过一次
         */
        [[nodiscard]] bool hasArrived()
        {
            const std::lock_guard lock(m_mutex);
            return m_hasArrived;
        }

    private:
        std::mutex              m_mutex;             ///< 保护布防与到达两个标志
        std::condition_variable m_condition;         ///< 放行停在门闩里的断开
        bool                    m_isArmed{false};    ///< 是否布防：布防期间到达的断开原地等待
        bool                    m_hasArrived{false}; ///< 是否已有一次断开到达
    };

    /**
     * @brief 全局连接计数器，用于生成唯一 ID 并追踪创建/销毁总数
     */
    struct ConnectionCounter
    {
        std::atomic<std::int64_t> totalCreated{0};          ///< 累计创建的连接数
        std::atomic<std::int64_t> totalDestroyed{0};        ///< 累计销毁的连接数
        std::atomic<std::int64_t> healthCheckCount{0};      ///< isConnected() 调用次数
        std::atomic<std::int64_t> sessionResetCount{0};     ///< resetSessionState() 调用次数（会话状态复位钩子）
        std::atomic<bool>         connectionsHealthy{true}; ///< 全体连接的存活开关：用例据此造出「入栈后失联」
        ArrivalGate              *disconnectGate{nullptr};  ///< 断开门闩，空则 disconnect() 不额外停留
    };

    /**
     * @brief 打桩的数据库连接，用于测试连接池行为
     *
     * @details 不连接任何真实数据库：m_healthOk / m_connectOk 分别决定 isConnected() 与 connect() 的
     *          返回值（可设 false 模拟断连），每个实例持有唯一 ID 以便断言连接复用。
     *          两个全体开关挂在 ConnectionCounter 上：connectionsHealthy 让用例造出「入栈之后才失联」
     *          这种按实例设置够不着的时序，disconnectGate 把一次断开停在原地。
     */
    class MockConnection : public DatabaseConnection
    {
    public:
        /**
         * @brief 构造一个打桩连接
         * @param counter 全局计数器（非空），用于统计
         * @param id      连接唯一 ID
         */
        explicit MockConnection(ConnectionCounter &counter, std::int64_t id) : m_counter(&counter), m_id(id)
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
         * @brief 模拟断开连接：门闩布防时原地等到解除
         * @details 停留点是刻意给的：池把「断开」留在 m_mutex 的临界区里时，用例才能从别的线程
         *          观察到那把锁被一次阻塞的系统调用占着
         */
        void disconnect() override
        {
            m_isConnected = false;
            if (m_counter->disconnectGate != nullptr)
            {
                m_counter->disconnectGate->arrive();
            }
        }

        /**
         * @brief 模拟健康检查
         * @return m_healthOk、本连接状态与全体存活开关三者都与的结果
         */
        [[nodiscard]] bool isConnected() const override
        {
            m_counter->healthCheckCount.fetch_add(1);
            return m_healthOk && m_isConnected && m_counter->connectionsHealthy.load();
        }

        /// 归还路径上的会话状态复位：用例据此断言池在两条去向（空闲栈/等待者）之前都调过它
        void resetSessionState() noexcept override
        {
            m_counter->sessionResetCount.fetch_add(1);
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
        [[nodiscard]] std::int64_t id() const noexcept
        {
            return m_id;
        }

        /**
         * @brief 设置健康检查的返回值
         * @param ok true 表示健康
         */
        void setHealthOk(bool ok) noexcept
        {
            m_healthOk = ok;
        }

    private:
        ConnectionCounter *m_counter;           ///< 全局计数器
        std::int64_t       m_id;                ///< 连接唯一 ID
        bool               m_connectOk{true};   ///< connect() 的返回值
        bool               m_healthOk{true};    ///< isConnected() 的返回值
        bool               m_isConnected{true}; ///< 连接状态（由 connect/disconnect 控制）
    };

    /**
     * @brief 创建一个 MockConnection 的工厂
     * @param counter 全局计数器
     * @return 返回可传递给 ConnectionPool 的工厂函数
     */
    inline std::function<std::unique_ptr<DatabaseConnection>()> makeMockFactory(ConnectionCounter &counter)
    {
        return [&counter]() -> std::unique_ptr<DatabaseConnection>
        {
            const std::int64_t id = counter.totalCreated.fetch_add(1);
            return std::make_unique<MockConnection>(counter, id);
        };
    }

} // namespace AsynGyanis::Database::TestPoolSupport

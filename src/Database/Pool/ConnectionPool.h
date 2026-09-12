/**
 * @file ConnectionPool.h
 * @brief 高性能线程安全连接池
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 连接池核心实现，支持同步阻塞获取、非阻塞尝试获取、以及协程异步获取三种方式。
 *
 * ## 设计要点
 *
 * ### LIFO 空闲栈
 * 最新归还的连接最可能还在热点缓存中，减少冷启动。
 * 使用 std::vector 模拟栈，std::mutex 保护。
 * 热点池的单锁竞态可以接受：每个池实例绑一个数据库，单库的操作并行度不会高到使单锁成为瓶颈。
 *
 * ### 懒惰创建
 * acquire() 时若无空闲连接且 total < max 则新建，避免上线就建满。
 *
 * ### 超时与健康
 * acquire() 等待空闲连接最多 acquireTimeoutMs（用条件变量）；
 * 归还时检查连接是否过期（idleTimeoutSec / maxLifetimeSec），过期直接关闭不归还。
 * 后台 std::jthread 定期遍历空闲列表，驱逐超时连接（惰性 + 定期双重清理）。
 *
 * ### 健康检查
 * 在 acquire（从空闲栈取出时）做轻量探活（调用 isConnected()）。
 * 不健康则丢弃并从工厂重建。选择获取时做而非归还时做，是因为：
 * - 获取路径是调用方感知延迟的关键路径，此时探活可以确保调用方拿到的是可用连接；
 * - 归还路径应尽可能短以让后续等待者尽快拿到连接；
 * - 归还时连接刚被使用完，大概率还是健康的，探活收益低。
 *
 * ### 异步获取
 * acquireAsync 等不到空闲连接时把当前协程挂起到一个等待列表，
 * 有空闲时由归还路径唤醒。等待列表用 std::mutex + std::deque 保护，
 * 虽然含锁但操作频率低（仅在池空且并发协程等待时触发），安全且简单。
 * 协程被提前销毁时能从等待列表中自行移除。
 *
 * ### 性能意识
 * acquire / release 的公共路径尽可能短（第一次直接返回）。
 * 核心运算密集型操作（健康检查、过期驱逐）转移到后台线程。
 *
 * ## 线程安全
 * 所有公有方法（包括 acquire、tryAcquire、release 路径）均为线程安全。
 * 统计访问方法使用原子变量或加锁读取。
 */
#pragma once

#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/PooledConnection.h"
#include "Core/Coroutine/Task.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace AsynGyanis::Database
{

    /**
     * @brief 高性能线程安全连接池
     *
     * @details 管理一组 DatabaseConnection 实例，提供三种获取方式：
     *          - acquire()：阻塞等待，最多等 acquireTimeoutMs 毫秒；
     *          - tryAcquire()：非阻塞，有则返回，无则返回空；
     *          - acquireAsync()：协程异步，挂起等待，由归还路径唤醒。
     *
     *          连接生命周期由池管理，调用方通过 PooledConnection RAII 包装使用。
     *
     * @code
     *   ConnectionPool pool(
     *       []() { return DatabaseFactory::createSqlite(ConnectionConfig::sqliteDefault()); }
     *   );
     *
     *   // 方式一：阻塞获取
     *   PooledConnection connection = pool.acquire();
     *   if (connection)
     *   {
     *       connection->execute("SELECT 1");
     *   } // 析构自动归还
     *
     *   // 方式二：非阻塞尝试
     *   PooledConnection maybe = pool.tryAcquire();
     *
     *   // 方式三：协程异步
     *   // auto task = pool.acquireAsync(eventLoop);
     *   // auto connection = co_await task;
     * @endcode
     */
    class ConnectionPool
    {
    public:
        /**
         * @brief 构造连接池
         *
         * @details 构造时不预创建任何连接，所有连接均为懒惰创建。
         *          启动后台健康检查线程，定期清理过期空闲连接。
         *
         * @param factory 工厂回调，每次需要新连接时调用，返回已 connect() 的连接
         * @param config  连接池配置，使用默认值时可省略
         */
        explicit ConnectionPool(std::function<std::unique_ptr<DatabaseConnection>()> factory,
                                PoolConfig config = PoolConfig{});

        /**
         * @brief 析构连接池
         *
         * @details 请求后台线程停止，等待其退出，然后关闭所有空闲连接。
         *          当前已取出的连接（PooledConnection）仍可正常使用直到归还，
         *          归还时由于池已析构，连接将被直接关闭。
         */
        ~ConnectionPool();

        // 连接池独占底层连接集合的所有权，禁止拷贝与移动
        ConnectionPool(const ConnectionPool &)            = delete;
        ConnectionPool &operator=(const ConnectionPool &) = delete;
        ConnectionPool(ConnectionPool &&)                 = delete;
        ConnectionPool &operator=(ConnectionPool &&)      = delete;

        /**
         * @brief 阻塞获取连接
         *
         * @details 优先从空闲栈返回（LIFO），无空闲且未达上限时创建新连接。
         *          忙时等待最多 acquireTimeoutMs 毫秒，超时返回空。
         *
         * @return PooledConnection 持有有效连接时 operator bool() 返回 true；
         *         超时或创建失败时返回空
         */
        PooledConnection acquire();

        /**
         * @brief 协程异步获取连接
         *
         * @details 当没有可用连接时，当前协程被挂起到等待列表；
         *          连接被归还时唤醒等待列表中的协程。
         *          使用前需确保 useAsyncAcquire 配置已启用，且传入有效的 EventLoop。
         *
         * @param loop 事件循环引用，用于协程调度
         * @return Core::Task<PooledConnection> 协程任务，co_await 后获得连接
         *
         * @note 调用方必须确保协程不被提前销毁（Task 析构）以免悬挂指针。
         *       协程被提前销毁时自动从等待列表中移除。
         */
        Core::Task<PooledConnection> acquireAsync(Core::EventLoop &loop);

        /**
         * @brief 非阻塞尝试获取连接
         *
         * @details 有闲置连接或可建新连接时立即返回，否则返回空。
         *          不等待，不抛出异常。
         *
         * @return PooledConnection 获取成功时持有有效连接，否则为空
         */
        PooledConnection tryAcquire() noexcept;

        // ========================================================================
        // 统计查询
        // ========================================================================

        /**
         * @brief 当前活跃（已取出未归还）连接数
         * @return std::size_t 活跃连接数
         */
        [[nodiscard]] std::size_t activeCount() const noexcept;

        /**
         * @brief 当前空闲（池中等待）连接数
         * @return std::size_t 空闲连接数
         */
        [[nodiscard]] std::size_t idleCount() const noexcept;

        /**
         * @brief 总连接数（活跃 + 空闲）
         * @return std::size_t 总连接数
         */
        [[nodiscard]] std::size_t totalCount() const noexcept;

        /**
         * @brief 当前等待获取的连接数（同步 + 异步等待者总数）
         * @return std::size_t 等待获取的连接数
         */
        [[nodiscard]] std::size_t waitingCount() const noexcept;

        // ========================================================================
        // 内部接口（被 PooledConnection 调用）
        // ========================================================================

        /**
         * @brief 归还连接至池
         *
         * @details 由 PooledConnection 在析构或 release() 时调用。
         *          连接会先经过过期检查：若空闲超时或超最大存活时间则关闭不归还。
         *          若存在异步等待者，直接将连接交付给等待者而不入空闲栈。
         *
         * @param connection 待归还的连接所有权
         */
        void returnConnection(std::unique_ptr<DatabaseConnection> connection);

    private:
        // ========================================================================
        // 内部类型
        // ========================================================================

        /**
         * @brief 空闲连接条目，附带时间戳以便过期判定
         */
        struct IdleEntry
        {
            std::unique_ptr<DatabaseConnection>         connection;   ///< 数据库连接
            std::chrono::steady_clock::time_point        createdTime;  ///< 连接创建时刻
            std::chrono::steady_clock::time_point        returnedTime; ///< 归还时刻（空闲超时的起始点）
        };

        /**
         * @brief 异步等待者节点，用于协程级等待—唤醒
         */
        struct AsyncWaiter
        {
            std::coroutine_handle<>                     handle;       ///< 等待协程的句柄
            std::unique_ptr<DatabaseConnection>         result;       ///< 归还路径填充的连接
        };

        /**
         * @brief acquireAsync 内部使用的可等待对象
         *
         * @details 在 await_ready 中优先尝试非阻塞获取；
         *          在 await_suspend 中再尝试一次，若仍无则加入等待列表。
         *          析构时若尚未被唤醒，从等待列表中自行移除（防悬挂）。
         */
        class AcquireAwaiter
        {
        public:
            /**
             * @brief 构造等待体
             * @param pool 所属连接池
             */
            explicit AcquireAwaiter(ConnectionPool *pool) noexcept
                : m_pool(pool)
            {
            }

            /**
             * @brief 析构时自动从等待列表移除（若尚未被唤醒）
             */
            ~AcquireAwaiter();

            AcquireAwaiter(const AcquireAwaiter &)            = delete;
            AcquireAwaiter &operator=(const AcquireAwaiter &) = delete;

            /**
             * @brief 尝试非阻塞获取，成功则不挂起
             * @return true 已获取到连接，无需挂起
             */
            bool await_ready() noexcept;

            /**
             * @brief 挂起当前协程并加入等待列表
             * @param handle 当前协程句柄
             * @return true 挂起；false 在挂起前已获取到连接，不挂起
             */
            bool await_suspend(std::coroutine_handle<> handle) noexcept;

            /**
             * @brief 协程恢复后获取连接
             * @return PooledConnection 获取到的连接包装
             */
            PooledConnection await_resume() noexcept;

            // MSVC 需要显式友元声明以允许外围类访问嵌套类的私有成员
            friend class ConnectionPool;

        private:
            std::coroutine_handle<>               m_handle{nullptr}; ///< 等待协程的句柄（await_suspend 时保存）
            ConnectionPool                       *m_pool;            ///< 所属连接池
            std::unique_ptr<DatabaseConnection> m_result;    ///< 获取到的连接（await_ready 或 notify 时设置）
            bool                                m_inList{false}; ///< 是否已加入等待列表，用于析构时判断
        };

        friend class AcquireAwaiter;

        // ========================================================================
        // 内部方法
        // ========================================================================

        /**
         * @brief 尝试从空闲栈弹出连接（内部，不操作 activeCount）
         * @return std::unique_ptr<DatabaseConnection> 获取成功返回连接，否则空
         */
        std::unique_ptr<DatabaseConnection> tryAcquireInternal() noexcept;

        /**
         * @brief 创建新连接（调用工厂 + connect）
         * @return std::unique_ptr<DatabaseConnection> 创建成功且已连接时返回有效连接
         */
        std::unique_ptr<DatabaseConnection> createNewConnection() noexcept;

        /**
         * @brief 判断连接是否过期
         * @param entry 空闲连接条目
         * @return true 已过期，应关闭丢弃
         */
        [[nodiscard]] bool isEntryExpired(const IdleEntry &entry) const noexcept;

        /**
         * @brief 检查连接健康状态
         * @param connection 待检查的连接
         * @return true 连接健康可用
         */
        [[nodiscard]] static bool isConnectionHealthy(DatabaseConnection *connection) noexcept;

        /**
         * @brief 后台线程主循环：定期清理过期空闲连接
         * @param stopToken 停止令牌
         */
        void healthCheckLoop(std::stop_token stopToken);

        /**
         * @brief 尝试唤醒一个异步等待者
         * @param connection 归还的连接引用，成功唤醒时被移走，失败时保持有效
         * @return true 成功唤醒了一个等待者
         */
        bool notifyAsyncWaiter(std::unique_ptr<DatabaseConnection> &connection);

        /**
         * @brief 将等待者从异步等待列表移除
         * @param waiter 待移除的等待者指针
         */
        void removeAsyncWaiter(AcquireAwaiter *waiter) noexcept;

        // ========================================================================
        // 数据成员
        // ========================================================================

        std::function<std::unique_ptr<DatabaseConnection>()> m_factory; ///< 连接工厂，每次调用的返回值应是已 connect() 的状态
        PoolConfig                                          m_config;  ///< 连接池配置

        // ----- 空闲栈（受 m_mutex 保护） -----
        std::vector<IdleEntry> m_idleStack;                             ///< LIFO 空闲连接栈
        mutable std::mutex     m_mutex;                                 ///< 保护空闲栈及相关计数
        std::condition_variable m_cv;                                   ///< 条件变量：通知等待者有空闲连接

        // ----- 原子统计 -----
        std::atomic<std::size_t> m_activeCount{0};                      ///< 已取出未归还的连接数
        std::atomic<std::size_t> m_totalCreated{0};                     ///< 已创建的连接总数（含已被丢弃的）
        std::atomic<std::size_t> m_syncWaitingCount{0};                 ///< 同步等待者数量

        // ----- 异步等待列表（受 m_asyncMutex 保护） -----
        mutable std::mutex                        m_asyncMutex;         ///< 保护异步等待列表
        std::deque<AcquireAwaiter *>              m_asyncWaiters;       ///< 异步协程等待列表

        // ----- 连接创建时间追踪 -----
        // 用于在 returnConnection 时获知连接的原始创建时间，以正确设置 IdleEntry::createdTime
        mutable std::mutex                                                                   m_ctMapMutex;     ///< 保护创建时间映射表
        std::unordered_map<DatabaseConnection *, std::chrono::steady_clock::time_point>       m_creationTimeMap; ///< 连接指针 → 创建时刻

        // ----- 后台线程 -----
        std::jthread m_healthThread;                                    ///< 后台健康检查线程
    };

} // namespace AsynGyanis::Database
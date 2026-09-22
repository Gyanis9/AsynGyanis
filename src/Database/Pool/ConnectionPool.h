/**
 * @file ConnectionPool.h
 * @brief 高性能线程安全连接池
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 管理一组 DatabaseConnection，提供阻塞获取、非阻塞尝试、协程异步获取三种方式。
 *          空闲连接用 LIFO 栈（最新归还的最可能还在热点缓存）、懒惰创建、单锁保护；归还时做
 *          空闲/存活期过期判定，后台 jthread 定期驱逐，探活放在获取路径上（调用方拿到的一定是
 *          可用连接，归还路径保持最短）。m_mutex 的临界区内一律不做 disconnect：关闭底层连接是
 *          会阻塞的系统调用，留在锁内会把取出路径、统计读取与后台驱逐一起堵住（丢弃动作因此都
 *          先摘出、出锁再断）。所有公有方法均为线程安全。
 */
#pragma once

#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/PoolLiveness.h"
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
     * @details 连接生命周期由池管理，调用方通过 PooledConnection RAII 包装使用。
     *          acquire() 阻塞至多 acquireTimeoutMs 毫秒，tryAcquire() 非阻塞，acquireAsync() 由归还路径唤醒。
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
        explicit ConnectionPool(std::function<std::unique_ptr<DatabaseConnection>()> factory, const PoolConfig &config = PoolConfig{});

        /**
         * @brief 析构连接池
         *
         * @details 请求后台线程停止，等待其退出，然后关闭所有空闲连接。
         *          当前已取出的连接（PooledConnection）仍可正常使用直到归还，
         *          归还时由于池已析构，连接将被直接关闭。
         */
        ~ConnectionPool();

        // 连接池独占底层连接集合的所有权，禁止拷贝与移动
        ConnectionPool(const ConnectionPool &) = delete;

        ConnectionPool &operator=(const ConnectionPool &) = delete;

        ConnectionPool(ConnectionPool &&) = delete;

        ConnectionPool &operator=(ConnectionPool &&) = delete;

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
         * @details 有空闲连接（或未达上限）时立即返回，不涉及任何线程切换；否则当前协程挂起到
         *          等待列表，待连接归还时由归还方把恢复动作**投递回本方法给定的 EventLoop**，
         *          因此协程恢复后的代码仍运行在该事件循环线程上，与异步数据库执行器的约定一致。
         *
         * @param loop 恢复本协程用的事件循环；其 run() 必须正在运行（或即将运行），
         *             且对象生命周期要覆盖到协程完成之后
         * @return Core::Task<PooledConnection> 协程任务，co_await 后获得连接
         *
         * @note 提前销毁协程（Task 析构）是安全的：还在等待列表里时会自行移除；已被交接或
         *       超时唤醒、但恢复动作尚未执行时，那次恢复会变成空操作（见 AcquireAwaiter::ResumeTicket），
         *       不会有人去 resume 已经随帧释放的内存。
         * @note 池被 shutdown 时会以「空连接」唤醒全部等待者，且那一次**就地恢复**而不是
         *       投回事件循环：池都停摆了，投递到循环里可能永远不被执行，那会让协程永久挂起。
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
         * @brief 空闲连接条目，附带归还时刻以便空闲期过期判定
         * @details 建立时刻不在这里：它跟着连接本身（DatabaseConnection::establishedAt()），
         *          连接被借出期间池拿不到别的载体。
         */
        struct IdleEntry
        {
            std::unique_ptr<DatabaseConnection>   connection;   ///< 数据库连接
            std::chrono::steady_clock::time_point returnedTime; ///< 归还时刻（空闲超时的起始点）
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
             * @param completionLoop 协程恢复时要回到的事件循环，由 acquireAsync() 的调用方给出
             */
            AcquireAwaiter(ConnectionPool *pool, Core::EventLoop *completionLoop) noexcept :
                m_pool(pool), m_completionLoop(completionLoop), m_liveness(pool->livenessToken())
            {
            }

            /**
             * @brief 析构时自动从等待列表移除（若尚未被唤醒）
             */
            ~AcquireAwaiter();

            AcquireAwaiter(const AcquireAwaiter &) = delete;

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
            /**
             * @brief 恢复票据：把「恢复这次等待」与「等待器是否还活着」分开
             * @details 交接连接时池把恢复动作投回事件循环，而那一刻之后调用方随时可能销毁 Task
             *          （帧连同等待器一起析构）。池投出去的若是裸句柄，循环那边就会 resume 一块
             *          已释放的帧——那是释放后使用。票据由等待器与投递方共享：等待器析构时把里面的
             *          句柄清空，投递方执行时看到空句柄就什么都不做
             */
            struct ResumeTicket
            {
                std::atomic<std::coroutine_handle<>> handle{nullptr}; ///< 待恢复的协程；等待器析构后为空。**必须是原子的**：清空发生在等待器（任意线程）的析构里，而读取发生在投递回事件循环的 lambda 里，两者之间没有任何 happens-before（shared_ptr 的引用计数不建立它），用非原子字段就是数据竞争、甚至 resume 一块已释放的帧；取用一律 `exchange(nullptr)`，同一个句柄只允许被恢复一次

                /**
                 * @brief 取走句柄并恢复协程；已被取走（或等待器已析构）时空操作
                 * @note 三处投递点（交接、超时、池停摆）共用这一份「取走再恢复」，
                 *       任何一处改成先判后取都会退化成两次恢复同一个帧
                 * @note 非 const：exchange 会改原子本身，GCC 下 const 版本编不过（MSVC 放行）
                 */
                void resumeOnce() noexcept
                {
                    if (const std::coroutine_handle<> resumeHandle = handle.exchange(nullptr); resumeHandle != nullptr)
                    {
                        resumeHandle.resume();
                    }
                }
            };

            ConnectionPool *                    m_pool;            ///< 所属连接池
            Core::EventLoop *                   m_completionLoop;  ///< 恢复本协程的事件循环，恒非空
            std::shared_ptr<PoolLiveness>       m_liveness;        ///< 池的存活令牌（构造时取）：析构里的归还动作要经它判活，见 ~AcquireAwaiter
            std::unique_ptr<DatabaseConnection> m_result;          ///< 获取到的连接（await_ready 或 notify 时设置）
            bool                                m_inList{false};   ///< 是否已加入等待列表，用于析构时判断
            std::shared_ptr<ResumeTicket>       m_resumeTicket;    ///< 恢复票据（await_suspend 时创建）：析构时清空其中的句柄，投递回来的恢复动作因此失效
            std::chrono::steady_clock::time_point m_deadline{};    ///< 等待截止时刻（await_suspend 时按 acquireTimeoutMilliseconds 定下）：与同步 acquire() 同一上限，到点由后台线程以「空连接」唤醒
        };

        friend class AcquireAwaiter;

        // ========================================================================
        // 内部方法
        // ========================================================================

        /**
         * @brief 尝试取得一条可用连接：先取空闲栈，未达上限则新建（内部，不操作 activeCount）
         *
         * @details 同步 tryAcquire() 与异步等待体的快路径共用这一份判定，
         *          避免两条路径对「池未满时能否立刻拿到连接」给出相反答案。
         * @return std::unique_ptr<DatabaseConnection> 连接；空闲栈为空且已达上限时为空
         */
        std::unique_ptr<DatabaseConnection> tryAcquireOrCreateInternal() noexcept;

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
         * @brief 判断空闲条目是否过期（存活期或空闲期任一到达）
         * @param entry 空闲连接条目
         * @return true 已过期，应关闭丢弃
         */
        [[nodiscard]] bool isEntryExpired(const IdleEntry &entry) const noexcept;

        /**
         * @brief 判断一条连接相对 `now` 是否已超过最大存活期
         * @details 取出路径与归还路径共用这一份判定，避免两处对「存活期到没到」给出不同口径。
         * @param connection 待判的连接，不可为空
         * @param now 判定时刻（一次调用里只取一次时钟，不在本函数内部读）
         * @return true 已超过；`maximumLifetimeSeconds == 0` 视为「立即过期」，恒为 true
         */
        [[nodiscard]] bool isPastMaximumLifetime(const DatabaseConnection &connection,
                                                 std::chrono::steady_clock::time_point now) const noexcept;

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
        void healthCheckLoop(const std::stop_token& stopToken);

        /**
         * @brief 尝试唤醒一个异步等待者
         * @param connection 归还的连接引用，成功唤醒时被移走，失败时保持有效
         * @return true 成功唤醒了一个等待者
         */
        bool notifyAsyncWaiter(std::unique_ptr<DatabaseConnection> &connection);

        /**
         * @brief 将等待者从异步等待列表移除
         * @param waiter 待移除的等待者指针
         * @note 要求调用方已持有 m_asyncMutex：等待器析构要在一段锁里同时完成
         *       「摘表」与「取走交接结果」，不能在这个锁里再取同一把非递归锁
         */
        void removeAsyncWaiterLocked(AcquireAwaiter *waiter) noexcept;

        /**
         * @brief 把连接关闭（释放 unique_ptr 即断开），不退还名额
         * @param connection 待关闭的连接；为空时空操作
         * @note 池析构与「丢弃后立刻重建」两条路径用本方法：它们的名额由后续动作接手
         * @warning 内含一次 disconnect（会阻塞的系统调用）：除池自身析构（此刻已不再服务任何线程）
         *          之外，调用方不得持有 m_mutex
         */
        void closeTrackedConnection(std::unique_ptr<DatabaseConnection> connection) noexcept;

        /**
         * @brief 在 closeTrackedConnection 之外把连接占用的名额一并退还
         * @param connection 待丢弃的连接
         * @note 名额在取出（或预占）时就已记上，丢弃不还回去会让池永久少一个位置
         */
        void discardConnection(std::unique_ptr<DatabaseConnection> connection) noexcept;

        /**
         * @brief 池仍存活时归还连接：判活与归还调用在同一段令牌锁内完成
         * @param connection 待归还的连接，成功交接后被移走
         * @param liveness 池存活令牌，与池共享
         * @param countAsActive 是否先补记一次活跃取出（协程等待器交接时归还路径已减过活跃计数）
         * @return true 已交给池；false 池已停摆，连接留在调用方手里自行关闭
         */
        bool returnConnectionIfAlive(std::unique_ptr<DatabaseConnection> &connection, const std::shared_ptr<PoolLiveness> &liveness,
                                     bool countAsActive) noexcept;

        /**
         * @brief 唤醒已到截止时刻的异步等待者（以「空连接」收尾）
         * @details 由后台线程按秒节拍调用，语义与同步 acquire() 的超时一致；
         *          恢复投回各自的事件循环，不就地恢复（本函数不在那些循环的线程上）
         */
        void expireTimedOutWaiters() noexcept;

        /**
         * @brief 取存活令牌的副本（供 PooledConnection 归还时判活）
         * @return std::shared_ptr<PoolLiveness> 与池共享的令牌；池析构前会先置假
         */
        [[nodiscard]] std::shared_ptr<PoolLiveness> livenessToken() const noexcept;

        friend class PooledConnection;

        // ========================================================================
        // 数据成员
        // ========================================================================

        std::function<std::unique_ptr<DatabaseConnection>()> m_factory; ///< 连接工厂，每次调用的返回值应是已 connect() 的状态
        PoolConfig                                           m_config;  ///< 连接池配置

        std::shared_ptr<PoolLiveness> m_liveness{std::make_shared<PoolLiveness>()}; ///< 池存活令牌：析构一开始就置假并持锁到收尾结束，与之共享的 PooledConnection 归还时据此决定

        // ----- 空闲栈（受 m_mutex 保护） -----
        std::vector<IdleEntry>  m_idleStack;     ///< LIFO 空闲连接栈
        mutable std::mutex      m_mutex;         ///< 保护空闲栈及相关计数
        std::condition_variable m_idleCondition; ///< 条件变量：通知等待者有空闲连接
        std::atomic<bool> m_isShuttingDown{false}; ///< 池正在停摆：析构一置位，同步等待者的等待谓词随之成立，它们返回空连接后自减计数，析构等计数归零才继续销毁成员（否则等待者还睡在即将销毁的 m_idleCondition 上）

        // ----- 原子统计 -----
        std::atomic<std::size_t> m_activeCount{0};      ///< 已取出未归还的连接数
        std::atomic<std::size_t> m_totalCreated{0};     ///< 已创建的连接总数（含已被丢弃的）
        std::atomic<std::size_t> m_syncWaitingCount{0}; ///< 同步等待者数量

        // ----- 异步等待列表（受 m_asyncMutex 保护） -----
        mutable std::mutex           m_asyncMutex;   ///< 保护异步等待列表
        std::deque<AcquireAwaiter *> m_asyncWaiters; ///< 异步协程等待列表

        // ----- 后台线程 -----
        // 这两个必须声明在 m_healthThread 之前：成员按声明逆序销毁，jthread 的隐式 join 会先跑，
        // 而它要在这对同步原语还活着时才能把睡眠中的循环叫醒
        std::mutex m_healthWakeMutex;               ///< 只给健康线程的等待用，不保护任何数据
        std::condition_variable m_healthWakeCondition; ///< 停止请求的落点：让健康线程不必睡满一个分片

        std::jthread m_healthThread; ///< 后台健康检查线程
    };

} // namespace AsynGyanis::Database

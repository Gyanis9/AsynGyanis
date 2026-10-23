/**
 * @file ConnectionPoolHealth.cpp
 * @brief 连接池的健康检查、过期驱逐与配置换算校验
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 健康线程、空闲栈驱逐与借出前探针放在同一份翻译单元：三者共用「什么算过期」
 *          这一套判据。取出/归还的主路径留在 ConnectionPool.cpp。
 */

#include "Database/Pool/ConnectionPool.h"

#include "Base/Exception/InvalidArgumentException.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace AsynGyanis::Database
{
    bool ConnectionPool::probeConnection(DatabaseConnection &connection) noexcept
    {
        m_probeCount.fetch_add(1, std::memory_order_relaxed);
        return connection.probeAlive();
    }

    bool ConnectionPool::isPastMaximumLifetime(const DatabaseConnection &connection, const std::chrono::steady_clock::time_point now) const noexcept
    {
        // 0 视为「立即过期」：连接一归还就被丢弃，永不进空闲栈
        if (m_config.maximumLifetimeSeconds == 0)
        {
            return true;
        }

        const auto lifetimeSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - connection.establishedAt()).count();
        return static_cast<std::size_t>(lifetimeSeconds) >= m_config.maximumLifetimeSeconds;
    }

    bool ConnectionPool::isEntryExpired(const IdleEntry &entry) const noexcept
    {
        const auto now = std::chrono::steady_clock::now();

        if (isPastMaximumLifetime(*entry.connection, now))
        {
            return true;
        }

        // 空闲超时检查：0 表示不设空闲超时限制
        if (m_config.idleTimeoutSeconds > 0)
        {
            const auto idleSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - entry.returnedTime).count();
            if (static_cast<std::size_t>(idleSeconds) >= m_config.idleTimeoutSeconds)
            {
                return true;
            }
        }

        return false;
    }

    bool ConnectionPool::isConnectionHealthy(DatabaseConnection *connection) noexcept
    {
        if (connection == nullptr)
        {
            return false;
        }

        // 纯状态查询：各驱动的 isConnected() 都只读内部状态标志与句柄非空，
        // 刻意不发任何网络请求（无探活往返），因此在获取与归还路径上直接调用是安全的
        try
        {
            return connection->isConnected();
        } catch (const std::exception &failure)
        {
            // 各驱动的 isConnected() 按约定是纯状态查询、不该抛；真抛了说明驱动内部失序，
            // 这里按「不可用」处理是对的，但原因必须留痕，否则下一次排查只能从头猜
            LOG_ERROR_EXCEPTION(failure, "ConnectionPool: 探活查询抛出异常，本条连接按不可用处理（原因：{}）", failure.what());
            return false;
        } catch (...)
        {
            LOG_ERROR("ConnectionPool: 探活查询抛出异常（非标准异常），本条连接按不可用处理");
            return false;
        }
    }

    PoolConfig ConnectionPool::validateConfiguration(const PoolConfig &config)
    {
        // 只判「换算得过来」这一件事：健康检查间隔要乘 1000 变成毫秒片长，
        // 超过 7 天的取值乘完会越过 int64 之外，负数喂给 wait_for 就是每秒空转一轮
        if (config.healthCheckIntervalSeconds > kMaximumHealthCheckIntervalSeconds)
        {
            throw Base::InvalidArgumentException("ConnectionPool: healthCheckIntervalSeconds 不能超过 " + std::to_string(kMaximumHealthCheckIntervalSeconds) +
                                                 " 秒（7 天），当前 " + std::to_string(config.healthCheckIntervalSeconds) + " 秒；换算成毫秒会溢出，那条后台线程会退化成空转");
        }
        // 借用的等待时限同属「换算得过来」这一格，此前只判了间隔没判时限。它是无符号的 std::size_t，
        // 而 std::chrono::milliseconds 的 rep 是有符号 64 位：越过 2^63 的取值绕成负时长，截止时刻落在
        // 「现在之前」，两条等待路径（同步 acquire 与异步 await）当场判超时——配得越大的时限反倒一条都不等，
        // 借用者拿到空连接而队列里刚腾出的名额还没人看过；没绕过去的取值则把 now()+时限 这一步折溢出。
        // 上限按本平台把毫秒折进时钟刻度所能表达的量现算并取一半，另一半让给「现在这一刻」本身
        // （与 QUIC 那条 max_idle_timeout 同一手法：量不出来的那一档不是「更长」，是「没有意义」）。
        const std::size_t maximumTimeoutMilliseconds =
                static_cast<std::size_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::duration::max()).count() / 2U);
        if (config.acquireTimeoutMilliseconds > maximumTimeoutMilliseconds)
        {
            throw Base::InvalidArgumentException("ConnectionPool: acquireTimeoutMilliseconds 不能超过 " + std::to_string(maximumTimeoutMilliseconds) +
                                                 " 毫秒（本端时钟能表达的等待上限），当前 " + std::to_string(config.acquireTimeoutMilliseconds) +
                                                 "；越界的取值会绕成负的等待时限，借用者一条都不等就拿到空连接");
        }
        return config;
    }

    void ConnectionPool::healthCheckLoop(const std::stop_token &stopToken)
    {
        // 下限压到 1 秒（0 会让那条线程按秒自旋）；上限由构造时的 validateConfiguration 兜住，
        // 因此下面的换算不会溢出
        const auto interval = std::max(m_config.healthCheckIntervalSeconds, std::size_t{1});

        // 停止请求直接把本线程从等待里叫醒：jthread 的 join 因此不必等满当前那个 1 秒分片。
        // 回调随本函数返回而解除，所以它引用的 this 一直在有效期内
        const std::stop_callback wakeupOnStop(stopToken, [this] { m_healthWakeCondition.notify_all(); });

        while (!stopToken.stop_requested())
        {
            // 分段睡眠，每 1 秒醒一次：既检查停止标志，也推进异步等待者的截止时刻
            const auto            totalSleepMilliseconds  = static_cast<std::int64_t>(interval) * 1000;
            constexpr std::size_t kSleepChunkMilliseconds = 1000;

            auto remainingMilliseconds = totalSleepMilliseconds;
            while (remainingMilliseconds > 0 && !stopToken.stop_requested())
            {
                const auto chunk             = std::min(static_cast<int64_t>(kSleepChunkMilliseconds), remainingMilliseconds);
                bool       isExpiryRequested = false;
                {
                    std::unique_lock lock(m_healthWakeMutex);
                    // 谓词判定与 notify 共用这把锁，因此停止或取消落在「刚要进 wait_for 之前」也不会漏唤醒
                    m_healthWakeCondition.wait_for(lock, std::chrono::milliseconds(chunk),
                                                   [&stopToken, this] { return stopToken.stop_requested() || m_asyncExpiryRequested.load(std::memory_order_acquire); });
                    isExpiryRequested = m_asyncExpiryRequested.exchange(false, std::memory_order_acq_rel);
                }

                if (stopToken.stop_requested())
                {
                    break;
                }
                if (isExpiryRequested)
                {
                    // 取消请求把本线程提前叫醒：立即扫一遍等待表，不再等满这个分片
                    expireTimedOutWaiters();
                    continue;
                }
                remainingMilliseconds -= chunk;

                // 每秒一次：把等到截止时刻或已请求取消的异步等待者以「空连接」唤醒。
                // 放在这里而不是等下一个健康检查周期，是为了让异步超时的粒度与同步一致（秒级以内）
                expireTimedOutWaiters();
            }

            if (stopToken.stop_requested())
            {
                break;
            }

            // ---- 遍历空闲栈，驱逐过期连接 ----
            // 锁内只做「摘出与计数」；disconnect 可能走网络/系统调用，必须留到锁外，
            // 否则一次慢断开会让所有取出路径一起等在这把锁上
            std::vector<std::unique_ptr<DatabaseConnection>> expiredConnections;
            try
            {
                std::lock_guard lock(m_mutex);

                // 先要容量再动栈：push_back 在谓词里发生，分配失败会穿出线程入口，
                // 而 remove_if 已经搬动过的槽会留下空 connection——后续判定会解引用空指针
                expiredConnections.reserve(m_idleStack.size());

                auto removeBegin = std::ranges::remove_if(m_idleStack,
                                                          [this, &expiredConnections](IdleEntry &entry) -> bool
                                                          {
                                                              if (isEntryExpired(entry))
                                                              {
                                                                  expiredConnections.push_back(std::move(entry.connection));
                                                                  m_totalCreated.fetch_sub(1, std::memory_order_relaxed);
                                                                  m_totalDiscarded.fetch_add(1, std::memory_order_relaxed);
                                                                  return true;
                                                              }
                                                              return false;
                                                          })
                                           .begin();

                if (removeBegin != m_idleStack.end())
                {
                    m_idleStack.erase(removeBegin, m_idleStack.end());
                }
            } catch (const std::exception &failure)
            {
                LOG_ERROR_FMT("ConnectionPool: 驱逐过期连接时分配失败，本轮跳过（原因：{}）", failure.what());
            } catch (...)
            {
                LOG_ERROR("ConnectionPool: 驱逐过期连接时分配失败（非标准异常），本轮跳过");
            }
            // 驱逐同样腾出了名额：正在注册的等待者不在等待表里，靠世代号让它重走一轮建连。
            // 锁外断开：析构 unique_ptr 即关闭底层连接
            if (!expiredConnections.empty())
            {
                noteSlotFreedForRegisteringWaiters();
                wakeWaitersForFreedSlot();
            }
            expiredConnections.clear();
        }
    }
} // namespace AsynGyanis::Database

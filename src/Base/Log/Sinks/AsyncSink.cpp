/**
 * @file AsyncSink.cpp
 * @brief 异步日志输出目标（队列 + 后台消费线程）
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Log/Sinks/AsyncSink.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

namespace AsynGyanis::Base
{
    AsyncSink::AsyncSink(std::unique_ptr<LogSink> wrappedSink, const size_t queueSize, const OverflowPolicy policy) :
        m_wrappedSink(std::move(wrappedSink))
        , m_maximumQueueSize(queueSize)
        , m_overflowPolicy(policy)
    {
        m_workerThread = std::thread(&AsyncSink::workerLoop, this);
    }

    AsyncSink::~AsyncSink()
    {
        stop();
    }

    void AsyncSink::write(const LogEvent &event)
    {
        std::unique_lock lock(m_queueMutex);
        if (m_overflowPolicy == OverflowPolicy::Drop)
        {
            if (m_queue.size() >= m_maximumQueueSize)
            {
                // 丢弃新到事件并计数，供运维监控日志丢失规模
                // 事件从未入队，不计入待落地计数，否则 flush() 会等到永远无法满足的条件
                m_droppedEventCount.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            m_queue.push(event);
            ++m_pendingCount;
        } else if (m_overflowPolicy == OverflowPolicy::DropOldest)
        {
            if (m_queue.size() >= m_maximumQueueSize)
            {
                // 淘汰队首最旧事件，为最新日志腾出空间
                // 被淘汰的事件不会再被 worker 处理，需同步核销它的待落地计数
                m_queue.pop();
                --m_pendingCount;
                m_droppedEventCount.fetch_add(1, std::memory_order_relaxed);
            }
            m_queue.push(event);
            ++m_pendingCount;
        } else
        {
            m_queueCondition.wait(lock, [this]
            {
                return m_queue.size() < m_maximumQueueSize || !m_running.load();
            });
            if (m_running.load())
            {
                m_queue.push(event);
                ++m_pendingCount;
            }
        }
        lock.unlock();
        m_queueCondition.notify_one();
    }

    uint64_t AsyncSink::droppedEventCount() const noexcept
    {
        return m_droppedEventCount.load(std::memory_order_relaxed);
    }

    void AsyncSink::flush()
    {
        std::unique_lock lock(m_queueMutex);
        // 等待条件用「待落地数为 0」而非「队列为空」：worker 取出事件后队列即空，
        // 但下游 write 尚未返回，此时放行会让 flush() 在日志仍在途时提前返回
        m_flushCondition.wait(lock, [this]
        {
            return m_pendingCount == 0 || !m_running.load();
        });
        if (m_wrappedSink)
        {
            m_wrappedSink->flush();
        }
    }

    void AsyncSink::stop()
    {
        if (!m_running.exchange(false))
        {
            return;
        }
        m_queueCondition.notify_all();
        if (m_workerThread.joinable())
        {
            m_workerThread.join();
        }
        if (m_wrappedSink)
        {
            m_wrappedSink->flush();
        }
    }

    void AsyncSink::workerLoop()
    {
        // 取出队首事件并在锁外落地：下游 write 可能长时间阻塞（如磁盘 IO），
        // 持锁写出会卡住所有生产者。落地返回后才核销待落地计数
        const auto drainOneEvent = [this](std::unique_lock<std::mutex> &lock)
        {
            LogEvent event = std::move(m_queue.front());
            m_queue.pop();
            lock.unlock();
            try
            {
                if (m_wrappedSink)
                {
                    m_wrappedSink->write(event);
                }
            } catch (...)
            {
                // 防止单个 Sink 异常导致整个 worker 线程崩溃
            }
            lock.lock();
            // 队列出现空间，唤醒因队列满而阻塞的写入者
            m_queueCondition.notify_one();
            // 事件已交给下游，待落地计数归零时唤醒全部 flush 等待者
            if (--m_pendingCount == 0)
            {
                m_flushCondition.notify_all();
            }
        };

        while (m_running.load())
        {
            std::unique_lock lock(m_queueMutex);
            m_queueCondition.wait(lock, [this]
            {
                return !m_queue.empty() || !m_running.load();
            });
            while (!m_queue.empty())
            {
                drainOneEvent(lock);
            }
            m_flushCondition.notify_all();
        }
        // 停止后尽力排空队列中残留的事件
        {
            std::unique_lock lock(m_queueMutex);
            while (!m_queue.empty())
            {
                drainOneEvent(lock);
            }
            m_flushCondition.notify_all();
        }
    }
} // namespace AsynGyanis::Base

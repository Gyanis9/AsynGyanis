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
                m_droppedEventCount.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            m_queue.push(event);
        } else if (m_overflowPolicy == OverflowPolicy::DropOldest)
        {
            if (m_queue.size() >= m_maximumQueueSize)
            {
                // 淘汰队首最旧事件，为最新日志腾出空间
                m_queue.pop();
                m_droppedEventCount.fetch_add(1, std::memory_order_relaxed);
            }
            m_queue.push(event);
        } else
        {
            m_queueCondition.wait(lock, [this]
            {
                return m_queue.size() < m_maximumQueueSize || !m_running.load();
            });
            if (m_running.load())
            {
                m_queue.push(event);
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
        m_flushCondition.wait(lock, [this]
        {
            return m_queue.empty();
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
        while (m_running.load())
        {
            std::unique_lock lock(m_queueMutex);
            m_queueCondition.wait(lock, [this]
            {
                return !m_queue.empty() || !m_running.load();
            });
            while (!m_queue.empty())
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
            }
            m_flushCondition.notify_all();
        }
        // 停止后尽力排空队列中残留的事件
        std::unique_lock lock(m_queueMutex);
        while (!m_queue.empty())
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
            }
            lock.lock();
            m_queueCondition.notify_one();
        }
        m_flushCondition.notify_all();
    }
} // namespace AsynGyanis::Base

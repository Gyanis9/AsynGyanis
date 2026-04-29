#include "ThreadPool.h"

namespace Core
{
    ThreadPool::ThreadPool(const size_t threadCount, const unsigned queueDepth) :
        m_threadCount(threadCount > 0 ? threadCount : std::thread::hardware_concurrency()),
        m_queueDepth(queueDepth)
    {
        if (m_threadCount == 0)
            m_threadCount = 1;

        m_eventLoops.reserve(m_threadCount);
        for (size_t i = 0; i < m_threadCount; ++i)
        {
            m_eventLoops.push_back(std::make_unique<EventLoop>(m_queueDepth));
        }
    }

    ThreadPool::~ThreadPool()
    {
        stop();
    }

    void ThreadPool::start()
    {
        m_threads.reserve(m_threadCount);
        for (size_t i = 0; i < m_threadCount; ++i)
        {
            m_threads.emplace_back([this, i]()
            {
                m_eventLoops[i]->run();
            });
        }
    }

    void ThreadPool::stop()
    {
        for (auto &loop: m_eventLoops)
        {
            if (loop)
            {
                loop->stop();
            }
        }

        // jthread 析构时自动 join
        m_threads.clear();
    }

    size_t ThreadPool::threadCount() const noexcept
    {
        return m_threadCount;
    }

    EventLoop &ThreadPool::eventLoop(const size_t index) const
    {
        return *m_eventLoops.at(index);
    }

    Scheduler &ThreadPool::scheduler(const size_t index) const
    {
        return m_eventLoops.at(index)->scheduler();
    }

} // namespace Core

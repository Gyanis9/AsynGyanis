#include "Core/Coroutine/ThreadPool.h"

#include "Base/Log/LogMacros.h"

namespace AsynGyanis::Core
{
    ThreadPool::ThreadPool(const size_t threadCount) :
        m_threadCount(threadCount > 0 ? threadCount : std::thread::hardware_concurrency())
    {
        if (m_threadCount == 0)
        {
            m_threadCount = 1;
        }

        m_eventLoops.reserve(m_threadCount);
        for (size_t i = 0; i < m_threadCount; ++i)
        {
            m_eventLoops.push_back(std::make_unique<EventLoop>());
        }
    }

    ThreadPool::~ThreadPool()
    {
        stop();
    }

    void ThreadPool::start()
    {
        // 防止重复启动导致同一 EventLoop 被多线程并发运行
        if (!m_threads.empty())
            return;

        m_threads.reserve(m_threadCount);
        for (size_t i = 0; i < m_threadCount; ++i)
        {
            m_threads.emplace_back([this, i]()
            {
                // 事件循环里的异常会一路穿到线程入口：postRemote 的投递体抛异常时，循环按契约
                // 重抛，而线程体不接就是 std::terminate——整个进程连同在途请求一起没了，
                // 收尾也不会跑。这里兜住并如实记一条 ERROR，让该线程体面退出
                try
                {
                    m_eventLoops[i]->run();
                } catch (const std::exception &loopError)
                {
                    LOG_ERROR_FMT("ThreadPool: 工作线程 {} 的事件循环因异常退出：{}", i, loopError.what());
                } catch (...)
                {
                    LOG_ERROR_FMT("ThreadPool: 工作线程 {} 的事件循环因未知异常退出", i);
                }
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

        // std::jthread 的析构会 join，因此这里必须先把停止请求发给全部 EventLoop 再清空
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

}

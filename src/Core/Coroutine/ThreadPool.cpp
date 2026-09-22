#include "Core/Coroutine/ThreadPool.h"

#include "Base/Log/LogMacros.h"
#include "Platform/System/CpuAffinity.h"

namespace AsynGyanis::Core
{
    ThreadPool::ThreadPool(const size_t threadCount) :
        // 自动档按「本进程实际能跑到多少并行」定容，而不是宿主核数：容器里每条循环都自带一份
        // epoll 与定时器描述符，按宿主核数起会把内存、文件描述符和上下文切换一起拉满
        m_threadCount(threadCount > 0 ? threadCount : Platform::CpuAffinity::recommendedWorkerCount())
    {
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

    void ThreadPool::setThreadsPinnedToCores(const bool pinThreadsToCores) noexcept
    {
        // 线程已经起来就不会再走一遍绑核路径，此时改开关对它们无效：如实记一条 WARN，
        // 而不是把开关写进去让人以为生效了
        if (!m_threads.empty())
        {
            LOG_WARN("ThreadPool：线程已启动，绑核开关不生效；请在 start() 之前调用 setThreadsPinnedToCores()");
            return;
        }
        m_pinsThreadsToCores = pinThreadsToCores;
    }

    void ThreadPool::start()
    {
        // 防止重复启动导致同一 EventLoop 被多线程并发运行
        if (!m_threads.empty())
            return;

        m_threads.reserve(m_threadCount);
        // 可绑的核数按「本进程被允许的核」算而不是硬件核数：容器 cpuset 收窄过的机器上两者不等，
        // 按硬件核数绑就会撞上许可集合外的编号
        const size_t availableCoreCount = m_pinsThreadsToCores ? Platform::CpuAffinity::availableCoreCount() : 0;
        if (m_pinsThreadsToCores && availableCoreCount < m_threadCount)
        {
            LOG_WARN_FMT("ThreadPool：线程数 {} 多于可用逻辑核 {}，只有前 {} 个线程被绑核，其余保持可迁移",
                         m_threadCount, availableCoreCount, availableCoreCount);
        }
        for (size_t i = 0; i < m_threadCount; ++i)
        {
            m_threads.emplace_back([this, i, availableCoreCount]()
            {
                // 绑核要在各自线程体内做：亲和性是线程级属性，在启动线程里调只会绑到调用方
                if (m_pinsThreadsToCores && i < availableCoreCount)
                {
                    if (const auto pinResult = Platform::CpuAffinity::pinCurrentThreadToCore(i); !pinResult)
                    {
                        LOG_WARN_FMT("ThreadPool：第 {} 个工作线程绑核失败，该线程保持可迁移：{}", i, pinResult.error());
                    }
                }

                // 事件循环里的异常会一路穿到线程入口：postRemote 的投递体抛异常时，循环按契约
                // 重抛，而线程体不接就是 std::terminate——整个进程连同在途请求一起没了，
                // 收尾也不会跑。这里兜住并如实记一条 ERROR，让该线程体面退出
                try
                {
                    m_eventLoops[i]->run();
                } catch (const std::exception &loopError)
                {
                    LOG_ERROR_EXCEPTION(loopError, "ThreadPool: 工作线程 {} 的事件循环因异常退出：{}", i, loopError.what());
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

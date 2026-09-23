#include "Core/Coroutine/ThreadPool.h"

#include "Base/Exception/LogicException.h"
#include "Base/Log/LogMacros.h"
#include "Platform/System/CpuAffinity.h"

#include <algorithm>

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
        // 生命周期锁把 start() 与 stop() 串起来：两者改的是同一只 m_threads（emplace_back 对
        // clear），并发跑就是 vector 上的数据竞争。本池经由 IoContext::threadPool() 对外可见，
        // 示例就直接拿它 start()，因此这把锁得由池自己负责，不能指望调用方另外上一层锁
        const std::lock_guard lock(m_lifecycleMutex);

        // 防止重复启动导致同一 EventLoop 被多线程并发运行
        if (!m_threads.empty())
            return;

        // 停过的循环不能重跑：EventLoop 的停止请求是粘性的，再 run() 只会立刻返回。不换掉它们，
        // 重启就变成「线程照样起、threadCount() 照样报 N，但一条事件都不驱动」的假启动
        if (m_hasBeenStopped)
        {
            for (auto &loop: m_eventLoops)
            {
                loop = std::make_unique<EventLoop>();
            }
            m_hasBeenStopped = false;
        }

        m_threads.reserve(m_threadCount);
        m_workerThreadIds.reserve(m_threadCount);
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
            // 线程号在起出来当场记下：jthread 已被建好，get_id() 稳定，而读这张表的 stop() 持同一把锁
            m_workerThreadIds.push_back(m_threads.back().get_id());
        }
    }

    bool ThreadPool::isCurrentThreadWorker() const
    {
        const std::lock_guard lock(m_lifecycleMutex);
        const std::thread::id self = std::this_thread::get_id();
        return std::find(m_workerThreadIds.begin(), m_workerThreadIds.end(), self) != m_workerThreadIds.end();
    }

    void ThreadPool::stop()
    {
        // 自 join 先挡在改动任何状态之前：std::jthread 的析构会 join 自己，那是
        // resource_deadlock_would_occur 从析构里抛出来＝terminate。宁可抛一个可 catch 的用法错误
        if (isCurrentThreadWorker())
        {
            throw Base::LogicException("ThreadPool::stop() 不能从自己的工作线程上调用：它要 join 调用线程自身。"
                                        "工作线程要收尾整个运行时，请把这件事交给池外的线程（例如持有本对象的那条）");
        }

        const std::lock_guard lock(m_lifecycleMutex);
        for (auto &loop: m_eventLoops)
        {
            if (loop)
            {
                loop->stop();
            }
        }

        // std::jthread 的析构会 join，因此这里必须先把停止请求发给全部 EventLoop 再清空
        m_threads.clear();
        m_workerThreadIds.clear();
        // 记下这批循环已经用过：EventLoop 一个实例只跑一轮生命周期，下次 start() 要换新的
        m_hasBeenStopped = true;
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

#include "Core/EventLoop/IoContext.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/System/PlatformError.h"

#include <cstdio>

namespace AsynGyanis::Core
{
    IoContext::IoContext(const size_t threadCount) :
        m_threadPool(threadCount)
    {
        Platform::Socket::initialize();
    }

    IoContext::~IoContext()
    {
        // 先 stop()（它会 join 全部工作线程）再 finalize()：析构时可能仍有线程在执行
        // Socket 操作，若先做 Socket::finalize() 回收底层库，正在跑的 socket 调用就会
        // 落到已卸载的运行时上；线程全部退出后 socket 层才确定无人使用
        stop();
        Platform::Socket::finalize();
    }

    void IoContext::run()
    {
        {
            // 与 stop() 互斥：ThreadPool::start() 会向 m_threads 追加线程，
            // 若与 stop() 的 m_threads.clear() 并发执行即构成数据竞争
            std::lock_guard lock(m_mutex);
            // 已请求停止则不再启动线程池：此时再 spawn 的线程只会白白建好又销毁
            if (m_stopped)
            {
                return;
            }
            m_threadPool.start();
        }

        // 启动完成后才进入等待，且谓词读取 m_stopped，锁外的 stop() 不会丢失唤醒
        std::unique_lock lock(m_mutex);
        m_condition.wait(lock, [this]
        {
            return m_stopped;
        });
    }

    void IoContext::stop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopped = true;
        }
        m_condition.notify_all();
        // 线程池的停止与 join 放在锁外：worker 线程退出前可能仍需获取 m_mutex
        m_threadPool.stop();
    }

    ThreadPool &IoContext::threadPool() noexcept
    {
        return m_threadPool;
    }

    Scheduler &IoContext::mainScheduler() const
    {
        // 固定取 0 号工作线程而非随机挑一个：调用方（如跨线程回调、定时器）需要知道
        // 任务会落在哪个事件循环上，索引定型才能预期恢复它的线程
        return m_threadPool.scheduler(0);
    }

} // namespace AsynGyanis::Core

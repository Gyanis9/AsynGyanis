/**
 * @file IoContext.cpp
 * @brief 异步运行时主入口实现：线程池启停与阻塞等待停止
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

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
        return m_threadPool.scheduler(0);
    }

} // namespace AsynGyanis::Core

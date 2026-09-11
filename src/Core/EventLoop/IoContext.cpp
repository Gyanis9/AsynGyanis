/**
 * @file IoContext.cpp
 * @brief IO 上下文实现
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Core/EventLoop/IoContext.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/System/PlatformError.h"

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
        m_threadPool.start();

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

}

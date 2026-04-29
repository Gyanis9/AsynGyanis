#include "IoContext.h"

namespace Core
{
    IoContext::IoContext(const size_t threadCount, const unsigned queueDepth) :
        m_threadPool(threadCount, queueDepth)
    {
    }

    IoContext::~IoContext()
    {
        stop();
    }

    void IoContext::run()
    {
        m_threadPool.start();

        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this]
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
        m_cv.notify_all();
        m_threadPool.stop();
    }

    ThreadPool &IoContext::threadPool() noexcept
    {
        return m_threadPool;
    }

    Scheduler &IoContext::mainScheduler()
    {
        return m_threadPool.scheduler(0);
    }

}

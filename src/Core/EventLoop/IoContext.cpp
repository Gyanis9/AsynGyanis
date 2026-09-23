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
            // 停止标志的检查与 start() 必须连成同一次持锁：stop() 要先拿到同一把锁才能置标志，
            // 于是「整轮 stop() 插在检查与起线程之间」这条交错被排掉——它要么在进锁之前就置好标志
            // （这里直接不起线程），要么排到 start() 之后（那次 join 收尾的正是本函数起出来的池）。
            // 少了这道串行，两条线程撞在一起会留下没人收尾的池子
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
        // 线程池的停止与 join 放在 m_mutex 之外：工作线程调本函数时会在池内部自 join 判定上
        // 当场失败，而持锁 join 会让它先卡在锁上——那条线程正是被 join 的对象
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

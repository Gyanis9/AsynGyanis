#include "Core/Process/WorkerHeartbeat.h"

#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/ThreadPool.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/Platform.h"
#include "Platform/System/PlatformError.h"

#include <format>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 每拍写出的那一个字节：内容无意义，master 只数「有没有新字节」
        constexpr char kHeartbeatByte = '\x01';
    } // namespace

    WorkerHeartbeat::WorkerHeartbeat(ThreadPool &pool, const int heartbeatWriteDescriptor, const std::chrono::milliseconds pingInterval) noexcept :
        m_pool(pool), m_writeDescriptor(heartbeatWriteDescriptor), m_pingInterval(pingInterval), m_ticker(pool, [this]() { writePing(); })
    {
    }

    WorkerHeartbeat::~WorkerHeartbeat()
    {
        // 拆的是拍协程的帧：调用方必须已经 stop() 过线程池（工作线程已 join）。
        // 管道写端刻意不关：它归父进程传下来的那份继承，关它不是本对象的活
    }

    bool WorkerHeartbeat::arm()
    {
#if ASYN_PLATFORM_WIN32
        m_lastError = "Windows 没有 systemd 看门狗这条通道，worker 心跳不启用";
        return false;
#else
        if (m_ticker.isArmed())
        {
            return true;
        }
        if (m_writeDescriptor < 0)
        {
            m_lastError = "心跳管道的写端描述符不合法（master 没有传下来，或参数被错读）";
            return false;
        }
        if (m_pingInterval <= std::chrono::milliseconds::zero())
        {
            m_lastError = std::format("心跳拍间隔 {} 毫秒不是正数", m_pingInterval.count());
            return false;
        }

        m_lastError.clear();
        static_cast<void>(m_ticker.arm(m_pingInterval));

        LOG_INFO_FMT("WorkerHeartbeat: worker 心跳已挂上：每 {} 毫秒由 {} 条循环凑齐一轮写一拍（写端 fd {}）", m_pingInterval.count(), m_ticker.armedLoopCount(),
                     m_writeDescriptor);
        return true;
#endif
    }

    void WorkerHeartbeat::writePing()
    {
#if !ASYN_PLATFORM_WIN32
        // 计数在写之前落：这条读数要回答的是「本进程交出过几拍」，写失败也一样已经交出
        // （失败原因在下面那条一次性 WARN 里，不在这里）
        m_pingCount.fetch_add(1, std::memory_order_relaxed);

        if (Platform::FileDescriptor::write(m_writeDescriptor, &kHeartbeatByte, 1) == 1)
        {
            return;
        }

        const int errorCode = Platform::PlatformError::lastErrorCode();
        // 管道满（master 一时没读）不算事：字节攒着，master 读一次就全清了；被信号打断时
        // 一拍字节都没写进去，下一拍照常再写。其余错误（EPIPE：master 不在了）只报首条——
        // 每拍一条会把日志刷满，而这一档的后果由父死子亡机制处理
        if (errorCode != Platform::PlatformError::kWouldBlock && errorCode != Platform::PlatformError::kInterrupted &&
            !m_hasLoggedWriteFailure.exchange(true, std::memory_order_relaxed))
        {
            LOG_WARN_FMT("WorkerHeartbeat: 心跳写入失败（fd {}，平台错误码 {}）：master 可能已经不在，"
                         "本 worker 的收场交给父进程机制",
                         m_writeDescriptor, errorCode);
        }
#else
        static_cast<void>(kHeartbeatByte);
#endif
    }

    bool WorkerHeartbeat::isArmed() const noexcept
    {
        return m_ticker.isArmed();
    }

    std::uint64_t WorkerHeartbeat::pingCount() const noexcept
    {
        return m_pingCount.load(std::memory_order_relaxed);
    }

    std::size_t WorkerHeartbeat::armedLoopCount() const noexcept
    {
        return m_ticker.armedLoopCount();
    }

    const std::string &WorkerHeartbeat::lastError() const noexcept
    {
        return m_lastError;
    }
} // namespace AsynGyanis::Core

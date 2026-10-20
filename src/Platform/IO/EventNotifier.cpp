#include "Platform/IO/EventNotifier.h"

#include "Platform/IO/FileDescriptor.h"
#include "Platform/System/PlatformError.h"

#include <cstdint>

namespace AsynGyanis::Platform
{
    EventNotifier::EventNotifier()
    {
#if ASYN_PLATFORM_LINUX
        // eventfd 的读写共用同一个描述符
        const int fileDescriptor = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        m_readDescriptor         = fileDescriptor;
        m_writeDescriptor        = fileDescriptor;
#else
        int readDescriptor  = FileDescriptor::kInvalid;
        int writeDescriptor = FileDescriptor::kInvalid;
        if (FileDescriptor::createPair(readDescriptor, writeDescriptor))
        {
            m_readDescriptor  = readDescriptor;
            m_writeDescriptor = writeDescriptor;
        }
#endif
    }

    EventNotifier::~EventNotifier()
    {
        FileDescriptor::close(m_readDescriptor);
        if (m_writeDescriptor != m_readDescriptor)
        {
            // Linux 下 eventfd 两端同源，只关闭一次
            FileDescriptor::close(m_writeDescriptor);
        }
    }

    int EventNotifier::readDescriptor() const noexcept
    {
        return m_readDescriptor;
    }

    bool EventNotifier::isValid() const noexcept
    {
        return FileDescriptor::isValid(m_readDescriptor) && FileDescriptor::isValid(m_writeDescriptor);
    }

    bool EventNotifier::notify() const noexcept
    {
        if (!isValid())
        {
            // 构造期就无效的通知器是装配处已知的降级（见类注释的「空操作」约定），
            // 不是「本记唤醒没送出去」——报 false 只会把同一个事实在每条投递上重复告警
            return true;
        }

        // 合并唤醒：只有把标记从「无人待处理」翻成「已有待处理」的那一次才真正写描述符。
        // 后到的通知看到标记已置位就直接返回——它们要送达的信息是「队列里有活」，
        // 而这一次写成字节的唤醒已经足够让目标循环醒来查看队列
        if (m_wakeupPending.exchange(true, std::memory_order_acq_rel))
        {
            return true;
        }

        const auto writeWakeupMarker = [this]() -> ssize_t
        {
#if ASYN_PLATFORM_LINUX
            // eventfd 要求写入 sizeof(std::uint64_t) 字节，短写会返回 EINVAL
            constexpr std::uint64_t increment = 1;
            return FileDescriptor::write(m_writeDescriptor, &increment, sizeof(increment));
#else
            constexpr char kmarker = 1;
            return FileDescriptor::write(m_writeDescriptor, &kmarker, sizeof(kmarker));
#endif
        };

        ssize_t writtenBytes = writeWakeupMarker();
        // 被信号打断不算失败：重试一次即可等价完成（与 InotifyFileWatcher 对 EINTR 的处置同一口径）。
        // 错误码取 socket 空间：写路径在 Windows 上是 ::send（错误落 WSA 空间），lastErrorCode() 在那
        // 一侧恒读 errno、比较恒假；lastSocketErrorCode() 在 Linux 下返回的仍是 errno，两边同解
        if (writtenBytes < 0 && PlatformError::lastSocketErrorCode() == PlatformError::kInterrupted)
        {
            writtenBytes = writeWakeupMarker();
        }
        if (writtenBytes < 0)
        {
            // 写失败=这一记唤醒没送出去，而合并标记已置位：不复位它，后续 notify 会一路
            // 「已有人待处理」直接返回、目标循环可能永远睡在无限等待里（这才是真正的卡死形态）。
            // 复位让下一次 notify 重新尝试写；此刻之前被合并掉的几记没有第二次机会——它们要
            // 送达的只是「队列里有活」，队列本身没丢东西
            m_wakeupPending.store(false, std::memory_order_release);
            return false;
        }
        return true;
    }

    void EventNotifier::drain() const noexcept
    {
        if (!isValid())
        {
            return;
        }

        // 循环直到「清除标记」这一步发现它本来就是 false。这同时等价于两件事：
        //   1) 描述符里没有未读字节——清除之后置位的生产者一定会写字节，而它若先于本次
        //      清除置位，则本次 exchange 会返回 true 让我们再读一轮；
        //   2) 标记最终一定停在 false——否则后续通知会一直以为有人待处理而永不写字节，
        //      目标循环就会永远睡下去（丢唤醒）。
        // 光靠「把描述符读空」做不到第 2 点：生产者可能在读到空之后、清除之前置位并
        // 因合并而没写字节，此时标记必须仍被视为待处理
        do
        {
            flushDescriptor();
        } while (m_wakeupPending.exchange(false, std::memory_order_acq_rel));
    }

    void EventNotifier::flushDescriptor() const noexcept
    {
#if ASYN_PLATFORM_LINUX
        // eventfd 的一次 read 取走累计计数并把计数器清零，第二次读必然 EAGAIN——
        // 唤醒路径上每跨线程投递一次就要走这里，白付的那一次系统调用直接加在尾延迟上
        std::uint64_t pendingWakeups = 0;
        static_cast<void>(FileDescriptor::read(m_readDescriptor, &pendingWakeups, sizeof(pendingWakeups)));
#else
        // socket 对是字节流：每次通知落一个字节，合并之后仍可能有好几个，循环读到空
        char buffer[64];
        while (FileDescriptor::read(m_readDescriptor, buffer, sizeof(buffer)) > 0)
        {
        }
#endif
    }
} // namespace AsynGyanis::Platform

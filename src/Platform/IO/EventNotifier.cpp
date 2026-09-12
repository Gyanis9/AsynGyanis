#include "Platform/IO/EventNotifier.h"

#include "Platform/IO/FileDescriptor.h"

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

    void EventNotifier::notify() const noexcept
    {
        if (!isValid())
        {
            return;
        }

        // 合并唤醒：只有把标记从「无人待处理」翻成「已有待处理」的那一次才真正写描述符。
        // 后到的通知看到标记已置位就直接返回——它们要送达的信息是「队列里有活」，
        // 而这一次写成字节的唤醒已经足够让目标循环醒来查看队列
        if (m_wakeupPending.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }

#if ASYN_PLATFORM_LINUX
        // eventfd 要求写入 sizeof(std::uint64_t) 字节，短写会返回 EINVAL
        constexpr std::uint64_t increment = 1;
        FileDescriptor::write(m_writeDescriptor, &increment, sizeof(increment));
#else
        constexpr char kmarker = 1;
        FileDescriptor::write(m_writeDescriptor, &kmarker, sizeof(kmarker));
#endif
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
        //   2) 标记最终一定停在 false——否则后续通知会一直以为有人待处理而**永不写字节**，
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
        // 非阻塞描述符读到 0 或 -1 即表示已排空，循环覆盖多次 notify 合并的情况
        char buffer[64];
        while (FileDescriptor::read(m_readDescriptor, buffer, sizeof(buffer)) > 0)
        {
        }
    }
} // namespace AsynGyanis::Platform

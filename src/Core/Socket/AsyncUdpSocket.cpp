#include "Core/Socket/AsyncUdpSocket.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/SystemException.h"
#include "Platform/IO/DatagramSocket.h"
#include "Platform/System/PlatformError.h"

#include <string>
#include <utility>

namespace AsynGyanis::Core
{
    AsyncUdpSocket::AsyncUdpSocket(EventLoop &loop, Platform::DatagramSocket socket) : m_loop(&loop), m_socket(std::move(socket))
    {
    }

    AsyncUdpSocket::AsyncUdpSocket(AsyncUdpSocket &&other) noexcept : m_loop(other.m_loop), m_socket(std::move(other.m_socket)), m_watcher(std::move(other.m_watcher))
    {
    }

    AsyncUdpSocket &AsyncUdpSocket::operator=(AsyncUdpSocket &&other) noexcept
    {
        if (this != &other)
        {
            m_loop    = other.m_loop;
            m_socket  = std::move(other.m_socket);
            m_watcher = std::move(other.m_watcher);
        }
        return *this;
    }

    bool AsyncUdpSocket::isValid() const noexcept
    {
        return m_socket.isValid();
    }

    int AsyncUdpSocket::fileDescriptor() const noexcept
    {
        return m_socket.fileDescriptor();
    }

    Platform::SocketAddress AsyncUdpSocket::localAddress() const noexcept
    {
        return m_socket.localAddress();
    }

    IoWatcher *AsyncUdpSocket::ensureWatcher() const
    {
        if (m_watcher == nullptr && m_socket.isValid() && m_loop != nullptr)
        {
            // 与流式套接字同一手法：注册一次常驻，关注位在等待期间按需武装。
            // 挂在 const 方法上是因为「等就绪」不改动套接字本身，等待器自己管理登记状态
            m_watcher = std::make_unique<IoWatcher>(*m_loop, m_socket.fileDescriptor());
        }
        return m_watcher.get();
    }

    IoWatcher::Awaiter AsyncUdpSocket::waitReadable() const
    {
        // 与 AsyncSocket 同一条：拿不到注册对象说明描述符无效或已被关闭，等待没有意义，
        // 抛出可定位的中文原因，而不是让调用方在这里静默挂起
        IoWatcher *const watcher = ensureWatcher();
        if (watcher == nullptr)
        {
            throw Base::SystemException("等待数据报套接字可读失败：套接字无效或已关闭");
        }
        return watcher->waitReadable();
    }

    IoWatcher::Awaiter AsyncUdpSocket::waitWritable() const
    {
        IoWatcher *const watcher = ensureWatcher();
        if (watcher == nullptr)
        {
            throw Base::SystemException("等待数据报套接字可写失败：套接字无效或已关闭");
        }
        return watcher->waitWritable();
    }

    Task<AsyncUdpSocket::DatagramReceiveResult> AsyncUdpSocket::asyncReceiveFrom(void *const buffer, const std::size_t capacity)
    {
        // 本端自己就能判定的失败要挡在系统调用之前，并且要说清该怎么改：底层只回一个 EINVAL，
        // 顺着错误码翻译出来的文案既不指到「缓冲」这个真实起因，也带着一句无关的提示
        if (!m_socket.isValid())
        {
            throw Base::SystemException("数据报接收失败：套接字无效或已被移动走（本对象不再持有描述符）");
        }
        if (buffer == nullptr || capacity == 0)
        {
            throw Base::InvalidArgumentException("数据报接收失败：缓冲为空或容量为 0：连一条空报文也要至少一字节的空间，"
                                                 "请给出足够的缓冲容量");
        }

        Platform::SocketAddress peerAddress;
        while (true)
        {
            const ssize_t receivedByteCount = m_socket.receive(buffer, capacity, peerAddress);
            if (receivedByteCount >= 0)
            {
                // 0 是合法的空报文（对端确实发了一条零长数据报），不能当成「没收到」处理
                co_return DatagramReceiveResult{.receivedByteCount = receivedByteCount, .peerAddress = peerAddress};
            }

            const int errorCode = Platform::PlatformError::lastSocketErrorCode();
            if (errorCode == Platform::PlatformError::kWouldBlock)
            {
                // 同 AsyncSocket：等待失败即套接字已关闭，用 -1 交给调用方收手
                if (!co_await waitReadable())
                {
                    co_return DatagramReceiveResult{};
                }
                continue;
            }
            if (errorCode == Platform::PlatformError::kInterrupted)
            {
                continue;
            }
            // 见头文件里的 @note：这里抛出去等于把调用方的循环静默干掉，故按「-1 + 错误码」交出，
            // 由调用方决定是继续读还是收手（它才知道自己是不是已经关了套接字）
            co_return DatagramReceiveResult{.receivedByteCount = -1, .peerAddress = Platform::SocketAddress{}, .socketErrorCode = errorCode};
        }
    }

    Task<AsyncUdpSocket::DatagramBatchReceiveResult> AsyncUdpSocket::asyncReceiveBatch(Platform::DatagramSocket::BatchSlot *const slots, const std::size_t slotCount)
    {
        // 与 asyncReceiveFrom 同一组前置判定：本端自己能决定的失败不等系统调用去报
        if (!m_socket.isValid())
        {
            throw Base::SystemException("数据报批次接收失败：套接字无效或已被移动走（本对象不再持有描述符）");
        }
        if (slots == nullptr || slotCount == 0)
        {
            throw Base::InvalidArgumentException("数据报批次接收失败：槽位数组为空或条数为 0：批次至少要有一个带好缓冲与容量的槽位");
        }

        while (true)
        {
            const ssize_t deliveredCount = m_socket.receiveBatch(slots, slotCount);
            if (deliveredCount > 0)
            {
                co_return DatagramBatchReceiveResult{.receivedDatagramCount = static_cast<std::size_t>(deliveredCount)};
            }
            if (deliveredCount == 0)
            {
                // 「此刻没有可读的报文」不算错误：等下一次可读再来，别让调用方拿着 0 条空转
                if (!co_await waitReadable())
                {
                    co_return DatagramBatchReceiveResult{};
                }
                continue;
            }

            const int errorCode = Platform::PlatformError::lastSocketErrorCode();
            if (errorCode == Platform::PlatformError::kWouldBlock)
            {
                // 平台层没把它折成 0 条的那一侧（Windows 的逐条退化档）走这里，语义同「等可读再来」
                if (!co_await waitReadable())
                {
                    co_return DatagramBatchReceiveResult{};
                }
                continue;
            }
            if (errorCode == Platform::PlatformError::kInterrupted)
            {
                continue;
            }
            // 见 asyncReceiveFrom 的那条 @note：抛出去会让正在 await 的监听循环当场消失，
            // 一个消失的对端就能让整台服务器不再接受任何来源，因此按「0 条 + 错误码」交出
            co_return DatagramBatchReceiveResult{.receivedDatagramCount = 0, .socketErrorCode = errorCode};
        }
    }

    Task<AsyncUdpSocket::DatagramBatchSendResult> AsyncUdpSocket::asyncSendBatch(const Platform::DatagramSocket::BatchSendItem *const items, const std::size_t itemCount)
    {
        if (!m_socket.isValid())
        {
            throw Base::SystemException("数据报批次发送失败：套接字无效或已被移动走（本对象不再持有描述符）");
        }
        if (items == nullptr || itemCount == 0)
        {
            throw Base::InvalidArgumentException("数据报批次发送失败：条目数组为空或条数为 0：批次至少要有一条带地址、缓冲与长度的报文");
        }

        std::size_t sentDatagramCount = 0;
        while (sentDatagramCount < itemCount)
        {
            const ssize_t batchSentCount = m_socket.sendBatch(items + sentDatagramCount, itemCount - sentDatagramCount);
            if (batchSentCount > 0)
            {
                sentDatagramCount += static_cast<std::size_t>(batchSentCount);
                continue;
            }

            const int errorCode = Platform::PlatformError::lastSocketErrorCode();
            if (batchSentCount == 0 && errorCode == Platform::PlatformError::kWouldBlock)
            {
                // 发送缓冲暂时放不下：等可写，然后从**没交出的那一条**接着发。已交出的绝不重发——
                // 数据报没有「部分写出」，重发就是让对端收到两条同样的报文
                if (!co_await waitWritable())
                {
                    co_return DatagramBatchSendResult{.sentDatagramCount = sentDatagramCount, .isComplete = false, .socketErrorCode = 0};
                }
                continue;
            }
            if (batchSentCount < 0 && errorCode == Platform::PlatformError::kInterrupted)
            {
                continue;
            }
            co_return DatagramBatchSendResult{.sentDatagramCount = sentDatagramCount, .isComplete = false, .socketErrorCode = errorCode};
        }

        co_return DatagramBatchSendResult{.sentDatagramCount = sentDatagramCount, .isComplete = true};
    }

    Task<ssize_t> AsyncUdpSocket::asyncSendTo(const Platform::SocketAddress peerAddress, const void *const buffer, const std::size_t length)
    {
        // 同 asyncReceiveFrom：超限与空缓冲在这一层就报出可操作的原文，不等底层回 EINVAL
        if (!m_socket.isValid())
        {
            throw Base::SystemException("数据报发送失败：套接字无效或已被移动走（本对象不再持有描述符）");
        }
        if (buffer == nullptr)
        {
            throw Base::InvalidArgumentException("数据报发送失败：待发缓冲为空（长度为 0 时也要给出一个有效地址）");
        }
        if (length > Platform::DatagramSocket::kMaximumDatagramBytes)
        {
            throw Base::InvalidArgumentException("数据报发送失败：单条报文 " + std::to_string(length) + " 字节超过上限 " +
                                                 std::to_string(Platform::DatagramSocket::kMaximumDatagramBytes) +
                                                 " 字节：数据报按整条交付、不会被内核切开，请自行分片或改用流式套接字");
        }

        while (true)
        {
            const ssize_t sentByteCount = m_socket.send(peerAddress, buffer, length);
            if (sentByteCount >= 0)
            {
                // 数据报不会部分写出：返回长度即整条已交给内核。真出现短写说明平台语义与预期不符，
                // 当场报出来比让上层以为「发出去了」安全
                if (static_cast<std::size_t>(sentByteCount) != length)
                {
                    throw Base::SystemException("数据报发送失败：内核只接下了 " + std::to_string(sentByteCount) + " / " + std::to_string(length) +
                                                " 字节，数据报不该部分写出（请检查底层实现）");
                }
                co_return sentByteCount;
            }

            const int errorCode = Platform::PlatformError::lastSocketErrorCode();
            if (errorCode == Platform::PlatformError::kWouldBlock)
            {
                // 发送缓冲暂时放不下：等可写后整条重发（数据报不会被内核切开，重发是唯一的续法）
                if (!co_await waitWritable())
                {
                    co_return -1;
                }
                continue;
            }
            if (errorCode == Platform::PlatformError::kInterrupted)
            {
                continue;
            }
            throw Base::SystemException("数据报发送失败：" + Platform::PlatformError::message(errorCode));
        }
    }

    void AsyncUdpSocket::close() noexcept
    {
        // 顺序不能反：销毁注册对象会把仍挂在上面的等待协程唤醒（等到的结果是「注册已失效」），
        // 而关掉描述符本身不会让 epoll/IOCP 的等待者醒来
        m_watcher.reset();
        m_socket.close();
    }
} // namespace AsynGyanis::Core

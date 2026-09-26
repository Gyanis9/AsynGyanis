#include "Core/Socket/AsyncUdpSocket.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/SystemException.h"
#include "Platform/IO/DatagramSocket.h"
#include "Platform/System/PlatformError.h"

#include <string>
#include <utility>

namespace AsynGyanis::Core
{
    AsyncUdpSocket::AsyncUdpSocket(EventLoop &loop, Platform::DatagramSocket socket) :
        m_loop(&loop), m_socket(std::move(socket))
    {
    }

    AsyncUdpSocket::AsyncUdpSocket(AsyncUdpSocket &&other) noexcept :
        m_loop(other.m_loop), m_socket(std::move(other.m_socket)), m_watcher(std::move(other.m_watcher))
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
            throw Base::SystemException("数据报接收失败：" + Platform::PlatformError::message(errorCode) +
                                "（对端不可达一类错误在报文层面上不改变本端状态，但这里按硬失败上报，"
                                "以免把「收不到」静默成「没有报文」）");
        }
    }

    Task<ssize_t> AsyncUdpSocket::asyncSendTo(const Platform::SocketAddress peerAddress, const void *const buffer,
                                              const std::size_t length)
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
            throw Base::InvalidArgumentException(
                    "数据报发送失败：单条报文 " + std::to_string(length) + " 字节超过上限 "
                    + std::to_string(Platform::DatagramSocket::kMaximumDatagramBytes)
                    + " 字节：数据报按整条交付、不会被内核切开，请自行分片或改用流式套接字");
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
                    throw Base::SystemException("数据报发送失败：内核只接下了 " + std::to_string(sentByteCount) + " / " +
                                        std::to_string(length) + " 字节，数据报不该部分写出（请检查底层实现）");
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

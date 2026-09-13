#include "Core/Socket/AsyncSocket.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoWatcher.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Socket/VectoredSendCursor.h"
#include "Base/Exception/SystemException.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/System/PlatformError.h"

#include <cerrno>
#include <limits>
#include <string>
#include <system_error>

namespace AsynGyanis::Core
{
    namespace
    {
        /**
         * @brief 取最近一次 socket 系统调用的失败码
         * @details winsock 失败**不写 errno**，而 SystemException 的隐式错误码构造函数读的正是 errno，
         *          于是文本里带的是与本次失败无关的陈旧值（实测发送失败报成「[112] There is not
         *          enough space on the disk」），真正的失败码反而看不到：socket 调用失败必须显式传码。
         * @return std::error_code socket 空间的错误码（描述文本由 std::system_category 提供）
         */
        std::error_code lastSocketError()
        {
            // 读取时机必须在失败之后、其它可能覆盖它的调用之前
            return {Platform::PlatformError::lastSocketErrorCode(), std::system_category()};
        }

        /**
         * @brief 本端套接字被关闭时用于收尾等待者的错误码
         * @details 这类失败由框架自己合成，没有底层系统调用可读：映射成本端中止连接，
         *          比让 SystemException 去读一个不存在的 errno 更能说明发生了什么
         * @return std::error_code 本端中止连接的错误码
         */
        std::error_code localSocketClosedError()
        {
            return {Platform::PlatformError::kConnectionAborted, std::system_category()};
        }
    } // namespace

    AsyncSocket::AsyncSocket(EventLoop &loop, const int fileDescriptor) :
        m_loop(loop), m_fileDescriptor(fileDescriptor)
    {
        if (m_fileDescriptor >= 0)
        {
            setNonBlocking();

            // 常驻注册：注册一次覆盖整个描述符生命周期，此后每次等待都不再有 ADD/DEL 往返；
            // 关注位由注册对象在等待期间按需武装（见 IoWatcher 的说明：Windows 侧 wepoll
            // 只有水平触发，长期武装一个「几乎总是就绪」的方向会让事件循环空转）
            m_watcher = std::make_unique<IoWatcher>(loop, m_fileDescriptor);
        }
    }

    AsyncSocket::~AsyncSocket()
    {
        close();
    }

    AsyncSocket::AsyncSocket(AsyncSocket &&other) noexcept :
        m_loop(other.m_loop), m_fileDescriptor(std::exchange(other.m_fileDescriptor, -1)),
        m_watcher(std::move(other.m_watcher))
    {
    }

    AsyncSocket &AsyncSocket::operator=(AsyncSocket &&other) noexcept
    {
        if (this != &other)
        {
            close();
            m_fileDescriptor = std::exchange(other.m_fileDescriptor, -1);
            // 注册对象随指针转移：它在堆上，epoll 里记的地址因此保持不变
            m_watcher = std::move(other.m_watcher);
        }
        return *this;
    }

    AsyncSocket AsyncSocket::create(EventLoop &loop, const int domain, const int type)
    {
#if ASYN_PLATFORM_WIN32
        const int fileDescriptor = ::socket(domain, type, 0);
#else
        const int fileDescriptor = ::socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#endif
        if (fileDescriptor < 0)
        {
            throw Base::SystemException("创建套接字失败", lastSocketError());
        }

#if ASYN_PLATFORM_WIN32
        // Windows 没有 SOCK_NONBLOCK，只能在创建后补设非阻塞
        Platform::FileDescriptor::setNonBlocking(fileDescriptor);
#endif

        if (type == SOCK_STREAM)
        {
            // 流式套接字默认关掉 Nagle：框架承载的是小包请求/响应，攒包会明显抬高首字节延迟
            [[maybe_unused]] const bool isNoDelaySet = Platform::Socket::setNoDelay(fileDescriptor);
        }

        return AsyncSocket(loop, fileDescriptor);
    }

    bool AsyncSocket::bind(const sockaddr *const address, const socklen_t addressLength) const
    {
        // 统一开启地址复用：服务重启时上一代连接的 TIME_WAIT 会占住端口
        [[maybe_unused]] const bool isReuseAddressSet = Platform::Socket::setReuseAddress(m_fileDescriptor);
        return ::bind(m_fileDescriptor, address, addressLength) == 0;
    }

    bool AsyncSocket::bind(const InetAddress &address) const
    {
        return bind(address.nativeAddress(), address.nativeAddressLength());
    }

    bool AsyncSocket::listen(const int backlog) const
    {
        return ::listen(m_fileDescriptor, backlog) == 0;
    }

    Task<> AsyncSocket::asyncConnect(const sockaddr *const address, const socklen_t addressLength) const
    {
        if (const int result = ::connect(m_fileDescriptor, address, addressLength); result == 0)
        {
            co_return;
        } else if (Platform::PlatformError::lastSocketErrorCode() != Platform::PlatformError::kInProgress)
        {
            throw Base::SystemException("发起连接失败", lastSocketError());
        }

        // 非阻塞 connect 的完成由可写事件通知；等待失败（套接字被关闭）时不再重试
        if (!co_await waitWritable())
        {
            throw Base::SystemException("等待连接完成期间套接字被关闭", localSocketClosedError());
        }

        // 非阻塞 connect 完成后靠 SO_ERROR 判定成败，该读取由 Platform 统一封装
        if (const int pendingError = Platform::Socket::takePendingError(m_fileDescriptor); pendingError != 0)
        {
            throw Base::SystemException("连接对端失败", std::error_code(pendingError, std::system_category()));
        }
        co_return;
    }

    Task<> AsyncSocket::asyncConnect(const InetAddress address) const
    {
        // 按值接收的理由见头文件：惰性协程到首次 resume 才读入参，按引用会悬垂。
        // 这里把值转交给原生地址重载，地址长度在拷贝期间始终有效
        co_return co_await asyncConnect(address.nativeAddress(), address.nativeAddressLength());
    }

    Task<ssize_t> AsyncSocket::asyncReceive(void *const buffer, const size_t length) const
    {
        if (length == 0)
        {
            co_return 0;
        }

        // recv 的长度形参是 int（Windows 上就是 int），超限会被静默窄化成一个可疑的负数，
        // 因此宁可当场失败也不让底层收到一个已被改写过的长度
        if (length > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            throw Base::SystemException("单次接收长度超过上限（" + std::to_string(length) +
                                        " 字节 > INT_MAX）：底层 recv 的长度形参是 int，"
                                        "超限会被静默窄化；请把数据分成多次接收");
        }

        while (true)
        {
            const ssize_t receivedBytes =
                    ::recv(m_fileDescriptor, static_cast<char *>(buffer), static_cast<int>(length), MSG_NOSIGNAL);
            if (receivedBytes > 0)
                co_return receivedBytes;
            if (receivedBytes == 0)
                co_return 0;
            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kWouldBlock)
            {
                // 等待失败说明套接字已被关闭：继续重试只会拿到 EBADF，直接以错误结束
                if (!co_await waitReadable())
                {
                    throw Base::SystemException("接收数据失败：等待可读期间套接字被关闭", localSocketClosedError());
                }
                continue;
            }
            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kInterrupted)
                continue;
            throw Base::SystemException("接收数据失败", lastSocketError());
        }
    }

    Task<ssize_t> AsyncSocket::asyncSend(const void *const buffer, const size_t length) const
    {
        if (length == 0)
        {
            co_return 0;
        }

        // 同 asyncReceive：先把会静默窄化的长度挡在底层 C API 之外
        if (length > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            throw Base::SystemException("单次发送长度超过上限（" + std::to_string(length) +
                                        " 字节 > INT_MAX）：底层 send 的长度形参是 int，"
                                        "超限会被静默窄化；请把数据分成多次发送");
        }

        while (true)
        {
            const ssize_t sentBytes =
                    ::send(m_fileDescriptor, static_cast<const char *>(buffer), static_cast<int>(length), MSG_NOSIGNAL);
            if (sentBytes > 0)
                co_return sentBytes;
            // send 返回 0 说明对端已关闭：这条路径下 errno 未被设置，
            // 因此以 -1 作为可判定的返回值，让调用方不必去读一个没有意义的 errno
            if (sentBytes == 0)
                co_return -1;
            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kWouldBlock)
            {
                // 同 asyncReceive：等待失败即套接字已关闭，不再重试
                if (!co_await waitWritable())
                {
                    throw Base::SystemException("发送数据失败：等待可写期间套接字被关闭", localSocketClosedError());
                }
                continue;
            }
            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kInterrupted)
                continue;
            throw Base::SystemException("发送数据失败", lastSocketError());
        }
    }

    Task<ssize_t> AsyncSocket::asyncSendVectored(const Platform::Socket::WriteBuffer *const buffers, const std::size_t bufferCount) const
    {
        // 段数与空数组属于调用方契约：静默拆分会让「一次系统调用」的前提悄悄失效，
        // 静默补齐零段则会让调用方以为数据发出去了，两者都必须当场失败
        if (buffers == nullptr || bufferCount == 0)
        {
            throw Base::SystemException("聚合发送失败：段数组为空");
        }
        if (bufferCount > Platform::Socket::kMaximumVectorCount)
        {
            throw Base::SystemException("聚合发送失败：段数 " + std::to_string(bufferCount) + " 超过平台上限 " +
                                        std::to_string(Platform::Socket::kMaximumVectorCount) +
                                        "（Windows 的 WSASend 最多 16 段），请先把相邻小段合并后再发送");
        }

        // 游标负责「部分写之后从哪继续、跨段怎么推进」这段最容易出错的账目；
        // 它单独成类并被用例直接覆盖——回环上很难自然触发部分写，可它一旦写错是静默的数据错位
        detail::VectoredSendCursor cursor(buffers, bufferCount);

        while (!cursor.isFinished())
        {
            Platform::Socket::WriteBuffer pending[Platform::Socket::kMaximumVectorCount]{};
            const std::size_t pendingCount = cursor.snapshotPending(pending, Platform::Socket::kMaximumVectorCount);
            if (pendingCount == 0)
            {
                // 游标说没发完，却拼不出任何待发段：段数组内部不一致（长度之和与游标对不上），
                // 继续下去只会空转，当场报错比静默死循环好
                throw Base::SystemException("聚合发送失败：段数组长度与游标不一致，无法拼出待发段");
            }

            const ssize_t sentBytes = Platform::Socket::writeVectored(m_fileDescriptor, pending, pendingCount);
            if (sentBytes > 0)
            {
                cursor.advance(static_cast<std::size_t>(sentBytes));
                continue;
            }

            // 与 asyncSend 一致：底层返回 0 说明对端已关闭，errno 未被设置，以 -1 作为可判定的返回值
            if (sentBytes == 0)
            {
                co_return -1;
            }

            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kWouldBlock)
            {
                if (!co_await waitWritable())
                {
                    throw Base::SystemException("聚合发送失败：等待可写期间套接字被关闭", localSocketClosedError());
                }
                continue;
            }
            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kInterrupted)
            {
                continue;
            }
            throw Base::SystemException("聚合发送数据失败", lastSocketError());
        }
        co_return static_cast<ssize_t>(cursor.sentLength());
    }

    int AsyncSocket::releaseFileDescriptor() noexcept
    {
        // 注册对象必须已经清空：它绑定的是本对象的循环，跟着描述符搬过去只会指向错误的循环
        m_watcher.reset();

        const int releasedFileDescriptor = m_fileDescriptor;
        m_fileDescriptor                 = -1;
        return releasedFileDescriptor;
    }

    void AsyncSocket::close()
    {
        if (m_fileDescriptor >= 0)
        {
            // 先销毁注册对象再关描述符：反注册在描述符仍然有效时执行才最稳妥，
            // 而且它会唤醒仍挂在上面的等待协程——关闭描述符并不会唤醒 epoll 的等待者，
            // 少了这一步，正在等待可读/可写的协程会永久挂起
            m_watcher.reset();

            ::shutdown(m_fileDescriptor, SHUT_RDWR);
            Platform::FileDescriptor::close(m_fileDescriptor);
            m_fileDescriptor = -1;
        }
    }

    int AsyncSocket::fileDescriptor() const noexcept
    {
        return m_fileDescriptor;
    }

    IoWatcher::Awaiter AsyncSocket::waitReadable() const
    {
        // 没有注册对象意味着描述符无效或已被关闭：等待没有意义，直接抛出可定位的中文原因
        if (m_watcher == nullptr)
        {
            throw Base::SystemException("等待套接字可读失败：套接字无效或已关闭");
        }
        return m_watcher->waitReadable();
    }

    IoWatcher::Awaiter AsyncSocket::waitWritable() const
    {
        if (m_watcher == nullptr)
        {
            throw Base::SystemException("等待套接字可写失败：套接字无效或已关闭");
        }
        return m_watcher->waitWritable();
    }

    void AsyncSocket::setNonBlocking() const
    {
        if (m_fileDescriptor >= 0)
        {
            Platform::FileDescriptor::setNonBlocking(m_fileDescriptor);
        }
    }

    bool AsyncSocket::setSockOpt(const int level, const int opt, const void *const value, const socklen_t length) const
    {
        return setsockopt(m_fileDescriptor, level, opt, static_cast<const char *>(value), length) == 0;
    }

    InetAddress AsyncSocket::remoteAddress() const
    {
        sockaddr_storage address{};
        socklen_t        addressLength = sizeof(address);
        if (getpeername(m_fileDescriptor, reinterpret_cast<sockaddr *>(&address), &addressLength) != 0)
        {
            throw Base::SystemException("获取对端地址失败");
        }
        return InetAddress(address, addressLength);
    }

    InetAddress AsyncSocket::localAddress() const
    {
        sockaddr_storage address{};
        socklen_t        addressLength = sizeof(address);
        if (getsockname(m_fileDescriptor, reinterpret_cast<sockaddr *>(&address), &addressLength) != 0)
        {
            throw Base::SystemException("获取本地地址失败");
        }
        return InetAddress(address, addressLength);
    }
}

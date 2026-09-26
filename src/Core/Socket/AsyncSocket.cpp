#include "Core/Socket/AsyncSocket.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/SystemException.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoWatcher.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Socket/VectoredSendCursor.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/System/PlatformError.h"

#include <array>
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

        /// 收口时丢弃入站字节的单轮缓冲大小
        constexpr std::size_t kDiscardChunkBytes = 8 * 1024;

        /// 收口时丢弃入站字节的轮数上限（合计 64 KiB）
        constexpr int kDiscardRoundLimit = 8;

        /**
         * @brief 丢弃内核接收队列里本端再也不会读的字节
         * @details 描述符必须是已非阻塞的（构造与 create() 都保证了这点）：每轮 recv 要么立刻取走
         *          一段、要么以 WOULD_BLOCK 收场，因此整段清理不会挂住事件循环线程。轮数封顶是
         *          必需的——对端持续洪泛时，无界地读下去等于让一条坏连接拖慢整个循环。
         * @param fileDescriptor 待清理的套接字描述符
         */
        void discardUnreadInboundData(const int fileDescriptor) noexcept
        {
            // 这块缓冲只作为 recv 的落点，内容从不被读，因此刻意不初始化：
            // 带 {} 会让每次 close() 先 memset 掉 8 KiB，而绝大多数连接收口时接收队列本来就是空的
            // （第一轮 recv 直接以 WOULD_BLOCK 收场）
            std::array<char, kDiscardChunkBytes> scratchBuffer;
            for (int roundCount = 0; roundCount < kDiscardRoundLimit; ++roundCount)
            {
                const ssize_t receivedBytes = ::recv(fileDescriptor, scratchBuffer.data(), static_cast<int>(scratchBuffer.size()), MSG_DONTWAIT);
                // 0 是对端已收口、-1 配 WOULD_BLOCK 是队列已空、配其它错误码是连接已不可用：
                // 三种情况都没有需要继续丢弃的字节
                if (receivedBytes <= 0)
                {
                    return;
                }
            }
        }
    } // namespace

    AsyncSocket::AsyncSocket(EventLoop &loop, const int fileDescriptor) : m_loop(loop), m_fileDescriptor(fileDescriptor)
    {
        if (m_fileDescriptor >= 0)
        {
            // 只补上非阻塞：注册（IoWatcher）推迟到第一次等待，见 ensureWatcher() 的说明
            setNonBlocking();
        }
    }

    IoWatcher *AsyncSocket::ensureWatcher() const
    {
        if (m_watcher == nullptr && m_fileDescriptor >= 0)
        {
            // 注册一次覆盖整个描述符生命周期，此后每次等待都不再有 ADD/DEL 往返；
            // 关注位由注册对象在等待期间按需武装（见 IoWatcher 的说明：Windows 侧 wepoll
            // 只有水平触发，长期武装一个「几乎总是就绪」的方向会让事件循环空转）
            m_watcher = std::make_unique<IoWatcher>(m_loop, m_fileDescriptor);
        }
        return m_watcher.get();
    }

    AsyncSocket::~AsyncSocket()
    {
        close();
    }

    AsyncSocket::AsyncSocket(AsyncSocket &&other) noexcept :
        m_loop(other.m_loop), m_fileDescriptor(std::exchange(other.m_fileDescriptor, -1)), m_watcher(std::move(other.m_watcher)), m_isListening(other.m_isListening),
        m_advertisedPeer(std::move(other.m_advertisedPeer))
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
            // 监听标记也要跟着搬：丢了它，一个监听套接字会在被移动过一次之后按连接的收口方式
            // 去 shutdown 端点（正是换代那条链不能做的事）
            m_isListening = other.m_isListening;
            // 代理交来的身份也要跟着搬：丢了它，限额与日志会退回按代理记账（见 setAdvertisedPeerAddress）
            m_advertisedPeer = std::move(other.m_advertisedPeer);
        }
        return *this;
    }

    AsyncSocket AsyncSocket::create(EventLoop &loop, const int domain, const int type)
    {
#if ASYN_PLATFORM_WIN32
        // Windows 的 SOCKET 是无符号句柄类型：显式窄化成 int 是既有的描述符约定（全仓按 int 传递），
        // 失败值 INVALID_SOCKET 恰好变成 -1，与下面的有效性判定对齐。POSIX 用 SOCK_CLOEXEC 在建好时
        // 就断掉继承，Windows 的 ::socket 没有等价标志位，只能建完取消继承位——监听套接字与已建立的
        // 连接都不该随库外消费者的 CreateProcess(bInheritHandles=TRUE) 传下去
        const SOCKET socketHandle   = ::socket(domain, type, 0);
        const int    fileDescriptor = static_cast<int>(socketHandle);
        static_cast<void>(Platform::FileDescriptor::markNonInheritable(fileDescriptor));
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
        if (::listen(m_fileDescriptor, backlog) != 0)
        {
            return false;
        }
        markAsListening();
        return true;
    }

    void AsyncSocket::markAsListening() const noexcept
    {
        m_isListening = true;
    }

    Task<> AsyncSocket::asyncConnect(const sockaddr *const address, const socklen_t addressLength) const
    {
#if ASYN_PLATFORM_WIN32
        // Windows 上把连接做成一次真正的重叠操作。走 POSIX 那条「非阻塞 connect + 等可写」在这里是死路：
        // 连接中的套接字上零字节 WSASend 连投递都上不去（实测 WSAENOTCONN 贯穿整个连接期），
        // IOCP 因此永远不会报可写，等待方只能靠上层的看门狗收场——连接被拒要等到预算耗尽。
        // ConnectEx 的 OVERLAPPED 归后端所有，所以先建好注册对象再投递。
        if (ensureWatcher() == nullptr)
        {
            throw Base::SystemException("发起连接失败：套接字已无效", lastSocketError());
        }

        int immediateError = 0;
        if (!m_loop.epoll().beginConnect(m_fileDescriptor, address, static_cast<int>(addressLength), &immediateError))
        {
            throw Base::SystemException("发起连接失败", std::error_code(immediateError, std::system_category()));
        }

        if (!co_await waitWritable())
        {
            throw Base::SystemException("等待连接完成期间套接字被关闭", localSocketClosedError());
        }

        // 结果优先从完成包里取：实测被拒的 ConnectEx 完成时 SO_ERROR 仍是 0，
        // 只有包里的状态码译得回 WSAECONNREFUSED。没取到（例如被别的等待者收走）再退回问内核
        int connectError = 0;
        if (!m_loop.epoll().takeConnectResult(m_fileDescriptor, &connectError))
        {
            connectError = Platform::Socket::takePendingError(m_fileDescriptor);
        }
        if (connectError != 0)
        {
            throw Base::SystemException("连接对端失败", std::error_code(connectError, std::system_category()));
        }
        co_return;
#else
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
#endif
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
            throw Base::InvalidArgumentException("单次接收长度超过上限（" + std::to_string(length) +
                                                 " 字节 > INT_MAX）：底层 recv 的长度形参是 int，"
                                                 "超限会被静默窄化；请把数据分成多次接收");
        }

        while (true)
        {
            const ssize_t receivedBytes = ::recv(m_fileDescriptor, static_cast<char *>(buffer), static_cast<int>(length), MSG_NOSIGNAL);
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
            throw Base::InvalidArgumentException("单次发送长度超过上限（" + std::to_string(length) +
                                                 " 字节 > INT_MAX）：底层 send 的长度形参是 int，"
                                                 "超限会被静默窄化；请把数据分成多次发送");
        }

        while (true)
        {
            const ssize_t sentBytes = ::send(m_fileDescriptor, static_cast<const char *>(buffer), static_cast<int>(length), MSG_NOSIGNAL);
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
        // 静默补齐零段则会让调用方以为数据发出去了，两者都必须当场失败。
        // 报错走 logic_error 分支而不是运行期故障链：失败点是**传进来的取值**，重试不会变好
        if (buffers == nullptr || bufferCount == 0)
        {
            throw Base::InvalidArgumentException("聚合发送失败：段数组为空");
        }
        if (bufferCount > Platform::Socket::kMaximumVectorCount)
        {
            throw Base::InvalidArgumentException("聚合发送失败：段数 " + std::to_string(bufferCount) + " 超过平台上限 " + std::to_string(Platform::Socket::kMaximumVectorCount) +
                                                 "（Windows 的 WSASend 最多 16 段），请先把相邻小段合并后再发送");
        }

        // 游标负责「部分写之后从哪继续、跨段怎么推进」这段最容易出错的账目；
        // 它单独成类并被用例直接覆盖——回环上很难自然触发部分写，可它一旦写错是静默的数据错位
        detail::VectoredSendCursor cursor(buffers, bufferCount);

        while (!cursor.isFinished())
        {
            Platform::Socket::WriteBuffer pending[Platform::Socket::kMaximumVectorCount]{};
            const std::size_t             pendingCount = cursor.snapshotPending(pending, Platform::Socket::kMaximumVectorCount);
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

#if ASYN_PLATFORM_WIN32
    std::optional<int> AsyncSocket::takeAcceptedConnection()
    {
        // AcceptEx 完成时连接已经接入后端自己的接受套接字，::accept() 看不到它，只能问后端要
        int acceptedDescriptor = -1;
        if (!m_loop.epoll().takeAcceptedSocket(m_fileDescriptor, &acceptedDescriptor))
        {
            return std::nullopt;
        }
        return acceptedDescriptor;
    }
#endif

#if !ASYN_PLATFORM_WIN32
    Task<ssize_t> AsyncSocket::asyncSendFile(const int fileDescriptor, const std::uint64_t offset, const std::size_t length) const
    {
        // 源描述符与长度属于调用方契约，交给底层会被折成同一个 kInvalidArgument，
        // 在这里当场报错能把「哪一侧的参数不对」说清楚
        if (fileDescriptor < 0)
        {
            throw Base::InvalidArgumentException("零拷贝发送失败：源文件描述符无效（" + std::to_string(fileDescriptor) + "），请先把文件打开再交给本方法");
        }
        if (length == 0)
        {
            throw Base::InvalidArgumentException("零拷贝发送失败：待发字节数为 0，本方法只用于发送文件正文，"
                                                 "空正文请直接跳过发送");
        }

        std::uint64_t currentOffset   = offset;
        std::size_t   remainingLength = length;

        while (remainingLength != 0)
        {
            const ssize_t sentBytes = Platform::Socket::sendFileChunk(m_fileDescriptor, fileDescriptor, currentOffset, remainingLength);
            if (sentBytes > 0)
            {
                currentOffset += static_cast<std::uint64_t>(sentBytes);
                remainingLength -= static_cast<std::size_t>(sentBytes);
                continue;
            }

            if (sentBytes == 0)
            {
                // sendfile 只在「已到文件末尾」时返回 0（本循环的待发字节数恒大于 0），
                // 与 send/sendmsg 返回 0 表示对端关闭不是一回事。
                // 两种成因要分开报：一次都没发出去是调用方给的起点本就在文件之外，
                // 发过一部分才是文件比声称的长度短——归错因会把排查支到相反的方向
                if (remainingLength == length)
                {
                    throw Base::SystemException("零拷贝发送失败：起点已在源文件末尾之后（偏移 " + std::to_string(currentOffset) +
                                                " 处读不到任何字节），请按源文件实际长度校正偏移与长度");
                }
                // 已经上线的前缀收不回来：调用方若整块重发，这段字节会在流里重复一遍
                throw Base::SystemException("零拷贝发送失败：已发出 " + std::to_string(length - remainingLength) + " 字节后到达文件末尾，源文件比声称的长度 " +
                                            std::to_string(length) + " 短（此刻偏移 " + std::to_string(currentOffset) +
                                            "）。长度多半取自早前的 stat、文件随后被改写；"
                                            "已上线的前缀不可整块重发，只能收口本条连接");
            }

            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kWouldBlock)
            {
                // 同 asyncSend：等待失败即套接字已关闭，不再重试
                if (!co_await waitWritable())
                {
                    throw Base::SystemException("零拷贝发送失败：等待可写期间套接字被关闭", localSocketClosedError());
                }
                continue;
            }
            if (Platform::PlatformError::lastSocketErrorCode() == Platform::PlatformError::kInterrupted)
            {
                continue;
            }
            throw Base::SystemException("零拷贝发送失败", lastSocketError());
        }
        co_return static_cast<ssize_t>(length);
    }
#endif

    int AsyncSocket::releaseFileDescriptor() noexcept
    {
        // 注册对象必须已经清空：它绑定的是本对象的循环，跟着描述符搬过去只会指向错误的循环
        m_watcher.reset();

        const int releasedFileDescriptor = m_fileDescriptor;
        m_fileDescriptor                 = -1;
        return releasedFileDescriptor;
    }

    bool AsyncSocket::isWaitingReadable() const noexcept
    {
        return m_watcher != nullptr && m_watcher->isWaitingFor(EPOLLIN);
    }

    bool AsyncSocket::isWaitingWritable() const noexcept
    {
        return m_watcher != nullptr && m_watcher->isWaitingFor(EPOLLOUT);
    }

    void AsyncSocket::close()
    {
        if (m_fileDescriptor >= 0)
        {
            // 先销毁注册对象再关描述符：反注册在描述符仍然有效时执行才最稳妥，
            // 而且它会唤醒仍挂在上面的等待协程——关闭描述符并不会唤醒 epoll 的等待者，
            // 少了这一步，正在等待可读/可写的协程会永久挂起
            m_watcher.reset();

            // 监听套接字只关自己这一份引用：shutdown 打在**端点**上，同一端点的其它引用会一起停掉。
            // 零停机换代里新一代正是拿着同一端点的另一份引用在服务，交棒方一 shutdown 就等于把端口
            // 上的监听一起关掉（实测父代收口后子代 10/10 连不上）。监听套接字也没有半关、
            // 也没有「本端不再读的入站字节」一说，直接关描述符就到位
            if (!m_isListening)
            {
                // 只关发送半轴：FIN 当场发出，随后清理入站队列期间对端仍能读完我们已经写出去的响应
                ::shutdown(m_fileDescriptor, SHUT_WR);

                // 接收队列里还有本端不再会读的字节时关闭，栈会改发 RST 而不是 FIN，对端把自己已收到、
                // 还没来得及读的响应一并丢掉（管线化时第二个请求最容易踩到）；先把它们丢干净
                discardUnreadInboundData(m_fileDescriptor);

                ::shutdown(m_fileDescriptor, SHUT_RDWR);
            }
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
        // 注册推迟到这一次等待（见 ensureWatcher()）；拿不到注册对象意味着描述符无效或已被关闭，
        // 等待没有意义，直接抛出可定位的中文原因
        IoWatcher *const watcher = ensureWatcher();
        if (watcher == nullptr)
        {
            throw Base::SystemException("等待套接字可读失败：套接字无效或已关闭");
        }
        return watcher->waitReadable();
    }

    IoWatcher::Awaiter AsyncSocket::waitWritable() const
    {
        // 注册推迟到这一次等待，理由同 waitReadable()
        IoWatcher *const watcher = ensureWatcher();
        if (watcher == nullptr)
        {
            throw Base::SystemException("等待套接字可写失败：套接字无效或已关闭");
        }
        return watcher->waitWritable();
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
        // 代理交来的身份优先：限额键、请求的 remoteAddress()、日志都读这一个值，
        // 让它们不必各自认得 PROXY 协议（理由见 setAdvertisedPeerAddress()）
        if (m_advertisedPeer.has_value())
        {
            return *m_advertisedPeer;
        }

        sockaddr_storage address{};
        socklen_t        addressLength = sizeof(address);
        if (getpeername(m_fileDescriptor, reinterpret_cast<sockaddr *>(&address), &addressLength) != 0)
        {
            // winsock 失败不写 errno，本文件的 lastSocketError() 就是为这条规矩准备的
            throw Base::SystemException("获取对端地址失败", lastSocketError());
        }
        return InetAddress(address, addressLength);
    }

    void AsyncSocket::setAdvertisedPeerAddress(const InetAddress address) noexcept
    {
        m_advertisedPeer = address;
    }

    InetAddress AsyncSocket::localAddress() const
    {
        sockaddr_storage address{};
        socklen_t        addressLength = sizeof(address);
        if (getsockname(m_fileDescriptor, reinterpret_cast<sockaddr *>(&address), &addressLength) != 0)
        {
            throw Base::SystemException("获取本地地址失败", lastSocketError());
        }
        return InetAddress(address, addressLength);
    }
} // namespace AsynGyanis::Core

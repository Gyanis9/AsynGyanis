#include "Net/Tcp/TcpAcceptor.h"

#include "Base/Exception/SystemException.h"
#include "Core/EventLoop/EventLoop.h"
#include "Platform/IO/FileDescriptor.h"
#include "Platform/IO/Socket.h"
#include "Platform/System/PlatformError.h"

#include <chrono>
#include <system_error>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 内核资源紧张类错误的退避时长，单位毫秒
         *
         * @details 描述符或内核缓冲耗尽时监听描述符一直保持「可读」，用等待可读事件来退避
         *          等价于忙等；只有按时间等待才能给上层腾出释放描述符的窗口。
         *          5ms 是经验值：短到不会让排队连接明显堆积，长到足以让一次 fd 回收完成。
         */
        constexpr int kResourcePressureBackoffMs = 5;

        /**
         * @brief 没有待接受连接时的重试间隔，单位毫秒
         *
         * @details 不用 EpollAwaiter 死等监听描述符可读：那个等待器无超时，而 wepoll 的
         *          边缘触发在「EAGAIN 之后重新注册」之间会丢边沿（偶发错过新连接），
         *          关闭监听描述符也不保证唤醒挂起的等待者（stop() 之后收不了尾）。
         *          按固定间隔轮询同时覆盖这两点，代价是空闲监听器每 50ms 唤醒一次：
         *          远低于人类可感知的连接延迟，也不会让空闲服务显著占用 CPU。
         */
        constexpr int kIdleAcceptPollIntervalMs = 50;

        /**
         * @brief 判断 socket 错误码是否属于「内核资源暂时不足、稍后重试即可恢复」一类
         * @param socketErrorCode Platform::PlatformError::lastSocketErrorCode() 的返回值
         * @return true 需要退避后继续接受连接
         * @return false 不属于资源紧张类错误
         */
        bool isResourcePressureError(const int socketErrorCode) noexcept
        {
            // 四种语义都指向「连接本身没坏，是内核暂时给不出资源」：进程 fd 上限、
            // 系统级 fd 表、网络缓冲空间与内存。它们在 Windows 上被 Platform 层映射到
            // 对应的 WSA 错误码，故此处不再需要任何平台分支
            return socketErrorCode == Platform::PlatformError::kTooManyOpenFiles
                || socketErrorCode == Platform::PlatformError::kSystemFileTableFull
                || socketErrorCode == Platform::PlatformError::kNoBufferSpace
                || socketErrorCode == Platform::PlatformError::kOutOfMemory;
        }
    } // namespace

    TcpAcceptor::TcpAcceptor(Core::EventLoop &loop, const Core::InetAddress &address) :
        m_loop(loop),
        m_listenSocket(Core::AsyncSocket::create(loop, address.family() == AF_INET6 ? AF_INET6 : AF_INET)),
        m_address(address),
        m_backoffTimer(loop)
    {
        // 协议族只按「是否 IPv6」二选一：其余地址族本框架暂不支持，落到 IPv4 更可预期。
        // 创建即由 Core 置好非阻塞与 close-on-exec，本层不再重复设置。
        // 退避定时器也在此刻建好：需要退避的场景多半是描述符耗尽，那时再去申请定时器描述符
        // 只会连续失败，把「等一会儿再来」变成「直接报错停机」
    }

    bool TcpAcceptor::bind()
    {
        const int listenDescriptor = m_listenSocket.fileDescriptor();

        // 监听套接字统一开启地址复用：重启服务时上一代连接留下的 TIME_WAIT 会占住端口，
        // 不开就会偶发「地址已被占用」。设置失败不必提前返回——真正的成败由 bind() 判定
        [[maybe_unused]] const bool isReuseAddressSet = Platform::Socket::setReuseAddress(listenDescriptor);

        // 端口复用只在 Linux 3.9+ 存在，Windows 上 Platform 层直接返回 false；
        // 这里按「平台不支持即降级」处理，不作为绑定失败上抛
        [[maybe_unused]] const bool isReusePortSet = Platform::Socket::setReusePort(listenDescriptor);

        // IPv6 套接字的 V6ONLY 默认值各平台不一致：显式关掉，让一个端口同时接住
        // IPv4 映射地址，避免调用方为两个协议各起一个监听器。非 IPv6 套接字调用必然失败，
        // 故只在协议族匹配时设置；失败同样忽略（内核可能强制单栈，不影响 IPv6 自身监听）
        if (m_address.family() == AF_INET6)
        {
            [[maybe_unused]] const bool isIpv6OnlySet = Platform::Socket::setIpv6Only(listenDescriptor, false);
        }

        // 绑定失败时保持 m_bound 为 false：listen() 会据此拒绝，调用方换端口后可直接重试
        if (!m_listenSocket.bind(m_address))
        {
            return false;
        }

        m_bound = true;
        return true;
    }

    bool TcpAcceptor::listen(const int backlog) const
    {
        // 未绑定就监听属于用法错误：直接返回 false，不把语义含糊的内核 EINVAL 抛给调用方
        if (!m_bound)
        {
            return false;
        }

        return m_listenSocket.listen(backlog);
    }

    Core::Task<std::optional<Core::AsyncSocket>> TcpAcceptor::accept()
    {
        // 暂存队列的连接其就绪事件已在上一次抽干时被消费，不会再产生新事件，
        // 因此必须优先出队；若先去等 epoll 会把它们永久留在队列里
        if (!m_pending.empty())
        {
            Core::AsyncSocket pendingSocket = std::move(m_pending.front());
            m_pending.pop_front();
            // std::move 之后 pendingSocket 只负责把描述符所有权交给返回值，离开作用域时
            // 其持有的已是无效描述符，不会重复关闭
            co_return pendingSocket;
        }

        while (true)
        {
            // 每轮重新取描述符并判定有效性：close() 或从未创建时按契约返回空值让服务器循环
            // 正常收尾，不抛异常；同时避免沿用上一轮可能已被内核复用的编号
            const int listenDescriptor = m_listenSocket.fileDescriptor();
            if (!Platform::FileDescriptor::isValid(listenDescriptor))
            {
                co_return std::nullopt;
            }

            sockaddr_storage peerAddress{};
            socklen_t        peerAddressLength = static_cast<socklen_t>(sizeof(peerAddress));
            // Platform 层已保证返回的描述符是非阻塞且不被子进程继承，本层无需二次设置
            const int acceptedDescriptor = Platform::Socket::accept(listenDescriptor, reinterpret_cast<sockaddr *>(&peerAddress), &peerAddressLength);

            if (Platform::FileDescriptor::isValid(acceptedDescriptor))
            {
                // 服务端连接一律关闭 Nagle：HTTP 与 RPC 的大量小包若被攒到 ACK 才发，
                // 首字节延迟会被明显抬高；设置失败只影响性能，不丢弃连接
                [[maybe_unused]] const bool isNoDelaySet = Platform::Socket::setNoDelay(acceptedDescriptor);

                // 描述符所有权自此刻交给 AsyncSocket：后续任何提前返回都由它负责关闭
                Core::AsyncSocket acceptedSocket(m_loop, acceptedDescriptor);

                // 事件循环用边沿触发：只在「变为可读」的一刻给一次通知。若此处只收一条就返回，
                // 监听队列里的其余连接不会再产生事件，会一直滞留到对端超时，所以必须一次抽干
                while (true)
                {
                    sockaddr_storage pendingAddress{};
                    socklen_t        pendingAddressLength = static_cast<socklen_t>(sizeof(pendingAddress));
                    const int        pendingDescriptor    = Platform::Socket::accept(listenDescriptor, reinterpret_cast<sockaddr *>(&pendingAddress), &pendingAddressLength);
                    if (Platform::FileDescriptor::isValid(pendingDescriptor))
                    {
                        [[maybe_unused]] const bool isPendingNoDelaySet = Platform::Socket::setNoDelay(pendingDescriptor);
                        m_pending.emplace_back(m_loop, pendingDescriptor);
                        continue;
                    }

                    const int batchErrorCode = Platform::PlatformError::lastSocketErrorCode();
                    // 队列已被抽空：结束本轮批量，把第一条连接交给调用方
                    if (batchErrorCode == Platform::PlatformError::kWouldBlock)
                    {
                        break;
                    }
                    // 被信号中断，或对端在握手完成后立刻断开（连接在队列里被本地中止）：
                    // 这只影响当前这一条，监听套接字仍然可用，必须继续抽取而不是退出
                    if (batchErrorCode == Platform::PlatformError::kInterrupted || batchErrorCode == Platform::PlatformError::kConnectionAborted)
                    {
                        continue;
                    }
                    // 资源紧张或未知错误：手上已有一条可交付的连接，先交出去。
                    // 下一轮 accept() 会走到外层退避分支，在这里忙等只会加剧资源不足
                    break;
                }

                co_return acceptedSocket;
            }

            const int socketErrorCode = Platform::PlatformError::lastSocketErrorCode();

            // 暂无待接受连接：间隔一小段时间后重试。
            // 不用 EpollAwaiter 死等可读事件：wepoll 的边缘触发在「accept 返回 EAGAIN」到
            // 「重新注册到 epoll」之间存在丢边沿的窗口，会偶发错过新连接；而关闭监听描述符
            // 也不保证唤醒挂在 epoll 上的协程，会让 stop() 之后收不了尾。
            // 轮询同时解决这两件事，代价是空闲监听器每 kIdleAcceptPollIntervalMs 唤醒一次
            if (socketErrorCode == Platform::PlatformError::kWouldBlock)
            {
                co_await m_backoffTimer.waitFor(std::chrono::milliseconds(kIdleAcceptPollIntervalMs));
                continue;
            }

            // 被信号中断，或对端在队列中被中止：重试即可，不算失败
            if (socketErrorCode == Platform::PlatformError::kInterrupted || socketErrorCode == Platform::PlatformError::kConnectionAborted)
            {
                continue;
            }

            // 描述符/缓冲/内存暂时不够：见 kResourcePressureBackoffMs 的说明，这里按时间退避。
            // 复用构造时预建的定时器而不是就地新建——此刻正是申请描述符会失败的时候；
            // 等待用协程定时器而不是 sleep_for，否则同线程上的其余协程会被整段阻塞
            if (isResourcePressureError(socketErrorCode))
            {
                co_await m_backoffTimer.waitFor(std::chrono::milliseconds(kResourcePressureBackoffMs));
                continue;
            }

            // 落到这里的是无法靠重试恢复的终止性错误（例如监听描述符被外部关闭）。
            // 错误码显式传入异常：Windows 上 socket 错误来自 WSAGetLastError，与 errno 不同源
            throw Base::SystemException("TcpAcceptor::accept 接受新连接失败", std::error_code(socketErrorCode, std::system_category()));
        }
    }

    void TcpAcceptor::close()
    {
        // 先关监听描述符再清暂存队列：清队列会逐个析构 AsyncSocket 并关闭各自连接，
        // 反过来做则关闭期间新到达的连接仍可能被收进队列
        m_listenSocket.close();
        m_pending.clear();
        // 复位绑定标记：描述符已失效，此后 listen() 必须被拒绝而不是拿旧状态蒙混过关
        m_bound = false;

        // 不需要额外唤醒等待者：接受轮按 kIdleAcceptPollIntervalMs 轮询，最迟一个周期内
        // 就会重新读取到失效的描述符并返回空值，服务器因此能在确定的时间内完成收尾
    }

    Core::InetAddress TcpAcceptor::localAddress() const
    {
        return m_address;
    }

    int TcpAcceptor::fileDescriptor() const
    {
        return m_listenSocket.fileDescriptor();
    }

} // namespace AsynGyanis::Net

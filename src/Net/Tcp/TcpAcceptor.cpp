#include "Net/Tcp/TcpAcceptor.h"

#include "Base/Exception/InvalidArgumentException.h"
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
        /**
         * @brief 校验接手用的描述符，无效即报中文可行动原因
         * @param adoptedDescriptor 调用方交来的监听套接字描述符
         * @return int 原样返回的描述符，便于直接用在初始化列表里
         */
        [[nodiscard]] int requireValidAdoptedDescriptor(const int adoptedDescriptor)
        {
            if (!Platform::FileDescriptor::isValid(adoptedDescriptor))
            {
                throw Base::InvalidArgumentException(
                        "TcpAcceptor: 接手的监听套接字描述符无效。它必须是已经在监听中的套接字"
                        "（由 socket activation 或父进程传递而来），请检查传给构造函数的值");
            }
            return adoptedDescriptor;
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

    TcpAcceptor::TcpAcceptor(Core::EventLoop &loop, const int adoptedListeningDescriptor) :
        m_loop(loop),
        // 校验放在 AsyncSocket 之前：无效描述符是用法错误，先报成中文原因，而不是让底层 socket
        // 调用抛一个看不出所以然的系统错误
        m_listenSocket(loop, requireValidAdoptedDescriptor(adoptedListeningDescriptor)),
        // 地址只能问内核：接手方并不知道上一代绑的是哪个地址，而日志与 listeningPort() 都要报真值
        m_address(m_listenSocket.localAddress()),
        m_backoffTimer(loop),
        // 已经在监听就说明已经绑定：bind()/listen() 的短路口径依赖这个标记。
        // 漏了它会去重新绑定一个已经绑好的套接字，必然失败
        m_bound(true),
        m_isAdopted(true)
    {
        // 非阻塞由 AsyncSocket 的该构造函数设置（继承来的监听套接字通常是阻塞的）
    }

    bool TcpAcceptor::bind()
    {
        // 接手的套接字已经在监听中：bind() 的后置条件此刻成立，直接返回成功。
        // 与 m_bound 一起判断，是为了让 close() 之后的同一个对象不再声称自己处于监听状态
        if (m_isAdopted && m_bound)
        {
            return true;
        }

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

        const int listenDescriptor = m_listenSocket.fileDescriptor();

        // 监听套接字的调参在这里统一下发（bind() 之后、listen() 之前）：缓冲区上限对新接受的
        // 连接可由内核继承，延迟接受则必须在 listen 之前设置；设置失败只影响性能，不中止监听
        if (m_tuning.receiveBufferBytes > 0)
        {
            [[maybe_unused]] const bool isReceiveBufferSet =
                    Platform::Socket::setReceiveBufferSize(listenDescriptor, m_tuning.receiveBufferBytes);
        }
        if (m_tuning.sendBufferBytes > 0)
        {
            [[maybe_unused]] const bool isSendBufferSet =
                    Platform::Socket::setSendBufferSize(listenDescriptor, m_tuning.sendBufferBytes);
        }
        if (m_tuning.deferAcceptSeconds > 0)
        {
            // 平台不支持（Windows）时返回 false：按「不支持即降级」处理，不是监听失败
            [[maybe_unused]] const bool isDeferAcceptSet =
                    Platform::Socket::setDeferAccept(listenDescriptor, m_tuning.deferAcceptSeconds);
        }
        if (m_tuning.fastOpenQueueLength > 0)
        {
            // TFO 同样只对监听套接字有意义；内核与服务端开关两处都就位时才真正接受 TFO 连接，
            // 设置失败（老内核头没有该选项）按「不支持即降级」处理，不影响监听本身
            [[maybe_unused]] const bool isFastOpenSet =
                    Platform::Socket::setFastOpen(listenDescriptor, m_tuning.fastOpenQueueLength);
        }

        // 接手的套接字已经在监听中，理由同 bind()：后置条件成立，且绝不能重新 listen
        if (m_isAdopted)
        {
            return true;
        }

        return m_listenSocket.listen(backlog);
    }

    void TcpAcceptor::setSocketTuning(const SocketTuning &tuning) noexcept
    {
        m_tuning = tuning;
    }

    const TcpAcceptor::SocketTuning &TcpAcceptor::socketTuning() const noexcept
    {
        return m_tuning;
    }

    void TcpAcceptor::applyAcceptedSocketTuning(const int descriptor) const noexcept
    {
        // 缓冲区上限与监听套接字同值：部分平台不保证继承，显式再设一遍保证跨平台一致；
        // 设置失败只影响性能，不丢弃连接（与 TCP_NODELAY 同一口径）
        if (m_tuning.receiveBufferBytes > 0)
        {
            [[maybe_unused]] const bool isReceiveBufferSet =
                    Platform::Socket::setReceiveBufferSize(descriptor, m_tuning.receiveBufferBytes);
        }
        if (m_tuning.sendBufferBytes > 0)
        {
            [[maybe_unused]] const bool isSendBufferSet = Platform::Socket::setSendBufferSize(descriptor, m_tuning.sendBufferBytes);
        }
    }

    Core::Task<std::optional<Core::AsyncSocket> > TcpAcceptor::accept()
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

#if ASYN_PLATFORM_WIN32
            // Windows（IOCP）：连接由后端的 AcceptEx 在完成通知里接入，::accept 看不到它，
            // 只能从后端取走。一次完成对应一条连接，因此这里没有 Linux 侧「一次抽干监听队列」
            // 那一段——队列里还有几条，后端就会再完成几次
            if (const std::optional<int> acceptedFromCompletion = m_listenSocket.takeAcceptedConnection();
                acceptedFromCompletion.has_value())
            {
                [[maybe_unused]] const bool isNoDelaySet = Platform::Socket::setNoDelay(*acceptedFromCompletion);
                applyAcceptedSocketTuning(*acceptedFromCompletion);
                co_return Core::AsyncSocket(m_loop, *acceptedFromCompletion);
            }

            // 手上没有已接入的连接：等一次可读（AcceptEx 完成时上报），等不到说明描述符已失效，
            // 按契约返回空值让服务器循环正常收尾
            if (!co_await m_listenSocket.waitReadable())
            {
                co_return std::nullopt;
            }
            continue;
#else
            sockaddr_storage peerAddress{};
            socklen_t        peerAddressLength  = static_cast<socklen_t>(sizeof(peerAddress));
            // Platform 层已保证返回的描述符是非阻塞且不被子进程继承，本层无需二次设置
            const int        acceptedDescriptor = Platform::Socket::accept(listenDescriptor, reinterpret_cast<sockaddr *>(&peerAddress), &peerAddressLength);

            if (Platform::FileDescriptor::isValid(acceptedDescriptor))
            {
                // 服务端连接一律关闭 Nagle：HTTP 与 RPC 的大量小包若被攒到 ACK 才发，
                // 首字节延迟会被明显抬高；设置失败只影响性能，不丢弃连接
                [[maybe_unused]] const bool isNoDelaySet = Platform::Socket::setNoDelay(acceptedDescriptor);
                applyAcceptedSocketTuning(acceptedDescriptor);

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
                        applyAcceptedSocketTuning(pendingDescriptor);
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

            // 暂无待接受连接：等监听描述符可读，而不是按固定间隔轮询。
            // 监听套接字是 AsyncSocket，它已经常驻注册在 epoll 上（一次注册、反复等待），
            // 因此这里既不产生 epoll_ctl，也不会丢边沿——空闲监听器从此零唤醒，
            // 新连接的接受延迟也不再受轮询周期限制
            if (socketErrorCode == Platform::PlatformError::kWouldBlock)
            {
                // 描述符已失效（close() 或从未创建）时按契约返回空值让服务器循环正常收尾。
                // 这里不需要额外的同步：accept 协程与 close() 都在同一个事件循环线程上，
                // 而本判断与下面的等待之间没有挂起点，close() 不可能插进来
                if (!Platform::FileDescriptor::isValid(m_listenSocket.fileDescriptor()))
                {
                    co_return std::nullopt;
                }

                // 等待失败意味着描述符在等待期间被关闭（注册对象被销毁会唤醒等待者），
                // 同样按契约返回空值——这正是「关闭监听描述符也能唤醒挂起的等待者」那条保证
                if (!co_await m_listenSocket.waitReadable())
                {
                    co_return std::nullopt;
                }
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
#endif
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

        // 正在等待新连接的 accept 协程会被自动唤醒：关闭监听套接字会销毁它的常驻注册对象，
        // 而注册对象的析构会把仍挂着的等待者以「未就绪」唤醒。接受轮据此返回空值，
        // 服务器因此立刻完成收尾，不必等下一个轮询周期
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

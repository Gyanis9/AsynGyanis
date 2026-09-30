#include "Net/Udp/UdpServer.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Exception/SystemException.h"
#include "Base/Log/LogMacros.h"
#include "Base/Log/LogThrottle.h"
#include "Platform/System/PlatformError.h"

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 读数报错的放行间隔：这类码是 ICMP 替某个已消失的对端捎回来的回声，发生率由对端控制，
        /// 逐条落盘会把日志刷满（与 QUIC 侧同一口径）
        constexpr std::chrono::seconds kReceiveErrorLogWindow{10};

        /// 处理器抛异常的放行间隔：起因在业务处理器那一侧，一次故障风暴会把它刷成万条
        constexpr std::chrono::seconds kHandlerFailureLogWindow{10};

        /// 报文发不出去时的放行间隔
        constexpr std::chrono::seconds kSendFailureLogWindow{10};
    } // namespace

    UdpServer::UdpServer(Core::EventLoop &eventLoop, Configuration configuration) : m_eventLoop(eventLoop), m_configuration(std::move(configuration))
    {
        // 缓冲容量必须放得下一条合法报文、又不超出单条报文上限：0 连空报文都放不下，
        // 超过上限则收得到却答不出去（发送侧按同一个常量整条拒发），两侧不对称只会留下
        // 「一条永远无法应答的报文」
        if (m_configuration.maximumDatagramByteCount == 0 || m_configuration.maximumDatagramByteCount > Platform::DatagramSocket::kMaximumDatagramBytes)
        {
            throw Base::InvalidArgumentException("UDP 服务端配置无效：收包缓冲容量 " + std::to_string(m_configuration.maximumDatagramByteCount) + " 字节不在 1.." +
                                                 std::to_string(Platform::DatagramSocket::kMaximumDatagramBytes) +
                                                 " 之间：0 连一条空报文都放不下，"
                                                 "超过上限则收到的报文无法整条应答");
        }

        m_metricHandles = {
                Core::ProcessMetricsRegistry::registerMetric("asyn_udp_datagrams_received_total", "交付给处理器的数据报条数（读数失败的空转不计）",
                                                             Core::ProcessMetricKind::Counter, Core::ProcessMetricMerge::Sum,
                                                             [this] { return m_receivedDatagramCount.load(std::memory_order_relaxed); }),
                Core::ProcessMetricsRegistry::registerMetric("asyn_udp_datagrams_sent_total", "本端发出的数据报条数（应答与主动下发都算）", Core::ProcessMetricKind::Counter,
                                                             Core::ProcessMetricMerge::Sum, [this] { return m_sentDatagramCount.load(std::memory_order_relaxed); }),
                Core::ProcessMetricsRegistry::registerMetric("asyn_udp_datagrams_unsent_total", "该发却没发出去的条数：超限、发送中套接字被关、平台报错",
                                                             Core::ProcessMetricKind::Counter, Core::ProcessMetricMerge::Sum,
                                                             [this] { return m_unsentDatagramCount.load(std::memory_order_relaxed); }),
                Core::ProcessMetricsRegistry::registerMetric("asyn_udp_handler_failures_total", "处理器抛出异常而被服务端接住的条数（不接就会打死收循环，但它必须可见）",
                                                             Core::ProcessMetricKind::Counter, Core::ProcessMetricMerge::Sum,
                                                             [this] { return m_failedHandlerCount.load(std::memory_order_relaxed); }),
        };
    }

    UdpServer::~UdpServer()
    {
        // 这里不主动收套接字：析构可能发生在非循环线程上，而那份注册对象只归所属循环销毁。
        // 调用方纪律与 QuicServer 相同——先让收循环退出（stop() 投递到循环线程），再销毁本对象
    }

    UdpServer::UdpServer(Core::EventLoop &eventLoop, Configuration configuration, Platform::DatagramSocket adoptedBoundSocket) : UdpServer(eventLoop, std::move(configuration))
    {
        // 「是不是数据报」「绑过没有」都在平台层的 DatagramSocket::adopt 里判过了，这里只挡下
        // 「交来一枚空对象」：那种服务端跑起来就是一条报文也收不到的空壳，而原因在调用方手里
        if (!adoptedBoundSocket.isValid())
        {
            throw Base::InvalidArgumentException("UDP 服务端接手数据报套接字失败：交来的套接字无效。自己绑的那一份大概是绑定就失败了"
                                                 "（bindTo 交回空对象），跨进程接手的那一份要走 Platform::DatagramSocket::adopt，"
                                                 "它会区分「描述符无效」「不是套接字」「不是 SOCK_DGRAM」「还没 bind」四种不合格");
        }
        m_adoptedSocket = std::move(adoptedBoundSocket);
    }

    Core::Task<> UdpServer::listen(Core::InetAddress localAddress)
    {
        requireFreshStart();
        // 接手来的服务端不该再去 bind 一个端口：那会让交过来的套接字被静默闲置，对端往那个端口发的
        // 报文一条也到不了，症状与「移交没做成」一模一样。两种顺序都说不通，协程一被驱动就指出来
        if (m_adoptedSocket)
        {
            throw Base::InvalidArgumentException("UDP 服务端启动失败：这台服务端是接手构造出来的，端口已经定在交过来的那个套接字上；"
                                                 "请调用不带地址的 listen()，不要再让它自己绑定端口");
        }
        requireMessageHandler();

        Platform::DatagramSocket boundSocket = Platform::DatagramSocket::bindTo(localAddress.platformAddress());
        if (!boundSocket.isValid())
        {
            throw Base::SystemException("UDP 服务端启动失败：绑定 UDP 端口 " + std::to_string(localAddress.port()) + " 失败（套接字错误码 " +
                                        std::to_string(Platform::PlatformError::lastSocketErrorCode()) + "）");
        }

        m_socket = std::make_unique<Core::AsyncUdpSocket>(m_eventLoop, std::move(boundSocket));
        co_await serveOnBoundSocket();
        co_return;
    }

    Core::Task<> UdpServer::listen()
    {
        requireFreshStart();
        // 接手模式与按地址模式不能混着用：无参的 listen() 没有地址可问，而带地址的那条会去 bind
        // 一个本对象已经不拥有的端口——两种顺序都说不通，协程一被驱动就指出来
        if (!m_adoptedSocket)
        {
            throw Base::InvalidArgumentException("UDP 服务端启动失败：这台服务端不是接手构造出来的，没有可服务的套接字；"
                                                 "请按地址调用 listen(地址)，或在构造时把已 bind 的数据报描述符交进来");
        }
        requireMessageHandler();

        m_socket = std::make_unique<Core::AsyncUdpSocket>(m_eventLoop, std::move(*m_adoptedSocket));
        m_adoptedSocket.reset();
        co_await serveOnBoundSocket();
        co_return;
    }

    void UdpServer::requireMessageHandler() const
    {
        // 没有处理器就别把端口开着：收了没人处理等于把每条报文丢掉，不如在启动时就点名。
        // 放在动套接字之前，是为了不让一次配置错误顺手占住一个端口
        if (!m_configuration.onMessage)
        {
            throw Base::InvalidArgumentException("UDP 服务端启动失败：没有设置报文处理器（Configuration::onMessage）：收了报文没人处理，"
                                                 "这台监听器只会把每一条都丢掉；请给出处理器再 listen()");
        }
    }

    void UdpServer::requireFreshStart() const
    {
        // 已在监听的服务端不许再启动一次：第二次 listen() 会把 m_socket 换成新的一份，而正在跑的收循环
        // 还挂在旧的那份上（接收缓冲与事件循环里的注册对象都归它），旧对象一被销毁，原先那个端口就再无
        // 人读——实测两侧都是「端口换了、原服务不再应答」。换端口没有「原地重来」这种用法：新建一台即可
        const std::uint16_t currentPort = m_listeningPort.load(std::memory_order_acquire);
        if (currentPort != 0U)
        {
            throw Base::InvalidArgumentException("UDP 服务端启动失败：这台服务端已经在 UDP 端口 " + std::to_string(currentPort) +
                                                 " 上监听，不能再次 listen()；要换个端口请新建一台服务端");
        }
    }

    Core::Task<> UdpServer::serveOnBoundSocket()
    {
        // 端口从套接字问回来：按地址绑的与接手来的都只有这一个来源（端口给 0 时只有内核知道实际端口）
        const Platform::SocketAddress boundAddress = m_socket->localAddress();

        // 端口最后发布：非 0 值就是「已在监听」的唯一凭据，读到它时必须连带上面的套接字已就位
        // （release 与外部线程读侧的 acquire 配对）
        m_listeningPort.store(Core::InetAddress(boundAddress.storage, boundAddress.length).port(), std::memory_order_release);
        LOG_INFO_FMT("UdpServer: 已在 UDP 端口 {} 上监听", m_listeningPort.load(std::memory_order_relaxed));

        // 缓冲在循环外一次分配：每条报文都新建会把「一条报文一次堆分配」塞进热路径
        std::vector<std::uint8_t> receiveBuffer(m_configuration.maximumDatagramByteCount);
        while (!m_isStopped.load(std::memory_order_acquire))
        {
            // 结果按值回来：惰性协程不往调用方的引用里写，实参可能比 await 先亡
            const Core::AsyncUdpSocket::DatagramReceiveResult received = co_await m_socket->asyncReceiveFrom(receiveBuffer.data(), receiveBuffer.size());
            if (received.receivedByteCount < 0)
            {
                if (continuesAfterReceiveFailure(received.socketErrorCode, m_socket->isValid(), m_isStopped.load(std::memory_order_acquire)))
                {
                    if (auto &throttle = ASYN_LOG_THROTTLED(kReceiveErrorLogWindow); throttle.acquire())
                    {
                        LOG_WARN_FMT("UdpServer: 数据报读数报错（错误码 {}），已跳过这一次读数并继续监听"
                                     "（过去 {} 秒内另有 {} 条同类被压掉）",
                                     received.socketErrorCode, kReceiveErrorLogWindow.count(), throttle.droppedCount());
                    }
                    continue;
                }
                // 到这里就是「只是没数据且套接字已不可用」：stop() 关掉了本端，或本端故障，收手
                break;
            }

            static_cast<void>(m_receivedDatagramCount.fetch_add(1, std::memory_order_relaxed));
            // 视图指向 receiveBuffer：串行派发保证本条处理期间不会有下一条覆盖它
            co_await serveOne(received.peerAddress, std::span<const std::uint8_t>(receiveBuffer.data(), static_cast<std::size_t>(received.receivedByteCount)));
        }

        // 三条出口都落到这里（stop() 关掉的、读数发现套接字没的、循环自己跑完的）：把收口做全，
        // 别留一枚还开着的描述符给析构
        stop();
        co_return;
    }

    void UdpServer::stop() noexcept
    {
        // 先置标记再关套接字：挂在读数上的协程醒来要能从循环条件读出「该收手」，
        // 否则它会把这次唤醒当成一次普通就绪接着去收
        m_isStopped.store(true, std::memory_order_release);
        if (m_socket != nullptr)
        {
            // 关掉套接字才是叫醒动作：AsyncUdpSocket::close() 先销毁注册对象（唤醒等待者）
            // 再关描述符，只翻标记的话这条协程会一直停在读上
            m_socket->close();
        }
    }

    Core::Task<bool> UdpServer::sendTo(Core::InetAddress targetAddress, const std::span<const std::uint8_t> payload)
    {
        co_return co_await sendDatagram(targetAddress.platformAddress(), payload);
    }

    std::uint16_t UdpServer::listeningPort() const noexcept
    {
        return m_listeningPort.load(std::memory_order_acquire);
    }

    UdpServer::Stats UdpServer::stats() const noexcept
    {
        return Stats{
                .receivedDatagramCount = m_receivedDatagramCount.load(std::memory_order_relaxed),
                .sentDatagramCount     = m_sentDatagramCount.load(std::memory_order_relaxed),
                .unsentDatagramCount   = m_unsentDatagramCount.load(std::memory_order_relaxed),
                .failedHandlerCount    = m_failedHandlerCount.load(std::memory_order_relaxed),
        };
    }

    bool UdpServer::continuesAfterReceiveFailure(const int socketErrorCode, const bool isSocketValid, const bool isStopped) noexcept
    {
        // 有码的失败都是「对端已经不在了」那一类回声（Windows 的 WSAECONNRESET、Linux 的
        // EHOSTUNREACH/ECONNREFUSED）：报文层面没改变本端任何状态，套接字还能用，必须继续读。
        // 走到「不继续」的只有两种：没码（套接字已不可用），或已经请求停止
        return socketErrorCode != 0 && isSocketValid && !isStopped;
    }

    Core::Task<> UdpServer::serveOne(const Platform::SocketAddress peerAddress, const std::span<const std::uint8_t> payload)
    {
        std::vector<std::uint8_t> reply;
        bool                      isHandlerFailed = false;
        std::string               failureReason;
        try
        {
            reply = co_await m_configuration.onMessage(Core::InetAddress(peerAddress.storage, peerAddress.length), payload);
        } catch (const std::exception &failure)
        {
            isHandlerFailed = true;
            failureReason   = failure.what();
        } catch (...)
        {
            // 非标准异常没有 what()：给一句中文占位，好过把它当成「没有异常」
            isHandlerFailed = true;
            failureReason   = "非标准异常（无 what() 描述）";
        }

        if (isHandlerFailed)
        {
            // 接住它、丢掉这一条、继续服务：让一条报文里的业务异常穿出收循环，等于把整台服务
            // 交给对端——与「读数报错不能退出循环」是同一条判据的两侧
            static_cast<void>(m_failedHandlerCount.fetch_add(1, std::memory_order_relaxed));
            if (auto &throttle = ASYN_LOG_THROTTLED(kHandlerFailureLogWindow); throttle.acquire())
            {
                LOG_WARN_FMT("UdpServer: 报文处理器抛出异常，已丢弃这一条报文并继续监听（原因：{}；过去 {} 秒内另有 {} 条同类被压掉）", failureReason,
                             kHandlerFailureLogWindow.count(), throttle.droppedCount());
            }
            co_return;
        }

        // 不作答：处理器交回空字节就是「这一条不用回」。这与输入侧的零长报文是两件事——
        // 后者是一条合法的报文，前者是一种应答选择
        if (reply.empty())
        {
            co_return;
        }

        static_cast<void>(co_await sendDatagram(peerAddress, std::span<const std::uint8_t>(reply)));
        co_return;
    }

    Core::Task<bool> UdpServer::sendDatagram(const Platform::SocketAddress targetAddress, const std::span<const std::uint8_t> payload)
    {
        if (m_socket == nullptr || !m_socket->isValid())
        {
            // 未监听或已收口：这一条没发出去，说清是哪种
            static_cast<void>(m_unsentDatagramCount.fetch_add(1, std::memory_order_relaxed));
            LOG_WARN("UdpServer: 本端没有可用的套接字（还没 listen()，或已经 stop()），这一条报文没发出去");
            co_return false;
        }

        // 零长报文合法，而平台层的发送入口即便长度为 0 也拒绝空指针缓冲，因此给它一个一字节占位：
        // 交出去的仍是那条空报文，而不是被判成「调用方写错了」
        const std::uint8_t emptyDatagramPlaceholder{0};
        const void *const  data = payload.empty() ? std::addressof(emptyDatagramPlaceholder) : payload.data();

        bool        isWholeDatagramSent = false;
        std::string failureReason;
        try
        {
            const ssize_t sentByteCount = co_await m_socket->asyncSendTo(targetAddress, data, payload.size());
            // 负值只有一种来路：等可写期间套接字被关掉，本端正在收手
            isWholeDatagramSent = sentByteCount >= 0;
            if (!isWholeDatagramSent)
            {
                failureReason = "发送期间套接字被关闭";
            }
        } catch (const std::exception &failure)
        {
            // 超限（不会被内核切开，因此整条拒发）与平台报错都走这一支：一条报文的失败不能
            // 把收循环带走，而截断交出一半比一条也不交更坏
            failureReason = failure.what();
        }

        if (isWholeDatagramSent)
        {
            static_cast<void>(m_sentDatagramCount.fetch_add(1, std::memory_order_relaxed));
            co_return true;
        }

        static_cast<void>(m_unsentDatagramCount.fetch_add(1, std::memory_order_relaxed));
        if (auto &throttle = ASYN_LOG_THROTTLED(kSendFailureLogWindow); throttle.acquire())
        {
            LOG_WARN_FMT("UdpServer: 报文没发出去（原因：{}；过去 {} 秒内另有 {} 条同类被压掉）", failureReason, kSendFailureLogWindow.count(), throttle.droppedCount());
        }
        co_return false;
    }
} // namespace AsynGyanis::Net

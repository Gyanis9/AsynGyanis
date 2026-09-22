/**
 * @file QuicServer.h
 * @brief QUIC 服务端：一条 UDP 端口上承载多条连接（自研状态机 + OpenSSL 的 QUIC TLS 胶水）
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/AsyncUdpSocket.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/HttpServerStats.h"
#include "Net/Tcp/PerIpConnectionLimiter.h"
#include "Net/Http3/Http3Session.h"
#include "Net/Quic/QuicConnection.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <openssl/ssl.h>

namespace AsynGyanis::Net
{
    class Router;

    /**
     * @brief QUIC 服务端：绑定一条 UDP 端口，把报文按连接标识分派给各自的连接
     *
     * @details 与 TCP 侧的分层对应关系：`TcpAcceptor` ↔ 本类（接入），`TcpStream` ↔ `QuicConnection`
     *          （一条连接的收发）。差别在于 QUIC 的连接标识在报文头里而不是四元组：同一个端口上
     *          任意多个对端都按报文里的目的连接标识路由，因此这里维护「连接标识 → 连接」的表，
     *          键是本端生成的 SCID（对端报文里的 DCID）。
     *
     * @warning 线程契约与 TCP 侧一致：本对象与它管的连接都只在所属事件循环线程上创建、使用与销毁。
     */
    class QuicServer
    {
    public:
        /**
         * @brief 服务端配置
         */
        struct Configuration
        {
            std::string              certificateFile;               ///< 服务器证书（PEM）
            std::string              privateKeyFile;                ///< 私钥（PEM）
            std::size_t              maximumConnections{1024};      ///< 同时在线连接上限
            std::chrono::seconds     idleTimeout{30};               ///< 空闲超时：超过即由传输层收口
            std::string              applicationProtocol{"h3"};     ///< 必须协商出的 ALPN；不是它就拒绝握手
            std::chrono::milliseconds expiryTickInterval{10};       ///< 定时器驱动的节拍（见 runExpiryTicker 的说明）
            /// h3 会话的请求解析上限（正文总量上限等），与 h1/h2 同一套配置。
            /// 不设置时用 HttpParserLimits 的默认值——**不能没有上限**：一条 POST 就能把内存吃光
            HttpParserLimits         parserLimits{};
            /// 在途正文字节的全局预算（可空：空表示不受约束）。与 HTTP 侧共用同一份账——
            /// h3 的正文同样驻留在进程内存里，只限「单条流」挡不住 100 条流各压 8 MiB
            std::shared_ptr<HttpMemoryBudget> memoryBudget;
            /// 统计采集端（可空：空表示 h3 流量不采集）。
            /// **与 HTTP 侧共用同一个实例**，那台服务器的 /metrics 就一并覆盖 h3（见
            /// HttpServer::metricsCollector()）；单独采集时用 stats() 读本服务端的快照
            std::shared_ptr<HttpMetricsCollector> metricsCollector;
            /// request-id 生成器（可空：空表示 h3 不为请求落定 id）。**与 HTTP 侧共用同一个实例**：
            /// 同一来源不管走 h1/h2 还是 h3，日志与 x-request-id 里的前缀都指向同一台服务器
            std::shared_ptr<HttpRequestIdGenerator> requestIdGenerator;
            /// 连接级限额（可空）：目前用到「单连接最多处理多少条请求」——达到后 h3 会话发 GOAWAY
            /// 排空，在途请求答完再由本服务端收掉这条连接。空闲时长不在此列：QUIC 自带
            /// `idleTimeout`，那是传输层的收口时刻，与 HTTP 侧的 keep-alive 空闲不是一回事
            std::shared_ptr<const HttpServerLimits> serverLimits;
            /// 单来源并发连接上限的限额器（可空：空表示不按来源限制）。与两条 TCP 通道共用一个实例时，
            /// 同一来源不管从 TCP 还是 QUIC 进来都算在同一个名额里
            std::shared_ptr<PerIpConnectionLimiter> perIpConnectionLimiter;
        };

        QuicServer(Core::EventLoop &eventLoop, Configuration configuration);

        ~QuicServer();

        QuicServer(const QuicServer &) = delete;

        QuicServer &operator=(const QuicServer &) = delete;

        /**
         * @brief 绑定端口并开始服务（在所属事件循环上跑，直到 stop()）
         * @param localAddress 本地地址（端口给 0 表示由内核分配）。**按值收**：本方法是惰性协程，
         *        收引用会把「调用方传的临时量」留到协程恢复时再用，那是悬空引用（实测 ASan 报过
         *        stack-use-after-scope）
         * @return Core::Task<> 停止时完成
         * @throws Base::Exception 证书/私钥加载失败、套接字绑定失败等启动期就该拦住的问题
         */
        [[nodiscard]] Core::Task<> listen(Core::InetAddress localAddress);

        /**
         * @brief 请求停止：关掉套接字让收循环退出，随后由析构把连接送走
         */
        void stop() noexcept;

        /**
         * @brief 设置收到流数据时的直通回调（只在没接 HTTP/3 路由器时生效）
         * @param handler 回调
         */
        void setStreamDataHandler(QuicConnection::StreamDataHandler handler);

        /**
         * @brief 把 HTTP/3 接到既有路由器上
         * @param router 路由器；**必须活得比服务端久**（本类只存指针）
         * @note 接上之后，每条连接在收到第一条流数据时自动建立 HTTP/3 会话：控制流与两条 QPACK 流
         *       由本类替它开出来，h3 请求按既有路由与处理器派发——业务代码与 h1/h2 是同一份。
         *       不接路由器时服务端只做传输层，流数据交给 setStreamDataHandler() 的直通出口
         */
        void setRouter(Router &router) noexcept;

        /**
         * @brief 当前在线连接数
         * @return std::size_t 连接数
         */
        [[nodiscard]] std::size_t connectionCount() const noexcept;

        /**
         * @brief 取本服务端的统计快照（h3 会话的请求数、状态码类、单流取消与在线连接数）
         * @details 与 HttpServer::stats() 同一形态与同一套指标口径，只是数据来自 h3 会话：
         *          请求数与状态码类由各会话累加，耗时直方图不参与（h3 没有可信的请求起始戳，
         *          详见 Http3Session 构造函数的说明）
         * @return HttpServerStats 快照；未配置采集端时除在线连接数外各计数为零
         * @note 可从任意线程调用（计数是原子量、连接数是加锁读的近似值）
         */
        [[nodiscard]] HttpServerStats stats() const;

        /**
         * @brief 本端实际绑定的端口
         * @return std::uint16_t 端口；尚未绑定时为 0
         * @note 可从别的线程读：绑定成功时才写入非 0 值，因此读它等同于问「监听起来了吗」
         */
        [[nodiscard]] std::uint16_t listeningPort() const noexcept;

        /**
         * @brief 优雅收口：先挡新连接，给已有连接发 GOAWAY，等在途请求做完，到期再兜底强关
         * @details 与 `TcpServer::drain()` 同一形状。这里**不能**直接 `stop()`：收报文那条循环会
         *          随之退出，在途请求的后续报文与 ACK 再也进不来，「等它做完」就无从谈起。因此第一段
         *          只置「不再接受新连接」的标记，h3 侧则在各连接的控制流上发一条 GOAWAY（RFC 9114 §5.2）
         *          告诉对端别再发新请求；等待期间的驱动仍由定时循环与本协程的轮询完成。
         * @param drainTimeout 最长等待时长；非正数表示不等，直接收口全部连接
         * @return Core::Task<> 连接已清空或期限到时完成
         * @note 线程约束同 `stop()`：必须在运行本服务器的那个事件循环线程上调用（外部线程请走
         *       `scheduler().scheduleRemote()`），本协程要遍历并关闭连接
         */
        [[nodiscard]] Core::Task<> drain(std::chrono::milliseconds drainTimeout);

    private:
        /// 排空期间的轮询间隔：决定「没有在途工作」多快被发现，代价是等待期间多几次唤醒
        static constexpr std::chrono::milliseconds kDrainPollInterval{50};
        /**
         * @brief 定时器驱动：按固定节拍检查各连接的到期时刻
         * @details QUIC 的 PTO/空闲超时/握手超时都要在「没有报文到达」时也准时触发，因此不能只靠
         *          收到报文时顺手处理。这里用固定节拍（默认 10ms）而不是给每条连接各排一个精确定时器：
         *          节拍实现简单、行为可预期，代价是到期处理最多晚一个节拍——对 PTO（毫秒到秒级）足够。
         * @return Core::Task<> 停止时完成
         */
        [[nodiscard]] Core::Task<> runExpiryTicker();

        /**
         * @brief 按目的连接标识把报文交给对应的连接；不认识的长头 Initial 则开一条新连接
         * @param peerAddress 来源地址
         * @param datagram 报文净字节
         * @note 本方法是协程：连接侧的 handleDatagram 是惰性协程，必须被真正 co_await。早先写成
         *       static_cast<void>(handleDatagram(...)) 等于构造完就把 Task 丢掉、协程从不恢复——
         *       读报文与写出都没发生，服务端一条报文也发不出去（实测：客户端收不到任何回包）。
         *       分成「排进就绪队列 + 由对象持有」那条路也能走通，但收报文本来就是串行的，
         *       直接 co_await 更简单，也少一份任务表的记账
         */
        [[nodiscard]] Core::Task<> routeDatagram(const Platform::SocketAddress &peerAddress,
                                                std::span<const std::uint8_t> datagram);

        /**
         * @brief 把已收口的连接摘出路由表
         */
        void reapClosedConnections();

        /**
         * @brief 带原因收口所有还开着的连接，并立刻清理路由表
         * @note 与 drain() 的三条出口配合：无论等到什么程度，返回后本服务器不再持有连接。
         *       收口报文（CONNECTION_CLOSE）先刷出去再摘连接，否则对端只能等自己的空闲超时
         */
        [[nodiscard]] Core::Task<> closeAllOpenConnections();

        /**
         * @brief 取（必要时创建）某条连接上的 HTTP/3 会话
         * @param connection 目标连接
         * @return Http3Session& 该连接的会话
         * @note 懒建：h3 的流量必然在握手之后，第一条流数据到达时建正好；提前建反而要额外记握手状态
         */
        Http3Session &http3SessionFor(QuicConnection &connection);

        /**
         * @brief 找出某条连接上的 HTTP/3 会话
         * @param connection 目标连接
         * @return Http3Session* 会话；该连接还没有会话时为空
         */
        [[nodiscard]] Http3Session *findHttp3Session(const QuicConnection *connection) noexcept;

        /**
         * @brief 驱动某条连接上的 HTTP/3 会话：派发排队的请求并把响应写出去
         * @param connection 目标连接
         * @return Core::Task<> 驱动完成（该连接没有会话时立即返回）
         */
        [[nodiscard]] Core::Task<> pumpHttp3For(QuicConnection &connection);

        Core::EventLoop     &m_eventLoop;           ///< 所属事件循环
        Configuration        m_configuration;      ///< 服务端配置
        SSL_CTX             *m_tlsContext{nullptr}; ///< QUIC 用的 SSL_CTX（含证书与 ALPN）
        Platform::DatagramSocket m_datagramSocket;  ///< 绑定的 UDP 套接字
        std::unique_ptr<Core::AsyncUdpSocket> m_socket; ///< 套接字的事件循环封装
        Core::Timer          m_expiryTicker;       ///< 定时驱动的节拍定时器
        /// 实际绑定的端口：绑定成功才写入，故非 0 即「已在监听」。原子量是为了让外部线程能读这个
        /// 启动凭据（写侧在循环线程、读侧只观察它），不是允许跨线程碰本类的其他成员
        std::atomic<std::uint16_t> m_listeningPort{0};
        std::atomic<bool>    m_isStopped{false};   ///< 是否已请求停止：可从别的线程置位，因此必须是原子
        /// 排空期间只挡新连接（收报文与在途请求照常跑）：与 m_isStopped 分开，
        /// 因为后者会让收报文的循环退出，在途请求就永远做不完
        std::atomic<bool> m_isRefusingNewConnections{false};
        Platform::SocketAddress   m_localSocketAddress;   ///< 本端地址（建连接时要写进回包与日志）
        QuicConnection::StreamDataHandler m_streamDataHandler; ///< 流数据回调（缺省为空，即收到流数据不回应）

        Router *m_router{nullptr}; ///< 路由器（不持有；接上之后每条连接才会有 HTTP/3 会话）

        /// 每条连接上的 HTTP/3 会话：键是连接，会话的开流/写出/归还额度的口子都指向那条连接。
        /// 会话必须在连接被摘除时一起销毁（它内部存的是指向该连接的引用）
        std::map<const QuicConnection *, std::unique_ptr<Http3Session>> m_http3Sessions;

        /// 连接标识 → 连接。键是本端生成的 SCID（对端的 DCID）
        std::map<std::string, std::unique_ptr<QuicConnection>, std::less<>> m_connections;

        /// 别名索引：除本端 SCID 之外**可以寻址到本连接的目的连接标识** → 连接
        /// （存裸指针，所有权仍在上面那张表里）。目前只有一类来源：客户端最初选的 DCID——
        /// 客户端重传 Initial 时报文里的 DCID 仍是它最初选的那个（RFC 9000 §7.2 首包连接标识
        /// 固定到服务端回话为止），按本端 SCID 建的键对不上，少了这一路会把每条重传都当成新连接
        /// （实测：一个客户端握手却建出 8 条连接）。本端不签发额外连接标识，因此别名只有这一条；
        /// 将来做连接标识轮换时要在这里补登记
        std::map<std::string, QuicConnection *, std::less<>> m_connectionsByAliasConnectionId;

        /// 连接 → 它占住的单来源名额凭据：随连接一起摘除即归还名额（键与上面那张表同源）
        std::map<std::string, PerIpConnectionLimiter::Lease> m_perIpConnectionLeases;
    };
} // namespace AsynGyanis::Net

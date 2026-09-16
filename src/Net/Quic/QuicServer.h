/**
 * @file QuicServer.h
 * @brief QUIC 服务端：一条 UDP 端口上承载多条连接（ngtcp2 + OpenSSL 的 QUIC TLS 回调）
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
            std::chrono::seconds     idleTimeout{30};               ///< 空闲超时：超过即由 ngtcp2 收口
            std::string              applicationProtocol{"h3"};     ///< 必须协商出的 ALPN；不是它就拒绝握手
            std::chrono::milliseconds expiryTickInterval{10};       ///< 定时器驱动的节拍（见 runExpiryTicker 的说明）
            /// h3 会话的请求解析上限（正文总量上限等），与 h1/h2 同一套配置。
            /// 不设置时用 HttpParserLimits 的默认值——**不能没有上限**：一条 POST 就能把内存吃光
            HttpParserLimits         parserLimits{};
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
         * @brief 本端实际绑定的端口
         * @return std::uint16_t 端口；尚未绑定时为 0
         */
        [[nodiscard]] std::uint16_t listeningPort() const noexcept;

    private:
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
         * @brief 把一条连接当前可用的全部连接标识登记进别名索引
         * @details 握手期间 ngtcp2 会签发额外的连接标识给对端（NEW_CONNECTION_ID），对端之后可能
         *          改用其中之一作为目的连接标识。若只登记建连接时那一个，后续报文会因为「命不中
         *          任何键」被当成无主报文丢掉（实测：握手能通，但流数据一条都到不了）。本方法在每次
         *          报文处理之后调用，把该连接现有的全部连接标识都补进索引。
         * @param connection 目标连接
         */
        void registerConnectionIds(const QuicConnection &connection);

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
        std::uint16_t        m_listeningPort{0};   ///< 实际绑定的端口
        std::atomic<bool>    m_isStopped{false};   ///< 是否已请求停止：可从别的线程置位，因此必须是原子
        std::vector<std::uint8_t> m_statelessResetSecret; ///< 无状态重置令牌的服务端级密钥
        Platform::SocketAddress   m_localSocketAddress;   ///< 本端地址（建连接时要进 ngtcp2 的 path）
        QuicConnection::StreamDataHandler m_streamDataHandler; ///< 流数据回调（缺省为空，即收到流数据不回应）

        Router *m_router{nullptr}; ///< 路由器（不持有；接上之后每条连接才会有 HTTP/3 会话）

        /// 每条连接上的 HTTP/3 会话：键是连接，会话的开流/写出/归还额度的口子都指向那条连接。
        /// 会话必须在连接被摘除时一起销毁（它内部存的是指向该连接的引用）
        std::map<const QuicConnection *, std::unique_ptr<Http3Session>> m_http3Sessions;

        /// 连接标识 → 连接。键是本端生成的 SCID（对端的 DCID）
        std::map<std::string, std::unique_ptr<QuicConnection>, std::less<>> m_connections;

        /// 别名索引：除本端最初那个 SCID 之外的**任何可以寻址到本连接的目的连接标识** → 连接
        /// （存裸指针，所有权仍在上面那张表里）。目前有两类来源：
        /// 一是客户端最初选的 DCID——客户端重传 Initial 时报文里的 DCID 仍是它最初选的那个
        /// （RFC 9000 §7.2 首包连接标识固定到服务端回话为止），按本端 SCID 建的键对不上，
        /// 少了这一路会把每条重传都当成新连接（实测：一个客户端握手却建出 8 条连接）；
        /// 二是 ngtcp2 后续签发的额外 SCID——对端可能改用其中之一（实测：只认最初那个时，
        /// 握手能通但流数据一条都到不了）
        std::map<std::string, QuicConnection *, std::less<>> m_connectionsByAliasConnectionId;
    };
} // namespace AsynGyanis::Net

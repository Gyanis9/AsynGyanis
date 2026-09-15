/**
 * @file QuicConnection.h
 * @brief 一条 QUIC 连接：ngtcp2 状态机 + OpenSSL 的 QUIC TLS 回调 + 报文收发
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Platform/IO/Socket.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/ssl.h>

namespace AsynGyanis::Net
{
    /**
     * @brief 一条 QUIC 连接
     *
     * @details 与 TCP 侧 `TcpStream` 的位置对应：负责这一条连接上的收发。差别在于 QUIC 把 TLS
     *          握在传输内部（握手报文走 QUIC 自己的 CRYPTO 流），因此本类同时持有 ngtcp2 连接对象、
     *          一个 `SSL`，以及 ossl 后端要求的**每连接加密上下文** `ngtcp2_crypto_ossl_ctx`
     *          ——三者靠 `ngtcp2_conn_set_tls_native_handle()` 串起来，缺一环握手就走不下去。
     *
     * @warning 线程契约与 TCP 侧一致：本对象只在其所属事件循环线程上创建、使用与销毁。
     * @note 建连接要用客户端首个 Initial 报文里的原始目的连接标识与版本，因此没有空构造这一说，
     *       只能由 `accept()` 依报文建立；路由键是本端生成的 SCID（对端会用报文里的 DCID 指向它）。
     */
    class QuicConnection
    {
    public:
        /// 本端连接标识的长度。对端报文里的目的连接标识就是这个长度——而 **Short 头报文不携带**
        /// **DCID 长度**，解这种报文时必须把本长度告诉 ngtcp2，否则它会按别的长度去截，
        /// 解出来的「目的连接标识」里混进包号字段，永远命不中路由表（实测：1-RTT 起所有报文整包丢）
        static constexpr std::size_t kSourceConnectionIdLength = 18;

        /// 发送一条报文的出口：由服务端提供（内部就是 AsyncUdpSocket::asyncSendTo）
        using DatagramSender = std::function<Core::Task<bool>(const Platform::SocketAddress &peerAddress, const std::uint8_t *data,
                                                              std::size_t length)>;

        /**
         * @brief 收到流数据时的回调
         * @param connection 数据所属的连接（回调可能要在这条连接上开流或回写）
         * @param streamId QUIC 流号
         * @param data 本段数据
         * @param isEndStream 对端在本段之后收尾
         */
        using StreamDataHandler = std::function<void(QuicConnection &connection, std::int64_t streamId, std::span<const std::uint8_t> data,
                                                     bool isEndStream)>;

        /**
         * @brief 连接配置（服务端级共享的那几项）
         */
        struct Configuration
        {
            SSL_CTX                   *tlsContext{nullptr};  ///< QUIC 用的 SSL_CTX（含证书与 ALPN）
            DatagramSender             sendDatagram;         ///< 报文出口
            StreamDataHandler          onStreamData;         ///< 流数据回调（HTTP/3 层接在这里）
            /// 本端签发新连接标识（NEW_CONNECTION_ID）时的通知：服务端的路由表要在这一刻就认下它。
            /// 只靠「处理完报文再同步一遍」会漏掉定时器驱动的 flush 里签发的标识，对端随后改用
            /// 该标识寻址时，报文会因命不中任何键被整包丢掉（实测：客户端的请求就是这样丢的）
            std::function<void(QuicConnection &connection, std::span<const std::uint8_t> connectionId)> onConnectionIdIssued;
            std::vector<std::uint8_t>  statelessResetSecret; ///< 无状态重置令牌的密钥（服务端级固定）
            std::chrono::milliseconds  idleTimeout{30000};   ///< 空闲超时
        };

        /**
         * @brief 服务端接受一条新连接
         * @param configuration 连接配置
         * @param localAddress 本端地址（进 ngtcp2 的 path）
         * @param peerAddress 对端地址
         * @param clientInitial 客户端首个 Initial 报文
         * @return std::unique_ptr<QuicConnection> 新连接；报文不可接受（非 Initial、版本不支持等）时返回 nullptr
         */
        [[nodiscard]] static std::unique_ptr<QuicConnection> accept(const Configuration &configuration,
                                                                   const Platform::SocketAddress &localAddress,
                                                                   const Platform::SocketAddress &peerAddress,
                                                                   std::span<const std::uint8_t> clientInitial);

        ~QuicConnection();

        QuicConnection(const QuicConnection &) = delete;

        QuicConnection &operator=(const QuicConnection &) = delete;

        /**
         * @brief 取本端连接标识（路由键：报文里的目的连接标识就是它）
         * @return std::string 连接标识的字节串
         */
        [[nodiscard]] std::string sourceConnectionId() const;

        /**
         * @brief 取本端当前在用的全部连接标识
         * @details 握手期间 ngtcp2 会签发额外的连接标识（NEW_CONNECTION_ID），对端可能改用其中之一
         *          寻址本端——路由表若只认最初那一个，后续报文就会因为「命不中任何键」被丢掉
         *          （实测：握手能通，但流数据一条都到不了）。
         * @return std::vector<std::string> 本端当前签发的全部连接标识（含最初那个）
         */
        [[nodiscard]] std::vector<std::string> sourceConnectionIds() const;

        /**
         * @brief 开一条本端发起的单向流并返回流号
         * @details HTTP/3 的控制流与两条 QPACK 流都是本端发起的单向流，而 ngtcp2 在应用层往一条
         *          本端发起的流上写数据之前必须先把它开出来（否则写接口按 STREAM_NOT_FOUND 拒掉）。
         *          协议层不碰 ngtcp2，因此这里露一个窄口子给它。
         * @return std::int64_t 新流号；连接已收口或开流失败时为 -1
         */
        [[nodiscard]] std::int64_t openUnidirectionalStream();

        /**
         * @brief 归还接收额度：把应用已经消费掉的字节数写回流量控制窗口
         * @param streamId 流号（必须是对端发起的流）
         * @param consumedByteCount 本次消费掉的字节数
         * @note 流级与连接级两本账都要还。漏还的后果很隐蔽：对端此后被流控卡住，而本端看不出任何
         *       异常——只是「数据不再来了」，正文一大就必现
         */
        void extendReceiveWindow(std::int64_t streamId, std::size_t consumedByteCount);

        /**
         * @brief 把一条收到的报文交给本连接处理，并把由此产生的待发字节写出去
         * @param peerAddress 来源地址
         * @param datagram 报文净字节
         * @return Core::Task<> 处理并写出完成
         */
        [[nodiscard]] Core::Task<> handleDatagram(const Platform::SocketAddress &peerAddress, std::span<const std::uint8_t> datagram);

        /**
         * @brief 把待发字节写出去（ACK、握手、流数据都从这里走）
         * @return Core::Task<> 写完或出错退出（错误已记日志，连接随后会被收口）
         */
        [[nodiscard]] Core::Task<> flush();

        /**
         * @brief 下一个到期时刻（PTO、空闲超时、握手超时都靠它）
         * @return std::chrono::steady_clock::time_point 到期时刻；没有定时器时为 time_point::max()
         */
        [[nodiscard]] std::chrono::steady_clock::time_point nextExpiry() const noexcept;

        /**
         * @brief 处理到期定时器（由服务端的定时驱动调用）
         * @return Core::Task<> 处理完成
         */
        [[nodiscard]] Core::Task<> handleExpiry();

        /**
         * @brief 连接是否已收口（正常关闭、被重置或空闲超时）
         * @return true 已收口，服务端应把它摘出路由表
         */
        [[nodiscard]] bool isClosed() const noexcept;

        /**
         * @brief 从外部请求收口（例如会话层判定不可用时）
         * @details 只置标志：后续 handleDatagram()/flush() 不再产出，服务端的清理循环随后摘除本连接
         */
        void requestClose() noexcept;

        /**
         * @brief 在一条流上排队一段待发数据（应用层用）
         * @param streamId 目标流号
         * @param data 数据
         * @param endStream 发完是否收尾这条流
         */
        void queueStreamData(std::int64_t streamId, std::span<const std::uint8_t> data, bool endStream);

        /**
         * @brief 是否攒下了还没刷出去的字节
         * @return true 表示有待发字节等着 flush
         * @note 收报文那条路会顺手 flush，但业务协程可能在**收报文路径之外**写下响应（例如先 await
         *       了一个定时器或一次磁盘写）：那时没人替它刷，响应会一直躺在待发队列里等某个 ngtcp2
         *       定时器把它想起来。服务端的定时循环据此补一刀
         */
        [[nodiscard]] bool needsFlush() const noexcept;

        // ---- 以下几项只供本文件里的 ngtcp2 回调取用（回调是自由函数，需要这条窄通道）----

        /// 把流数据交给应用层
        void deliverStreamData(std::int64_t streamId, std::span<const std::uint8_t> data, bool isEndStream);

        /**
         * @brief 把「本端刚签发了一个连接标识」告诉配置里的通知方
         * @param connectionId 新签发的连接标识字节
         */
        void notifyConnectionIdIssued(std::span<const std::uint8_t> connectionId);

        /// 丢掉某条流尚未发完的排队数据（流被重置或收尾时）
        void dropPendingStreamData(std::int64_t streamId);

        /**
         * @brief 记下某条流的待发数据被对端确认了多少，全确认了就释放这条条目
         * @param streamId 流号
         * @param offset 本次确认覆盖的起始偏移
         * @param dataLength 本次确认的字节数
         * @note 释放只能发生在这里（或流关闭时）：ngtcp2 重传时会再读一遍未确认的字节
         */
        void acknowledgePendingStreamData(std::int64_t streamId, std::uint64_t offset, std::uint64_t dataLength);

        /// 无状态重置令牌的密钥（服务端级）
        [[nodiscard]] const std::vector<std::uint8_t> &statelessResetSecret() const noexcept;

        /// 协商出的 ALPN（握手完成时记日志用）
        void selectedApplicationProtocol(const unsigned char *&protocol, unsigned int &protocolLength) const noexcept;

    private:
        /**
         * @brief 私有构造：只有 accept() 能建
         * @param configuration 连接配置
         */
        explicit QuicConnection(Configuration configuration);

        /**
         * @brief 待发送的流数据
         *
         * @note ngtcp2 并**不拷贝**交给它的流数据：它只记下这段字节的位置，丢包重传时会再读一遍。
         *       因此这段字节必须一直活到对端确认收到为止，不能一交出去就释放——早了就是重传时读
         *       已释放内存（实测：Linux ASan 抓到 ngtcp2 编码 STREAM 帧时 UAF，31 字节的待发缓冲）。
         */
        struct PendingStreamData
        {
            std::string bytes;            ///< 待发字节
            std::size_t offset{0};        ///< 已交给 ngtcp2 的字节数（它内部的重传仍要用这些字节）
            std::size_t ackedOffset{0};   ///< 已被对端确认的字节数：到多少才能释放多少
            bool        isEndStream{false}; ///< 发完是否收尾
            /// 收尾标志是否已经交给 ngtcp2：随最后一段数据发出（或经由零字节写）之后置位。
            /// 光看「还有没有待发数据」判不出这件事——数据发完但尚未确认时条目还在表里，
            /// 收尾只差一步的状态必须与「还没收尾」区分开
            bool        isFinSent{false};
        };

        Configuration                  m_configuration;      ///< 连接配置
        ngtcp2_conn                   *m_connection{nullptr}; ///< ngtcp2 连接对象
        SSL                           *m_tlsSession{nullptr}; ///< 本连接的 TLS 会话（QUIC 模式）
        ngtcp2_crypto_ossl_ctx        *m_cryptoContext{nullptr}; ///< ossl 后端的每连接上下文（要交给 set_tls_native_handle）
        ngtcp2_cid                     m_sourceConnectionId{}; ///< 本端连接标识（路由键）
        Platform::SocketAddress        m_peerAddress;         ///< 对端地址
        Platform::SocketAddress        m_localAddress;        ///< 本端地址
        ngtcp2_path_storage            m_path{};              ///< ngtcp2 的收发路径（地址在初始化时拷入）

        /// TLS 会话与 ngtcp2 连接之间的相互引用：ossl 胶水的回调拿到 SSL 后要靠它找回连接
        /// （app data 必须是 ngtcp2_crypto_conn_ref，库里会把它当函数指针调用取 conn）
        ngtcp2_crypto_conn_ref         m_cryptoConnectionReference{};
        std::map<std::int64_t, PendingStreamData> m_pendingStreamData; ///< 待发流数据
        bool m_isClosed{false}; ///< 本地判定的收口标志（空闲超时、致命写失败这类路径）
        bool m_isFlushing{false}; ///< 是否已有 flush 在写这条连接（收报文与定时器两条协程都会驱动它）
        bool m_hasFlushRequest{false}; ///< flush 进行中又有人要求写：让在跑的那一轮末再转一圈
        bool m_needsFlush{false}; ///< 攒下了还没刷出去的字节（服务端的定时循环据此补一刀）
    };
} // namespace AsynGyanis::Net

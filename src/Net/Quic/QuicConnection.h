/**
 * @file QuicConnection.h
 * @brief 一条 QUIC 连接：把自研状态机 `QuicConnectionCore` 接到 UDP 套接字与事件循环上
 * @author Gyanis
 * @date 2026-09-19
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 本类不含任何协议逻辑：报文进来交给状态机，产出的数据报写出去，定时器到点喂一个时刻。
 *          时间戳由单调时钟换算成「自本连接建立起多少微秒」，协议侧因此可以在单测里被完整复现，
 *          而这里只负责真实世界的那一层。流数据由 `QuicStreamLayer` 拷贝入队，交出后即可释放，
 *          不再有「指针必须活到对端确认」的那份跨层承诺。
 *
 * @note 能力边界之外刻意没有做几项：无状态重置令牌与 STATELESS RESET、连接标识轮换（本端全程
 *       只用一个连接标识）、路径迁移与 PATH_CHALLENGE、0-RTT、RETRY、版本协商包。
 *       协议侧的同一份清单见 `QuicConnectionCore.h` 的 @note。
 * @warning 线程契约与 TCP 侧一致：本对象只在其所属事件循环线程上创建、使用与销毁。
 * @note 建连接要用客户端首个 Initial 报文里的原始目的连接标识，因此没有空构造这一说，
 *       只能由 `accept()` 依报文建立；路由键是本端生成的连接标识。
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Platform/IO/Socket.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <openssl/ssl.h>

namespace AsynGyanis::Net
{
    class QuicConnectionCore;

    /**
     * @brief 一条 QUIC 连接的外壳
     *
     * @details 与 TCP 侧 `TcpStream` 的位置对应：只管这一条连接的收发与生命周期。握手字节、TLS、
     *          流量控制、丢包恢复全在 `QuicConnectionCore` 里，本类只做三件事——喂报文、写数据报、
     *          把流数据与流的收口转交给应用层（HTTP/3）。
     */
    class QuicConnection
    {
    public:
        /// 本端连接标识的长度。对端报文里的目的连接标识就是这个长度——而短头报文**不携带** DCID
        /// 长度字段，解这种报文时必须把本长度告诉状态机，否则它会按别的长度去截，解出来的
        /// 「目的连接标识」里混进包号字段，永远命不中路由表
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
            SSL_CTX *tlsContext{nullptr};  ///< 已配好证书与 ALPN 的上下文，生命周期须覆盖本连接
            DatagramSender sendDatagram;   ///< 报文出口
            StreamDataHandler onStreamData; ///< 流数据回调（HTTP/3 层接在这里）
            /// 对端取消（RESET_STREAM / STOP_SENDING）了某条请求流时的通知：
            /// 上层的 HTTP/3 会话据此回收该流的请求与响应状态。只对**对端发起的双向流**触发
            /// （流号低两位为 0）——请求只跑在这类流上，控制流与 QPACK 流另有各自己的一套规矩
            std::function<void(QuicConnection &connection, std::int64_t streamId)> onPeerStreamClosed;
            std::chrono::milliseconds idleTimeout{30000}; ///< 空闲超时，也是本端宣告的 max_idle_timeout
        };

        /**
         * @brief 服务端接受一条新连接
         * @param configuration 连接配置
         * @param localAddress 本端地址
         * @param peerAddress 对端地址
         * @param clientInitial 客户端首个 Initial 报文，只用来取版本与两条连接标识
         * @return std::unique_ptr<QuicConnection> 新连接；报文不可接受（非 Initial、版本不支持等）
         *         时返回 nullptr。首包本身不在这里交付，调用方随后照常调 `handleDatagram`
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
        [[nodiscard]] const std::string &sourceConnectionId() const noexcept;

        /**
         * @brief 开一条本端发起的单向流并返回流号
         * @details HTTP/3 的控制流与两条 QPACK 流都是本端发起的单向流，写之前必须先开出来。
         * @return std::int64_t 新流号；连接已收口或对端的单向流额度用尽时为 -1
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
         * @brief 下一个到期时刻（PTO、判丢定时器、空闲超时都靠它）
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
         * @brief 「有协程正拿着本连接」的记账守卫
         *
         * @details 收报文路径与定时循环都会 `co_await` 本连接的方法（handleDatagram / flush /
         *          handleExpiry 都可能在等网络时挂起）。挂起期间另一条路径可能把它判成「已收口」
         *          并摘除销毁——恢复后手里那份引用与迭代器就是悬垂的。持有守卫的整个区间内，
         *          服务端不得把它摘掉（见 QuicServer::reapClosedConnections()）。
         * @note 只归本连接所属的事件循环线程使用（计数不是原子量，两条路径都在该线程上）
         */
        class ActivityGuard
        {
        public:
            /**
             * @brief 记账 +1
             * @param connection 被记账的连接，其寿命必须覆盖本守卫
             */
            explicit ActivityGuard(QuicConnection &connection) noexcept :
                m_connection(&connection)
            {
                ++m_connection->m_activityCount;
            }

            /**
             * @brief 记账 -1
             */
            ~ActivityGuard() { --m_connection->m_activityCount; }

            ActivityGuard(const ActivityGuard &) = delete;
            ActivityGuard &operator=(const ActivityGuard &) = delete;

        private:
            QuicConnection *m_connection{nullptr}; ///< 被记账的连接（非拥有）
        };

        /**
         * @brief 当前是否有协程正持有本连接（守卫计数不为零）
         * @return true 有在途动作，摘除必须推迟到它结束
         */
        [[nodiscard]] bool hasActivity() const noexcept
        {
            return m_activityCount != 0;
        }

        /**
         * @brief 从外部请求收口（例如会话层判定不可用时）
         * @details 只置标志：后续 handleDatagram()/flush() 不再产出，服务端的清理循环随后摘除本连接
         */
        void requestClose() noexcept;

        /**
         * @brief 在一条流上排队一段待发数据（应用层用）
         * @param streamId 目标流号
         * @param data 数据；本层会拷进待发队列，交出后即可释放
         * @param endStream 发完是否收尾这条流
         */
        void queueStreamData(std::int64_t streamId, std::span<const std::uint8_t> data, bool endStream);

        /**
         * @brief 是否攒下了还没刷出去的字节
         * @return true 表示有待发字节等着 flush
         * @note 收报文那条路会顺手 flush，但业务协程可能在**收报文路径之外**写下响应（例如先 await
         *       了一个定时器或一次磁盘写）：那时没人替它刷，响应会一直躺在待发队列里等某个定时器
         *       把它想起来。服务端的定时循环据此补一刀
         */
        [[nodiscard]] bool needsFlush() const noexcept;

    private:
        /**
         * @brief 私有构造：只有 accept() 能建
         * @param configuration 连接配置
         */
        explicit QuicConnection(Configuration configuration);

        /// @return 自本连接建立起的微秒数，状态机要的那个时刻
        [[nodiscard]] std::chrono::microseconds currentTime() const noexcept;

        /// 把状态机攒下的流交付与流收口挨个转交给配置里的回调
        void pumpStreamCallbacks();

        /**
         * @brief 握手完成时记一条日志（带协商出的 ALPN），一条连接只记一次
         * @details 状态机不碰日志——它要能在没有输出的条件下于内存里跑完，这条观测因此留在外壳这一侧
         */
        void logHandshakeCompletionOnce();

        /**
         * @brief 把「对端取消了这个请求流」告诉配置里的通知方
         * @param streamId 流号（只对端发起的双向流才转交，其余流静默忽略）
         */
        void notifyPeerStreamClosed(std::int64_t streamId);

        Configuration m_configuration;                            ///< 服务端共享的那几项
        std::unique_ptr<QuicConnectionCore> m_core{};              ///< 协议状态机，本连接唯一一份
        std::string m_sourceConnectionId;                          ///< 本端连接标识（路由键）
        Platform::SocketAddress m_peerAddress{};                   ///< 对端地址
        Platform::SocketAddress m_localAddress{};                  ///< 本端地址
        std::chrono::steady_clock::time_point m_timeOrigin{};      ///< 时刻换算的基准，协议侧只看相对量
        int m_activityCount{0};                                    ///< 正持有本连接的协程数，见 ActivityGuard
        bool m_isClosed{false};                                    ///< 本地判定的收口标志（致命写失败这类路径）
        bool m_isFlushing{false};                                  ///< 是否已有 flush 在写这条连接
        bool m_hasFlushRequest{false};                             ///< flush 进行中又有人要求写：让在跑的那轮末再转一圈
        bool m_needsFlush{false};                                  ///< 攒下了还没刷出去的字节
        bool m_isHandshakeLogged{false};                           ///< 「握手完成」那条日志是否已记，见 logHandshakeCompletionOnce()
    };
} // namespace AsynGyanis::Net

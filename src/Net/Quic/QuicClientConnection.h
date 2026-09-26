/**
 * @file QuicClientConnection.h
 * @brief 出站 QUIC 连接：自持一个 UDP 套接字，把 `QuicConnection` 外壳按客户端角色跑起来
 * @author Gyanis
 * @date 2026-09-26
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 与 `QuicServer` 相对的那一半。为什么是「组合」而不是把外壳再抄一份：一条连接上的收发
 *          机制（喂报文、flush、恢复层定时、流数据交付）两个角色完全一样，不同的只有起点——服务端
 *          由收到的第一个 Initial 起连接，客户端自己造连接标识并先出声。抄一份的漂移形态历来是
 *          「修了一边的收口漏了另一边」。
 *
 * @note 握手之外的推进靠 `pumpOnce()`：本类不自带后台协程，调用方（将来的 HTTP/3 出站会话）
 *       在自己的循环里一条条收。这与服务端侧「收报文 → flush」的分工同构，也让用例能精确控制
 *       每一拍送进去什么。
 * @warning 只能在所属事件循环线程上使用（继承 `QuicConnection` 的同一份约定）。
 * @see QuicConnection
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/Socket/AsyncUdpSocket.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsPolicy.h"
#include "Net/Quic/QuicConnection.h"
#include "Platform/IO/Socket.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
    class TlsContext;
} // namespace AsynGyanis::Core

namespace AsynGyanis::Net
{
    /**
     * @brief 一条到服务端的 QUIC 连接
     */
    class QuicClientConnection
    {
    public:
        /**
         * @brief 出站连接的配置
         *
         * @details 信任库、证书吊销名单与版本区间都从 `tlsPolicy` 走，本结构不另开一套同义字段：
         *          出站侧的校验开关也**不在这里**——`Core::TlsContext` 对客户端角色的约定是「校验默认开启，
         *          且不许因为没配 CA 而静默降级」，这里放一个 `verifyPeer=false` 等于给那条约定开后门
         *          （任何受信 CA 签的证书都能冒充目标主机）。要连不校验对端的自签测试环境，就把那份
         *          PEM 交给 `tlsPolicy.certificateAuthorityFile` 当信任锚。
         */
        struct Configuration
        {
            std::string              hostName{};                                        ///< 服务端的规范主机名：SNI 与证书里的校验目标，必填
            std::vector<std::string> applicationProtocolIdentifiers{std::string{"h3"}}; ///< 要提供的 ALPN，按优先级排列
            /// 本端要出示的客户端证书（PEM）与配套私钥，双向 TLS 的客户端一侧。**两项必须同时给或
            /// 同时不给**，只给一项在构造期就抛：那种配置在握手里的形态是「服务端要证书而我们给得出
            /// 证书、给不出签名」，失败点离成因很远。留空即不提客户端证书——服务端要求时握手会被拒，
            /// 那正是服务端的意图，本端不替调用方猜要不要身份
            std::string               clientCertificateFile{};
            std::string               clientPrivateKeyFile{}; ///< 与上面那张证书配套的私钥（PEM）
            Core::TlsPolicy           tlsPolicy{};            ///< TLS 策略：信任库/吊销名单/版本区间等，出站侧校验恒开
            std::chrono::milliseconds handshakeTimeout{5000}; ///< 握手时限，到点直接收场（对端不说话时靠它）
            std::chrono::milliseconds idleTimeout{30000};     ///< 空闲超时，同时是本端宣告的 max_idle_timeout
        };

        /**
         * @brief 建出站连接对象（不碰网络，也不碰 TLS）
         * @param loop 所属事件循环
         * @param configuration 见 `Configuration`
         * @throws Base::InvalidArgumentException 用法错误：主机名为空。SNI 与证书校验目标都取自它，
         *         空值等于放一条「不校验对端身份」的连接出去，因此这一项在构造期就拒
         */
        QuicClientConnection(Core::EventLoop &loop, Configuration configuration);

        /**
         * @brief 收口并释放套接字
         * @details 不承诺把 CONNECTION_CLOSE 送上线：析构连接对象时还想着发东西是错的用法，
         *          要收尾的调用方请先 `close()` 再让一轮 `pumpOnce()` 跑完
         */
        ~QuicClientConnection();

        QuicClientConnection(const QuicClientConnection &) = delete;

        QuicClientConnection &operator=(const QuicClientConnection &) = delete;

        QuicClientConnection(QuicClientConnection &&) = delete;

        QuicClientConnection &operator=(QuicClientConnection &&) = delete;

        /**
         * @brief 发起握手并跑完它
         * @param serverAddress 服务端地址（取它的地址族挑本端绑定，并向它发包）
         * @return true 握手完成，此后可以开流收发；false 失败（对端不说话被时限掐断、握手被服务端拒绝、
         *         或收不到可信的证书）。失败时本连接已不可用，应直接销毁
         * @throws Base::SystemException 运行期故障：UDP 套接字建不起来
         * @note 地址按 `Core::InetAddress` 收：与本仓库其余出站入口（`HttpClient`、`AsyncResolver`
         *       的产出）同一口径，调用方不必自己再拼一次平台结构体
         */
        [[nodiscard]] Core::Task<bool> connect(const Core::InetAddress &serverAddress);

        /**
         * @brief 收一条报文、交给本连接处理、再把攒下的写出去
         * @details 无报文可收时挂起等待；套接字被关（时限掐断或 `close()`）时本轮什么都不做即返回
         */
        [[nodiscard]] Core::Task<> pumpOnce();

        /**
         * @brief 握手是否已完成
         * @return true 已可开流收发
         */
        [[nodiscard]] bool isReady() const noexcept;

        /**
         * @brief 连接是否已收口
         * @return true 本端或対端已收口，或状态机判定的空闲超时已到
         */
        [[nodiscard]] bool isClosed() const noexcept;

        /**
         * @brief 协商出的 ALPN 协议名
         * @return std::string 握手完成前为空
         */
        [[nodiscard]] std::string negotiatedApplicationProtocol() const;

        /**
         * @brief 开一条本端发起的双向流（出站 HTTP/3 一条请求一条）
         * @return std::int64_t 新流号；连接未就绪或对端给的双向流额度用尽时为 -1
         */
        [[nodiscard]] std::int64_t openStream();

        /**
         * @brief 开一条本端发起的单向流（HTTP/3 的控制流与两条 QPACK 流走这里）
         * @return std::int64_t 新流号（客户端侧 0x02、0x06……）；额度用尽时为 -1
         */
        [[nodiscard]] std::int64_t openUnidirectionalStream();

        /**
         * @brief 把这条连接上已排好的字节送上线
         * @details 写完流数据之后必须显式叫它一次，否则字节只躺在 QUIC 流的待发队列里：本框架不做
         *          「后台自动 flush」，为的是让调用方能精确控制每一拍送什么（与服务端侧同一分工）
         */
        [[nodiscard]] Core::Task<> sendPending();

        /**
         * @brief 往一条流上写数据
         * @param streamId 流号
         * @param data 待写字节
         * @param isEndStream 本段之后由本端收尾（FIN）
         * @return std::size_t 被接受的字节数；小于 `data.size()` 说明这条流的待发队列到了上界，
         *         剩下的要先 `pumpOnce()` 把窗口腾出来再交
         */
        std::size_t writeStream(std::int64_t streamId, std::span<const std::uint8_t> data, bool isEndStream);

        /**
         * @brief 把收到的流数据**直接转交**给一个出口，不再在本对象里排队
         * @details 上层是 HTTP/3 时必须要这一条：h3 那层自己按流分派、自己管额度，中间再放一层
         *          「按流缓冲、等谁来取」就是把同一批字节存两遍，还会让它看不到收尾（FIN）。
         *          设了它之后 `takeReceivedData` 就只会拿到空——一份字节只交给一边。
         * @param sink 转交出口；交空即退回「本对象排队、由调用方取走」那一档（默认就是这一档）
         */
        void setStreamDataSink(std::function<void(std::int64_t streamId, std::span<const std::uint8_t> data, bool isEndStream)> sink);

        /**
         * @brief 取走一条流上已收到的字节（取完即清空）
         * @param streamId 流号
         * @return std::vector<std::uint8_t> 自上次取走之后到达的字节
         */
        [[nodiscard]] std::vector<std::uint8_t> takeReceivedData(std::int64_t streamId);

        /**
         * @brief 对端是否已在某条流上收尾
         * @param streamId 流号
         * @return true 收到了 FIN
         */
        [[nodiscard]] bool isEndStreamReceived(std::int64_t streamId) const noexcept;

        /**
         * @brief 归还接收额度：把应用消费掉的字节数写回流量控制窗口
         * @param streamId 流号（必须是对端发起的那一档，出站侧即服务端推下来的单向流）
         * @param consumedByteCount 本次消费掉的字节数
         */
        void extendReceiveWindow(std::int64_t streamId, std::size_t consumedByteCount);

        /**
         * @brief 本端主动收口：排一条 CONNECTION_CLOSE 并停止
         * @details 只把收口报文排进待发队列，真正上线要调用方再跑一轮 `pumpOnce()`
         */
        void close();

        /**
         * @brief 所属事件循环
         * @details 上层要在这条连接上挂 `Core::DeadlineGuard`（它的构造需要循环提供定时器），
         *          而自己再造一份「哪个循环」的记账就会与本类的线程契约脱钩
         * @return Core::EventLoop& 构造时交来的那一个
         */
        [[nodiscard]] Core::EventLoop &eventLoop() const noexcept;

        /**
         * @brief 本端地址（绑定后由内核定的那个端口）
         * @return Platform::SocketAddress 未连接时是未设置的地址
         */
        [[nodiscard]] Platform::SocketAddress localAddress() const noexcept;

    private:
        /// 一条流在本端的接收账：已到达、还没被取走的字节，加上对端是否已收尾
        struct IncomingStreamState
        {
            std::vector<std::uint8_t> receivedBytes{};            ///< 自上次取走之后到达的字节
            bool                      isEndStreamReceived{false}; ///< 对端已发 FIN
        };

        /// 流数据回调的落点：按流号记账，供 `takeReceivedData` 取走
        void noteStreamData(std::int64_t streamId, std::span<const std::uint8_t> data, bool isEndStream);

        Core::EventLoop                                                       &m_loop;             ///< 所属事件循环（非拥有）
        Configuration                                                          m_configuration;    ///< 建好本对象时那份配置
        std::unique_ptr<Core::TlsContext>                                      m_tlsContext{};     ///< 客户端 TLS 上下文，连接销毁前一直持有
        std::unique_ptr<Core::AsyncUdpSocket>                                  m_socket{};         ///< 自持的 UDP 套接字，connect() 时建
        std::unique_ptr<QuicConnection>                                        m_connection{};     ///< 跑这条连接的状态机外壳
        Platform::SocketAddress                                                m_serverAddress{};  ///< 服务端地址，收包时据此丢弃旁来的报文
        std::vector<std::uint8_t>                                              m_receiveBuffer{};  ///< 收包缓冲，一次一条数据报
        std::map<std::int64_t, IncomingStreamState>                            m_incoming{};       ///< 按流号记的接收账
        std::function<void(std::int64_t, std::span<const std::uint8_t>, bool)> m_streamDataSink{}; ///< 已设的转交出口；空即在本对象排队
        bool                                                                   m_isStopped{false}; ///< 本端已收口或被时限掐断
    };
} // namespace AsynGyanis::Net

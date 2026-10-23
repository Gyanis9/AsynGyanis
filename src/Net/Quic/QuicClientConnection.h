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
 * @note 收报文的推进靠 `pumpOnce()`：调用方（将来的 HTTP/3 出站会话）在自己的循环里一条条收，
 *       这与服务端侧「收报文 → flush」的分工同构，也让用例能精确控制每一拍送进去什么。握手完成
 *       之后另有一条到期驱动的后台协程（见 `runExpiryLoop()`）：PTO 重传与空闲超时不能等对端先
 *       说话，这一路是客户端侧在静默期唯一的推进入口。
 * @warning 只能在所属事件循环线程上使用（继承 `QuicConnection` 的同一份约定）。
 * @see QuicConnection
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/AsyncUdpSocket.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsPolicy.h"
#include "Net/Quic/QuicConnection.h"
#include "Net/Quic/Recovery/QuicCongestionAlgorithm.h"
#include "Platform/IO/Socket.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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
    class ASYN_NET_API QuicClientConnection
    {
    public:
        /**
         * @brief 出站连接的配置
         *
         * @details 信任库、证书吊销名单与版本区间都从 `tlsPolicy` 走，本结构不另开一套同义字段：
         *          出站侧的校验开关也不在这里——`Core::TlsContext` 对客户端角色的约定是「校验默认开启，
         *          且不许因为没配 CA 而静默降级」，这里放一个 `verifyPeer=false` 等于给那条约定开后门
         *          （任何受信 CA 签的证书都能冒充目标主机）。要连不校验对端的自签测试环境，就把那份
         *          PEM 交给 `tlsPolicy.certificateAuthorityFile` 当信任锚。
         */
        struct Configuration
        {
            std::string              hostName{};                                        ///< 服务端的规范主机名：SNI 与证书里的校验目标，必填
            std::vector<std::string> applicationProtocolIdentifiers{std::string{"h3"}}; ///< 要提供的 ALPN，按优先级排列
            /// 本端要出示的客户端证书（PEM）与配套私钥，双向 TLS 的客户端一侧。两项必须同时给或
            /// 同时不给，只给一项在构造期就抛：那种配置在握手里的形态是「服务端要证书而我们给得出
            /// 证书、给不出签名」，失败点离成因很远。留空即不提客户端证书——服务端要求时握手会被拒，
            /// 那正是服务端的意图，本端不替调用方猜要不要身份
            std::string               clientCertificateFile{};
            std::string               clientPrivateKeyFile{}; ///< 与上面那张证书配套的私钥（PEM）
            Core::TlsPolicy           tlsPolicy{};            ///< TLS 策略：信任库/吊销名单/版本区间等，出站侧校验恒开
            std::chrono::milliseconds handshakeTimeout{5000}; ///< 握手时限，到点直接收场（对端不说话时靠它）
            std::chrono::milliseconds idleTimeout{30000};     ///< 空闲超时，同时是本端宣告的 max_idle_timeout
            /// 本连接用哪一家拥塞控制算法：缺省 NewReno（与这一格落地之前的部署逐字一致），
            /// CUBIC 见 RFC 9438。理由与取值见 `QuicConnectionCoreConfiguration::congestionAlgorithm`
            QuicCongestionAlgorithmKind congestionAlgorithm{QuicCongestionAlgorithmKind::NewReno};
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
         * @param serverAddress 服务端地址（取它的地址族挑本端绑定，并向它发包）。按值收：本方法是
         *        惰性协程，收引用会把「调用方传的临时量」（如 `resolve(...).value()`）留到协程恢复时
         *        再用，那是悬空引用（实测 ASan 报过 stack-use-after-scope）
         * @return true 握手完成，此后可以开流收发；false 失败（对端不说话被时限掐断、握手被服务端拒绝、
         *         或收不到可信的证书）。失败时本连接已不可用，应直接销毁
         * @throws Base::SystemException 运行期故障：UDP 套接字建不起来
         * @note 地址按 `Core::InetAddress` 收：与本仓库其余出站入口（`HttpClient`、`AsyncResolver`
         *       的产出）同一口径，调用方不必自己再拼一次平台结构体
         */
        [[nodiscard]] Core::Task<bool> connect(Core::InetAddress serverAddress);

        /**
         * @brief 收一条报文、交给本连接处理、再把攒下的写出去
         * @details 无报文可收时挂起等待；套接字被关（时限掐断或 `close()`）时本轮什么都不做即返回
         */
        [[nodiscard]] Core::Task<> pumpOnce();

        /**
         * @brief 有上限的收包：到点没有报文就返回，把控制权交回请求循环
         * @param timeout 本轮等待上限
         * @details 与 pumpOnce 的差别只有这一处：不把请求协程永远挂在 UDP 收包上，
         *          取消检查因此能按自己的节拍跑
         */
        [[nodiscard]] Core::Task<> pumpOnceFor(std::chrono::milliseconds timeout);

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
         * @brief 收口一条本端发起的流：交出 RESET_STREAM，并视需要请对端 STOP_SENDING
         * @param streamId 要收口的流号
         * @param applicationErrorCode 写进帧里的应用错误码（RFC 9114 §8.1 那一档）
         * @details 存在的理由是「这一条不要了，但连接还要留给别的流」。出站 HTTP/3 的响应正文越过
         *          本端胃口时就走这里：不复位的话对端会一直往一条我们不再读的流上发字节，而本端的
         *          接收窗口额度也得等这条连接收口才还回去。
         * @note 连接已收口时静默返回（与 close() 一样幂等）：调用方的结论已经拿到了，不该因为
         *       「收尾的时机晚了」多出一条报错
         */
        void abortStream(std::int64_t streamId, std::uint64_t applicationErrorCode) noexcept;

        /**
         * @brief 把这条连接上已排好的字节送上线
         * @details 写完流数据之后要显式叫它一次才能立刻上线：握手后的到期驱动只在有到期或欠刷时
         *          补一刀（见 `runExpiryLoop()`），不承担「写完即发」的时延承诺。要精确控制每一拍
         *          送什么，仍然走这里（与服务端侧同一分工）
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
         * @brief 把收到的流数据直接转交给一个出口，不再在本对象里排队
         * @details 上层是 HTTP/3 时必须要这一条：h3 那层自己按流分派、自己管额度，中间再放一层
         *          「按流缓冲、等谁来取」就是把同一批字节存两遍，还会让它看不到收尾（FIN）。
         *          设了它之后 `takeReceivedData` 就只会拿到空——一份字节只交给一边。
         * @param sink 转交出口；交空即退回「本对象排队、由调用方取走」那一档（默认就是这一档）
         */
        void setStreamDataSink(std::function<void(std::int64_t streamId, std::span<const std::uint8_t> data, bool isEndStream)> sink);

        /**
         * @brief 接上「对端打断了某条流」的出口（RESET_STREAM / STOP_SENDING）
         * @details 不接就等于本端看不见对端的复位：上层那条请求只能干等到自己的时限，而对端给的应用层
         *          错误码也一并丢掉——h3 那边「服务端没做任何应用层处理就拒了这条请求」正是靠这个码认的
         *          （RFC 9114 §7 的 H3_REQUEST_REJECTED），认出来客户端才敢把一条非幂等请求换条连接重来。
         *          与服务端那一路是同一个通知，只是这边交给设置的出口而不是 h3 会话
         * @param sink 出口；参数依次是流号、对端给的应用层错误码、以及是复位（true）还是叫停（false）。
         *             交空即不再转交（默认就是这一档）
         */
        void setPeerStreamAbortSink(std::function<void(std::int64_t streamId, std::uint64_t applicationErrorCode, bool isResetByPeer)> sink);

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

        /**
         * @brief 带着握手看门狗把握手跑到完成或收场
         * @details 单独成一条协程有两个理由：看门狗的守卫对象要在这条协程的帧销毁时一起撤销
         *          （放在 `connect()` 的函数体上会让它活过握手，到点掐掉已经可用的套接字），而
         *          `connect()` 还要管套接字与 TLS 的装配，两段混在一起读不出「谁在掐表」。
         *          收进来的每条数据报都按本端能否读到 ECN 字段决定交不交那一格（Windows 读不到，
         *          那一侧就该不报计数，RFC 9000 §13.4.1）。
         * @return Core::Task<bool> 握手是否完成；被对端拒绝、时限掐断或本端已关都交 false
         */
        [[nodiscard]] Core::Task<bool> driveHandshake();

        /**
         * @brief 握手完成后的到期驱动：按 `nextExpiry()` 分片等待，落实 PTO 与空闲超时
         * @details 服务端那一半由 `QuicServer::runExpiryTicker()` 周期调用 `QuicConnection::handleExpiry()`；
         *          客户端此前只有 `pumpOnce()`，它挂在 UDP 收包上，对端不说话时 PTO 重传与空闲超时永远
         *          不会到点。本协程在 `connect()` 握手成功之后起，每觉不超过 100 毫秒（`nextExpiry()`
         *          更近时按剩余时长精确唤醒），醒来在到期点调用 `handleExpiry()`、欠刷时补一次 `flush()`；
         *          连接收口、本端停止或对象析构即退出。
         * @note 与 `pumpOnce()` 同在事件循环线程上串行执行，两者都不加锁。任一方先 flush 之后队列已空，
         *       `QuicConnection::flush()` 的空队列早退能承受这一次重叠。
         * @return Core::Task<> 连接收口、本端停止或到期驱动出错后完成
         */
        [[nodiscard]] Core::Task<> runExpiryLoop();

        Core::EventLoop                      &m_loop;          ///< 所属事件循环（非拥有）
        Configuration                         m_configuration; ///< 建好本对象时那份配置
        std::unique_ptr<Core::TlsContext>     m_tlsContext{};  ///< 客户端 TLS 上下文，连接销毁前一直持有
        std::unique_ptr<Core::AsyncUdpSocket> m_socket{};      ///< 自持的 UDP 套接字，connect() 时建
        std::unique_ptr<QuicConnection>       m_connection{};  ///< 跑这条连接的状态机外壳
        Core::Timer                           m_expiryTimer;   ///< 到期驱动的等待定时器（只在其所属循环上使用）
        /// 握手完成后的到期驱动协程；声明在 m_connection 之后，析构逆序保证它先于连接与套接字销毁
        std::optional<Core::Task<void>>                                        m_expiryTask{};
        Platform::SocketAddress                                                m_serverAddress{};  ///< 服务端地址，收包时据此丢弃旁来的报文
        std::vector<std::uint8_t>                                              m_receiveBuffer{};  ///< 收包缓冲，一次一条数据报
        std::map<std::int64_t, IncomingStreamState>                            m_incoming{};       ///< 按流号记的接收账
        std::function<void(std::int64_t, std::span<const std::uint8_t>, bool)> m_streamDataSink{}; ///< 已设的转交出口；空即在本对象排队
        /// 已设的「对端打断了某条流」出口；空即不转交（上层看不到对端的复位，只能等时限）
        std::function<void(std::int64_t, std::uint64_t, bool)> m_peerStreamAbortSink{};
        bool                                                   m_isStopped{false}; ///< 本端已收口或被时限掐断
    };
} // namespace AsynGyanis::Net

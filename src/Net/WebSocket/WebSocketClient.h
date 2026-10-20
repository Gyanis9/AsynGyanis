/**
 * @file WebSocketClient.h
 * @brief 出站 WebSocket 会话（RFC 6455 的客户端方向）：升级、掩码帧收发与关闭握手
 * @author Gyanis
 * @date 2026-10-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Core/Coroutine/Task.h"
#include "Net/WebSocket/WebSocketFrame.h"
#include "Net/WebSocket/WebSocketHandshake.h"
#include "Net/WebSocket/WebSocketError.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis
{
    namespace Core
    {
        class EventLoop;
        class TlsContext;
    } // namespace Core

    namespace Net
    {
        class HttpOutboundConnection;

        /**
         * @brief 出站 WebSocket 会话：主动连出去、完成 101 升级，然后按帧收发
         *
         * @details 一次 `connect()` 走完「解析地址 → 建 TCP（可选 TLS）→ 发升级请求 → 核对 101」，
         *          成功后本对象代表那条已经换了协议的连接：发出去的帧一律带掩码，收回来的帧要求
         *          对端不带掩码（RFC 6455 §5.1 给两个方向定的是相反的规矩）。收到的 Ping 自动回 Pong，
         *          分片消息由解码器重组后才交付。
         *
         * @note 与 `WebSocketPeer` 的分工：peer 是服务端那一面（等别人升上来），本类是客户端这一面；
         *       两者共用同一台帧编解码器与同一份 Close 状态码判据，不在本类里另写一套帧格式。
         * @note `handshakeTimeout` 管住 `connect()` 全程：建 TCP 连接、TLS 握手、以及发出升级请求后
         *       等 101 那三段各拿剩余预算。对端一言不发时看门狗到点把通路关掉，`connect()` 带着点明
         *       阶段的中文原因回来，不会把调用方的协程留在那里等一个永远不会到的应答。
         * @warning 本对象属于构造它的那个事件循环：`connect()`、`receive()`、`send*()`、`close()` 与析构
         *          都必须在循环自己的线程上调用。跨线程请先 `postRemote()` 把动作送过去。
         */
        class ASYN_NET_API WebSocketClient
        {
        public:
            /**
             * @brief 一次出站握手的参数
             *
             * @details 只收「线上要写出去」与「本端要判」的那几项；没有对应消费方的开关不列在这里。
             */
            struct Configuration
            {
                std::string               hostName{};             ///< 目标主机名或 IP 字面量：同时用作 Host、SNI 与证书主机名校验的对象
                std::uint16_t             port{0};                ///< 端口；0 表示按 TLS 那一位取默认（明文 80、TLS 443）
                std::string               requestTarget{"/"};     ///< origin-form 的请求目标，如 `/chat?room=1`
                std::vector<std::string>  subprotocols{};         ///< 提议的子协议名，按优先级；空表示不提
                const Core::TlsContext   *clientTls{nullptr};     ///< 非空即走 wss；上下文由调用方持有并须活过本会话
                std::chrono::milliseconds handshakeTimeout{5000}; ///< `connect()` 全程（建 TCP、TLS 握手、等到 101）的总时限，各段拿剩余预算；必须是正数
                std::size_t               maximumMessageSize{WebSocketFrameDecoder::kMaximumMessagePayloadLength}; ///< 一条消息的字节上限；0 表示不设（解压输出上限同吃这一格）
                bool                      negotiatePerMessageDeflate{true}; ///< 是否在升级请求里提议 permessage-deflate（RFC 7692，要约形态见 kPerMessageDeflateClientOffer）。
                                                                            ///< 默认开＝与主流客户端一致；对端不回应约时连接照常按明文收发。对端回出可接受的参数后，
                                                                            ///< 数据消息按协商窗口压缩发送、带 RSV1 的帧按协商窗口解压（上限取 maximumMessageSize）
            };

            /**
             * @brief 连出去并完成升级握手
             * @details 失败一律交回中文原因并点明断在哪一段（解析 / 连接 / TLS / 101 核对），调用方无需再看日志即可分支。
             * @param loop 本会话所属的事件循环
             * @param configuration 握手参数
             * @return Core::Task<WebSocketUpgradeExpected<std::unique_ptr<WebSocketClient>>> 成功交出可用的会话
             * @throws Base::Exception 系统的密码学随机源不可用：本端取不出 `Sec-WebSocket-Key` 或帧掩码键，
             *         没有它们就不能开始这次握手（RFC 6455 §5.3 的抗缓存重放全靠这两把键）
             */
            [[nodiscard]] static Core::Task<WebSocketUpgradeExpected<std::unique_ptr<WebSocketClient>>> connect(Core::EventLoop &loop, Configuration configuration);

            WebSocketClient(const WebSocketClient &)            = delete;
            WebSocketClient &operator=(const WebSocketClient &) = delete;
            WebSocketClient(WebSocketClient &&)                 = delete;
            WebSocketClient &operator=(WebSocketClient &&)      = delete;

            /**
             * @brief 析构并归还底层通路
             * @details 只关闭通路，不补发 Close 帧：析构可能发生在异常展开上，那时候任何一线程外的写出都不可靠，
             *          要按协议收口的调用方应在析构前显式 `close()`。
             */
            ~WebSocketClient();

            /**
             * @brief 这次握手谈成的事实（选中的子协议；扩展一概不谈）
             * @return const WebSocketUpgradeAgreement & 握手成功那一刻定下，之后不再变
             */
            [[nodiscard]] const WebSocketUpgradeAgreement &agreement() const noexcept;

            /**
             * @brief 本端还能发出帧吗
             * @return true 可以发；false 表示本端已发起关闭、对端已 Close 或通路出错
             * @note 这一位不管收：本端发出 Close 之后 `receive()` 仍能读回对端那条回帧，直到通路真的关掉
             */
            [[nodiscard]] bool isOpen() const noexcept;

            /**
             * @brief 对端 Close 帧交来的状态码原值
             * @details 交回的是原值，包括本规范禁止上线的取值（1004、1005、1006、1015 等）：那种
             *          情况下本端回帧按协议错误收口（1002），而「对端到底说了什么」仍然留得住，业务
             *          据此分档收尾（该不该重试、要不要告警、要不要记审计）。
             * @return std::optional<std::uint16_t> 有值＝对端交来了状态码；空＝对端没交出号码——
             *         Close 帧只带空负载、链路被直接切断（那样的帧根本没到过），或 Close 还没到
             */
            [[nodiscard]] std::optional<std::uint16_t> remoteCloseCode() const noexcept;

            /**
             * @brief 对端 Close 帧交来的原因文本
             * @details 只有当它是合法 UTF-8 时才交回：原因不是 UTF-8 时这条关闭按负载非法收口
             *          （RFC 6455 §7.4.1 的 1007），而那串原始字节不交给业务——它一旦被写进日志，
             *          就是把控制码注入日志的原料。
             * @return std::string_view 对端的原因（合法 UTF-8 时）；对端没交、或非法时为空
             */
            [[nodiscard]] std::string_view remoteCloseReason() const noexcept;

            /**
             * @brief 收下一条数据消息（文本或二进制）
             * @details 分片消息由解码器重组完才交；控制帧在这一格处理掉：收到的 Ping 自动按原负载回 Pong，
             *          收到的 Pong 就地消费，收到的 Close 会按同一状态码回一条 Close（RFC 6455 §5.5.1）
             *          并把帧交回调用方，同时本会话置为不可用。
             * @return Core::Task<WebSocketSessionExpected<WebSocketFrame>> 交出的帧操作码只可能是 Text、Binary、Close；
             *         失败原因点明是对端违反协议、文本负载不是合法 UTF-8（按 1007 收口）、对端未留 Close 就收口，还是本端已经收口
             */
            [[nodiscard]] Core::Task<WebSocketSessionExpected<WebSocketFrame>> receive();

            /**
             * @brief 发一条文本消息（单个末帧，负载必须是合法 UTF-8 由调用方保证）
             * @param text 文本内容，按「指针 + 长度」取，可以含 NUL 之外的任意字节
             * @return Core::Task<bool> 全部写出为 true；通路已死或本端已收口为 false
             * @note 帧在本函数返回之前就拼好（含取掩码键）：Task 是惰性启动的，把编码放到协程体里
             *       等于让用法错误要到首次恢复时才露出来
             */
            [[nodiscard]] Core::Task<bool> sendText(std::string_view text);

            /**
             * @brief 发一条二进制消息
             * @param binary 任意字节，可以含 NUL
             * @return Core::Task<bool> 全部写出为 true；通路已死或本端已收口为 false
             */
            [[nodiscard]] Core::Task<bool> sendBinary(std::string_view binary);

            /**
             * @brief 发一条 Ping
             * @details 对端应以同样负载回 Pong；本端的 Pong 由 `receive()` 就地消费，因此调用方看不到它，
             *          要判活就按「发了 Ping 之后有没有别的流量」自己在上层计时。
             * @param payload 调试用的负载，可为空
             * @return Core::Task<bool> 全部写出为 true；通路已死或本端已收口为 false
             * @throws Base::InvalidArgumentException 负载超过控制帧的 125 字节上限（RFC 6455 §5.5）——
             *         在调用点当场抛出，不建协程帧，也不留半条帧在线上
             */
            [[nodiscard]] Core::Task<bool> sendPing(std::string_view payload = {});

            /**
             * @brief 发起关闭握手：发一条 Close 帧，之后本会话不再发出任何帧
             * @details 只发一条，不按 §5.5.1 等对端的 Close 回来——那一段留给调用方自己 `receive()`，
             *          因为「要不要等」取决于业务是否还要看对端给的原因。
             * @param statusCode 状态码，默认 1000（正常收口）
             * @param reason 供人看的原因文本，可为空；必须是合法 UTF-8
             * @return Core::Task<bool> 写出成功为 true；通路已死或本端已发过 Close 为 false
             * @throws Base::InvalidArgumentException 状态码不允许出现在线上（1004/1005/1006/1015 与 1016-2999 段，
             *         RFC 6455 §7.4.1/§7.4.2）、原因超过 123 字节，或原因不是合法 UTF-8——三道判据都在调用点
             *         当场抛出，不建协程帧
             */
            [[nodiscard]] Core::Task<bool> close(std::uint16_t statusCode = 1000, std::string_view reason = {});

        private:
            WebSocketClient(std::unique_ptr<HttpOutboundConnection> transport, WebSocketUpgradeAgreement agreement, const Configuration &configuration, std::string pendingBytes);

            /**
             * @brief 拼一帧的线上字节：取本帧的掩码键并编码
             * @details 刻意不是协程：编码器对用法错误（控制帧超长、未定义的操作码）当场抛出，放在非协程
             *          这一步里，调用方就能在自己的调用点上看见异常，而不是等首次恢复
             * @param opCode 操作码
             * @param payload 负载
             * @return std::string 完整帧字节
             * @throws Base::InvalidArgumentException 负载对这一类帧来说过长（RFC 6455 §5.5）
             * @throws Base::Exception 密码学随机源不可用
             */
            [[nodiscard]] std::string prepareFrame(WebSocketOpCode opCode, std::string_view payload);

            /**
             * @brief 把已经拼好的 Close 帧写出去，并把「还能不能发」这一位落定
             * @param frameBytes 完整帧字节
             * @return Core::Task<bool> 写出成功为 true；通路已死或本端已发过 Close 为 false
             */
            [[nodiscard]] Core::Task<bool> sendCloseBytes(std::string frameBytes);

            /**
             * @brief 把一段已经编码好的帧字节写到通路上
             * @param frameBytes 完整帧字节
             * @return Core::Task<bool> 写出成功为 true
             */
            [[nodiscard]] Core::Task<bool> writeFrameBytes(std::string frameBytes);

            /**
             * @brief 解完一帧后推进读游标：已消费部分过半才搬移缓冲
             * @details 解码器一次只消化到帧尾为止，帧之后的字节留到下一次再喂；逐帧 erase 开头是 O(n²)。
             */
            void compactInboundBytes();

            /**
             * @brief 发 Close、置本端不可用并关掉传输，把失败原因按给定类别交回调用方
             * @param closeCode 随 Close 帧发出去的状态码
             * @param kind 交回的错误类别
             * @param errorText 交回的原因文案
             * @return Core::Task<WebSocketSessionExpected<WebSocketFrame>> 恒为失败
             */
            [[nodiscard]] Core::Task<WebSocketSessionExpected<WebSocketFrame>> failSession(std::uint16_t closeCode, WebSocketSessionErrorKind kind, std::string errorText);

            /**
             * @brief 应答对端那条 Close：按同一状态码回一条，然后把通路收掉
             * @param closeFrame 解码器交回的 Close 帧，负载是 {2 字节大端码}[原因]
             * @return Core::Task<WebSocketSessionExpected<WebSocketFrame>> 交回对端那条帧，让业务看见对端给的码与原因
             */
            [[nodiscard]] Core::Task<WebSocketSessionExpected<WebSocketFrame>> answerPeerClose(const WebSocketFrame &closeFrame);

            /// 取一条消息用的读缓冲大小：与 TcpStream 同档，够让一帧在少数几轮里读完
            static constexpr std::size_t kReadChunkByteLength = 16384;

            std::unique_ptr<HttpOutboundConnection> m_transport; ///< 已经换了协议的通路（明文或 TLS 都由它承载）
            WebSocketUpgradeAgreement               m_agreement; ///< 握手谈成的事实
            WebSocketFrameDecoder                   m_decoder;   ///< 帧解码器，已按客户端档位配置
            /// 协商出的两侧窗口位数（未协商成功为空）：发送侧据此压缩数据消息、接收侧据此解压带 RSV1 的帧
            std::optional<PerMessageDeflateWindow> m_deflateWindow{};
            std::string                            m_readBuffer; ///< 一次读取的落地缓冲，容量固定为 kReadChunkByteLength
            /// 读进来还没交给解码器的字节：解码器一次只消化到帧尾为止，帧之后的字节必须留到下一次
            /// `receive()` 再喂，丢掉就等于把对端连着发的那条消息截掉
            std::string m_inboundBytes;
            /// m_inboundBytes 里已消费前缀的字节数：解码起点由它给出。契约与 H3 帧读取器一致：
            /// 已消费部分过半才搬移，避免一次读入多帧时逐帧搬动整段缓冲（O(n²)）
            std::size_t m_inboundByteOffset{0};
            /// 本端还能不能发出帧：发过 Close、收到对端 Close、或通路出错都把它置为假。
            /// 收侧不看它——本端发起关闭之后，对端那条回帧仍要能读
            bool m_isOpen{true};
            /// 对端 Close 帧里交来的状态码原值（见 remoteCloseCode()）：对端没交码、或链路被直接切断时保持空
            std::optional<std::uint16_t> m_remoteCloseCode{};
            /// 对端 Close 帧里交来的原因文本（仅在合法 UTF-8 时保留，见 remoteCloseReason()）
            std::string m_remoteCloseReason{};
        };
    } // namespace Net
} // namespace AsynGyanis

/**
 * @file Http2ClientConnection.h
 * @brief HTTP/2 客户端连接层：本端发前奏与 SETTINGS，按流提交请求并把响应组装回来
 * @author Gyanis
 * @date 2026-09-25
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once

#include "Core/Coroutine/Task.h"
#include "Net/Http/Client/HttpOutboundConnectionPool.h"
#include "Net/Http2/Hpack.h"
#include "Net/Http2/Http2Frame.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    /// HTTP/2 客户端收到的响应：伪头与普通头分开留，正文按 DATA 拼回
    struct Http2ClientResponse
    {
        int         statusCode{0};                              ///< :status 的值；0 表示没拿到响应
        std::vector<std::pair<std::string, std::string>> headers; ///< 除伪头之外的响应字段，按收到的顺序留着
        std::string body;                                       ///< 正文（DATA 帧拼接，已按本端消耗归还流控窗口）
        std::string errorMessage;                               ///< 失败时的中文原因；为空表示这条响应是正常收齐的

        /// 是否成功收齐（拿到状态码且没有被对端中止）
        [[nodiscard]] bool isOk() const noexcept
        {
            return statusCode != 0 && errorMessage.empty();
        }
    };

    /**
     * @brief HTTP/2 的客户端一侧：与自家 Http2Connection（服务端角色）互为对端
     * @details 为什么要另写一层而不是给 Http2Connection 加个角色开关：那个类把「校验对端前奏」「收
     *          请求发响应」「流号必须是奇数」这些**服务端方向**的规则编进了状态机与 API 形状（
     *          takeRequests()/sendResponseHeaders()），反过来就是一套另一方向的账本，开关式合并只会让
     *          每条分支都要问「现在是谁」。帧层（Http2Frame）与 HPACK（Hpack）是角色中立的，这一层就
     *          建在它们之上。
     * @warning 与所有连接一样，本对象只在自己的事件循环上用；传输通路（HttpOutboundConnection）由调用方
     *          建好并交出所有权，明文 h2c 与 ALPN 协商出 h2 的 TLS 两条都走得通。
     */
    class Http2ClientConnection
    {
    public:
        /// 本端 SETTINGS 通告的接收能力；两项都既要发出去、也要被本端自己守住
        struct Config
        {
            std::uint32_t initialWindowByteCount{64u * 1024};  ///< 本端愿意为一条流缓冲多少未读正文字节
            std::uint32_t maximumFrameByteSize{16u * 1024};    ///< 本端能收的最大帧负载，合法区间 [16384, 16777215]
        };

        /**
         * @brief 用一条已建立的字节通路构造连接
         * @param loop 所属事件循环：时限看门狗的定时器用它，本对象此后只在这条循环上用
         * @param transport 已连上（TLS 已握手且 ALPN 选到 h2）的通路，所有权交给本对象
         * @param config 本端通告的接收能力
         * @throws Base::InvalidArgumentException config.maximumFrameByteSize 越出合法区间：那等于通告
         *         一个非法的 SETTINGS_MAX_FRAME_SIZE，帧解码器当场就拒
         */
        explicit Http2ClientConnection(Core::EventLoop &loop, std::unique_ptr<HttpOutboundConnection> transport,
                                       Config config = {});

        Http2ClientConnection(const Http2ClientConnection &) = delete;
        Http2ClientConnection &operator=(const Http2ClientConnection &) = delete;

        /**
         * @brief 走完连接前奏：发客户端前奏与本端 SETTINGS，收对端 SETTINGS 并回 ACK
         * @param waitTimeout 等对端 SETTINGS 的上限
         * @return true 连接可用（本端已能提交请求）
         * @return false 通路或协商失败，本对象此后不可再用
         */
        [[nodiscard]] Core::Task<bool> start(std::chrono::milliseconds waitTimeout);

        /**
         * @brief 提交一条请求并等它**收齐**（本层一次只提一条请求，对端的 MAX_CONCURRENT_STREAMS 因此无需记账）
         * @param scheme 目标 URI 的协议名，写进 :scheme（只允许 "http" 与 "https"）
         * @param authority 目标主机[:端口]，写进 :authority
         * @param method 请求方法，写进 :method
         * @param path 请求路径（含查询串），写进 :path；必须是 "/" 开头的绝对路径或 "*"
         * @param extraHeaders 附加字段，按给出的顺序排在四个伪头之后
         * @param body 请求正文；为空时头块直接带 END_STREAM。超出对端流控窗口的部分会等 WINDOW_UPDATE
         *        续发，等待期间照常处理对端送来的帧
         * @return Http2ClientResponse 响应；失败时 errorMessage 给出断在哪一段
         */
        [[nodiscard]] Core::Task<Http2ClientResponse> request(std::string_view scheme, std::string_view authority,
                                                              std::string_view method, std::string_view path,
                                                              const std::vector<std::pair<std::string, std::string>> &extraHeaders,
                                                              std::string_view body, std::chrono::milliseconds waitTimeout);

        /**
         * @brief 礼貌收尾：先尽力把 GOAWAY 发出去，再关掉通路
         * @details 顺序是有意的——GOAWAY 要让对端看见才有意义，通路一关就什么都发不出去了。发不出也不
         *          值得多等：本端紧接着就关，对端至多把这次收口记成 abrupt。
         */
        Core::Task<void> shutdown();

        /**
         * @brief 直接关掉通路（不发 GOAWAY）
         * @details 时限看门狗叫醒的就是这一条：等不到对端时先把通路断了让所有挂在收发上的协程收口，
         *          此时再发 GOAWAY 已没有意义。礼貌收尾请走 shutdown()。
         * @note 幂等；之后 isHealthy() 为 false
         */
        void close() noexcept;

        /// 通路是否还能用（没被对端收掉、也没被本端判死或关掉）
        [[nodiscard]] bool isHealthy() const noexcept;

    private:
        /// 一条在途请求的收包状态
        struct PendingStream
        {
            std::uint32_t streamId{0};
            Http2ClientResponse response;
            std::string pendingHeaderBlock;        ///< 头块累积字节（CONTINUATION 之前先攒着）
            bool isHeaderOpen{false};              ///< 正在收一段头块（等 CONTINUATION）
            bool isResponseComplete{false};        ///< 收到带 END_STREAM 的帧
            bool isReset{false};                   ///< 对端 RST 掉了这条流
        };

        /// 处理一层已解出的帧；返回 false 表示连接不可再用
        bool handleFrame(const Http2Frame &frame);
        bool handleSettingsFrame(const Http2Frame &frame);
        bool handleHeadersFrame(const Http2Frame &frame);
        bool handleContinuationFrame(const Http2Frame &frame);
        bool handleDataFrame(const Http2Frame &frame);
        bool handleWindowUpdateFrame(const Http2Frame &frame);
        bool handleGoAwayFrame(const Http2Frame &frame);
        bool handleRstStreamFrame(const Http2Frame &frame);
        bool handlePingFrame(const Http2Frame &frame);

        /// 把收完的一段头块解进对应流的响应里；解码失败时把连接判死（动态表已错位）
        bool finishHeaderBlock(std::uint32_t streamId);

        /// 从通路上读一段字节、处理其中完整的帧，并把攒下的回帧一次写出；返回 false 表示通路不可用
        Core::Task<bool> pumpSome();

        /// 把攒下的待发字节一次写出去（写完清空）；通路出错时为 false
        Core::Task<bool> flushOutgoing();

        /// 按两个窗口的余量把正文发完，必要时等 WINDOW_UPDATE 续发
        Core::Task<bool> sendBody(std::uint32_t streamId, std::string_view body);

        /// 本端 SETTINGS 的编码结果（通告 INITIAL_WINDOW_SIZE 与 MAX_FRAME_SIZE 两项）
        [[nodiscard]] std::string encodeLocalSettings() const;

        /// 归还本端已消费的接收窗口（连接级 + 流级各一条）
        void creditWindow(std::uint32_t streamId, std::uint32_t increment);

        /// 攒进待发缓冲
        void appendOutgoing(std::string frameBytes);

        void fail(std::string reason);

        Core::EventLoop &m_loop;                     ///< 所属事件循环：时限看门狗的定时器用它
        std::unique_ptr<HttpOutboundConnection> m_transport;
        Config m_config;
        Http2FrameDecoder m_decoder;
        HpackEncoder m_encoder;
        HpackDecoder m_headerDecoder;

        std::string m_outgoing;                    ///< 待写字节：本端把所有帧先攒在这里再一次写出
        std::uint32_t m_nextStreamId{1};           ///< 客户端流号：奇数且严格递增（RFC 7540 §5.1.1）
        std::int64_t m_connectionSendWindowByteCount{65535};  ///< 连接级发送窗口，初值是协议默认（§6.9.2）
        std::int64_t m_streamSendWindowByteCount{65535};      ///< 当前这条流的发送窗口（每次请求换一条流）
        std::int64_t m_peerMaximumFrameByteSize{16384};       ///< 对端能收的最大帧负载
        std::uint32_t m_peerInitialStreamWindowByteCount{65535}; ///< 对端通告的流初始窗口，用于换算新流窗口
        std::map<std::uint32_t, PendingStream> m_pendingStreams;
        bool m_isHealthy{true};                    ///< 连接层是否还能用
        bool m_isPeerGoAway{false};                ///< 对端是否已通告收尾
        bool m_isPeerSettingsReceived{false};      ///< 是否已收到对端的 SETTINGS（能提请求的前提）
        bool m_isAwaitingContinuation{false};      ///< 正在收一段头块，等 CONTINUATION
        std::uint32_t m_continuationStreamId{0};   ///< 那段没收完的头块属于哪条流
        std::string m_pendingHeaderBlock;          ///< 头块片段攒在这里，END_HEADERS 时一次解码
        std::string m_errorMessage;                ///< 最后一次失败的中文原因

        /// 客户端前奏的字节（RFC 7540 §3.4），本端在 start() 里第一个写出
        static constexpr std::string_view kClientPrefaceBytes = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    };
} // namespace AsynGyanis::Net

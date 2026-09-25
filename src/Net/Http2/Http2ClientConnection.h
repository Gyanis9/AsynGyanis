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
        /// 这条流上有没有收到过对端的任何帧。复用连接时靠它区分「对端在我们手里把连接收了」（可以重来
        /// 一次）与「响应本身出问题了」（重发会把非幂等请求做两遍）——与 HTTP/1.1 侧同一位判据
        bool isAnyByteReceived{false};
        /// 这条流上有没有把字节写上过通路（只在写成功之后置位，写失败的算「没发出去」）。
        /// 光看 isAnyByteReceived 不够：请求正文已整个交出去、对端还没答完时被时限掐掉，同样是
        /// 「一个字节没收到」，按那条判就会把非幂等请求悄悄做两遍
        bool isAnyByteSent{false};

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
        /// 一条连接最多能开几条流的上界：客户端流号取 1、3、5…，且不得越过 2^31-1（RFC 7540 §5.1.1）
        static constexpr std::uint32_t kMaximumOpenedStreamCount = 1U << 30;

        /// 本端的接收能力：前两项既写进 SETTINGS 通告给对端，也被本端自己守住；
        /// 后面两项是本端内部的闸门（头块缓冲、开流配额），不对外承诺
        struct Config
        {
            std::uint32_t initialWindowByteCount{64u * 1024};  ///< 本端愿意为一条流缓冲多少未读正文字节
            std::uint32_t maximumFrameByteSize{16u * 1024};    ///< 本端能收的最大帧负载，合法区间 [16384, 16777215]
            /// 单个头块（HEADERS 与其后 CONTINUATION 片段之和）的压缩后字节上限，与服务端侧同档：
            /// CONTINUATION 可以无限续，不设闸门等于让对端用一个头块把本端内存撑掉
            std::size_t maximumHeaderBlockByteCount{16u * 1024};
            /// 本端在这条连接上最多开几条流。缺省即 RFC 7540 §5.1.1 给客户端流号的上界：流号取奇数且
            /// 严格递增、不过 2^31-1，故 (2^31-1 + 1) / 2 = 2^30 条到顶。见顶之后本端不再提新流，并在
            /// 最后一条流收齐时交代一条 NO_ERROR 的 GOAWAY 主动退场，由连接池换一条新的——长命连接的
            /// 流号会用完，这不是理论问题：一条待命连接按一万请求每秒约 30 小时就到界
            std::uint32_t maximumOpenedStreamCount{kMaximumOpenedStreamCount};
        };

        /**
         * @brief 用缺省的本端能力建立连接
         * @param loop 所属事件循环：时限看门狗的定时器用它，本对象此后只在这条循环上用
         * @param transport 已连上（TLS 已握手且 ALPN 选到 h2）的通路，所有权交给本对象
         * @throws Base::InvalidArgumentException 帧上限越出合法区间：那等于通告一个非法的
         *         SETTINGS_MAX_FRAME_SIZE，帧解码器当场就拒。开流额度填 0 或填得比 §5.1.1 的上界
         *         （2^30 条）还大也走这条：前者一条流都提不出，后者会让流号越过 2^31-1
         */
        Http2ClientConnection(Core::EventLoop &loop, std::unique_ptr<HttpOutboundConnection> transport)
            : Http2ClientConnection(loop, std::move(transport), Config{})
        {
        }

        /**
         * @brief 指定本端能力建立连接
         * @param loop 所属事件循环
         * @param transport 已连上（TLS 已握手且 ALPN 选到 h2）的通路，所有权交给本对象
         * @param config 本端通告的接收能力
         * @details 能力取不到参数默认值上：Config 是本类的嵌套聚合，它的成员初值属于本类的
         *          complete-class context，写成默认参数在 GCC 下非法（[class.mem]）而 MSVC 放行——
         *          与 Http3Connection 那处同型，故补一把委托构造，调用方写法一字不变。
         * @throws Base::InvalidArgumentException 同二参那把
         */
        Http2ClientConnection(Core::EventLoop &loop, std::unique_ptr<HttpOutboundConnection> transport,
                              Config config);

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
         * @brief 提交一条请求并等它收齐；同一条连接上可以并发提多条（各占一条流）
         * @details 并发的正确性由两件事保证：通路同一时刻只有一个协程在读（驱动租约），而解出来的帧
         *          按流号分发到各自的记录上；驱动者跑完一轮把挂着的人都叫醒，谁发现自己那条流还没收齐
         *          就接手驱动。对端的 MAX_CONCURRENT_STREAMS 本层不代作节流：几条并发请求就占几条流，
         *          越限时由对端回 REFUSED_STREAM，本层既不排队也不重发（把它翻译成「等一等再来」是调用方
         *          的策略，本层不知道调用方愿意排队还是愿意快速失败）。
         * @note 本端自己也有一条额度：客户端流号是奇数、严格递增且不过 2^31-1（§5.1.1），一条连接最多
         *       提 Config::maximumOpenedStreamCount 条流。见顶之后的请求会被直接拒回（一个字节都没发出去，
         *       故调用方按「可以重来一次」那一支处理），并且在最后一条流收齐时本端会交代一条 NO_ERROR 的
         *       GOAWAY 主动退场——不这样下一条请求就会拿着回绕过的小号或偶数号去提流，被对端判 PROTOCOL_ERROR。
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

        /// 在途（已提出、还没收齐）的流条数：连接池据此判断这条连接是不是正被人用着
        [[nodiscard]] std::size_t inFlightStreamCount() const noexcept { return m_pendingStreams.size(); }

    private:
        /**
         * @brief 驱动权的作用域守卫：离开作用域（含异常展开）时放开租约并叫醒挂着的人
         * @details 少了这一层，一次穿过请求协程的异常就会把租约留在一个已经不跑的协程手上：通路再没人
         *          去读，别人挂着的流也就永远等不到叫醒——表现是整条连接静默卡死，比一次超时长得多。
         */
        class PumpLease
        {
        public:
            /// @param connection 已被 tryTakePumpLease() 取走租约的那条连接
            explicit PumpLease(Http2ClientConnection &connection) noexcept : m_connection(connection) {}

            /// 放开租约并叫醒等待者
            ~PumpLease() noexcept
            {
                m_connection.releasePumpLease();
                m_connection.wakeWaitingStreams();
            }

            PumpLease(const PumpLease &) = delete;
            PumpLease &operator=(const PumpLease &) = delete;

        private:
            Http2ClientConnection &m_connection; ///< 归属连接
        };

        /**
         * @brief 写出权的作用域守卫：离开作用域（含异常展开）时放开写权并叫醒排队的人
         * @details 与 PumpLease 分开的理由：读整条通路只能有一个等待者，写却没有这个限制，但
         *          「一次 send 只送一段字节」意味着两个协程同时在写会把帧撕成跨两半的字节流。
         *          写权可以层层嵌套地问（驱动者读完顺手回帧时也在写），所以它必须独立于读租约。
         */
        class FlushTurnGuard
        {
        public:
            /// @param connection 已把 m_isFlushInProgress 置真的那条连接
            explicit FlushTurnGuard(Http2ClientConnection &connection) noexcept : m_connection(connection) {}

            /// 放开写权并叫醒排队的写者
            ~FlushTurnGuard() noexcept
            {
                m_connection.m_isFlushInProgress = false;
                m_connection.wakeFlushWaiters();
            }

            FlushTurnGuard(const FlushTurnGuard &) = delete;
            FlushTurnGuard &operator=(const FlushTurnGuard &) = delete;

        private:
            Http2ClientConnection &m_connection; ///< 归属连接
        };

        /**
         * @brief flushOutgoing() 的等待体：写权在别人手上时挂起来，等它放开
         * @details 醒来不带走任何东西——它只看一眼待发缓冲还剩多少，所以等待体不额外带状态。
         */
        class FlushTurnAwaiter
        {
        public:
            /// @param connection 所属连接（生命周期由本次等待覆盖）
            explicit FlushTurnAwaiter(Http2ClientConnection &connection) noexcept : m_connection(&connection) {}

            /**
             * @brief 写权空着就不用挂：调用方会自己去抢这一轮
             * @return true 不必挂起
             */
            [[nodiscard]] bool await_ready() const noexcept { return !m_connection->m_isFlushInProgress; }

            /**
             * @brief 把本协程排进写队
             * @param waiter 当前协程句柄
             */
            void await_suspend(std::coroutine_handle<> waiter) noexcept;

            /// 醒来即完成：接下来由调用方自己再看一眼写权与待发缓冲
            void await_resume() const noexcept {}

        private:
            Http2ClientConnection *m_connection; ///< 归属连接（非拥有）
        };

        /**
         * @brief request() 的等待体：这条流暂时没有我可驱动的份，挂起来等驱动者叫醒
         * @details 一条连接同一时刻只能有一个协程在读通路（通路读是单等待者的），但响应可以落到
         *          任何一条流上——所以「驱动者」顺手替所有人读，读完把挂着的人叫醒。这里的关键是
         *          await_ready 的判断：只要没人驱动这条连接，等待者就必须自己上，否则大家都在等
         *          一个不存在的驱动者，连接就地僵住。
         */
        class StreamAwaiter
        {
        public:
            /**
             * @brief 构造等待体
             * @param connection 所属连接对象（生命周期由本次等待覆盖）
             * @param streamId 等的是哪条流
             */
            StreamAwaiter(Http2ClientConnection &connection, const std::uint32_t streamId) noexcept
                : m_connection(&connection), m_streamId(streamId)
            {
            }

            /**
             * @brief 这条流已有结论、或此刻没人驱动连接时就地完成（后者由我本人去驱动）
             * @return true 不必挂起
             */
            [[nodiscard]] bool await_ready() const noexcept;

            /**
             * @brief 把本协程登记在这条流上，等驱动者的下一轮叫醒
             * @param waiter 当前协程句柄
             * @return true 已登记、可以挂起；false 没处登记（记录已不在），协程直接继续往下走
             */
            bool await_suspend(std::coroutine_handle<> waiter) noexcept;

            /// 醒来即完成：结论本来就在流记录里，等待体不额外带东西
            void await_resume() const noexcept {}

        private:
            Http2ClientConnection *m_connection; ///< 所属连接（非拥有）
            std::uint32_t m_streamId;            ///< 等的那条流
        };

        /// 一条在途请求的收包状态。窗口与头块片段按流记：同一条连接上并发跑几条时，各自的账不能互相顶
        struct PendingStream
        {
            std::uint32_t streamId{0};
            Http2ClientResponse response;
            std::int64_t sendWindowByteCount{0};   ///< 这条流的发送窗口，建流时取对端通告的初值（§6.9.2）
            std::string pendingHeaderBlock;        ///< 头块累积字节（CONTINUATION 之前先攒着）
            bool isAwaitingContinuation{false};    ///< 正在收一段头块（等 CONTINUATION）
            bool isResponseComplete{false};        ///< 收到带 END_STREAM 的帧
            bool isReset{false};                   ///< 对端 RST 掉了这条流
            std::coroutine_handle<> waiter{};      ///< 挂在这条流上的请求协程；空表示没人等
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

        /// 把收完的一段头块解进这条流的响应里；解码失败时把连接判死（动态表已错位）
        bool finishHeaderBlock(PendingStream &stream);

        /// 把一段头块片段攒进这条流的缓冲；越过本端上限时终止连接——不肯存的片段没法交给 HPACK 解码器，两边的动态表会从此错位
        bool appendHeaderBlockFragment(PendingStream &stream, std::string_view fragment);

        /// 从通路上读一段字节、处理其中完整的帧，并把攒下的回帧一次写出；返回 false 表示通路不可用
        Core::Task<bool> pumpSome();

        /// 本端已经开过几条流：流号从 1 起按 2 递增，故「下一条 - 1」除以 2 就是已用条数
        [[nodiscard]] std::uint32_t openedStreamCount() const noexcept { return (m_nextStreamId - 1U) / 2U; }

        /**
         * @brief 取一个客户端流号：按 §5.1.1 取奇数且严格递增的那条，且不许越过 2^31-1
         * @param streamId 输出参数：只在返回 true 时写入
         * @return true 拿到了号
         * @return false 本端在这条连接上开流的额度已用完
         */
        [[nodiscard]] bool tryReserveStreamId(std::uint32_t &streamId) noexcept;

        /**
         * @brief 开流额度用尽且手上没有在途的流时，按 §6.8 交代一条 NO_ERROR 的 GOAWAY 让这条连接退场
         * @details 额度没走完、还有流在途、或已经判过死时本方法什么都不做；真正退场时先把那句 GOAWAY
         *          尽力写出去再返回，因此要在请求协程里 co_await
         */
        Core::Task<void> retireIfStreamBudgetSpent();

        /**
         * @brief 试着当这一轮的连接驱动者：已经有人在读通路时返回 false
         * @return true 租约归我，调用方必须配对调用 releasePumpLease()
         */
        [[nodiscard]] bool tryTakePumpLease() noexcept;

        /// 交出驱动权。只放开租约不叫醒人——叫醒由驱动者在处理完这一轮之后统一做
        void releasePumpLease() noexcept;

        /// 叫醒挂在这条连接上的请求协程：它们会各自再看一眼自己的流，需要驱动的那个来接租约
        void wakeWaitingStreams() noexcept;

        /// 叫醒排队等写权的协程：它们会各自再抢一轮，抢到的那个把待发字节写出去
        void wakeFlushWaiters() noexcept;

        /// 把攒下的待发字节一次写出去（写完清空）；通路出错时为 false
        Core::Task<bool> flushOutgoing();

        /// 按连接级与这条流两层的窗口余量把正文发完，必要时等 WINDOW_UPDATE 续发
        Core::Task<bool> sendBody(PendingStream &stream, std::string_view body);

        /// 本端 SETTINGS 的编码结果（通告 INITIAL_WINDOW_SIZE 与 MAX_FRAME_SIZE 两项）
        [[nodiscard]] std::string encodeLocalSettings() const;

        /// 归还本端已消费的接收窗口（连接级 + 流级各一条）
        void creditWindow(std::uint32_t streamId, std::uint32_t increment);

        /**
         * @brief 按 §6.5.2 与 §6.9.2 过一遍对端 SETTINGS 的取值并落进本端账本
         * @details 取值法排在应用之前：一条越界的参数哪怕排在末尾，也不能先把前面几条吃进账本再判死。
         *          ENABLE_PUSH 与 ENABLE_CONNECT_PROTOCOL 只允许 0/1，INITIAL_WINDOW_SIZE 不得越过
         *          2^31-1（连"改大之后在途流的窗口越界"也算），MAX_FRAME_SIZE 必须落在 [16384, 2^24-1]。
         * @param payload 已解析的对端 SETTINGS
         * @return true 全部取值合法且已生效
         */
        bool applyPeerSettings(const Http2SettingsPayload &payload);

        /// 本端开过的最后一条流号：GOAWAY 的 last-stream-id 用它（没开过任何流时为 0）
        [[nodiscard]] std::uint32_t lastOpenedStreamId() const noexcept;

        /// 连接级失败：先按 §6.8 攒一条带错误码的 GOAWAY，再把连接判死
        void failConnection(Http2ErrorCode errorCode, std::string reason);

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
        std::int64_t m_peerMaximumFrameByteSize{16384};       ///< 对端能收的最大帧负载
        std::uint32_t m_peerInitialStreamWindowByteCount{65535}; ///< 对端通告的流初始窗口，用于换算新流窗口
        std::map<std::uint32_t, PendingStream> m_pendingStreams;
        bool m_isPumpLeaseTaken{false};            ///< 这一轮谁在读通路并顺带回帧：同一时刻只许一个
        std::size_t m_waitingStreamCount{0};       ///< 挂在 StreamAwaiter 上的请求协程数
        bool m_isFlushInProgress{false};           ///< 写权在谁手上：两个协程同时 send 会把帧撕开
        std::vector<std::coroutine_handle<>> m_flushWaiters; ///< 排队等写权的协程
        bool m_isHealthy{true};                    ///< 连接层是否还能用
        bool m_isPeerGoAway{false};                ///< 对端是否已通告收尾
        bool m_isPeerSettingsReceived{false};      ///< 是否已收到对端的 SETTINGS（能提请求的前提）
        bool m_isOwnSettingsAcknowledged{false};   ///< 对端是否已 ACK 过本端那一条 SETTINGS（只许 ACK 一次）
        std::string m_errorMessage;                ///< 最后一次失败的中文原因

        /// 客户端前奏的字节（RFC 7540 §3.4），本端在 start() 里第一个写出
        static constexpr std::string_view kClientPrefaceBytes = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    };
} // namespace AsynGyanis::Net

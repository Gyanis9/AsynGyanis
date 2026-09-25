/**
 * @file Http2Session.h
 * @brief HTTP/2 会话：先完成 TLS 握手，再按 ALPN 协商结果选择协议循环
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Tls/TlsSocket.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpRequestId.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/HttpServerStats.h"
#include "Net/Http/HttpMemoryBudget.h"
#include "Net/Http/HttpRequestBody.h"
#include "Net/Http/HttpSession.h"
#include "Net/Http/Router.h"
#include "Net/Http/HttpStreamBody.h"
#include "Net/Http2/Http2Connection.h"

#include <cstddef>
#include <cstdint>
#include <coroutine>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Net
{
    /// ALPN 协商出 HTTP/2 时客户端与服务端一致使用的协议名（RFC 7540 §3.3）
    inline constexpr std::string_view kHttp2AlpnProtocolName = "h2";

    /**
     * @brief HTTP/2 会话类：一条连接对应一个 Http2Session，TLS 与明文两条传输都走它。
     *
     * @details 两种构造方式决定传输与进入协议循环的方式：TLS（ALPN 分流）在 start() 里先握手再按 ALPN 结果
     *          选协议，协商出 h2 才跑本循环，否则原样交回 HTTP/1.1 事务循环；明文（h2c，先验知识）按
     *          RFC 9113 §3.4 前奏直接进 h2，不探测、不回退。协议循环只经 isTransportOpen()/transportReceive()/
     *          transportSend() 三处用到传输层。
     * @note TLS 分流为什么不在 HttpsServer::createConnection() 里做：ALPN 结果产生于 TLS 握手过程，
     *       而 createConnection() 在握手之前被同步调用，此刻读到的必然是空串。服务器统一创建本类，
     *       由它在握手完成后按 TlsSocket::selectedAlpnProtocol() 选协议。
     * @note 明文侧只支持先验知识（RFC 9113 §3.4）：不做前奏嗅探，也不做 RFC 9113 §3.2 已废弃的
     *       HTTP/1.1 Upgrade 流程，因此同一个明文端口不混跑两种协议。要做混跑得先有嗅探（读进前 24 字节
     *       再决定由哪条循环接管），那会让连接在协议未定之前处于「半读」状态，收益与代价不成比例。
     *
     * @note 请求正文按「收齐再路由」处理，与 HTTP/1.1 侧（解析器攒完整条报文才交业务）同口径：
     *       正文超过 HttpParserLimits::maximumBodySize 时停止缓冲并回 413；WebSocket 升级
     *       （RFC 8441 的扩展 CONNECT）不在本片，登记了升级的响应回 501。
     * @note 流式响应（HttpResponse::startChunkedResponse()）在 h2 上照常可用：头部（不含
     *       transfer-encoding 等连接特定头，RFC 9113 §8.2.2）随首段正文上线，此后每段 writeChunk
     *       各发一个 DATA 帧，会话收尾补末片 DATA（END_STREAM）。SseStream 因此零改动即可工作。
     * @note HttpServerLimits::maximumRequestsPerConnection 由本类收口：达到上限即发 GOAWAY
     *       （h2 没有连接级的 close 头可用），既有流继续做完，本侧无在途请求后收口连接。
     * @note 对端一直不回 ACK 本端 SETTINGS（RFC 7540 §6.5.3 的 SETTINGS_TIMEOUT）时，握手期按
     *       HttpServerLimits::settingsAcknowledgementTimeout 约束空闲截止时间：清扫协程到点收口连接，
     *       会话在还能写字节时先尽力把 GOAWAY(SETTINGS_TIMEOUT) 送出去。
     * @note 响应的写出时机：一轮里收齐的请求先全部服务完（各自把响应排进待发字节），随后**一次性写出**。
     *       因此对端深流水线时，某条流的响应延迟会随排在它前面的流数增长——多路复用省的是连接数，
     *       不减少单条请求的排队延迟（要压这一点就得每条响应各刷一次，代价是更多次写出）。
     * @note 对端用 RST_STREAM 取消某条流只影响这条流：响应发送据此只停该流，连接与其它流照旧工作
     *       （h2 的多路复用语义，与 h1 侧「连接级失败」的处置不是一回事）；被取消的条数计入
     *       HttpServerStats::streamCancelledCount，既不算已应答也不算坏请求。
     *
     * @see Http2Connection, HttpSession, Core::TlsSocket
     */
    class Http2Session final : public HttpSession
    {
    public:
        /**
         * @brief 构造 TLS 上的 HTTP/2 会话（ALPN 分流用）。
         * @param loop 事件循环，仅用于给基类造一条不持有描述符的占位套接字
         * @param tlsSocket 已创建但尚未握手的 TlsSocket，所有权转移给本会话
         * @param router 全局路由器，用于分发请求；生命周期必须不短于本会话
         * @param limits 连接级限额的共享只读配置；传空指针表示按 HttpServerLimits 的默认值执行
         * @param metrics 统计采集端；传空指针表示本会话不采集统计
         * @param requestIdGenerator request-id 生成器；传空指针表示不为请求落定 request-id
         * @param parserLimits 解析上限；HTTP/2 路径只用其中的 maximumBodySize（头块上限由
         *        Http2ConnectionConfiguration 管），语义与 HTTP 侧一致
         * @param memoryBudget 在途正文字节的全局预算，与服务器共享；传空指针表示不受该预算约束
         * @param http2Configuration HTTP/2 连接层配置（SETTINGS 通告值、头块与流控上限等）；
         *        留默认值即按 Http2ConnectionConfiguration 的缺省跑
         * @note 构造函数不做握手：握手是协程动作，放在 start() 的第一步
         */
        Http2Session(Core::EventLoop &loop, Core::TlsSocket tlsSocket, Router &router,
                     std::shared_ptr<const HttpServerLimits> limits = nullptr,
                     std::shared_ptr<HttpMetricsCollector> metrics = nullptr,
                     std::shared_ptr<HttpRequestIdGenerator> requestIdGenerator = nullptr,
                     HttpParserLimits parserLimits = {},
                     std::shared_ptr<HttpMemoryBudget> memoryBudget = nullptr,
                     Http2ConnectionConfiguration http2Configuration = {});

        /**
         * @brief 构造明文连接上的 HTTP/2 会话（h2c 先验知识）。
         *
         * @details 套接字直接交给基类持有：明文传输没有第二份通道，不需要 TLS 那样的占位套接字。
         *          本构造函数不做任何协议协商——调用方（HttpServer::createConnection()）已经在按
         *          「这个端口只说 h2」创建会话，因此 start() 直接进 HTTP/2 循环。
         * @param loop 事件循环；本会话只取它的调度器来挂主协程
         * @param socket 已建立的异步套接字，所有权转移给基类
         * @param router 全局路由器，用于分发请求；其生命周期必须不短于本会话
         * @param limits 连接级限额的共享只读配置；传空指针表示按 HttpServerLimits 的默认值执行
         * @param metrics 统计采集端；传空指针表示本会话不采集统计
         * @param requestIdGenerator request-id 生成器；传空指针表示不为请求落定 request-id
         * @param parserLimits 解析上限；HTTP/2 路径只用其中的 maximumBodySize，语义与上一个构造函数一致
         * @param memoryBudget 在途正文字节的全局预算，与服务器共享；传空指针表示不受该预算约束
         * @param http2Configuration HTTP/2 连接层配置，含义与上一个构造函数同名参数一致
         */
        Http2Session(Core::EventLoop &loop, Core::AsyncSocket socket, Router &router,
                     std::shared_ptr<const HttpServerLimits> limits = nullptr,
                     std::shared_ptr<HttpMetricsCollector> metrics = nullptr,
                     std::shared_ptr<HttpRequestIdGenerator> requestIdGenerator = nullptr,
                     HttpParserLimits parserLimits = {},
                     std::shared_ptr<HttpMemoryBudget> memoryBudget = nullptr,
                     Http2ConnectionConfiguration http2Configuration = {});

        /**
         * @brief 启动会话主协程：TLS 会话先握手并按 ALPN 选协议，明文会话直接进 HTTP/2 循环。
         *
         * @details 差异只在入口：TLS 会话第一步做握手（失败即收口），随后按 ALPN 协商出 h2 进本类的
         *          HTTP/2 循环、否则按 HTTP/1.1 事务循环收尾；明文会话没有可选协议，直接进 HTTP/2 循环。
         *          两条路径共用同一个 HTTP/2 循环，只有传输对象不同。
         *
         * @return Core::Task<> 协程任务，连接结束时完成
         * @throws close() 之外的异常不做处理，原样抛给 TcpServer::handleConnection()
         * @see runHttp2Loop()
         */
        Core::Task<> start() override;

        /**
         * @brief 关闭会话：SETTINGS 迟迟未被 ACK 时先把 GOAWAY(SETTINGS_TIMEOUT) 尽力送出去再收口
         *
         * @details 重写基类 close()：TLS 模式下真实描述符归 TlsSocket（基类那条是占位），必须先收 TLS 通道；
         *          明文模式直接走基类。清扫协程按空闲截止时间收口时会先调本函数，此刻描述符还在——这是「会话被
         *          关停」路径上唯一还能告知对端的时刻：SETTINGS 待 ACK 且已过专项限额时先尽力写出
         *          GOAWAY(SETTINGS_TIMEOUT)，写出失败只记日志，连接照常关闭。
         */
        void close() override;

        /**
         * @brief 会话是否仍然可用：基类存活位与传输通道都要健在
         * @details 重写以覆盖 TLS 模式：描述符已关闭时（对端断开、清扫协程收口）必须立刻反映出来
         * @return true 基类存活且当前传输通道的描述符有效
         */
        [[nodiscard]] bool isAlive() const noexcept override;

        /**
         * @brief 取对端地址：TLS 模式下必须问 TlsSocket，明文模式走基类
         * @return std::string 形如 "127.0.0.1:54321" 的地址；取不到时返回空串
         */
        [[nodiscard]] std::string remoteAddress() const override;

        /**
         * @brief 取本端地址：理由同 remoteAddress()
         * @return std::string 形如 "127.0.0.1:8080" 的地址；取不到时返回空串
         */
        [[nodiscard]] std::string localAddress() const override;

        /**
         * @brief 服务器要优雅收口本连接时，发一条收尾 GOAWAY 告诉对端「不再受理新流」
         *
         * @details 重写 onGracefulShutdownRequested()：服务器决定结束无在途工作的连接时、在 close() 之前调用，
         *          此刻通道还可用——对端因此拿到带 last-stream-id 的收尾通告而不是裸 TCP 关闭，据此知道哪些
         *          请求已生效（RFC 9113 §6.8）。本实现直接发最终形态 GOAWAY，不走两段式（连接随后就关，没有
         *          中间状态要留缓冲）。
         * @note 已经发过收尾通告（达到单连接请求上限）或连接已失败/未完成协商时什么都不做：
         *       sendGoAway() 会拒绝，理由已由它的错误出参给出
         * @note 写出是尽力而为：这条路径不等待可写，写不出去只记一条告警，连接照常关闭
         */
        void onGracefulShutdownRequested() override;

    private:
        /// 单条流「窗口不足排队」的正文上界：超了就让流式发送方当场失败，别把内存堆到把进程拖垮。
        /// h1 的等价物是套接字背压（写满就挂住），h2 侧窗口完全由对端控制——对端只读不授窗口时
        /// 队列是唯一还在涨的东西。取 1 MiB：远高于任何正常慢消费者的在途量，又远小于单请求上限
        static constexpr std::size_t kStreamingSendQueueLimitByteCount = 1024U * 1024U;

        /// 整条连接「窗口不足排队」的正文合计上界：单流那道闸乘上并发流数（本端默认 100）仍是一条
        /// 与流数同增的账，逐流各卡一点就能绕过它。取单流上界的八倍——一条大响应的慢消费者照常跑，
        /// 八条流同时不排空才触顶。与 QUIC 侧的 `kMaximumConnectionPendingSendByteCount` 同值同口径
        static constexpr std::size_t kStreamingSendQueueConnectionLimitByteCount = 8U * 1024U * 1024U;

        /**
         * @brief 这条流还有没有地方排队正文：单流与整条连接两道闸一起判
         * @param streamId 目标流号
         * @return true 两道闸都还没触顶，本段可以入队
         * @note 写正文的两处出口（流式正文与 WebSocket 隧道帧）必须走同一个判据：同一阈值在两个
         *       消费点各自解析成两样，就是留一条能绕过闸门的口子
         */
        [[nodiscard]] bool hasSendQueueRoom(std::uint32_t streamId) const noexcept;

        /**
         * @brief 一条请求的服务结论
         *
         * @note 新增取值一律追加在末尾。
         */
        enum class RequestServeOutcome
        {
            Served,            ///< 响应已排入待发字节
            StreamCancelled,   ///< 对端已取消这条流：本条不再有响应，连接继续服务其它流
            ConnectionUnusable,///< 连接不可再用（连接层失败或响应字节写不出去）：调用方应停止循环
            StreamFailed       ///< 本端把这条流按错误中止了（响应不合规或越过对端上限，错在本端）：连接层已写出 RST_STREAM，连接继续服务其它流
        };

        /**
         * @brief 一条头块已收齐的请求：正文随 DATA 片段追加，收齐后（或超限后）交给路由
         * @details 这条流的响应对象与流式正文读取器同样归记录持有（一条流一份），记录摘掉即一并释放
         */
        struct PendingRequest
        {
            HttpRequest request;           ///< 已按 HTTP/1.1 语义映射的请求对象
            std::uint32_t streamId{0};     ///< 请求所属的流号，回响应时按它定位
            bool isRemoteEndStream{false}; ///< 对端是否已 END_STREAM：正文收齐，可以路由
            bool isBodyTooLarge{false};    ///< 正文超过 maximumBodySize：不再缓冲，回 413
            bool isHeaderListTooLarge{false}; ///< 头块超出本端上限：字段全为空，不派发也不缓冲正文，回 431
            bool isBudgetExceeded{false};  ///< 正文超出全局在途预算：不再缓冲，回 503；额度由 bodyBudget 在记录销毁时归还
            bool isExtendedConnect{false}; ///< 该请求带了 :protocol（RFC 8441 的扩展 CONNECT）：没有请求正文，收齐即可路由
            bool isWebSocketTunnel{false}; ///< 其中 :protocol=websocket 的那一类：应答是 200 且这条流随后成为隧道；其余协议值回 501
            bool isStreamingBody{false};   ///< 命中流式路由：头部收齐即派发，正文经 request.bodyStream() 边收边读，不必等 END_STREAM
            /// 本条流的全局正文额度：随记录一起析构，流被摘掉（服务完/被取消/连接关闭）即归还
            HttpMemoryBudget::Reservation bodyBudget;

            /// 流式路由专用的正文缓冲（来源）。按流各持一份而不是全连接共用一份：同一条连接上
            /// 可以同时有多条流在收正文，共用一份会让后来者的 DATA 覆盖前者的未读正文
            HttpStreamBody streamBody;

            // 下面两件与 request/streamBody 同属这条流的记录：一条流一份，填写与发送都在记录内
            // 完成，全连接共用一份会让后来者覆盖前一条流尚未发出的内容
            HttpResponse response;      ///< 本条流的响应对象：路由前本来就是空的，不必为「上一条报文残留」复位
            HttpRequestBody bodyStream; ///< 本条流的流式正文读取器：交付给 request.bodyStream()，来源就是上面那份 streamBody

            /// 等正文的协程（本条流的处理器只有一条，因此至多一个等待者）。由会话在收到这段流的新
            /// 正文、收尾或断开时唤醒——读套接字只有会话循环这一个驱动者，处理器不许自己去读
            std::coroutine_handle<> bodyWaiter{};
            bool isServeClaimed{false};  ///< 已进入服务：处理器协程已创建，或隧道已就地接手
            bool isTaskStarted{false};   ///< 协程已 resume 过第一次（惰性协程创建时停在初始挂起点）
            bool isServeFinished{false}; ///< 处理器协程已跑完，结论在 serveOutcome 里，等会话摘记录
            bool hasPendingWake{false};  ///< 本流有新正文/收尾/断开，等回到安全点唤醒挂着的处理器
            RequestServeOutcome serveOutcome{RequestServeOutcome::Served}; ///< 跑完的结论，由会话摘记录时处置

            /// 本条流的处理器协程。**必须排在记录的最后**：成员按声明逆序销毁，帧要在这条流的
            /// 请求/响应/正文还在时先拆掉（帧里的局部对象按引用使它们）
            std::optional<Core::Task<>> serveTask;
        };

        /**
         * @brief 等这条流的下一次正文到达（或收尾、断开）
         * @details 与 h3 侧 BodyWaitAwaiter 同一形状：h2 的读通路只有会话循环一个驱动者，处理器
         *          不能自己去读一批，只能挂起；新字节到达时只置记录上的标记，回到循环的安全点再由
         *          wakeStreamingRequestWaiters() 唤醒，避免在连接层回调里就地恢复协程。
         */
        class BodyWaitAwaiter
        {
        public:
            /**
             * @brief 绑定要等的那条流
             * @param pending 目标流的记录（活在 m_pendingRequests 里，非拥有）；为空时视为无进展
             */
            explicit BodyWaitAwaiter(PendingRequest *pending) noexcept : m_pending(pending) {}

            /// 已收尾、已断开、或缓冲里还有没交付的字节时不必挂起：调用方回头就能拿到结论
            [[nodiscard]] bool await_ready() const noexcept
            {
                return m_pending == nullptr || m_pending->streamBody.isComplete() || m_pending->streamBody.isBroken()
                       || m_pending->streamBody.pendingByteCount() != 0;
            }

            /// 记下等待者（本条流的处理器只有一条协程，因此至多一个）
            void await_suspend(const std::coroutine_handle<> waiter) const noexcept
            {
                m_pending->bodyWaiter = waiter;
            }

            static void await_resume() noexcept {}

        private:
            PendingRequest *m_pending{nullptr}; ///< 目标流的记录（非拥有）
        };

        /**
         * @brief 写权等待体：写权在别人手上时挂起来，等它放开
         * @details 与客户端侧 Http2ClientConnection 的同一判据：一条通路同一时刻只许一个协程在 send，
         *          否则两条各写一半套接字缓冲，对端解出来的就是撕开的帧；更糟的是第二个等待者会撞上
         *          传输层「一个方向只许一个等待者」的约束，当场把连接判死。
         */
        class FlushTurnAwaiter
        {
        public:
            /// @param session 所属会话（生命周期由本次等待覆盖）
            explicit FlushTurnAwaiter(Http2Session &session) noexcept : m_session(&session) {}

            /// 写权空着就不用挂：调用方会自己去抢这一轮
            [[nodiscard]] bool await_ready() const noexcept { return !m_session->m_isFlushInProgress; }

            /// 把本协程排进写队
            void await_suspend(const std::coroutine_handle<> waiter) const noexcept;

            /// 醒来即完成：接下来由调用方自己再看一眼写权与待发缓冲
            void await_resume() const noexcept {}

        private:
            Http2Session *m_session; ///< 所属会话（非拥有）
        };

        /**
         * @brief 写权的 RAII 放开：析构即让给排队的下一个写者
         */
        class FlushTurnGuard
        {
        public:
            /// @param session 拿走写权的会话
            explicit FlushTurnGuard(Http2Session &session) noexcept : m_session(&session) {}

            FlushTurnGuard(const FlushTurnGuard &) = delete;
            FlushTurnGuard &operator=(const FlushTurnGuard &) = delete;

            /// 放开写权并叫醒排队的写者
            ~FlushTurnGuard() noexcept
            {
                m_session->m_isFlushInProgress = false;
                m_session->wakeFlushWaiters();
            }

        private:
            Http2Session *m_session; ///< 归属会话
        };

        /**
         * @brief HTTP/2 主循环：读字节 → feedBytes() → 写出 → 取请求与正文 → 路由 → 写出
         * @details 循环退出即收口：对端关闭、传输失败、协议失败（GOAWAY 已写出）或本侧被关停。
         * @return Core::Task<> 协程，循环结束时完成
         */
        Core::Task<> runHttp2Loop();

        /**
         * @brief 取走连接层已校验的请求，映射成 HttpRequest 并挂进待服务表
         */
        void absorbPendingRequests();

        /**
         * @brief 取走连接层已收到的正文：追加进对应请求，并按量把接收窗口还回去
         */
        void absorbReceivedData();

        /**
         * @brief 处理单条接收正文：按上限与预算判定后追加进对应请求，并归还接收窗口
         * @details 抽成单条形式是为隧道路径复用：隧道期间读循环由隧道协程驱动，
         *          同连接其它流的正文要按与主循环完全相同的口径累积与还窗口
         * @param receivedData 连接层交出的一条接收数据
         */
        void absorbOneReceivedData(const Http2ReceivedData &receivedData);

        /**
         * @brief 把正文已收齐（或已超限）且尚未发起的请求各自起一条处理器协程
         * @details 一条流一条协程：起完之后本函数**不等业务**，立刻返回给主循环继续读下一批字节。
         *          业务挂起时（等正文、等自己的 I/O）由调度器就地恢复它自己那条协程，会话循环不必陪着等
         *          ——这正是消除队头阻塞的那一步：以前慢处理器会把同连接其它流的请求挡在读通路之外。
         *          隧道不在此列：它要自己驱动这条连接的读写，因此留到 serveWebSocketTunnels() 就地跑。
         */
        void startReadyRequestTasks();

        /**
         * @brief 收掉跑完的处理器协程：按各自结论记数、摘记录
         * @details 摘记录这一步必须留在会话循环里做，不能由协程自己摘——它跑完最后一行时就站在
         *          自己那条记录的成员之上（协程帧按引用使着响应与正文），就地释放等于从舞台上拆地板。
         * @return true 没有连接级的失败
         * @return false 有协程报出「连接不可用」，调用方应停止循环
         */
        [[nodiscard]] bool retireFinishedRequestTasks();

        /**
         * @brief 把「对端已取消这条流」转发给正在等正文的处理器
         * @details 处理器发起之后，未就绪记录的那条清理路径就不再经过它：RST 只有连接层看得见，
         *          不标断不叫醒，业务会永远挂在 bodyStream()->readNext() 上
         */
        void noteCancelledStreams();

        /**
         * @brief 唤醒有新正文、已收尾或已断开的流式请求处理器
         * @details 只在主循环的安全点调用（连接层回调之外）：正文到达发生在 absorb 里，就地恢复
         *          协程会让调用栈在连接层的帧里穿到业务里去
         */
        void wakeStreamingRequestWaiters();

        /**
         * @brief 就地服务隧道请求（它自己驱动这条连接的读写，因此必须排在所有普通派发之后）
         * @details 一条连接同一时刻只许一个读驱动者：隧道开着时再来的扩展 CONNECT 回 503，
         *          这条判据与「会话循环此刻在隧道协程里、不能同时再读一次」是同一件事
         * @return true 隧道正常收尾或没有隧道
         * @return false 写出失败（连接已不可用），调用方应停止循环
         */
        [[nodiscard]] Core::Task<bool> serveWebSocketTunnels();

        /**
         * @brief 叫醒排队等写权的协程
         */
        void wakeFlushWaiters() noexcept;

        /// 收口时每轮等处理器跑完的一拍时长：一拍 1 毫秒，让事件循环有时间推进到期定时器与别的会话
        static constexpr std::chrono::milliseconds kServeDrainTickInterval{1};

        /// 收口时等在飞处理器跑完的让轮上限（≈ 1 秒）。正常一两条就能收完（承载已死，挂在正文上的
        /// 等待都立刻落空），走到上限说明有处理器卡在与会话无关的等待上——此时只记一条错误日志，
        /// 记录**不**摘：它的协程帧还可能停在业务自己的等待上，摘掉等于把活帧留在调度器手里
        static constexpr std::size_t kServeDrainRoundLimit = 1024;

        /**
         * @brief 一条流的处理器协程外壳：跑完把结论写回记录
         * @details 结论写进记录而不是返回给谁：这条协程是分离跑的，起它的主循环不等它，
         *          只在每轮的安全点摘已经跑完的记录（retireFinishedRequestTasks）
         * @param pending 本条流的记录（协程帧按引用使着它，因此摘记录的顺序有讲究）
         * @return Core::Task<> 协程；惰性启动，第一次 resume 才开跑
         */
        [[nodiscard]] Core::Task<> serveRequestTask(PendingRequest &pending);

        /**
         * @brief 为一条已可服务的请求创建处理器协程并排进叫醒队列
         * @param pending 本条流的记录
         */
        void startOneServeTask(PendingRequest &pending);

        /**
         * @brief 会话退出前收拢在飞的处理器：先让它们的每一处等待落空，再等它们跑完自己的收尾
         * @details 带着活帧退出等于把悬空协程留在调度器手里——业务恢复时踩的是已析构的会话与记录。
         *          做法与 h3 侧「先唤醒再销毁」同一条：正文标断、叫醒挂在正文上的协程，然后一拍一拍
         *          等它们自己跑完（每一拍是事件循环上的一小段计时器等待，见 kServeDrainTickInterval）。
         *          业务停在与会话无关的等待上时本函数会一直等下去，只在到上限时记一条错误日志——
         *          摘记录等于把还在跑的协程帧连同它按着的请求与响应一起毁掉。
         */
        [[nodiscard]] Core::Task<> drainInFlightServes();

        /**
         * @brief 服务一条请求：统计、request-id、路由、错误改写与响应发送
         * @param pending 待服务的请求（正文已收齐或已超限）
         * @return RequestServeOutcome 服务结论：排入待发字节 / 对端已取消这条流 / 连接不可再用
         * @note 对端取消是它的正当权利（RFC 9113 §8.1 的 RST_STREAM CANCEL），只停这一条流；
         *       连接不可用与用法错误才让调用方停止循环，两者都已由各自的发送入口记过原因
         */
        [[nodiscard]] Core::Task<RequestServeOutcome> serveOneRequest(PendingRequest &pending);

        /**
         * @brief 按畸形请求处置一次「声明长度 vs 实收长度」：先发 400，再发 RST_STREAM(PROTOCOL_ERROR)
         * @details RFC 7540 §8.1.2.6 三条要连着读：「A request or response is also malformed if the
         *          value of a content-length header field does not equal the sum of the DATA frame
         *          payload lengths that form the body」→「Malformed requests or responses that are
         *          detected MUST be treated as a stream error (Section 5.4.2) of type
         *          PROTOCOL_ERROR」→「For malformed requests, a server MAY send an HTTP response
         *          prior to closing or resetting the stream」。那句 MAY 是嵌在 MUST 之前的许可，
         *          因此两步都要做：只回 400 是不成立的那一支，只发 RST 则白白丢掉了能给客户端的原因。
         *          次序也有讲究：400 的那帧 DATA 不能带 END_STREAM。对端已经 END_STREAM（正文收齐
         *          才谈得上比对长度），本端一发 END_STREAM 流就进 closed，而 §5.1 规定「An endpoint
         *          MUST NOT send frames other than PRIORITY on a closed stream」——RST 就再也发不
         *          出去了。对端凭头部里补齐的 content-length 判正文收齐，不受影响。
         * @param streamId 目标流号
         * @param declaredLength 请求头部声明的正文长度（已由 parseContentLengthValue 解析出来）
         * @param receivedLength 该流实收正文的字节数
         * @param isHeadRequest 本请求是不是 HEAD：HEAD 的响应本就不许带正文，只能发完头就收尾，
         *        那条 RST 也就发不出去了（对端此刻没在等正文，少一个 RST 不影响它判正文收齐）
         * @return RequestServeOutcome 发完之后的结论：已排入待发字节 / 对端已取消这条流 / 连接不可再用。
         *         「对端已取消」的统计与日志留在调用方，与其它几处出口共用同一处记账
         */
        [[nodiscard]] Core::Task<RequestServeOutcome> rejectMalformedBodyLength(std::uint32_t streamId,
                                                                                std::size_t declaredLength,
                                                                                std::size_t receivedLength,
                                                                                bool isHeadRequest);

        /**
         * @brief 记下一条已服务的请求，达到单连接上限时发 GOAWAY 收尾通告
         * @details 上限取自 HttpServerLimits::maximumRequestsPerConnection（0 表示不限）。h2 没有
         *          h1 那种连接级 close 头可用（RFC 9113 §8.2.2 禁止），「不再受理请求」只能由
         *          GOAWAY 表达；通告只在首次触发时发一条，收口由主循环在无在途请求后进行。
         */
        void noteServedRequest();

        /**
         * @brief 把连接层交出的请求映射成 HttpRequest
         * @param http2Request 连接层交出的请求（伪头各自成字段）；其 :path 与整块头部被换进返回值，调用后为空
         * @return HttpRequest 按 HTTP/1.1 语义填好的请求对象
         */
        [[nodiscard]] static HttpRequest mapToHttpRequest(Http2Request &http2Request);

        /**
         * @brief 把 HttpResponse 的头列表整理成 HTTP/2 可发的形式
         * @details 丢掉连接特定头（RFC 9113 §8.2.2 禁止）、逐条保留可重复头，并按 HttpResponse::appendHead()
         *          的同一套规则补齐缺省的内容类型、长度与日期。流式响应例外：正文长度由 DATA 帧决定，
         *          因此既不自动补 content-length，也会丢掉业务后设的那条。
         * @param response 业务填好的响应
         * @return std::vector<HpackHeaderField> 可直接交给 sendResponseHeaders() 的头列表
         */
        [[nodiscard]] static std::vector<HpackHeaderField> collectResponseHeaderFields(const HttpResponse &response);

        /**
         * @brief 在一条流上发出一条响应（头 + 正文）
         * @param streamId 目标流号
         * @param response 业务填好的响应
         * @param isHeadRequest 请求方法是否是 HEAD：置位时只发头，正文一个字节都不发
         * @return Http2ResponseSendStatus 响应头与正文的发送结论：Sent 已排入待发字节（可能还在等窗口），
         *         StreamNotWritable 对端已取消或收尾了这条流，其余取值表示连接不可用或本响应无法应答
         */
        [[nodiscard]] Core::Task<Http2ResponseSendStatus> sendResponse(std::uint32_t streamId, const HttpResponse &response,
                                                                      bool isHeadRequest);

        /**
         * @brief 在一条流上跑 WebSocket 隧道（RFC 8441 的扩展 CONNECT）
         *
         * @details 与 h1 侧 101 升级后的阶段同构，差别只在承载：h1 上字节来自套接字、升级应答是 101；
         *          这里握手应答是一条**不带 END_STREAM 的 200**（RFC 8441 §5，h2 里没有 Upgrade 语义），
         *          此后的字节全部装在 DATA 帧里（本流的负载喂给 WebSocket 解码器，业务写出的帧发成 DATA）。
         *
         * @param streamId 该扩展 CONNECT 所属的流
         * @param pending 待服务的请求（其正文缓冲里可能已有对端在 200 之前抢先发来的帧）
         * @return RequestServeOutcome Served（隧道已按 RFC 6455 收尾）或 ConnectionUnusable
         * @note 隧道期间本协程就是这条连接的驱动者，因此**同连接的其它流照常服务**（见
         *       serveOtherStreamsDuringTunnel()）：普通请求就地 co_await 服务掉，第二条隧道仍回 503
         * @note 隧道在服务阶段**内联跑完**（由 servePendingRequests() 直接 co_await）：读循环与写循环
         *       都在本协程里，主循环在隧道存续期间不再拿回执行权。因此隧道期间本协程就是这条连接的驱动者：
         *       自己读传输字节、喂连接层、把本流的 DATA 交给解码器、归还接收窗口，并调用
         *       serveOtherStreamsDuringTunnel() 就地服务同连接其它流上已收齐的请求
         */
        [[nodiscard]] Core::Task<RequestServeOutcome> serveWebSocketTunnel(std::uint32_t streamId, PendingRequest &pending);

        /**
         * @brief 隧道期间就地服务同连接其它流上已收齐的请求（隧道协程即本连接的驱动者）
         * @details 隧道内联驱动期间，驱动者职责（喂正文、还窗口、服务请求）都由隧道协程承担：
         *          这里把「正文已收齐」的普通请求逐条 co_await 服务掉，响应随即随本轮写出上线。
         *          正文没收齐的流留在表里，等本协程的下一轮读继续攒。
         * @param tunnelStreamId 正在跑隧道的那条流，跳过它
         * @return true 全部就绪请求已服务完；false 连接已不可用，调用方应停止隧道循环
         * @note 第二条隧道仍回 503：一条连接上同时跑两条隧道需要嵌套驱动循环，不在本片范围
         */
        [[nodiscard]] Core::Task<bool> serveOtherStreamsDuringTunnel(std::uint32_t tunnelStreamId);

        /**
         * @brief h2 版流式发送回调：把 HttpResponse::writeChunk() 交出的段落发成 HTTP/2 帧
         *
         * @details 首个段落是 writeChunk 推上来的 HTTP/1.1 头部文本（HttpResponse 按 h1 语义序列化），
         *          在 h2 上只当「头部该上线了」的信号：真正发出的头块按响应对象现取，不带 END_STREAM；
         *          其余段落是 h1 分块帧，剥出负载后作为 DATA 帧发出（不带 END_STREAM）。
         * @param streamId 本段落所属的流号
         * @param response 这条流正在填写的响应（记录持有，头块按它现取）
         * @param segment writeChunk 交出的段落字节
         * @return true 本段已排入待发字节（窗口不足时留在发送队列里，等对端 WINDOW_UPDATE 续发）
         * @return false 本段未发出、业务应停止继续写：对端已取消这条流、待发队列已到
         *         kStreamingSendQueueLimitByteCount（对端长期不发 WINDOW_UPDATE，连接继续服务其它流），
         *         或连接已不可用——各种原因的日志分别由本方法与 serveOneRequest() 记出
         * @throws Base::LogicException 分块帧布局与 writeChunk 的文档不符（本段未发出，绝不把帧头当正文）
         */
        [[nodiscard]] Core::Task<bool> sendStreamingSegment(std::uint32_t streamId, HttpResponse &response,
                                                            std::string_view segment);

        /**
         * @brief 流式响应收尾：给这条流补上 END_STREAM
         *
         * @details 头部已随首段上线时补一个零长 DATA 帧带 END_STREAM（RFC 9113 §6.1 允许零长），
         *          与 h1 侧补 `0\r\n\r\n` 终止块同一个位置；一段正文都没写时头部与 END_STREAM 一起发。
         *          收尾帧立刻写出，对端因此不必等到下一轮读循环才看到消息结尾。
         * @param streamId 目标流号
         * @param response 这条流正在填写的响应（记录持有；一段都没写时头块按它补齐）
         * @return Http2ResponseSendStatus 收尾帧的发送结论；StreamNotWritable 表示对端已取消这条流
         *         （连接继续服务其它流），其余非 Sent 取值表示连接不可用或本响应无法应答
         */
        [[nodiscard]] Core::Task<Http2ResponseSendStatus> finishStreamingResponse(std::uint32_t streamId,
                                                                                 HttpResponse &response);

        /**
         * @brief 把响应状态码收口成可上线的取值
         * @param responseStatus HttpResponse::setStatus() 存下的取值（该接口不校验取值范围）
         * @param streamId 目标流号，只用于日志
         * @return std::uint32_t 可交给 sendResponseHeaders() 的状态码：越界时记一条日志后回 500
         */
        [[nodiscard]] static std::uint32_t normalizeWireStatusCode(int responseStatus, std::uint32_t streamId);

        /**
         * @brief 从 HttpResponse::writeChunk() 交出的分块帧里取出正文负载
         * @details 帧格式由 writeChunk 的文档给出：`<十六进制长度>\r\n<数据>\r\n`（RFC 9112 §7.1）。
         *          长度前缀是负载长度的唯一权威来源，因此负载里出现 CRLF 也不会被误当边界。
         * @param chunkFrame 完整分块帧
         * @return std::string_view 指向 chunkFrame 内部的负载视图
         * @throws Base::LogicException 帧布局与文档不符（长度行非法或字节数与长度前缀不符）
         */
        [[nodiscard]] static std::string_view chunkFramePayload(std::string_view chunkFrame);

        /**
         * @brief 把连接层的待发字节全部写到 TLS 通道上
         * @return true 已写完（没有待发字节也算成功）
         * @return false 传输失败，连接已不可用，调用方应停止循环
         */
        [[nodiscard]] Core::Task<bool> flushOutgoingBytes();

        /**
         * @brief 判「本端 SETTINGS 仍未被 ACK 且已过 HttpServerLimits::settingsAcknowledgementTimeout」
         * @details 期限从连接层记下的发帧时刻起算（不是从此刻重新计满）：不 ACK 却持续发帧的对端
         *          否则能把截止时间一轮轮往后推，SETTINGS_TIMEOUT 形同虚设。
         * @return true 该专项限额生效、SETTINGS 仍待 ACK，且已过期
         * @return false 限额为 0（不设这项保护）、SETTINGS 已被 ACK，或还没到期
         */
        [[nodiscard]] bool isSettingsAcknowledgementExpired() const noexcept;

        /**
         * @brief 取 SETTINGS 待 ACK 期间该刷给空闲截止时间的剩余时长
         * @return std::chrono::milliseconds 从「发帧时刻 + 专项限额」算出的剩余时长，下限 1 毫秒；
         *         该项保护为 0 或 SETTINGS 已被 ACK 时返回 0（「不适用」，调用方按常规相位时限刷新）；
         *         已经过期时为最小正数，让清扫协程在下一拍收口——refreshIdleDeadline(0) 是
         *         「清除截止时间」，不能用它表达「已超时」
         */
        [[nodiscard]] std::chrono::milliseconds settingsAcknowledgementIdleBudget() const noexcept;

        /**
         * @brief 把 GOAWAY(SETTINGS_TIMEOUT) 排进连接层的待发字节并记日志
         * @return true 已排进待发字节（连接层转入失败态），调用方应把它写出去
         * @return false 没有排入：连接层已失败过（GOAWAY 只发一次），原因已记日志
         */
        [[nodiscard]] bool queueSettingsTimeoutGoAway();

        /**
         * @brief 尽力把连接层的待发字节写出去（不挂起）
         *
         * @details 只在 close() 里用：此刻连接已经注定关闭，而会话协程还挂在读等待上。发送写成
         *          异步挂起就放弃剩余字节——挂起的帧随本函数返回销毁，等待器会自行从注册对象摘除；
         *          为一条 GOAWAY 把清扫协程拖住，代价高过对端少收一条收口通告。
         */
        void writeOutgoingBytesBestEffort();

        /**
         * @brief 读一批网络字节并让连接层消化：主循环与流式正文的泵共用这一推进
         *
         * @details 一步做完「刷新读时限 → 读传输 → 喂连接层 → 写出待发字节 → 吸收请求与正文」。
         *          抽出来是为了让流式正文的泵与主循环走同一条路径：处理器拉正文时驱动连接，
         *          与主循环每轮所做的事必须完全一致，否则窗口、状态机与待发字节会分叉。
         * @return true 连接仍可用；false 读失败、对端关闭或协议失败（GOAWAY 已尽力写出），调用方应停止
         */
        [[nodiscard]] Core::Task<bool> driveConnectionOnce();

        /**
         * @brief 收尾一条流式正文的流：把还挂着的正文按已消费处理，并对端未收尾时中止这条流
         *
         * @details 业务可能在正文读完之前就返回（它有权这样做）。此时不能把流晾着：未交付的字节
         *          要归还接收窗口，而对端若还在发，本端已经不需要了——按 RFC 9113 §8.1 用
         *          RST_STREAM(NO_ERROR) 请它停下，省掉把剩余字节全部收下再丢掉的带宽浪费。
         *
         * @warning 只能在响应已排入待发字节之后调用：RST_STREAM 与响应排在同一条待发字节流里，
         *          先中止就会让对端先看到 RST，响应反而到不了。
         * @param pending 该流对应的待服务记录（正文来源与流号都从它取）
         * @param isBodyTooLarge 是否因正文越过上限而收尾：是则中止原因写体量越界
         */
        void finishStreamingRequestBody(PendingRequest &pending, bool isBodyTooLarge);

        /**
         * @brief 把连接层的响应发送结论折叠成会话的服务结论
         * @param sendStatus 连接层给出的响应发送结论
         * @return RequestServeOutcome Sent → Served，StreamNotWritable → StreamCancelled，
         *         ConnectionUnavailable 与 Rejected → ConnectionUnusable
         */
        [[nodiscard]] static RequestServeOutcome toRequestServeOutcome(Http2ResponseSendStatus sendStatus) noexcept;

        /**
         * @brief 当前传输通道是否还开着：TLS 会话看 TLS 描述符，明文会话看基类套接字
         * @return true 描述符有效（TLS 模式下 SSL 对象仍在、底层描述符没被关掉）
         */
        [[nodiscard]] bool isTransportOpen() const noexcept;

        /**
         * @brief 取当前传输通道的描述符
         * @return int 描述符；通道已关时返回 -1。只用于日志与端口判断
         */
        [[nodiscard]] int transportFileDescriptor() const noexcept;

        /**
         * @brief 从当前传输通道读一段字节
         * @param buffer 接收缓冲
         * @param length 缓冲长度
         * @return Core::Task<ssize_t> 实际读到的字节数；0 表示对端正常关闭，负值表示连接不可用
         */
        [[nodiscard]] Core::Task<ssize_t> transportReceive(void *buffer, std::size_t length);

        /**
         * @brief 向当前传输通道写一段字节（允许部分写）
         * @param buffer 待发数据，按「指针 + 长度」取，可含 NUL
         * @param length 数据长度
         * @return Core::Task<ssize_t> 实际写出的字节数
         */
        [[nodiscard]] Core::Task<ssize_t> transportSend(const void *buffer, std::size_t length);

        /// TLS 通道；空表示本会话跑在明文连接上（h2c 先验知识），此时真实描述符归基类套接字所有
        std::optional<Core::TlsSocket> m_tlsSocket;

        /// HTTP/1.1 回退路径（TLS 上 ALPN 未协商出 h2 时）的解析器与接收窗口。
        /// 基类的同名成员是私有的，因此回退路径各持一份，两条路径不共用状态
        HttpParser m_parser;                  ///< HTTP/1.1 回退路径的解析器
        std::vector<char> m_receiveBuffer;    ///< 回退路径自己的接收窗口

        Http2Connection m_connection;                 ///< HTTP/2 连接层状态机（协议状态、帧与窗口全在它里面）
        Core::EventLoop &m_loop;                      ///< 本会话所在的事件循环：收口时要在它上面拍一小段（见 drainInFlightServes）
        Core::Scheduler &m_scheduler;                     ///< 本会话所在事件循环的调度器
        Router &m_router;                             ///< 路由器引用（与基类指向同一对象）
        HttpParserLimits m_parserLimits{};            ///< HTTP/2 路径只用 maximumBodySize，其余字段不适用
        std::shared_ptr<const HttpServerLimits> m_limits; ///< 连接级限额，与服务器共享、只读（构造时保证非空）
        std::shared_ptr<HttpMetricsCollector> m_metrics;  ///< 统计采集端；空指针表示不采集
        std::shared_ptr<HttpRequestIdGenerator> m_requestIdGenerator; ///< request-id 生成器；空指针表示不落定
        std::shared_ptr<HttpMemoryBudget> m_memoryBudget; ///< 在途正文字节的全局预算，与服务器共享；空指针表示不受该预算约束

        /// h2 主循环的接收缓冲：提到成员上是因为流式正文的泵也要用它——泵与主循环交替驱动
        /// 同一条连接，各持一份会让「谁读到什么」变得不可推理
        std::vector<char> m_http2ReceiveBuffer;

        /// 头块已收齐的请求：按流号（对端流号严格递增，因此遍历顺序就是请求的到达顺序）
        /// 响应对象与流式正文读取器都在记录里，一条流一份
        std::map<std::uint32_t, PendingRequest> m_pendingRequests;
        std::size_t m_servedRequestCount{0};          ///< 本连接已服务的请求条数（单连接上限的判据）
        bool m_isGoAwaySent{false};                   ///< 是否已因达到请求上限发过收尾 GOAWAY：同一原因只发一条
        /// 在飞的处理器协程条数：一条连接可以同时有多条流在服务，「忙」因此要计数而不是布尔量——
        /// 第一条起时置忙、最后一条收尾才置闲，否则先跑完的那条会把还在等业务的本连接交回空闲清扫
        std::size_t m_activeServeCount{0};
        /// 写权是否已被某个协程拿走：一条通路同一时刻只许一个协程在 send（两个协程各写一半会把帧撕开，
        /// 而且传输层一个方向只许一个等待者，抢槽会当场把连接判死）
        bool m_isFlushInProgress{false};
        std::vector<std::coroutine_handle<>> m_flushWaiters; ///< 排队等写权的协程（放开时一次性叫醒）
        bool m_isConnectionUnusable{false};           ///< 本侧是否已判定写不出去：置位后所有写出短路，同一次故障只留一条日志
    };
} // namespace AsynGyanis::Net

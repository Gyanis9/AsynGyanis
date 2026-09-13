/**
 * @file Http2Session.h
 * @brief HTTP/2 会话：先完成 TLS 握手，再按 ALPN 协商结果选择协议循环
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/Tls/TlsSocket.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpRequestId.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/HttpServerStats.h"
#include "Net/Http/HttpSession.h"
#include "Net/Http/Router.h"
#include "Net/Http2/Http2Connection.h"

#include <cstddef>
#include <cstdint>
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
     * @details 两种构造方式决定传输与进入协议循环的方式：
     *          - **TLS（ALPN 分流）**：start() 先做 TLS 握手，再按 ALPN 协商结果选协议；协商出 h2 就跑
     *            本类的 HTTP/2 循环，否则把连接原样交回 HTTP/1.1 事务循环（与 HttpsSession 同一份实现）。
     *          - **明文（h2c，先验知识）**：连接按 RFC 9113 §3.4 的前奏直接进入 HTTP/2 循环，不做探测、
     *            不提供 HTTP/1.1 回退——端口上的协议由部署决定（见 HttpServer::setHttp2CleartextEnabled()）。
     *          HTTP/2 循环的驱动顺序是「读字节 → feedBytes() → 立刻写出（SETTINGS/ACK/WINDOW_UPDATE/
     *          GOAWAY）→ 取请求与正文 → 路由 → sendResponseHeaders()/sendResponseData() → 再写出」，
     *          与连接层的文档约定一致；协议循环与传输无关，只通过 isTransportOpen()/transportReceive()/
     *          transportSend() 三处用到传输层。
     *
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
         * @param loop 事件循环，仅用于给基类造一条不持有描述符的占位套接字（与 HttpsSession 同）
         * @param tlsSocket 已创建但尚未握手的 TlsSocket，所有权转移给本会话
         * @param router 全局路由器，用于分发请求；生命周期必须不短于本会话
         * @param limits 连接级限额的共享只读配置；传空指针表示按 HttpServerLimits 的默认值执行
         * @param metrics 统计采集端；传空指针表示本会话不采集统计
         * @param requestIdGenerator request-id 生成器；传空指针表示不为请求落定 request-id
         * @param parserLimits 解析上限；HTTP/2 路径只用其中的 maximumBodySize（头块上限由
         *        Http2ConnectionConfiguration 管），语义与 HTTP 侧一致
         * @note 构造函数不做握手：握手是协程动作，放在 start() 的第一步
         */
        Http2Session(Core::EventLoop &loop, Core::TlsSocket tlsSocket, Router &router,
                     std::shared_ptr<const HttpServerLimits> limits = nullptr,
                     std::shared_ptr<HttpMetricsCollector> metrics = nullptr,
                     std::shared_ptr<HttpRequestIdGenerator> requestIdGenerator = nullptr,
                     HttpParserLimits parserLimits = {});

        /**
         * @brief 构造明文连接上的 HTTP/2 会话（h2c 先验知识）。
         *
         * @details 套接字直接交给基类持有：明文传输没有第二份通道，不需要 TLS 那样的占位套接字。
         *          本构造函数不做任何协议协商——调用方（HttpServer::createConnection()）已经在按
         *          「这个端口只说 h2」创建会话，因此 start() 直接进 HTTP/2 循环。
         * @param socket 已建立的异步套接字，所有权转移给基类
         * @param router 全局路由器；其它参数含义同上一个构造函数
         */
        Http2Session(Core::AsyncSocket socket, Router &router,
                     std::shared_ptr<const HttpServerLimits> limits = nullptr,
                     std::shared_ptr<HttpMetricsCollector> metrics = nullptr,
                     std::shared_ptr<HttpRequestIdGenerator> requestIdGenerator = nullptr,
                     HttpParserLimits parserLimits = {});

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
         * @details 重写 Core::Connection::close()：基类只会关自己那条套接字，而本类在 TLS 模式下
         *          真实描述符归 TlsSocket 所有（基类那条是占位），必须先收 TLS 通道；明文模式下基类
         *          那条就是真实套接字，直接走基类即可。
         *
         *          另外，清扫协程按空闲截止时间收口本连接时会先调用本函数，此刻描述符还在——会话自己的
         *          读协程要等描述符关闭才被唤醒，那时一个字节都写不出去。因此这里是「会话被关停」路径上
         *          唯一还能把收口原因告知对端的时刻：SETTINGS 仍待 ACK 且已过专项限额时先把 GOAWAY
         *          (SETTINGS_TIMEOUT) 写出去（RFC 7540 §6.5.3 允许按该错误码收口），写出失败只记日志，
         *          连接照常关闭。
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

    private:
        /**
         * @brief 一条请求的服务结论
         *
         * @note 新增取值一律追加在末尾。
         */
        enum class RequestServeOutcome
        {
            Served,            ///< 响应已排入待发字节
            StreamCancelled,   ///< 对端已取消这条流：本条不再有响应，连接继续服务其它流
            ConnectionUnusable ///< 连接不可再用（连接层失败、写出失败，或本条响应因用法错误无法应答）：调用方应停止循环
        };

        /**
         * @brief 一条头块已收齐的请求：正文随 DATA 片段追加，收齐后（或超限后）交给路由
         */
        struct PendingRequest
        {
            HttpRequest request;           ///< 已按 HTTP/1.1 语义映射的请求对象
            std::uint32_t streamId{0};     ///< 请求所属的流号，回响应时按它定位
            bool isRemoteEndStream{false}; ///< 对端是否已 END_STREAM：正文收齐，可以路由
            bool isBodyTooLarge{false};    ///< 正文超过 maximumBodySize：不再缓冲，回 413
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
         * @brief 把正文已收齐（或已超限）的请求逐条交给路由并发送响应
         * @return true 全部已服务的响应都排入待发字节
         * @return false 写出失败（连接已不可用），调用方应停止循环
         */
        [[nodiscard]] Core::Task<bool> servePendingRequests();

        /**
         * @brief 服务一条请求：统计、request-id、路由、错误改写与响应发送
         * @param pending 待服务的请求（正文已收齐或已超限）
         * @return RequestServeOutcome 服务结论：排入待发字节 / 对端已取消这条流 / 连接不可再用
         * @note 对端取消是它的正当权利（RFC 9113 §8.1 的 RST_STREAM CANCEL），只停这一条流；
         *       连接不可用与用法错误才让调用方停止循环，两者都已由各自的发送入口记过原因
         */
        [[nodiscard]] Core::Task<RequestServeOutcome> serveOneRequest(PendingRequest &pending);

        /**
         * @brief 记下一条已服务的请求，达到单连接上限时发 GOAWAY 收尾通告
         * @details 上限取自 HttpServerLimits::maximumRequestsPerConnection（0 表示不限）。h2 没有
         *          h1 那种连接级 close 头可用（RFC 9113 §8.2.2 禁止），「不再受理请求」只能由
         *          GOAWAY 表达；通告只在首次触发时发一条，收口由主循环在无在途请求后进行。
         */
        void noteServedRequest();

        /**
         * @brief 把连接层交出的请求映射成 HttpRequest
         * @param http2Request 连接层交出的请求（伪头各自成字段）
         * @return HttpRequest 按 HTTP/1.1 语义填好的请求对象
         */
        [[nodiscard]] static HttpRequest mapToHttpRequest(const Http2Request &http2Request);

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
         * @brief h2 版流式发送回调：把 HttpResponse::writeChunk() 交出的段落发成 HTTP/2 帧
         *
         * @details 首个段落是 writeChunk 推上来的 HTTP/1.1 头部文本（HttpResponse 按 h1 语义序列化），
         *          在 h2 上只当「头部该上线了」的信号：真正发出的头块按响应对象现取，不带 END_STREAM；
         *          其余段落是 h1 分块帧，剥出负载后作为 DATA 帧发出（不带 END_STREAM）。
         * @param streamId 本段落所属的流号
         * @param segment writeChunk 交出的段落字节
         * @return true 本段已排入待发字节（窗口不足时留在发送队列里，等对端 WINDOW_UPDATE 续发）
         * @return false 本段未发出、业务应停止继续写：对端已取消这条流（连接继续服务其它流），
         *         或连接已不可用——两种原因的日志分别由本方法与 serveOneRequest() 记出
         * @throws Base::LogicException 分块帧布局与 writeChunk 的文档不符（本段未发出，绝不把帧头当正文）
         */
        [[nodiscard]] Core::Task<bool> sendStreamingSegment(std::uint32_t streamId, std::string_view segment);

        /**
         * @brief 流式响应收尾：给这条流补上 END_STREAM
         *
         * @details 头部已随首段上线时补一个零长 DATA 帧带 END_STREAM（RFC 9113 §6.1 允许零长），
         *          与 h1 侧补 `0\r\n\r\n` 终止块同一个位置；一段正文都没写时头部与 END_STREAM 一起发。
         *          收尾帧立刻写出，对端因此不必等到下一轮读循环才看到消息结尾。
         * @param streamId 目标流号
         * @return Http2ResponseSendStatus 收尾帧的发送结论；StreamNotWritable 表示对端已取消这条流
         *         （连接继续服务其它流），其余非 Sent 取值表示连接不可用或本响应无法应答
         */
        [[nodiscard]] Core::Task<Http2ResponseSendStatus> finishStreamingResponse(std::uint32_t streamId);

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
        /// 基类的同名成员是私有的，且与 HttpsSession 同一做法：回退路径各持一份，两条路径不共用状态
        HttpParser m_parser;
        std::vector<char> m_receiveBuffer;

        Http2Connection m_connection;                 ///< HTTP/2 连接层状态机（协议状态、帧与窗口全在它里面）
        Router &m_router;                             ///< 路由器引用（与基类指向同一对象）
        HttpParserLimits m_parserLimits{};            ///< HTTP/2 路径只用 maximumBodySize，其余字段不适用
        std::shared_ptr<const HttpServerLimits> m_limits; ///< 连接级限额，与服务器共享、只读（构造时保证非空）
        std::shared_ptr<HttpMetricsCollector> m_metrics;  ///< 统计采集端；空指针表示不采集
        std::shared_ptr<HttpRequestIdGenerator> m_requestIdGenerator; ///< request-id 生成器；空指针表示不落定

        /// 头块已收齐的请求：按流号（对端流号严格递增，因此遍历顺序就是请求的到达顺序）
        std::map<std::uint32_t, PendingRequest> m_pendingRequests;
        HttpResponse m_response;                      ///< 响应对象按连接复用，每条请求发送前 reset()
        std::size_t m_servedRequestCount{0};          ///< 本连接已服务的请求条数（单连接上限的判据）
        bool m_isGoAwaySent{false};                   ///< 是否已因达到请求上限发过收尾 GOAWAY：同一原因只发一条
        HttpRequest *m_servingRequest{nullptr};       ///< 正在路由的请求（连接关停时对它转成协作式取消）
        bool m_isConnectionUnusable{false};           ///< 本侧是否已判定写不出去：置位后所有写出短路，同一次故障只留一条日志
    };
} // namespace AsynGyanis::Net

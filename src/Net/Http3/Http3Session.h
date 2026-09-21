/**
 * @file Http3Session.h
 * @brief 一条 QUIC 连接上的 HTTP/3 会话：接上自研的 HTTP/3 连接层，把 h3 请求映射到既有 HTTP 层
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/HttpRequestId.h"
#include "Net/Http/HttpServerStats.h"
#include "Net/Http/HttpMemoryBudget.h"
#include "Net/Http/HttpRequestBody.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpStreamBody.h"
#include "Net/Http3/Http3Error.h"
#include "Net/WebSocket/WebSocketPeer.h"

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    class Router;
    class Http3Connection;

    /**
     * @brief 一条 QUIC 连接上的 HTTP/3 会话
     *
     * @details 与 HTTP/2 侧 `Http2Session` 的位置对应：都是「一条连接上的多路复用」，区别只是多路的
     *          载体从 TCP 帧换成了 QUIC 流。h3 这一层里，帧的组装与拆分、QPACK 的编解码都由本仓库的
     *          `Http3Connection` 负责，本类把它的通知接成业务：把收全的请求交给既有 `Router`——
     *          业务处理器与 h1/h2 完全同一份，不需要为 h3 另写一套。
     *
     * @note 控制流与 QPACK 编解码流都是**本端发起的单向流**：连接层在构造时就开出这三条流、发掉
     *       SETTINGS 并接上 QPACK 两侧（RFC 9114 §6.2.1），流号由传输层给（`StreamOpener`），本类不碰。
     * @warning 线程契约与连接一致：本对象只在其所属事件循环线程上使用。
     */
    class Http3Session
    {
    public:
        /// 开一条本端发起的单向流并返回流号（由 QuicConnection 提供；失败返回 -1）
        using StreamOpener = std::function<std::int64_t()>;

        /// 待发流数据的出口（由 QuicConnection 提供，内部就是 queueStreamData）
        using StreamWriter = std::function<void(std::int64_t streamId, std::span<const std::uint8_t> data, bool endStream)>;

        /// 把已消费的字节归还给 QUIC 的接收窗口（参数：流号、本次可再收的字节数）
        using StreamCrediter = std::function<void(std::int64_t streamId, std::size_t consumedByteCount)>;

        /// 收口一条流：本端不再发、也请对端别再发（参数：流号、RFC 9114 §8.1 那一档的应用错误码）
        using StreamAborter = std::function<void(std::int64_t streamId, std::uint64_t applicationErrorCode)>;

        /// 时限判定用的时刻：与承载层节拍循环同一个时钟（steady_clock），用例因此能精确复现「过点」
        using Deadline = std::chrono::steady_clock::time_point;

        /**
         * @brief 一条正在流式写出响应的流（startChunkedResponse + writeChunk 那条路）
         * @note 正文是一段一段推给连接层的，本结构只记「响应头上线没有、写完没有、流还在不在」；
         *       还挂在连接层里没交出去的字节由 `Http3Connection::pendingOutputByteCount` 给出
         */
        struct StreamingResponse
        {
            bool                    isHeadSent{false};     ///< 响应头是否已提交
            bool                    isFinished{false};     ///< 处理器已写完（正文到此为止，收尾字节已交出）
            bool                    isStreamClosed{false}; ///< 承载侧的流已关闭：生产者据此收手，不再等下一次唤醒
            std::coroutine_handle<> spaceWaiter{};         ///< 生产者等缓冲排空时挂在这里
        };

        /**
         * @brief 建立一个 HTTP/3 服务端会话
         * @param opener 单向流的开流口
         * @param writer 流数据出口
         * @param crediter 接收窗口的归还口（可空：为空时不归还，正文一大就会把接收窗口用光）
         * @param metrics 统计采集端；传空指针表示本会话不采集统计
         * @param memoryBudget 进程级内存预算；有它时缓冲的请求正文按字节占全局额度，传空指针表示不做全局占用（仍受单请求上限约束）
         * @param requestIdGenerator request-id 生成器；传空指针表示本会话不为请求落定 request-id
         *        （与 h1/h2 同一取舍：id 由服务器持有、按 shared_ptr 共享，前缀标识服务器实例）
         * @param aborter 收口一条流的出口（可空：为空时本会话只丢掉自己的记账，对端收不到任何信号，
         *        只能等那条流随连接一起没掉。GOAWAY 之后拒收与越界请求回完响应后的「别再发了」都靠它落实）
         * @note 构造里就把 HTTP/3 连接层建起来：三条本端单向流、SETTINGS 与 QPACK 两侧都在那时接上。
         *       开流失败只记日志并让会话保持不可用（`isUsable()` 为假），不抛异常：
         *       一条连接建不起 h3 不该把服务端拖垮
         * @note 采集口径与 h1/h2 对齐：请求数与 413/协议性拒绝计入，响应按状态码类计数，
         *       被对端 RESET_STREAM 取消的流计入「单流取消」。**耗时直方图不参与**——
         *       h3 各流由传输层驱动，会话没有「收到完整请求」那一刻的戳，宁可不记也不用 0 秒糊弄
         */
        Http3Session(StreamOpener opener, StreamWriter writer, StreamCrediter crediter = {},
                     std::shared_ptr<HttpMetricsCollector> metrics = nullptr,
                     std::shared_ptr<HttpMemoryBudget> memoryBudget = nullptr,
                     std::shared_ptr<HttpRequestIdGenerator> requestIdGenerator = nullptr,
                     StreamAborter aborter = {});

        /**
         * @brief 析构会话：连接层与它持有的 QPACK 两侧动态表随本类一并释放
         * @note 声明在这里、定义在 .cpp：连接层只做了前向声明，析构要见到完整类型才能销毁那个 unique_ptr
         */
        ~Http3Session();

        // 禁拷贝：本类持有各流的派发协程与隧道对端对象，复制一份会让两条副本抢同一条 QUIC 流的字节
        Http3Session(const Http3Session &) = delete;

        Http3Session &operator=(const Http3Session &) = delete;

        /**
         * @brief 接上路由器：收全的请求按 h1/h2 同一套路由与处理器派发
         * @param router 路由器；**必须活得比本会话久**（由服务端持有，本类只存指针）
         * @note 不接路由器时会话照常收发，但请求会得到 503（明确失败，不静默丢弃）
         */
        void attachRouter(Router &router) noexcept;

        /**
         * @brief 设置请求解析上限（正文总量上限等），与 h1/h2 同一套配置
         * @param limits 解析上限；不设置时用 HttpParserLimits 的默认值
         * @note 必须在收到第一个请求之前设置（服务端在装配会话时调用）
         */
        void setParserLimits(HttpParserLimits limits) noexcept;

        /**
         * @brief 设置连接级限额（单连接请求条数上限、收请求的时限），与 h1/h2 同一份配置
         * @param limits 限额快照；传空指针表示不设（本会话不因请求条数收口，也不按读时限收口）
         * @note 达到请求条数上限后的处置与 h1/h2 不同处只在承载：这里发 GOAWAY 让对端换连接，
         *       在途请求做完后由承载层收掉这条连接
         */
        void setServerLimits(std::shared_ptr<const HttpServerLimits> limits) noexcept;

        /**
         * @brief 按时限收口「收不全」的请求：一段时段内没有新字节的流不再等下去
         * @details 由承载层的节拍循环按拍调用（h3 会话没有套接字可等，也没有自己的定时器）。
         *          h1/h2 撞到这个时限是掐掉整条连接，这里只处置那一条流（RESET_STREAM +
         *          STOP_SENDING）：多路复用是 h3 的常态，一条慢客户端不该连坐其它请求。
         *          处理器相位刻意不判：流式响应与隧道本就该长期挂着，没有「产出即刷新」的钩子。
         * @param now 本拍时刻（由调用方注入，用例据此精确复现「过点」）
         */
        void expireStaleRequests(Deadline now);

        /**
         * @brief 是否「已通告排空且手上没活」：承载层据此可以收掉这条连接
         * @details 两个条件都要——只看过 GOAWAY 但还有请求在跑就收，等于把在途响应丢掉
         */
        [[nodiscard]] bool isDrainedAndFinished() const noexcept;

        /**
         * @brief 会话是否可用（三条本端单向流都开出来了）
         * @return true 可用
         */
        [[nodiscard]] bool isUsable() const noexcept;

        /**
         * @brief 会话是否已作废
         * @return true 已作废（HTTP/3 连接层判定协议错误），此后唯一合法的动作是销毁
         */
        [[nodiscard]] bool isBroken() const noexcept;

        /**
         * @brief 把对端在一条流上送来的字节交给 HTTP/3 层
         * @param streamId 流号
         * @param data 本段字节
         * @param isEndStream 对端在这段之后收尾
         * @note 除 DATA 载荷之外的接收额度由连接层就地归还；DATA 载荷的额度归本会话按流式与否决定
         */
        void onStreamData(std::int64_t streamId, std::span<const std::uint8_t> data, bool isEndStream);

        /**
         * @brief 把 HTTP/3 层攒下的待发字节交给传输层
         */
        void flushPendingStreamData();

        /**
         * @brief 跑完排队中的请求（含路由与业务处理器）并把响应与攒下的字节发出去
         * @details 路由与业务处理器是协程（可能去等磁盘、等上游），而连接层的通知是同步的，
         *          因此「收全请求」与「派发业务」拆成两步：同步回调只入队，本协程再逐个 co_await。
         *          传输层每处理完一条报文调一次即可。
         * @return Core::Task<> 派发与发送完成
         */
        [[nodiscard]] Core::Task<> pump();

        /**
         * @brief 开始优雅收口：向对端发 GOAWAY，此后不再受理新请求流
         * @details 与 h2 侧的 `onGracefulShutdownRequested()` 同一职责：关停路径上给协议层最后一次
         *          「告诉对端」的机会，随后 `hasOutstandingWork()` 归零或期限到点再由承载层收连接。
         *          已经受理的请求照常处理完——通告值取的正是「最后一条已受理流之后的下一条流号」。
         * @return true 已把通告（或此前已发过）排进待发字节；false 连接层不可用，本会话发不出东西
         */
        bool beginGracefulShutdown();

        /**
         * @brief 这条连接上是否还有没做完的事（收集中的请求、待派发、流式正文、流式响应、隧道）
         * @details 收口时按它决定「这条连接可以关了吗」；一条都不剩才算空闲
         */
        [[nodiscard]] bool hasOutstandingWork() const noexcept;

        /**
         * @brief 承载连接已经没了：收掉本会话所有还没答完的流，并叫醒挂在上面的业务协程
         * @details 传输层按空闲上限收口时不逐条流发 RESET/STOP，本会话拿不到「这条流结束」的信号。
         *          缺这一步，挂在 `readNext()` / `receive()` 上的协程帧会随会话一起被销毁，等待之后的
         *          收尾永不执行；而已把恢复动作投回循环的在途等待（如外置到工作线程的响应压缩）会指向
         *          已释放的帧。唤醒只交一次；承载层每拍重复调用，用于收敛那些等业务跑完才能摘的记录
         */
        void abandonPendingStreams();

        // ---- 以下几项由 .cpp 里接连接层回调的转交（只收平类型，连接层的结构不外泄）----

        /**
         * @brief 记下一个请求头
         * @param streamId 流号
         * @param name 头名（HTTP/3 里一律小写）
         * @param value 头值
         */
        void addRequestHeader(std::int64_t streamId, std::string name, std::string value);

        /**
         * @brief 记下一段请求正文
         * @param streamId 流号
         * @param data 本段字节
         */
        void addRequestBody(std::int64_t streamId, std::span<const std::uint8_t> data);

        /**
         * @brief 一条流上的请求收全：整理成 HttpRequest 后排队等派发
         * @param streamId 流号
         */
        void finishRequest(std::int64_t streamId);

        /**
         * @brief 对端重置了一条流：还没答完的那条计入「单流取消」
         * @details 只把「还没答完就取消」算成取消——响应早已发完、事后被重置的流不该记进来
         *          （与 h2 同一判据：那边只在响应发不出去、原因是流被取消时计数）
         * @param streamId 被重置的流
         */
        void noteStreamResetByPeer(std::int64_t streamId) noexcept;

        /**
         * @brief 把一条流的收口信号交给传输层：本端不再发、也请对端别再发
         * @details 错误码原样写进 RESET_STREAM 与 STOP_SENDING；两个方向各由流层自己判断该不该发
         *          （FIN 已经上线的那一头不必复位，本端只能收的那一头不必请对端停发）。没接这个
         *          口子时只丢本端记账——对端一个字节也收不到，只能等那条流随连接一起没掉
         * @param streamId 要收口的流
         * @param errorCode 写进帧里的错误码（RFC 9114 §8.1 那一档）
         */
        void abortRequestStream(std::int64_t streamId, Http3ErrorCode errorCode);

        /**
         * @brief 下一次「该有进展」的时刻：从此刻按 readTimeout 往后推
         * @return Deadline 没设限额、或时限为 0（表示关闭这项保护）时给时钟上限，即永不过点
         */
        [[nodiscard]] Deadline nextRequestDeadline() const noexcept;

        /**
         * @brief 丢掉一条流上尚未收全的请求（流被重置或关闭）
         * @param streamId 流号
         */
        void dropRequest(std::int64_t streamId);

        /**
         * @brief 承载层通知：对端取消了一条流（RESET_STREAM 或 STOP_SENDING）
         *
         * @details HTTP/3 连接层自己看不到 QUIC 层的重置信号，只有被明确告知才会释放该流的状态；少了这一路，
         *          「对端取消一条已发正文的 POST」会让请求缓冲、流式等待者与隧道记录永久驻留
         *          （传输层归还的 MAX_STREAMS 额度还允许对端反复重来）。
         * @param streamId 被对端取消的流
         * @note 本函数只记下流号：它由传输层的流回调调用，而回调期间动连接层、唤醒业务协程都属
         *       「回调期间重入」；真正的回收在下一个安全点 pump() 里做（见 drainPeerCancelledStreams）
         */
        void cancelStreamByPeer(std::int64_t streamId);

        /**
         * @brief 头收齐时判一下：命中流式正文路由就提前派发（正文边收边交，不等整份收齐）
         * @param streamId 流号
         * @note 由 .cpp 里「头块收齐」的通知转交：那时方法/路径已可判，正文还在路上
         */
        void beginStreamingRequestIfMatched(std::int64_t streamId);

    private:
        /// HTTP/3 请求在 HttpRequest 里记下的版本号（业务读 httpVersion() 时与 h1/h2 同口径）
        static constexpr const char *kHttp3RequestVersion = "HTTP/3";

        /// 正在接收的一条请求
        struct IncomingRequest
        {
            HttpRequest request;    ///< 逐步填好的请求（头部在收头时写入）
            std::string method;     ///< :method 原文
            std::string path;       ///< :path 原文
            std::string authority;  ///< :authority 原文
            std::string protocol;   ///< :protocol 原文（RFC 9220 扩展 CONNECT 用；普通请求为空）
            std::string body;       ///< 正文（非流式路径：整段收齐后才派发；流式路径不从这里走）
            bool        hasHostHeader{false}; ///< 对端是否显式给了 host 头

            /// 正文总量越过 HttpParserLimits::maximumBodySize：此后到达的 DATA 一律丢弃，
            /// 服务阶段按 413 应答（与 h1/h2 同一口径）
            bool isBodyTooLarge{false};

            /// 正文超出全局在途预算：此后到达的 DATA 一律丢弃，服务阶段按 503 应答
            /// （额度由 bodyBudget 在记录销毁时归还）
            bool isBudgetExceeded{false};

            /// 本条流已缓冲正文占用的全局额度：随记录一起析构即归还
            HttpMemoryBudget::Reservation bodyBudget;

            /// 以下四项的口径与 h1 的 HttpParserLimits 一致：越限即置位，服务阶段按 431/414 应答
            std::size_t headerFieldCount{0};          ///< 已收到的头字段条数
            std::size_t headerBlockByteCount{0};      ///< 头块净字节（只算名与值的长度，不含帧头）
            bool        isHeaderLimitExceeded{false}; ///< 条数、单名/单值长度或整块净字节越过上限
            bool        isUriTooLong{false};          ///< :path 长度越过请求目标上限

            /// 下一次「该有进展」的时刻：每收到一段请求就按 readTimeout 往后推，过点即收口这条流
            Deadline deadline{std::chrono::steady_clock::now()};
        };

        /**
         * @brief 一条流式请求的本地状态：本流的正文来源、读取器，以及它自己的派发协程
         */
        struct StreamingRequest
        {
            HttpStreamBody          body;         ///< 本流正文（HttpBodySource，兼「消费才还窗口」）
            HttpRequestBody         reader;       ///< 交付给 request.bodyStream()
            HttpRequest             request;      ///< 头部收齐时填好的请求
            std::optional<Core::Task<>> serveTask; ///< 本流自己的派发协程（等正文时挂起）
            std::coroutine_handle<> bodyWaiter{}; ///< 正在等正文的协程（本流最多一个）
            bool isServeStarted{false};           ///< 派发协程是否已经起过
            bool isServeFinished{false};          ///< 派发协程已跑完（记录可随流关闭一起摘掉）
            bool isStreamClosed{false};           ///< 承载侧的流已关闭（此后不会再有 DATA 到达）
            bool hasPendingWake{false};           ///< 有新正文/收尾/断开，等回到安全点再唤醒
            /// 正文还没收完时「下一次该有进展」的时刻，随每段到达按 readTimeout 往后推；收完之后不再判
            Deadline deadline{std::chrono::steady_clock::now()};
        };

        /**
         * @brief 等某条流的下一次正文到达（或收尾、断开）
         * @details h3 的正文由承载推来，泵没有「主动去读一批」这种动作可做，只能挂起等；到达时
         *          只置标记，回到连接层回调之外的位置再由 wakeStreamingRequests() 唤醒
         */
        class BodyWaitAwaiter
        {
        public:
            /**
             * @brief 绑定要等的那条流
             * @param streamingRequest 目标流；为空时视为无进展（调用方回头自己判断）
             */
            explicit BodyWaitAwaiter(StreamingRequest *streamingRequest) noexcept : m_streamingRequest(streamingRequest) {}

            /// 已收尾或已断开时不必挂起：调用方回头就能得到结论
            [[nodiscard]] bool await_ready() const noexcept
            {
                return m_streamingRequest == nullptr || m_streamingRequest->body.isComplete() || m_streamingRequest->body.isBroken();
            }

            /// 记下等待者（本流的处理器是单条协程，因此至多一个）
            void await_suspend(const std::coroutine_handle<> waiter) const noexcept
            {
                m_streamingRequest->bodyWaiter = waiter;
            }

            static void await_resume() noexcept {}

        private:
            StreamingRequest *m_streamingRequest{nullptr}; ///< 目标流（非拥有；活在 m_streamingRequests 里）
        };

        /**
         * @brief 起一条流式请求的派发协程
         * @param streamId 流号
         * @param streamingRequest 该流的状态
         * @return Core::Task<> 路由与响应回写完成
         */
        [[nodiscard]] Core::Task<> serveStreamingRequest(std::int64_t streamId, StreamingRequest &streamingRequest);

        /**
         * @brief 造一个「等下一批正文」的泵
         * @param streamId 流号
         * @return HttpRequestBody::Pump 泵
         */
        [[nodiscard]] HttpRequestBody::Pump makeBodyPump(std::int64_t streamId);

        /**
         * @brief 记下「这条流有新进展」，等回到安全点再唤醒
         * @param streamId 流号
         * @note 不在连接层的回调里直接唤醒：处理器会回头提交响应，而回调期间重入连接层是未定义行为
         */
        void noteBodyProgress(std::int64_t streamId) noexcept;

        /**
         * @brief 在连接层回调之外的位置唤醒各条流：起还没起过的派发协程，或叫醒等正文的那个
         */
        void wakeStreamingRequests();

        /**
         * @brief 处理承载层攒下的「对端取消」：把那些流从连接层与会话两侧一并回收
         * @note 只能在安全点（pump() 里）调用：回收会唤醒业务协程，而它们随时可能回写响应
         */
        void drainPeerCancelledStreams();

        /**
         * @brief 唤醒被 dropRequest 记下的流式生产者
         * @note 同属安全点动作：被唤醒的业务会接着写响应（那要动连接层）
         */
        void resumeDeferredWaiters();

        /**
         * @brief 收口被记下的隧道（对端 END_STREAM 或重置）
         * @note 同属安全点动作：closeTunnel() 要动连接层
         */
        void closeDeferredTunnels();

        /// 流式响应的缓冲上界：超过就让生产者挂起，等网络排空再继续（不无限堆内存）
        static constexpr std::size_t kStreamingResponseBufferByteCount = 256U * 1024U;

        /**
         * @brief 一条流的响应里还有多少字节没交给传输层
         * @param streamId 流号
         * @return std::size_t 连接层该流的待发字节数；连接层已作废时为 0
         * @note 闸门口径按「交给传输层即视为排空」算：本端不等对端的确认，重传由传输层负责
         */
        [[nodiscard]] std::size_t streamingResponsePendingByteCount(std::int64_t streamId) const noexcept;

        /**
         * @brief 等流式响应的缓冲排空到上界以内
         */
        class ResponseSpaceAwaiter
        {
        public:
            /**
             * @brief 绑定要等的流
             * @param session 所属会话（问连接层要该流还剩多少待发字节）
             * @param streamId 流号
             * @param streamingResponse 目标流的状态（共享所有权：流被 dropRequest 摘掉后
             *        生产者手里的这份仍然有效，醒来时能看到 isStreamClosed 而不是踩空）
             */
            explicit ResponseSpaceAwaiter(Http3Session &session, const std::int64_t streamId,
                                          std::shared_ptr<StreamingResponse> streamingResponse) noexcept :
                m_session(&session), m_streamId(streamId), m_streamingResponse(std::move(streamingResponse))
            {
            }

            /// 已经退到上界以内（或状态没了、流已关闭）就不必挂起
            [[nodiscard]] bool await_ready() const noexcept
            {
                return m_streamingResponse == nullptr || m_streamingResponse->isStreamClosed ||
                       m_session->streamingResponsePendingByteCount(m_streamId) <= kStreamingResponseBufferByteCount;
            }

            /// 记下等待者（本流的响应只有一个生产者）
            void await_suspend(const std::coroutine_handle<> waiter) const noexcept
            {
                m_streamingResponse->spaceWaiter = waiter;
            }

            static void await_resume() noexcept {}

        private:
            Http3Session *m_session{nullptr};                        ///< 所属会话（问它要该流的待发字节数）
            std::int64_t m_streamId{0};                              ///< 目标流号
            std::shared_ptr<StreamingResponse> m_streamingResponse;  ///< 目标流的状态（与 map 共享所有权）
        };

        /**
         * @brief 路由之前给响应装好流式发送口
         * @param streamId 流号
         * @param response 待路由的响应
         * @note 装好之后处理器走 startChunkedResponse()/writeChunk() 那条路时，各块经本口直接出去
         */
        void attachChunkSender(std::int64_t streamId, HttpResponse &response);

        /**
         * @brief 取（必要时创建）某条流的流式响应状态
         * @param streamId 流号
         * @return std::shared_ptr<StreamingResponse> 该流的状态（活在 m_streamingResponses 里；
         *         生产者与等待器各持一份共享所有权，摘表不等于对象立刻销毁）
         */
        std::shared_ptr<StreamingResponse> streamingResponseFor(std::int64_t streamId);

        /**
         * @brief 查一条流已有的流式响应状态（**只查不建**）
         * @details 对端已经重置这条流时表里没有它：此时重建一个「未关闭」的新状态会让条目与
         *          其中的字节永久驻留（没人再置位、也没人清理），大帧还会卡在等缓冲空间那里
         *          永远等下去。发送侧因此一律只查表，查不到按「流没了」处理
         * @param streamId 流号
         * @return 状态；该流没有登记时为空
         */
        [[nodiscard]] std::shared_ptr<StreamingResponse> findStreamingResponse(std::int64_t streamId) const noexcept;

        /**
         * @brief 提交流式响应的响应头（第一次写块时才提交：此刻业务设的头部才齐）
         * @param streamId 流号
         * @param state 该流的状态
         * @param response 业务填好的响应（取状态码与头部）
         * @return true 提交成功
         * @note 只交头、**不结束这条流**：正文随后一段一段推给连接层
         */
        bool submitStreamingResponseHead(std::int64_t streamId, StreamingResponse &state, HttpResponse &response);

        /**
         * @brief 写一段流式响应正文（ChunkSender 的实现）
         * @param streamId 流号
         * @param state 该流的状态
         * @param response 业务填好的响应
         * @param chunk 本段字节
         * @return Core::Task<bool> 本段是否已收下（缓冲满时挂起等排空）
         */
        [[nodiscard]] Core::Task<bool> sendStreamingChunk(std::int64_t streamId, std::shared_ptr<StreamingResponse> state,
                                                          HttpResponse &response, std::string_view chunk);

        /**
         * @brief 把一段出向正文推给连接层并立刻往外送：缓冲超上界时挂起等排空
         * @param streamId 流号
         * @param state 该流的状态（流已关闭时直接按失败收手）
         * @param bytes 本段字节
         * @return Core::Task<bool> 本段是否已收下
         * @details 流式响应与隧道出向帧共用这一段：推一段、刷一次、超过闸门就等一跳
         */
        [[nodiscard]] Core::Task<bool> pushStreamingResponseBody(std::int64_t streamId,
                                                                 const std::shared_ptr<StreamingResponse> &state,
                                                                 std::string_view bytes);

        /**
         * @brief 流式响应写完：补交还没交的响应头，再交出收尾的 END_STREAM
         * @param streamId 流号
         * @param response 业务填好的响应（一次都没写过时要在这里补交响应头）
         */
        void finishStreamingResponse(std::int64_t streamId, HttpResponse &response);

        /**
         * @brief 刷完待发字节后唤醒等缓冲排空的生产者
         * @note 只能在连接层回调之外调用：被唤醒的生产者接着就要推下一段正文
         */
        void resumeStreamingResponseWaiters();

        /// 丢掉已经跑完的派发协程随记录一起摘掉
        void reapFinishedStreamingRequests();

        /**
         * @brief 一条 WebSocket 隧道（RFC 9220 扩展 CONNECT）
         * @details 应答 200 之后，这条 h3 流上跑的就不再是 h3 报文，而是 WebSocket 帧本身：入向字节
         *          交给对端对象解码，出向帧作为同一条流上的 DATA 发出去。出向复用流式响应那套
         *          「推一段、挡一段」的机制，因此隧道不会把内存堆起来
         */
        struct WebSocketTunnel
        {
            WebSocketHandler                handler;              ///< 业务处理器
            std::unique_ptr<WebSocketPeer>  peer;                 ///< 对端对象（帧的收发都经它）
            std::optional<Core::Task<>>     businessTask;         ///< 业务处理器所在的协程
            std::string                     pendingIncomingBytes; ///< 已收下、等安全点再交给对端对象的入向字节
            bool                            hasPendingFeed{false};     ///< 有待喂给对端对象的字节
            bool                            isBusinessFinished{false}; ///< 业务已返回
            bool                            isStreamClosed{false};     ///< 承载侧的流已关闭
        };

        /**
         * @brief 建立 WebSocket 隧道：应答 200（不结束流）并把业务处理器跑起来
         * @param streamId 流号
         * @param requestedExtensions 请求里的 Sec-WebSocket-Extensions 原文（空串表示对端没提扩展）；
         *        按值取是必须的——本协程会在处理器上挂起，届时请求记录可能已被回收
         * @param response 业务填好的响应（其中的升级登记给出处理器）
         * @return Core::Task<> 建立完成
         */
        [[nodiscard]] Core::Task<> serveWebSocketTunnel(std::int64_t streamId, std::string requestedExtensions, HttpResponse &response);

        /**
         * @brief 隧道里的业务协程：跑处理器，返回后收尾
         * @param streamId 流号
         * @return Core::Task<> 业务结束
         */
        [[nodiscard]] Core::Task<> runTunnelBusiness(std::int64_t streamId);

        /**
         * @brief 把隧道里的一段出向字节（WebSocket 帧）发出去
         * @param streamId 流号
         * @param frameBytes 帧字节
         * @return Core::Task<bool> 是否已收下（缓冲满时挂起等排空）
         */
        [[nodiscard]] Core::Task<bool> sendTunnelBytes(std::int64_t streamId, std::string_view frameBytes);

        /**
         * @brief 收尾一条隧道：结束出向、把对端对象标记为关闭
         * @param streamId 流号
         */
        void closeTunnel(std::int64_t streamId);

        /// 把已经跑完的业务协程随记录一起摘掉（业务跑完且流已关闭）
        void reapFinishedTunnels();

        /**
         * @brief 在安全点把攒下的入向字节交给各条隧道的对端对象
         * @note 不能在连接层的回调里喂：喂进去会让业务协程立刻跑起来，它回头就推帧出去，
         *       而回调期间重入连接层是未定义行为
         */
        void wakeWebSocketTunnels();

        /**
         * @brief 按 h3 的规矩给响应定稿：普通请求上登记的 WebSocket 升级改成明确失败
         * @param streamId 流号
         * @param response 业务填好的响应，就地修改
         * @details h3 的升级只有扩展 CONNECT（RFC 9220）一条路，且由 serveWebSocketTunnel() 单独
         *          处理；走到本函数的都是普通请求，此刻登记升级是对端接不住的形态，只能回 500
         */
        void finalizeResponseForHttp3(std::int64_t streamId, HttpResponse &response);

        /**
         * @brief 把攒下的头与正文整理成 HttpRequest 并排队
         * @param streamId 流号
         */
        void enqueueRequest(std::int64_t streamId);

        /**
         * @brief 把一条响应交给 HTTP/3 连接层：先交响应头，有正文时再一次性把正文推过去
         * @param streamId 流号
         * @param response 业务填好的响应
         * @param isHeadRequest 是否 HEAD 请求：true 时不发正文，content-length 仍按完整正文长度给出
         *        （RFC 9110 §9.3.2），与 h1/h2 的抑制口径一致
         */
        void submitResponse(std::int64_t streamId, const HttpResponse &response, bool isHeadRequest);

        /**
         * @brief 连接层判定请求头部畸形：按 RFC 9114 §4.1.2 先答一个 400 再结束这条流
         * @param streamId 出错的流
         * @param reason 连接层给出的中文原因（同时作为响应正文）
         * @note 这条流已经派发过或已经答过就只记日志：一条流只能有一个响应，
         *       而且业务此刻可能正在往里写正文
         */
        void answerMalformedRequest(std::int64_t streamId, std::string_view reason);

        /**
         * @brief 记下一条已答完的请求：达到单连接上限就地发 GOAWAY 排空
         */
        void noteRequestServed();

        /**
         * @brief 给请求落定 request-id（可采信就沿用客户端给的，否则新生成一个）
         * @param request 已收齐、正要交给路由的请求
         */
        void noteRequestId(HttpRequest &request) const;

        /**
         * @brief 请求带着 Expect: 100-continue 时，先交一个 :status 100 的头块
         * @details 时机与 h2 侧一致：头收齐、正文还在路上。h3 在这一刻判不出正文会不会来，因此只认
         *          「声明了正的 content-length」这一种请求（没声明的按 RFC 9110 §10.1.1 的兜底自己发）。
         * @param streamId 承载这条请求的流
         * @param incoming 头已收齐、还没派发的请求
         */
        void answerExpectContinueIfRequested(std::int64_t streamId, const IncomingRequest &incoming);

        /**
         * @brief 提交响应（头或正文）失败的处置：流已经不在了只作废这条流，其余按会话作废
         * @param streamId 流号
         * @param what 正在做的事（进日志）
         * @param reason 连接层给出的中文原因
         * @param errorCode 失败类别对应的线上错误码
         */
        void handleResponseSubmissionFailure(std::int64_t streamId, const char *what, std::string_view reason,
                                             Http3ErrorCode errorCode);

        /**
         * @brief 记日志并把会话作废
         * @param errorCode 要写进 QUIC 关闭帧的线上错误码
         * @param reason 中文原因（同时供传输层取用）
         */
        void markBroken(Http3ErrorCode errorCode, std::string_view reason);

        /// 待服务的一条请求：收齐的请求本体 + 收的过程中记下的越界标记。
        /// 标记要跟着请求走到服务阶段，413 才发得出来（与 h2 的 PendingRequest::isBodyTooLarge 同形）
        struct ReadyRequest
        {
            std::int64_t streamId{0};                 ///< 流号
            HttpRequest  request;                     ///< 已收齐的请求
            /// 本条请求正文占用的全局在途额度：随待派发记录一起活着，直到服务完这一条才归还。
            /// 早一步还掉（在排队时就还）会让「排队的正文」脱离预算，多条流能把实际占用推过上限
            HttpMemoryBudget::Reservation bodyBudget;
            bool         isBodyTooLarge{false};       ///< 正文越界：服务阶段回 413 而不是派发
            bool         isBudgetExceeded{false};     ///< 正文超出全局在途预算：服务阶段回 503 而不是派发
            bool         isHeaderLimitExceeded{false}; ///< 头部越限：服务阶段回 431 而不是派发
            bool         isUriTooLong{false};         ///< 请求目标越限：服务阶段回 414 而不是派发
        };

        std::unique_ptr<Http3Connection> m_connection;   ///< HTTP/3 连接层：帧的编解码与 QPACK 都在它那里；开不出本端单向流时为空
        StreamWriter              m_writer;              ///< 流数据出口
        StreamCrediter            m_crediter;            ///< 接收窗口归还口
        StreamAborter             m_aborter;             ///< 单条流的收口出口（可空：空则只丢本端记账，对端收不到信号）
        Router                   *m_router{nullptr};     ///< 路由器（不持有；由服务端保证其寿命）
        std::shared_ptr<HttpMetricsCollector> m_metrics; ///< 统计采集端（可空：空表示本会话不采集）
        /// request-id 生成器（可空）：与服务器共享一份，前缀标识服务器实例
        std::shared_ptr<HttpRequestIdGenerator> m_requestIdGenerator;
        HttpParserLimits          m_parserLimits{};      ///< 请求解析上限（正文总量上限等）
        /// 连接级限额（可空）：目前用到的是「单连接最多处理多少条请求」
        std::shared_ptr<const HttpServerLimits> m_serverLimits;
        std::size_t               m_servedRequestCount{0};   ///< 本会话已答完的请求条数，达到上限即排空
        bool                      m_isUsable{false};     ///< 三条本端单向流是否都开出来了
        bool                      m_isBroken{false};     ///< 是否已作废
        bool m_hasAbandonedPendingStreams{false}; ///< 承载连接的收口信号是否已交过：唤醒只做一次，之后每拍只收敛
        /// 正在接收的请求：键是流号
        std::map<std::int64_t, IncomingRequest> m_incomingRequests;
        /// 承载层报来的「对端取消」流号：它们到的时候正在传输层的回调里，只能先记下来，
        /// 等 pump() 这个安全点再统一回收（见 cancelStreamByPeer / drainPeerCancelledStreams）
        std::vector<std::int64_t> m_peerCancelledStreamIds;

        /// 待唤醒的流式生产者（同上：dropRequest 在连接层回调里被调用，不能当场恢复它们）
        std::vector<std::coroutine_handle<>> m_deferredWaiterResumes;

        /// 待收口的隧道流号（同上）
        std::vector<std::int64_t> m_deferredTunnelClosures;

        /// 「扩展 CONNECT 与 END_STREAM 同趟到达」的待建隧道流号：建成那一刻立刻收尾，
        /// 不记下来的话业务会永远挂在 receive() 上（隧道还没建，收尾无处可施）
        std::set<std::int64_t> m_pendingTunnelStreamsEnded;

        std::shared_ptr<HttpMemoryBudget> m_memoryBudget; ///< 在途正文字节的全局预算，与服务端共享；空表示不受约束
        /// 流式请求的本地状态：键是流号。用 unique_ptr 持有是为了地址稳定——里面存着等待者的
        /// 协程句柄，而记录本身会被移进移出（头收齐那一刻从 m_incomingRequests 转过来）
        std::map<std::int64_t, std::unique_ptr<StreamingRequest>> m_streamingRequests;
        /// 流式写出响应的状态：键是流号。用共享指针持有是为了让等待器与生产者各拿一份——
        /// 流被摘掉之后它们读到的是 isStreamClosed 而不是踩空
        std::map<std::int64_t, std::shared_ptr<StreamingResponse>> m_streamingResponses;
        /// 等派发的隧道流：扩展 CONNECT（:method=CONNECT + :protocol=websocket）的那些
        std::set<std::int64_t> m_pendingTunnelStreams;
        /// 隧道建立之前先到达的帧字节：隧道是在 pump() 里建的，而帧可能在同一批字节里就跟到了
        std::map<std::int64_t, std::string> m_pendingTunnelBytes;
        /// 已建立的 WebSocket 隧道：键是流号。这些流上的 DATA 是 WebSocket 帧，不是 h3 请求正文
        std::map<std::int64_t, std::unique_ptr<WebSocketTunnel>> m_webSocketTunnels;
        /// 已收全、等待派发的请求（按收全先后）
        std::deque<ReadyRequest> m_readyRequests;
    };
} // namespace AsynGyanis::Net

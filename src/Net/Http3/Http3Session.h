/**
 * @file Http3Session.h
 * @brief 一条 QUIC 连接上的 HTTP/3 会话：绑定控制流与 QPACK 流，把 h3 请求映射到既有 HTTP 层
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpRequestBody.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpStreamBody.h"
#include "Net/WebSocket/WebSocketPeer.h"

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
#include <utility>
#include <vector>

/// nghttp3 的连接对象只以指针形式出现在本文件里，实现细节留在 .cpp——这样 Net 可以私有链接
/// nghttp3（消费方不必被迫去 find_package 它，导出包也少一处 find_dependency）
struct nghttp3_conn;

namespace AsynGyanis::Net
{
    class Router;

    /**
     * @brief 一条 QUIC 连接上的 HTTP/3 会话
     *
     * @details 与 HTTP/2 侧 `Http2Session` 的位置对应：都是「一条连接上的多路复用」，区别只是多路的
     *          载体从 TCP 帧换成了 QUIC 流。h3 这一层里，帧的组装与拆分、QPACK 的编解码都交给
     *          nghttp3，本类负责把流数据在 nghttp3 与传输层之间搬，并把收全的请求交给既有
     *          `Router`——业务处理器与 h1/h2 完全同一份，不需要为 h3 另写一套。
     *
     * @note 控制流与 QPACK 编解码流都是**本端发起的单向流**，必须在会话建立时就开出来交给 nghttp3
     *       绑定：少了它们 nghttp3 连 SETTINGS 都发不出去（RFC 9114 §6.2.1）。流号由传输层给
     *       （`StreamOpener`），本类不碰 ngtcp2。
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

        /// 一条待发响应的正文：nghttp3 只借走指针（丢包重传时还会再用一次），因此字节要活到流关闭
        struct OutgoingBody
        {
            std::string bytes;         ///< 正文
            std::size_t offset{0};     ///< 已经交给 nghttp3 的字节数
        };

        /**
         * @brief 一条正在流式写出响应的流（startChunkedResponse + writeChunk 那条路）
         * @note 公开嵌套类型：.cpp 里的正文读取回调要靠它把 stream_user_data 还原回来
         */
        struct StreamingResponse
        {
            /// 尚未交付完的正文分片。**一片一块内存**：nghttp3 会把没写完的 vec 留到下一次写再取，
            /// 用一整块会重新分配的缓冲会让交出去的指针失效（实测：第二次追加后首片内容整体被搬走，
            /// 线上发出去的是新缓冲的簿记字节）
            std::deque<std::string> chunks;
            std::size_t             headOffset{0};        ///< 头一片里已经交给 nghttp3 的字节数
            std::size_t             pendingByteCount{0};  ///< 还挂在手上（未交付完）的字节总数
            bool                    isHeadSent{false};    ///< 响应头是否已提交
            bool                    isFinished{false};    ///< 处理器已写完（正文到此为止）
            bool                    isStreamClosed{false}; ///< 承载侧的流已关闭：生产者据此收手，不再等下一位唤醒
            std::coroutine_handle<> spaceWaiter{};        ///< 生产者等缓冲排空时挂在这里
        };

        /**
         * @brief 建立一个 HTTP/3 服务端会话
         * @param opener 单向流的开流口
         * @param writer 流数据出口
         * @param crediter 接收窗口的归还口（可空：为空时不归还，正文一大就会把接收窗口用光）
         * @note 构造里就把控制流与两条 QPACK 流绑上。开流失败只记日志并让会话保持不可用
         *       （`isUsable()` 为假），不抛异常：一条连接建不起 h3 不该把服务端拖垮
         */
        Http3Session(StreamOpener opener, StreamWriter writer, StreamCrediter crediter = {});

        ~Http3Session();

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
         * @brief 会话是否可用（控制流与 QPACK 流都绑上了）
         * @return true 可用
         */
        [[nodiscard]] bool isUsable() const noexcept;

        /**
         * @brief 会话是否已作废
         * @return true 已作废（nghttp3 判定协议错误），此后唯一合法的动作是销毁
         */
        [[nodiscard]] bool isBroken() const noexcept;

        /**
         * @brief 把对端在一条流上送来的字节交给 HTTP/3 层
         * @param streamId 流号
         * @param data 本段字节
         * @param isEndStream 对端在这段之后收尾
         */
        void onStreamData(std::int64_t streamId, std::span<const std::uint8_t> data, bool isEndStream);

        /**
         * @brief 把 HTTP/3 层攒下的待发字节交给传输层
         */
        void flushPendingStreamData();

        /**
         * @brief 跑完排队中的请求（含路由与业务处理器）并把响应与攒下的字节发出去
         * @details 路由与业务处理器是协程（可能去等磁盘、等上游），而 nghttp3 的回调是同步的，
         *          因此「收全请求」与「派发业务」拆成两步：同步回调只入队，本协程再逐个 co_await。
         *          传输层每处理完一条报文调一次即可。
         * @return Core::Task<> 派发与发送完成
         */
        [[nodiscard]] Core::Task<> pump();

        // ---- 以下几项由 .cpp 里的 nghttp3 回调转交（只收平类型，nghttp3 的结构不外泄）----

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
         * @brief 丢掉一条流上尚未收全的请求（流被重置或关闭）
         * @param streamId 流号
         */
        void dropRequest(std::int64_t streamId);

        /**
         * @brief 头收齐时判一下：命中流式正文路由就提前派发（正文边收边交，不等整份收齐）
         * @param streamId 流号
         * @note 由 .cpp 里的 end_headers 回调转交：那时方法/路径已可判，正文还在路上
         */
        void beginStreamingRequestIfMatched(std::int64_t streamId);

    private:
        /// 一次 flush 最多搬多少段：防止待发字节很多时在一条连接上转太久
        static constexpr std::size_t kMaximumWritesPerFlush = 64;

        /// 一次取待发数据最多用多少个分片描述
        static constexpr std::size_t kMaximumDataVectors = 16;

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
        };

        /**
         * @brief 等某条流的下一次正文到达（或收尾、断开）
         * @details h3 的正文由承载推来，泵没有「主动去读一批」这种动作可做，只能挂起等；到达时
         *          只置标记，回到不进 nghttp3 回调的位置再由 wakeStreamingRequests() 唤醒
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
         * @note 不在 nghttp3 的回调里直接唤醒：处理器会调用 nghttp3 提交响应，回调期间重入库是未定义行为
         */
        void noteBodyProgress(std::int64_t streamId) noexcept;

        /**
         * @brief 在不进 nghttp3 回调的位置唤醒各条流：起还没起过的派发协程，或叫醒等正文的那个
         */
        void wakeStreamingRequests();

        /// 流式响应的缓冲上界：超过就让生产者挂起，等网络排空再继续（不无限堆内存）
        static constexpr std::size_t kStreamingResponseBufferByteCount = 256U * 1024U;

        /**
         * @brief 等流式响应的缓冲排空到上界以内
         */
        class ResponseSpaceAwaiter
        {
        public:
            /**
             * @brief 绑定要等的流
             * @param streamingResponse 目标流的状态（共享所有权：流被 dropRequest 摘掉后
             *        生产者手里的这份仍然有效，醒来时能看到 isStreamClosed 而不是踩空）
             */
            explicit ResponseSpaceAwaiter(std::shared_ptr<StreamingResponse> streamingResponse) noexcept :
                m_streamingResponse(std::move(streamingResponse))
            {
            }

            /// 已经退到上界以内（或状态没了、流已关闭）就不必挂起
            [[nodiscard]] bool await_ready() const noexcept
            {
                return m_streamingResponse == nullptr || m_streamingResponse->isStreamClosed ||
                       m_streamingResponse->pendingByteCount <= kStreamingResponseBufferByteCount;
            }

            /// 记下等待者（本流的响应只有一个生产者）
            void await_suspend(const std::coroutine_handle<> waiter) const noexcept
            {
                m_streamingResponse->spaceWaiter = waiter;
            }

            static void await_resume() noexcept {}

        private:
            std::shared_ptr<StreamingResponse> m_streamingResponse; ///< 目标流的状态（与 map 共享所有权）
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
         * @brief 提交流式响应的响应头（第一次写块时才提交：此刻业务设的头部才齐）
         * @param streamId 流号
         * @param state 该流的状态
         * @param response 业务填好的响应（取状态码与头部）
         * @return true 提交成功
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
         * @brief 流式响应写完：标记收尾并让 nghttp3 把余下的取走
         * @param streamId 流号
         * @param response 业务填好的响应（一次都没写过时要在这里补交响应头）
         */
        void finishStreamingResponse(std::int64_t streamId, HttpResponse &response);

        /**
         * @brief 交给 nghttp3 的字节又排空了一部分：压掉已交付的前缀，并唤醒等空间的生产者
         * @param streamId 流号
         * @note 缓冲只在**这里**压：读回调里压会改掉已经交出去、库里还没拷走的 vec
         *       （libstdc++ 的 clear() 会把首字节写成 '\0'，实测交出去的负载首字节就是这样丢的）
         */
        void noteStreamingResponseDrained(std::int64_t streamId);

        /// 丢掉已经跑完的派发协程随记录一起摘掉
        void reapFinishedStreamingRequests();

        /**
         * @brief 一条 WebSocket 隧道（RFC 9220 扩展 CONNECT）
         * @details 应答 200 之后，这条 h3 流上跑的就不再是 h3 报文，而是 WebSocket 帧本身：入向字节
         *          交给对端对象解码，出向帧作为同一条流上的 DATA 发出去。出向复用流式响应那套
         *          「按需拉 + 没数据时挡流」的机制，因此隧道不会把内存堆起来
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
         * @param response 业务填好的响应（其中的升级登记给出处理器）
         * @return Core::Task<> 建立完成
         */
        [[nodiscard]] Core::Task<> serveWebSocketTunnel(std::int64_t streamId, HttpResponse &response);

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
         * @note 不能在 nghttp3 的回调里喂：喂进去会让业务协程立刻跑起来，它回头就调 nghttp3 发帧，
         *       而回调期间重入库是未定义行为
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
         * @brief 把一条响应交给 nghttp3（头部转成 nghttp3_nv，正文挂在数据读取回调上）
         * @param streamId 流号
         * @param response 业务填好的响应
         */
        void submitResponse(std::int64_t streamId, const HttpResponse &response);

        /**
         * @brief 用 nghttp3 的错误码记日志并把会话作废
         * @param errorCode nghttp3 返回的负错误码
         * @param what 正在做的事（进日志）
         */
        void markBroken(int errorCode, const char *what);

        /// 待服务的一条请求：收齐的请求本体 + 收的过程中记下的越界标记。
        /// 标记要跟着请求走到服务阶段，413 才发得出来（与 h2 的 PendingRequest::isBodyTooLarge 同形）
        struct ReadyRequest
        {
            std::int64_t streamId{0};                 ///< 流号
            HttpRequest  request;                     ///< 已收齐的请求
            bool         isBodyTooLarge{false};       ///< 正文越界：服务阶段回 413 而不是派发
        };

        nghttp3_conn             *m_connection{nullptr}; ///< nghttp3 连接对象
        StreamWriter              m_writer;              ///< 流数据出口
        StreamCrediter            m_crediter;            ///< 接收窗口归还口
        Router                   *m_router{nullptr};     ///< 路由器（不持有；由服务端保证其寿命）
        HttpParserLimits          m_parserLimits{};      ///< 请求解析上限（正文总量上限等）
        std::vector<std::uint8_t> m_pendingBytes;        ///< 一次 flush 用的连续缓冲（把分片拼在一起）
        bool                      m_isUsable{false};     ///< 三条单向流是否都绑上了
        bool                      m_isBroken{false};     ///< 是否已作废
        /// 正在接收的请求：键是流号
        std::map<std::int64_t, IncomingRequest> m_incomingRequests;
        /// 流式请求的本地状态：键是流号。用 unique_ptr 持有是为了地址稳定——里面存着等待者的
        /// 协程句柄，而记录本身会被移进移出（头收齐那一刻从 m_incomingRequests 转过来）
        std::map<std::int64_t, std::unique_ptr<StreamingRequest>> m_streamingRequests;
        /// 流式写出响应的状态：键是流号。正文缓冲要让 nghttp3 借指针，因此同样用 unique_ptr 保地址稳定
        std::map<std::int64_t, std::shared_ptr<StreamingResponse>> m_streamingResponses;
        /// 等派发的隧道流：扩展 CONNECT（:method=CONNECT + :protocol=websocket）的那些
        std::set<std::int64_t> m_pendingTunnelStreams;
        /// 隧道建立之前先到达的帧字节：隧道是在 pump() 里建的，而帧可能在同一批字节里就跟到了
        std::map<std::int64_t, std::string> m_pendingTunnelBytes;
        /// 已建立的 WebSocket 隧道：键是流号。这些流上的 DATA 是 WebSocket 帧，不是 h3 请求正文
        std::map<std::int64_t, std::unique_ptr<WebSocketTunnel>> m_webSocketTunnels;
        /// 已收全、等待派发的请求（按收全先后）
        std::deque<ReadyRequest> m_readyRequests;
        /// 待发响应的正文：std::map 的节点地址稳定，nghttp3 借走的指针不会因为它增删而失效
        std::map<std::int64_t, OutgoingBody> m_outgoingBodies;
    };
} // namespace AsynGyanis::Net

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
#include "Net/Http/HttpsSession.h"
#include "Net/Http/Router.h"
#include "Net/Http2/Http2Connection.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace AsynGyanis::Net
{
    /// ALPN 协商出 HTTP/2 时客户端与服务端一致使用的协议名（RFC 7540 §3.3）
    inline constexpr std::string_view kHttp2AlpnProtocolName = "h2";

    /**
     * @brief HTTP/2 会话类：一条 TLS 连接对应一个 Http2Session。
     *
     * @details start() 先做 TLS 握手，再按 ALPN 协商结果选协议：协商出 h2 就跑本类新写的 HTTP/2
     *          循环（前奏与 SETTINGS 协商、请求与正文、响应、接收方向流控），否则把连接原样交回
     *          HttpsSession 的事务循环——两条路径共用同一份实现，只有协议循环不同。
     *          HTTP/2 循环的驱动顺序是「读字节 → feedBytes() → 立刻写出（SETTINGS/ACK/WINDOW_UPDATE/
     *          GOAWAY）→ 取请求与正文 → 路由 → sendResponseHeaders()/sendResponseData() → 再写出」，
     *          与连接层的文档约定一致。
     *
     * @note 分流为什么不在 HttpsServer::createConnection() 里做：ALPN 结果产生于 TLS 握手过程，
     *       而 createConnection() 在握手之前被同步调用，此刻读到的必然是空串。服务器统一创建本类，
     *       由它在握手完成后按 TlsSocket::selectedAlpnProtocol() 选协议。
     *
     * @note 请求正文按「收齐再路由」处理，与 HTTP/1.1 侧（解析器攒完整条报文才交业务）同口径：
     *       正文超过 HttpParserLimits::maximumBodySize 时停止缓冲并回 413。流式响应
     *       （HttpResponse::startChunkedResponse()）是 HTTP/1.1 的机制，本片不支持，业务在 h2 上
     *       使用它会按业务异常收口（回 500 并记日志）；WebSocket 升级（RFC 8441 的扩展 CONNECT）
     *       同样不在本片，登记了升级的响应回 501。
     * @note 本片不强制 HttpServerLimits::maximumRequestsPerConnection：HTTP/2 里正确的收口方式是
     *       先发 GOAWAY 再等既有流做完（后续片），本片只保证空闲/读写超时与统计口径与 HTTP 侧一致。
     *
     * @see Http2Connection, HttpsSession, Core::TlsSocket
     */
    class Http2Session final : public HttpsSession
    {
    public:
        /**
         * @brief 构造 HTTP/2 会话。
         * @param loop 事件循环，仅用于给基类造一条不持有描述符的占位套接字（与 HttpsSession 同）
         * @param tlsSocket 已创建但尚未握手的 TlsSocket，所有权转移给基类
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
         * @brief 启动会话主协程：TLS 握手 → 按 ALPN 选协议 → 跑对应循环 → 关闭通道。
         *
         * @details 重写 HttpsSession::start()：第一步与基类相同（TLS 握手，失败即收口），差异在于
         *          握手之后先读 ALPN——协商出 h2 就转入本类的 HTTP/2 循环，其余（http/1.1 或客户端
         *          没提 ALPN）原样交回 HttpsSession::start()，因此 HTTP/1.1 行为与继承前逐字节一致。
         *
         * @return Core::Task<> 协程任务，连接结束时完成
         * @throws 基类 close() 之外的异常不做处理，原样抛给 TcpServer::handleConnection()
         * @see HttpsSession::start(), runHttp2Loop()
         */
        Core::Task<> start() override;

    private:
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
         * @return true 响应已排入待发字节
         * @return false 响应写出失败（连接已不可用）
         */
        [[nodiscard]] Core::Task<bool> serveOneRequest(PendingRequest &pending);

        /**
         * @brief 把连接层交出的请求映射成 HttpRequest
         * @param http2Request 连接层交出的请求（伪头各自成字段）
         * @return HttpRequest 按 HTTP/1.1 语义填好的请求对象
         */
        [[nodiscard]] static HttpRequest mapToHttpRequest(const Http2Request &http2Request);

        /**
         * @brief 把 HttpResponse 的头列表整理成 HTTP/2 可发的形式
         * @details 丢掉连接特定头（§8.1.2.2 禁止）、逐条保留可重复头，并按 HttpResponse::appendHead()
         *          的同一套规则补齐缺省的内容类型、长度与日期。
         * @param response 业务填好的响应
         * @return std::vector<HpackHeaderField> 可直接交给 sendResponseHeaders() 的头列表
         */
        [[nodiscard]] static std::vector<HpackHeaderField> collectResponseHeaderFields(const HttpResponse &response);

        /**
         * @brief 在一条流上发出一条响应（头 + 正文）
         * @param streamId 目标流号
         * @param response 业务填好的响应
         * @param isHeadRequest 请求方法是否是 HEAD：置位时只发头，正文一个字节都不发
         * @return true 响应头与正文都已排入待发字节（可能还在等窗口）
         * @return false 响应未能排入（用法错误），原因已记日志
         */
        [[nodiscard]] Core::Task<bool> sendResponse(std::uint32_t streamId, const HttpResponse &response, bool isHeadRequest);

        /**
         * @brief 把连接层的待发字节全部写到 TLS 通道上
         * @return true 已写完（没有待发字节也算成功）
         * @return false 传输失败，连接已不可用，调用方应停止循环
         */
        [[nodiscard]] Core::Task<bool> flushOutgoingBytes();

        Http2Connection m_connection;                 ///< HTTP/2 连接层状态机（协议状态、帧与窗口全在它里面）
        Router &m_router;                             ///< 路由器引用（与基类指向同一对象）
        HttpParserLimits m_parserLimits{};            ///< HTTP/2 路径只用 maximumBodySize，其余字段不适用
        std::shared_ptr<const HttpServerLimits> m_limits; ///< 连接级限额，与服务器共享、只读（构造时保证非空）
        std::shared_ptr<HttpMetricsCollector> m_metrics;  ///< 统计采集端；空指针表示不采集
        std::shared_ptr<HttpRequestIdGenerator> m_requestIdGenerator; ///< request-id 生成器；空指针表示不落定

        /// 头块已收齐的请求：按流号（对端流号严格递增，因此遍历顺序就是请求的到达顺序）
        std::map<std::uint32_t, PendingRequest> m_pendingRequests;
        HttpResponse m_response;                      ///< 响应对象按连接复用，每条请求发送前 reset()
        HttpRequest *m_servingRequest{nullptr};       ///< 正在路由的请求（连接关停时对它转成协作式取消）
        bool m_isConnectionUnusable{false};           ///< 本侧是否已判定写不出去：置位后所有写出短路，同一次故障只留一条日志
    };
} // namespace AsynGyanis::Net

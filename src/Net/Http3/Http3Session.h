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
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
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
            std::string body;       ///< 正文（本切片一次性收全）
            bool        hasHostHeader{false}; ///< 对端是否显式给了 host 头
        };

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

        nghttp3_conn             *m_connection{nullptr}; ///< nghttp3 连接对象
        StreamWriter              m_writer;              ///< 流数据出口
        StreamCrediter            m_crediter;            ///< 接收窗口归还口
        Router                   *m_router{nullptr};     ///< 路由器（不持有；由服务端保证其寿命）
        std::vector<std::uint8_t> m_pendingBytes;        ///< 一次 flush 用的连续缓冲（把分片拼在一起）
        bool                      m_isUsable{false};     ///< 三条单向流是否都绑上了
        bool                      m_isBroken{false};     ///< 是否已作废
        /// 正在接收的请求：键是流号
        std::map<std::int64_t, IncomingRequest> m_incomingRequests;
        /// 已收全、等待派发的请求（按收全先后）
        std::deque<std::pair<std::int64_t, HttpRequest>> m_readyRequests;
        /// 待发响应的正文：std::map 的节点地址稳定，nghttp3 借走的指针不会因为它增删而失效
        std::map<std::int64_t, OutgoingBody> m_outgoingBodies;
    };
} // namespace AsynGyanis::Net

/**
 * @file HttpClient.h
 * @brief 出站 HTTP 客户端与 URL 拆解：一次请求一条连接，明文与 https 两条路都支持
 * @author Gyanis
 * @date 2026-09-14
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#pragma once
#include "Core/Coroutine/Task.h"
#include "Net/Http/Client/HttpResponseParser.h"
#include "Net/Http/Client/HttpOutboundConnectionPool.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace AsynGyanis::Core { class EventLoop; }
namespace AsynGyanis::Net
{
    /// HTTP 客户端响应
    struct HttpClientResponse
    {
        int      statusCode{0};  ///< 状态码；0 表示没拿到响应（连接或 TLS 失败）
        std::string reasonPhrase; ///< 状态行里的原因短语
        std::vector<std::pair<std::string, std::string>> headers; ///< 头部字段，按收到的顺序原样留着
        std::string body;        ///< 正文；chunked 已按块拼回原样
    };
    /// URL 拆解结果
    /**
     * @brief 拆开的请求 URL
     */
    struct ParsedUrl
    {
        std::string scheme{"http"}; ///< 协议，只有 "http" 与 "https" 两种取值（识别大小写无关，存下来已归一化成小写）
        std::string host;           ///< 主机名或 IP 字面量，不做百分号解码；IPv6 已去掉方括号（发 Host 头时按规范加回）
        uint16_t    port{80};       ///< 端口；URL 里没写时 https 取 443、其余取 80
        std::string path{"/"};      ///< 请求路径，含查询串；没写路径时为 "/"
    };

    /**
     * @brief 拆一个 http(s) URL
     * @details 只做拆分，不改写：不百分号解码，也不接受空白与控制字符（那会撕裂请求行）。
     *          写错的端口不会回落到 80——那会让一个 https URL 静默连到明文端口上。
     * @param url 形如 http(s)://host[:port]/path；协议名大小写无关，IPv6 主机必须写成 "[::1]:8080"
     * @return ParsedUrl 拆好的协议、主机、端口与路径
     * @throws Base::InvalidArgumentException URL 含空白或控制字符、协议不是 http/https、没有主机、
     *         端口不是 1..65535 的十进制数、方括号没闭合，或 IPv6 字面量没加方括号
     */
    [[nodiscard]] ParsedUrl parseUrl(std::string_view url);
    /**
     * @brief 出站 HTTP 客户端
     * @details 每次请求新建一条连接，完成后关闭（https 走 TLS，并校验服务端证书与主机名）。
     */
    class HttpClient
    {
    public:
        /// 单次请求的默认整体时限（握手、发送、收完响应三段之和）
        static constexpr std::chrono::milliseconds kDefaultRequestTimeout{30000};

        /**
         * @brief 发起 GET 请求
         * @param loop 所属事件循环（提供套接字与定时器）
         * @param url 目标地址，形如 http(s)://host[:port]/path
         * @param requestTimeout 整体时限：到时直接掐断连接并返回空响应，避免对端只连不应答时
         *        把调用方永远挂住。域名解析与 TCP 连接不在其中，那两步各由系统解析器与
         *        内核的 SYN 重试定时兜底
         * @return std::unique_ptr<HttpClientResponse> 响应；失败（含超时）返回空
         */
        static Core::Task<std::unique_ptr<HttpClientResponse>> get(Core::EventLoop &loop, std::string_view url,
                                                                   std::chrono::milliseconds requestTimeout = kDefaultRequestTimeout);

        /**
         * @brief 发起 POST 请求（正文 Content-Type: application/x-www-form-urlencoded）
         * @param loop 所属事件循环
         * @param url 目标地址
         * @param contentType 正文媒体类型
         * @param body 正文
         * @param requestTimeout 整体时限，语义同 get()
         * @return std::unique_ptr<HttpClientResponse> 响应；失败（含超时）返回空
         */
        static Core::Task<std::unique_ptr<HttpClientResponse>> post(Core::EventLoop &loop, std::string_view url,
                                                                     std::string_view contentType, std::string_view body,
                                                                     std::chrono::milliseconds requestTimeout = kDefaultRequestTimeout);

        /**
         * @brief 建一个带空闲连接池的客户端：同一目标主机的连续请求复用一条 keep-alive 连接
         * @details 静态的 get()/post() 一次一条连接、收尾就关；对同一台主机反复出站时，每次都要重做
         *          DNS、TCP 与 TLS 握手。本实例把这些摊掉：用完且对端没声明 close 的连接还回池里，
         *          下次同主机同端口的请求先复用它。
         * @param loop 所属事件循环。实例连同它的池只在这条循环上用——协程挂起期间被别的线程驱动会
         *        踩坏套接字状态，因此本对象不跨线程共享（与框架里每条连接归属一个循环的约定同一口径）
         * @param poolConfig 池的规模参数：空闲多久收口、每个目标最多留几条
         */
        explicit HttpClient(Core::EventLoop &loop, HttpOutboundConnectionPool::Config poolConfig = {}) noexcept;

        /**
         * @brief 发一次 GET，能复用就复用空闲连接
         * @param url 目标地址，口径同静态的 get()
         * @param requestTimeout 整体时限，语义同静态的 get()：握手、发送、收完响应三段之和
         * @return std::unique_ptr<HttpClientResponse> 响应；失败（含超时）返回空
         * @note 复用的那条连接如果对端已经关掉，本次请求会**自动重开一条再来一次**——这是 keep-alive
         *       的固有竞态（对端随时可以收掉空闲连接），不是失败。读到过响应字节之后的失败不重发：
         *       那已经是「响应本身有问题」，重发会把非幂等请求做两遍
         */
        [[nodiscard]] Core::Task<std::unique_ptr<HttpClientResponse>> get(
                std::string_view url, std::chrono::milliseconds requestTimeout = kDefaultRequestTimeout);

        /**
         * @brief 发一次 POST，复用与重试口径同 get()
         * @param url 目标地址
         * @param contentType 正文媒体类型
         * @param body 正文
         * @param requestTimeout 整体时限，语义同 get()
         * @return std::unique_ptr<HttpClientResponse> 响应；失败（含超时）返回空
         */
        [[nodiscard]] Core::Task<std::unique_ptr<HttpClientResponse>> post(
                std::string_view url, std::string_view contentType, std::string_view body,
                std::chrono::milliseconds requestTimeout = kDefaultRequestTimeout);

        /// 当前空闲、可被复用的 HTTP/1.1 连接条数
        [[nodiscard]] std::size_t idleConnectionCount() const noexcept;

        /**
         * @brief 当前留着待命的 HTTP/2 连接条数（一台主机最多一条）
         * @details 与上面那条分开数是对的：h1 的连接按「一次一个请求」出租，h2 的连接按流复用，
         *          两者对同一次突发给出的答案本来就不同。
         * @return std::size_t 池里活着的 h2 连接条数
         */
        [[nodiscard]] std::size_t idleHttp2ConnectionCount() const noexcept;

        /**
         * @brief 最忙的那条待命 h2 连接上同时在途的流数（本端实测到的复用度）
         * @details 与上面的连接条数一起读：「一条连接 + 复用度二」才是并发请求共用同一条连接的证据
         * @return std::size_t 各条连接在途流数的最大值；池里没货返回 0
         */
        [[nodiscard]] std::size_t http2MaximumInFlightStreamCount() const noexcept;

        /// 收掉所有空闲连接：在途请求用的连接不受影响
        void closeIdleConnections() noexcept;

    private:
        Core::EventLoop *m_loop{nullptr};   ///< 所属事件循环（不拥有）
        HttpOutboundConnectionPool m_pool;  ///< 本客户端的空闲连接池
    };
} // namespace AsynGyanis::Net
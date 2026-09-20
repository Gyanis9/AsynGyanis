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
    struct ParsedUrl
    {
        std::string scheme{"http"}; ///< 协议；只有 http 与 https 两条路，其余按 http 处理
        std::string host;           ///< authority 里去掉端口的那一段（不做百分号解码）
        uint16_t    port{80};       ///< 端口；URL 里没写时 https 取 443、其余取 80
        std::string path{"/"};      ///< 请求路径，含查询串；没写路径时为 "/"
    };
    /// URL 拆解工具
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
    };
} // namespace AsynGyanis::Net
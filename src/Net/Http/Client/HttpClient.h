/** @file HttpClient.h 出站 HTTP 客户端 */
#pragma once
#include "Core/Coroutine/Task.h"
#include "Net/Http/Client/HttpResponseParser.h"
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
        int      statusCode{0};
        std::string reasonPhrase;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
    };
    /// URL 拆解结果
    struct ParsedUrl
    {
        std::string scheme{"http"};
        std::string host;
        uint16_t    port{80};
        std::string path{"/"};
    };
    /// URL 拆解工具
    [[nodiscard]] ParsedUrl parseUrl(std::string_view url);
    /**
     * @brief 出站 HTTP 客户端
     * @details 每次请求新建一条连接，完成后关闭。暂不支持 TLS/HTTPS。
     */
    class HttpClient
    {
    public:
        /// 发起 GET 请求
        static Core::Task<std::unique_ptr<HttpClientResponse>> get(Core::EventLoop &loop, std::string_view url);
        /// 发起 POST 请求（正文 Content-Type: application/x-www-form-urlencoded）
        static Core::Task<std::unique_ptr<HttpClientResponse>> post(Core::EventLoop &loop, std::string_view url,
                                                                     std::string_view contentType, std::string_view body);
    };
} // namespace AsynGyanis::Net
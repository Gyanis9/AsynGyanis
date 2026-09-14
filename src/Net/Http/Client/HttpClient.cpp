#include "Net/Http/Client/HttpClient.h"
#include "Core/EventLoop/EventLoop.h"
#include "Net/Tcp/TcpClient.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <charconv>
#include <string>
#include <system_error>
namespace AsynGyanis::Net
{
    ParsedUrl parseUrl(const std::string_view url)
    {
        ParsedUrl parsed;
        auto p = url;
        // scheme://
        auto colon = p.find("://");
        if (colon != std::string_view::npos)
        {
            parsed.scheme = std::string(p.substr(0, colon));
            p.remove_prefix(colon + 3);
        }
        // host[:port]
        auto slash = p.find('/');
        auto authority = (slash == std::string_view::npos) ? p : p.substr(0, slash);
        auto pathPart = (slash == std::string_view::npos) ? std::string_view{} : p.substr(slash);
        parsed.host = std::string(authority);
        auto portColon = authority.rfind(':');
        if (portColon != std::string_view::npos)
        {
            parsed.host = std::string(authority.substr(0, portColon));
            auto portStr = authority.substr(portColon + 1);
            auto [ptr, ec] = std::from_chars(portStr.data(), portStr.data() + portStr.size(), parsed.port);
            if (ec != std::errc{})
            {
                // 端口非法，恢复到 80
                parsed.port = 80;
            }
        } else
        {
            parsed.port = (parsed.scheme == "https") ? 443 : 80;
        }
        if (!pathPart.empty())
            parsed.path = std::string(pathPart);
        return parsed;
    }
    namespace
    {
        Core::Task<std::unique_ptr<HttpClientResponse>> doRequest(
                Core::EventLoop &loop, std::string_view method, std::string_view url,
                std::string_view contentType, std::string_view body)
        {
            auto u = parseUrl(url);
            auto streamPtr = co_await TcpClient::connect(loop, u.host, u.port);
            if (!streamPtr) co_return nullptr;
            auto &stream = *streamPtr;
            // 构造请求报文
            std::string request;
            request.reserve(256 + body.size());
            request += method; request += ' ';
            request += u.path; request += " HTTP/1.1\r\n";
            request += "Host: "; request += u.host; request += "\r\n";
            if (!body.empty())
            {
                request += "Content-Type: "; request += contentType; request += "\r\n";
                request += "Content-Length: "; request += std::to_string(body.size()); request += "\r\n";
            }
            request += "Connection: close\r\n\r\n";
            request += body;
            co_await stream.writeAll(request.data(), request.size());
            // 读响应
            HttpResponseParser parser;
            std::array<char, 4096> buffer{};
            while (true)
            {
                const ssize_t n = co_await stream.read(buffer.data(), buffer.size());
                if (n <= 0) break;
                parser.feed({buffer.data(), static_cast<std::size_t>(n)});
                if (parser.isComplete()) break;
            }
            if (!parser.isComplete())
                parser.endOfStream();
            if (!parser.isComplete() || parser.hasFailed())
                co_return nullptr;
            auto result = std::make_unique<HttpClientResponse>();
            result->statusCode   = parser.result().statusCode;
            result->reasonPhrase = parser.result().reasonPhrase;
            result->headers      = parser.result().headers;
            result->body         = parser.result().body;
            co_return result;
        }
    }
    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::get(Core::EventLoop &loop, std::string_view url)
    {
        co_return co_await doRequest(loop, "GET", url, {}, {});
    }
    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::post(
            Core::EventLoop &loop, std::string_view url, std::string_view contentType, std::string_view body)
    {
        co_return co_await doRequest(loop, "POST", url, contentType, body);
    }
} // namespace AsynGyanis::Net
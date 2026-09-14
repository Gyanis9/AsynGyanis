#include "Net/Http/Client/HttpClient.h"
#include "Base/Exception/Exception.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Socket/AsyncResolver.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Tls/TlsSocket.h"
#include "Net/Tcp/TcpClient.h"

#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <charconv>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace AsynGyanis::Net
{
    ParsedUrl parseUrl(const std::string_view url)
    {
        ParsedUrl parsed;
        auto p = url;
        auto colon = p.find("://");
        if (colon != std::string_view::npos)
        {
            parsed.scheme = std::string(p.substr(0, colon));
            p.remove_prefix(colon + 3);
        }
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
            if (ec != std::errc{}) parsed.port = 80;
        } else
        {
            parsed.port = (parsed.scheme == "https") ? 443 : 80;
        }
        if (!pathPart.empty()) parsed.path = std::string(pathPart);
        return parsed;
    }

    namespace
    {
        /// 进程级缓存的客户端 SSL_CTX（首次 HTTPS 请求时创建）
        static SSL_CTX *clientSslCtx()
        {
            // 函数内 static 指针 + 堆分配，确保进程退出时不参与全局析构顺序
            static SSL_CTX *ctx = []() -> SSL_CTX *
            {
                auto *c = SSL_CTX_new(TLS_client_method());
                if (!c) return nullptr;
                // 加载系统默认 CA 证书库（Linux /etc/ssl/certs, Windows 系统存储）
                SSL_CTX_set_default_verify_paths(c);
                // 要求校验服务端证书（但暂不设回调——基本校验由 OpenSSL 默认完成）
                SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
                return c;
            }();
            return ctx;
        }

        /// 用 asyncSend 发完一整段（TlsSocket 没有 writeAll，SSL_write 默认全部或失败）
        Core::Task<bool> sendAll(Core::TlsSocket &socket, const std::string_view data)
        {
            std::size_t offset = 0;
            while (offset < data.size())
            {
                const ssize_t n = co_await socket.asyncSend(data.data() + offset, data.size() - offset);
                if (n <= 0) co_return false;
                offset += static_cast<std::size_t>(n);
            }
            co_return true;
        }

        // ─── HTTPS 连路 ──────────────────────────────────────────────
        Core::Task<std::unique_ptr<HttpClientResponse>> doHttpsRequest(
                Core::EventLoop &loop, const std::string_view method,
                const ParsedUrl &u, const std::string_view contentType,
                const std::string_view body)
        {
            // 1. 解析主机名
            auto addrs = co_await Core::AsyncResolver::resolve(loop, u.host, u.port);
            if (addrs.empty()) co_return nullptr;

            // 2. 连接
            Core::AsyncSocket sock = Core::AsyncSocket::create(loop);
            try { co_await sock.asyncConnect(addrs[0]); }
            catch (const Base::Exception &) { co_return nullptr; }

            // 3. 创建 SSL
            auto *ctx = clientSslCtx();
            if (!ctx) co_return nullptr;
            SSL *ssl = SSL_new(ctx);
            if (!ssl) co_return nullptr;

            // 4. 设置 SNI
            SSL_set_tlsext_host_name(ssl, u.host.c_str());

            // 5. 创建 TlsSocket 并握手
            Core::TlsSocket tlsSocket(ssl, loop, std::move(sock));
            try { co_await tlsSocket.handshake(); }
            catch (const Base::Exception &) { co_return nullptr; }

            // 6. 发送请求
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
            if (!co_await sendAll(tlsSocket, request)) co_return nullptr;

            // 7. 接收响应
            HttpResponseParser parser;
            std::array<char, 4096> buf{};
            while (true)
            {
                const ssize_t n = co_await tlsSocket.asyncReceive(buf.data(), buf.size());
                if (n <= 0) break;
                parser.feed({buf.data(), static_cast<std::size_t>(n)});
                if (parser.isComplete()) break;
            }
            if (!parser.isComplete()) parser.endOfStream();
            if (!parser.isComplete() || parser.hasFailed()) co_return nullptr;

            auto res = std::make_unique<HttpClientResponse>();
            res->statusCode   = parser.result().statusCode;
            res->reasonPhrase = parser.result().reasonPhrase;
            res->headers      = parser.result().headers;
            res->body         = parser.result().body;
            co_return res;
        }

        // ─── 明文 HTTP 连路 ─────────────────────────────────────────
        Core::Task<std::unique_ptr<HttpClientResponse>> doPlainRequest(
                Core::EventLoop &loop, const std::string_view method,
                const ParsedUrl &u, const std::string_view contentType,
                const std::string_view body)
        {
            auto streamPtr = co_await TcpClient::connect(loop, u.host, u.port);
            if (!streamPtr) co_return nullptr;
            auto &stream = *streamPtr;

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

            HttpResponseParser parser;
            std::array<char, 4096> buf{};
            while (true)
            {
                const ssize_t n = co_await stream.read(buf.data(), buf.size());
                if (n <= 0) break;
                parser.feed({buf.data(), static_cast<std::size_t>(n)});
                if (parser.isComplete()) break;
            }
            if (!parser.isComplete()) parser.endOfStream();
            if (!parser.isComplete() || parser.hasFailed()) co_return nullptr;

            auto res = std::make_unique<HttpClientResponse>();
            res->statusCode   = parser.result().statusCode;
            res->reasonPhrase = parser.result().reasonPhrase;
            res->headers      = parser.result().headers;
            res->body         = parser.result().body;
            co_return res;
        }
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::get(Core::EventLoop &loop, std::string_view url)
    {
        auto u = parseUrl(url);
        if (u.scheme == "https")
            co_return co_await doHttpsRequest(loop, "GET", u, {}, {});
        co_return co_await doPlainRequest(loop, "GET", u, {}, {});
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::post(
            Core::EventLoop &loop, std::string_view url, std::string_view contentType, std::string_view body)
    {
        auto u = parseUrl(url);
        if (u.scheme == "https")
            co_return co_await doHttpsRequest(loop, "POST", u, contentType, body);
        co_return co_await doPlainRequest(loop, "POST", u, contentType, body);
    }
} // namespace AsynGyanis::Net
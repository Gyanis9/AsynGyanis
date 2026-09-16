#include "Net/Http/Client/HttpClient.h"
#include "Base/Exception/Exception.h"
#include "Base/Log/LogMacros.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/AsyncResolver.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Tls/TlsSocket.h"
#include "Platform/IO/Socket.h"
#include "Net/Tcp/TcpClient.h"

#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <charconv>
#include <chrono>
#include <memory>
#include <optional>
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

        /// 判断主机名是不是 IP 字面量：IP 与 DNS 名的证书校验规则不同，必须分开处理
        bool isIpLiteral(const std::string &host)
        {
            std::array<std::uint8_t, 16> addressBytes{};
            return ::inet_pton(AF_INET, host.c_str(), addressBytes.data()) == 1 ||
                   ::inet_pton(AF_INET6, host.c_str(), addressBytes.data()) == 1;
        }

        /**
         * @brief 到期就把套接字关掉，让在途的收发立刻以失败返回
         * @details 收发协程挂在套接字的等待器上，而等待器没有取消接口；「关掉描述符」是
         *          本框架里让它们立刻收尾的既定手段（与 stop()/close() 收尾走同一条路径）
         * @param loop 所属事件循环（提供定时器）
         * @param socket 被监视的套接字（非拥有；其生命周期由调用方保证长于本协程）
         * @param requestTimeout 整体时限
         * @param isCancelled 请求已结束的标志；为真时看门狗醒来什么都不做
         */
        template<typename SocketType>
        Core::Task<void> watchDeadline(Core::EventLoop &loop, SocketType &socket,
                                       const std::chrono::milliseconds requestTimeout, const bool &isCancelled)
        {
            Core::Timer timer(loop);
            co_await timer.waitFor(requestTimeout);
            if (isCancelled)
            {
                co_return;
            }
            // 超时是「对端不说话」这类外部状况，调用方只会拿到一个空响应，因此在这里留一条日志：
            // 否则排查现场时只能看到「请求没结果」，看不出是被时限掐断的
            LOG_WARN_FMT("HttpClient: 请求超过 {} 毫秒仍未完成，已关闭连接", requestTimeout.count());
            socket.close();
        }

        /**
         * @brief 看门狗的持有者：析构（含异常展开）时先撤销看门狗，套接字随后才销毁
         * @details **声明顺序即安全**：把它声明在套接字之后，任何返回路径都会先销毁本对象，
         *          绝不留下一个还在等时限、醒来却要关一个已销毁套接字的协程帧。
         *          撤销即销毁协程帧——帧里等待中的定时器随帧析构一起注销，不留常驻等待
         */
        template<typename SocketType>
        class DeadlineGuard
        {
        public:
            /**
             * @brief 启动看门狗
             * @param loop 所属事件循环
             * @param socket 被监视的套接字
             * @param requestTimeout 整体时限
             */
            DeadlineGuard(Core::EventLoop &loop, SocketType &socket, const std::chrono::milliseconds requestTimeout) :
                m_watchdog(watchDeadline(loop, socket, requestTimeout, m_isCancelled))
            {
                m_watchdog->handle().resume(); // 惰性协程：手动启动
            }

            ~DeadlineGuard()
            {
                m_isCancelled = true;
                m_watchdog.reset();
            }

            DeadlineGuard(const DeadlineGuard &) = delete;

            DeadlineGuard &operator=(const DeadlineGuard &) = delete;

            DeadlineGuard(DeadlineGuard &&) = delete;

            DeadlineGuard &operator=(DeadlineGuard &&) = delete;

        private:
            bool                        m_isCancelled{false}; ///< 请求已结束（看门狗协程按引用持有）
            std::optional<Core::Task<>> m_watchdog;           ///< 看门狗协程帧：置空即撤销
        };

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
                const std::string_view body, const std::chrono::milliseconds requestTimeout)
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

            // 4. 设置 SNI 与**主机名校验**：链校验只证明「证书由受信 CA 签发」，不证明
            //    「签发的对象就是我们要访问的那台主机」——少了这一步，任何受信 CA 给他域签的
            //    证书都能冒充目标（CWE-297）。IP 字面量与 DNS 名的匹配规则不同，必须分开设置
            SSL_set_tlsext_host_name(ssl, u.host.c_str());
            if (isIpLiteral(u.host))
            {
                X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), u.host.c_str());
            } else
            {
                SSL_set1_host(ssl, u.host.c_str());
            }

            // 5. 创建 TlsSocket 并握手。看门狗声明在套接字之后：此刻起（握手到收完响应）
            //    每一步都受请求级时限约束，而任何返回路径都会先撤销看门狗再销毁套接字
            Core::TlsSocket tlsSocket(ssl, loop, std::move(sock));
            const DeadlineGuard<Core::TlsSocket> deadline(loop, tlsSocket, requestTimeout);
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

            // 6-7. 发送与接收。整段包在 try 里：套接字在等待中被关闭（超时掐断、对端收尾）
            //      时底层会抛异常，而本接口的契约是失败返回空响应，异常不该逃给调用方
            try
            {
                if (!co_await sendAll(tlsSocket, request)) co_return nullptr;

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
            } catch (const Base::Exception &)
            {
                co_return nullptr;
            }
        }

        // ─── 明文 HTTP 连路 ─────────────────────────────────────────
        Core::Task<std::unique_ptr<HttpClientResponse>> doPlainRequest(
                Core::EventLoop &loop, const std::string_view method,
                const ParsedUrl &u, const std::string_view contentType,
                const std::string_view body, const std::chrono::milliseconds requestTimeout)
        {
            auto streamPtr = co_await TcpClient::connect(loop, u.host, u.port);
            if (!streamPtr) co_return nullptr;
            auto &stream = *streamPtr;

            // 与 HTTPS 连路同一道闸：连上之后的每一步都受请求级时限约束
            const DeadlineGuard<TcpStream> deadline(loop, stream, requestTimeout);

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

            // 与 HTTPS 连路同一处置：等待中被关闭的套接字会抛异常，这里统一折成空响应
            try
            {
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
            } catch (const Base::Exception &)
            {
                co_return nullptr;
            }
        }
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::get(Core::EventLoop &loop, std::string_view url,
                                                                   const std::chrono::milliseconds requestTimeout)
    {
        auto u = parseUrl(url);
        if (u.scheme == "https")
            co_return co_await doHttpsRequest(loop, "GET", u, {}, {}, requestTimeout);
        co_return co_await doPlainRequest(loop, "GET", u, {}, {}, requestTimeout);
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::post(
            Core::EventLoop &loop, std::string_view url, std::string_view contentType, std::string_view body,
            const std::chrono::milliseconds requestTimeout)
    {
        auto u = parseUrl(url);
        if (u.scheme == "https")
            co_return co_await doHttpsRequest(loop, "POST", u, contentType, body, requestTimeout);
        co_return co_await doPlainRequest(loop, "POST", u, contentType, body, requestTimeout);
    }
} // namespace AsynGyanis::Net
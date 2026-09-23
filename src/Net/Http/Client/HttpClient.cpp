#include "Net/Http/Client/HttpClient.h"
#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
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
    namespace
    {
        /**
         * @brief 把 left 按 ASCII 折成小写后与 right 比
         * @details right 必须已是小写字面量（本文件里只用来认 "http"/"https" 两个常量）。URL 的协议名与
         *          主机名都是 ASCII（RFC 3986 §6.1），因此只做 ASCII 折叠，不走 locale 的 tolower——
         *          那会让同一份 URL 在不同机器上得出不同结论
         */
        [[nodiscard]] bool equalsIgnoreAsciiCase(const std::string_view left, const std::string_view right)
        {
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t index = 0; index < left.size(); ++index)
            {
                char foldedLeft = left[index];
                if (foldedLeft >= 'A' && foldedLeft <= 'Z')
                {
                    foldedLeft = static_cast<char>(foldedLeft - 'A' + 'a');
                }
                if (foldedLeft != right[index])
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 把冒号之后的端口文本读成一个可用的端口
         * @param text 端口文本，必须全为十进制数字
         * @param port 输出端口
         * @return true 取值在 1..65535 内
         */
        [[nodiscard]] bool parsePort(std::string_view text, std::uint32_t &port)
        {
            // 超过 5 位必然大于 65535，先挡掉，省得 from_chars 溢出后还要判符号
            if (text.empty() || text.size() > 5U)
            {
                return false;
            }
            const std::from_chars_result converted = std::from_chars(text.data(), text.data() + text.size(), port);
            return converted.ec == std::errc{} && converted.ptr == text.data() + text.size() && port >= 1U &&
                   port <= 65535U;
        }
    } // namespace

    ParsedUrl parseUrl(const std::string_view url)
    {
        // 请求行是把 path 原样拼出来的：里面若有 CR/LF 或空白，等于让调用方自己结束请求行、
        // 甚至插进新的头部（请求分裂）。这不是「请求失败」，是用法错误，当场报出来
        if (url.find_first_of("\r\n\t ") != std::string_view::npos)
        {
            throw Base::InvalidArgumentException("HttpClient：URL 里不允许出现空白或控制字符（会撕裂请求行）：「" +
                                                 std::string(url) + "」");
        }

        ParsedUrl parsed;
        std::string_view remainder = url;
        const std::size_t schemeSeparator = remainder.find("://");
        if (schemeSeparator != std::string_view::npos)
        {
            const std::string_view schemeText = remainder.substr(0, schemeSeparator);
            // 协议名大小写无关（RFC 3986 §6.2.3）：HTTPS:// 悄悄当成 http 就是把 TLS 整段降级
            if (equalsIgnoreAsciiCase(schemeText, "https"))
            {
                parsed.scheme = "https";
            }
            else if (equalsIgnoreAsciiCase(schemeText, "http"))
            {
                parsed.scheme = "http";
            }
            else
            {
                throw Base::InvalidArgumentException(R"(HttpClient：只支持 "http" 与 "https" 两种协议，收到的是「)" +
                                                     std::string(schemeText) + R"(」：换成 http(s):// 开头再试)");
            }
            remainder.remove_prefix(schemeSeparator + 3);
        }

        const std::size_t pathSeparator = remainder.find('/');
        const std::string_view authority = pathSeparator == std::string_view::npos ? remainder : remainder.substr(0, pathSeparator);
        const std::string_view pathPart = pathSeparator == std::string_view::npos ? std::string_view{} : remainder.substr(pathSeparator);

        // 方括号里的是 IP 字面量（RFC 3986 §3.2.2）：IPv6 自带冒号，不这样区分就分不清哪段是端口
        std::string_view hostText = authority;
        std::string_view portText;
        bool hasExplicitPort = false;
        if (!authority.empty() && authority.front() == '[')
        {
            const std::size_t closingBracket = authority.find(']');
            if (closingBracket == std::string_view::npos)
            {
                throw Base::InvalidArgumentException(R"(HttpClient：URL 的主机部分方括号没闭合（IPv6 字面量要写成 "[::1]:8080" 的形式）：「)" +
                                                     std::string(authority) + R"(」)");
            }
            hostText = authority.substr(1, closingBracket - 1);
            const std::string_view afterBracket = authority.substr(closingBracket + 1);
            if (!afterBracket.empty())
            {
                if (afterBracket.front() != ':')
                {
                    throw Base::InvalidArgumentException(R"(HttpClient：URL 的主机部分在 "]" 之后还跟着多余字符（端口要写成 ":8080"）：「)" +
                                                         std::string(authority) + R"(」)");
                }
                hasExplicitPort = true;
                portText = afterBracket.substr(1);
            }
        }
        else
        {
            const std::size_t portSeparator = authority.rfind(':');
            if (portSeparator != std::string_view::npos)
            {
                // 冒号不止一个又没有方括号：那是没按规范包起来的 IPv6，拆出来的「主机」会是半截地址
                if (authority.find(':') != portSeparator)
                {
                    throw Base::InvalidArgumentException(R"(HttpClient：URL 的主机是 IPv6 时必须写成方括号形式（"[::1]:8080"）：「)" +
                                                         std::string(authority) + R"(」)");
                }
                hasExplicitPort = true;
                hostText = authority.substr(0, portSeparator);
                portText = authority.substr(portSeparator + 1);
            }
        }

        if (hostText.empty())
        {
            throw Base::InvalidArgumentException(R"(HttpClient：URL 里没有主机部分：「)" + std::string(url) + R"(」)");
        }
        parsed.host = std::string(hostText);

        if (hasExplicitPort)
        {
            std::uint32_t port = 0U;
            if (!parsePort(portText, port))
            {
                // 原先这里回落到 80：一个写错的 https 端口会静默连到明文端口上，
                // 宁可当场报错也不替调用方猜一个
                throw Base::InvalidArgumentException(R"(HttpClient：URL 的端口「)" + std::string(portText) +
                                                     R"(」不是 1..65535 的十进制数：完整 URL 是「)" + std::string(url) + R"(」)");
            }
            parsed.port = static_cast<uint16_t>(port);
        }
        else
        {
            parsed.port = (parsed.scheme == "https") ? 443 : 80;
        }
        if (!pathPart.empty())
        {
            parsed.path = std::string(pathPart);
        }
        return parsed;
    }

    namespace
    {
        /**
         * @brief 拼请求头的 Host 字段值
         * @details IP 字面量在拆 URL 时按 RFC 3986 §3.2.2 去掉了方括号，这里要加回去：地址里自带冒号，
         *          不加回去「Host: ::1」会把头部与端口分隔符混成一团
         */
        void appendHostHeader(std::string &request, const ParsedUrl &url)
        {
            request += "Host: ";
            if (url.host.find(':') != std::string::npos)
            {
                request += '[';
                request += url.host;
                request += ']';
            }
            else
            {
                request += url.host;
            }
            request += "\r\n";
        }
    } // namespace

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

            // SSL 必须绑上底层描述符才有 BIO 可用：少了这一步 SSL_connect 立刻失败，
            // 而且错误队列是空的（error:00000000），现场只剩「握手失败」四个字
            if (::SSL_set_fd(ssl, sock.fileDescriptor()) == 0)
            {
                SSL_free(ssl);
                co_return nullptr;
            }

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
            Core::TlsSocket tlsSocket(ssl, loop, std::move(sock), Core::TlsSocket::Role::Client);
            const DeadlineGuard<Core::TlsSocket> deadline(loop, tlsSocket, requestTimeout);
            try { co_await tlsSocket.handshake(); }
            catch (const Base::Exception &) { co_return nullptr; }

            // 6. 发送请求
            std::string request;
            request.reserve(256 + body.size());
            request += method; request += ' ';
            request += u.path; request += " HTTP/1.1\r\n";
            appendHostHeader(request, u);
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
                if (method == "HEAD")
                {
                    // 对 HEAD 的应答一律在头块之后结束（RFC 9112 §6.3 第 1 条）：不标记的话
                    // 「不带 Content-Length 的 HEAD 应答」会被当成读到关闭，客户端只能干等
                    parser.markAsHeadResponse();
                }
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
            appendHostHeader(request, u);
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
                if (method == "HEAD")
                {
                    // 与 HTTPS 连路同一处置：HEAD 的应答在头块之后结束，不等正文也不等关闭
                    parser.markAsHeadResponse();
                }
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
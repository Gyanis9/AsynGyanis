#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/Client/HttpOutboundConnectionPool.h"
#include "Net/Http/Client/RequestDeadlineGuard.h"
#include "Net/Http2/Http2ClientConnection.h"
#include "Net/Http2/Http2Session.h"
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
        /// 拼出 Host / :authority 用的主机文本：IPv6 字面量要带方括号，否则端口分隔符会糊进地址里
        std::string authorityText(const ParsedUrl &url)
        {
            if (url.host.find(':') != std::string::npos)
            {
                return '[' + url.host + ']';
            }
            return url.host;
        }

        void appendHostHeader(std::string &request, const ParsedUrl &url)
        {
            request += "Host: ";
            request += authorityText(url);
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

        /// 一次出站交换的结论：响应，以及「有没有读到过响应的第一个字节」
        struct OutboundExchange
        {
            std::unique_ptr<HttpClientResponse> response;
            bool isAnyByteReceived{false};
        };

        /**
         * @brief 从整体时限里扣掉已经花掉的那一段
         * @details 契约是「握手、发送、收完响应三段之和」：分两段各给一次完整时限，等于把契约放宽
         *          一倍。返回空表示预算已经用完，调用方据此直接判失败。
         * @param startedAt 本次请求的开始时刻
         * @param requestTimeout 整体时限
         * @return std::optional<std::chrono::milliseconds> 还剩多少；已经用尽则为空
         */
        std::optional<std::chrono::milliseconds> remainingBudget(const std::chrono::steady_clock::time_point startedAt,
                                                                const std::chrono::milliseconds requestTimeout)
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startedAt);
            if (elapsed >= requestTimeout)
            {
                return std::nullopt;
            }
            return requestTimeout - elapsed;
        }

        /**
         * @brief 按响应头判断这条连接还能不能接着用
         * @details 对端声明 close 就不能再用（RFC 9112 §9.6：那是对本条连接的收尾声明）。没写这条头
         *          就是 HTTP/1.1 的默认，可以继续用。1.0 的对端默认收完就关，而解析器不把版本号交出
         *          来：那一档走「用了才发现对端已关」的路径——读回来的是干净的 0 字节，调用方按
         *          「一个字节都没收到」重开一条，不会把上一条的尾巴接到下一条头上。
         * @param responseHeaders 已收齐响应的头部字段（名与值按收到的顺序原样留着）
         * @return true 这条连接还能再发一条请求
         */
        bool isResponseReusable(const std::vector<std::pair<std::string, std::string>> &responseHeaders)
        {
            constexpr std::string_view kConnectionHeaderName = "connection";
            for (const auto &header: responseHeaders)
            {
                if (header.first.size() != kConnectionHeaderName.size())
                {
                    continue;
                }
                bool isConnectionHeader = true;
                for (std::size_t index = 0; index < kConnectionHeaderName.size(); ++index)
                {
                    char folded = header.first[index];
                    if (folded >= 'A' && folded <= 'Z')
                    {
                        folded = static_cast<char>(folded + ('a' - 'A'));
                    }
                    if (folded != kConnectionHeaderName[index])
                    {
                        isConnectionHeader = false;
                        break;
                    }
                }
                if (isConnectionHeader)
                {
                    return header.second.find("close") == std::string::npos;
                }
            }
            return true;
        }

        /**
         * @brief 拼出请求文
         * @param method 方法，原样写进请求行
         * @param u 已拆开的 URL
         * @param contentType 正文媒体类型，只随非空正文写出
         * @param body 正文
         * @param isKeepAlive 这次请求是否走复用连接：走复用就明写 keep-alive，不走则沿用 close
         * @return std::string 完整的请求文（含正文）
         */
        std::string buildRequestText(const std::string_view method, const ParsedUrl &u, const std::string_view contentType,
                                     const std::string_view body, const bool isKeepAlive)
        {
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
            // 明写 keep-alive：HTTP/1.1 本就是它，但对端与中间盒都可能按 1.0 的默认值办事
            request += isKeepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
            request += "\r\n";
            request += body;
            return request;
        }

        /**
         * @brief 在一条已建立的连接上走完一次请求/响应
         * @param loop 所属事件循环（看门狗的定时器用它）
         * @param connection 已连上、TLS 也已握手完的连接
         * @param requestText 完整请求文
         * @param isHeadRequest 这是不是 HEAD：HEAD 的应答一律在头块之后结束（RFC 9112 §6.3 第 1 条），
         *        不标记的话「不带 Content-Length 的 HEAD 应答」会被当成读到关闭，客户端只能干等
         * @param requestTimeout 本次交换的时限（调用方已把前面几段花掉的时间扣掉）
         * @return OutboundExchange 响应与「有没有读到过字节」；响应为空即失败
         */
        Core::Task<OutboundExchange> exchangeOnConnection(Core::EventLoop &loop, HttpOutboundConnection &connection,
                                                         const std::string &requestText, const bool isHeadRequest,
                                                         const std::chrono::milliseconds requestTimeout)
        {
            const RequestDeadlineGuard<HttpOutboundConnection> deadline(loop, connection, requestTimeout, "HttpClient");
            OutboundExchange exchange;
            if (isHeadRequest)
            {
                connection.parser().markAsHeadResponse();
            }
            try
            {
                if (!co_await connection.send(requestText))
                {
                    co_return exchange;
                }

                std::array<char, 4096> buffer{};
                while (true)
                {
                    const ssize_t receivedByteCount = co_await connection.receive(buffer.data(), buffer.size());
                    if (receivedByteCount < 0)
                    {
                        co_return exchange;
                    }
                    if (receivedByteCount == 0)
                    {
                        break;
                    }
                    exchange.isAnyByteReceived = true;
                    connection.parser().feed({buffer.data(), static_cast<std::size_t>(receivedByteCount)});
                    if (connection.parser().isComplete())
                    {
                        break;
                    }
                }
            }
            catch (const std::exception &)
            {
                // 等待中套接字被关掉（超时掐断、对端收尾）时底层抛异常：本接口的契约是「失败返回空响应」，
                // 异常不许逃给调用方。抛出点之后的字节序已经不可信，这条连接当场收口——绝不还回池里
                connection.close();
                co_return exchange;
            }
            if (!connection.parser().isComplete())
            {
                connection.parser().endOfStream();
            }
            if (!connection.parser().isComplete() || connection.parser().hasFailed())
            {
                co_return exchange;
            }

            const HttpResponseInfo &parsed = connection.parser().result();
            auto response = std::make_unique<HttpClientResponse>();
            response->statusCode = parsed.statusCode;
            response->reasonPhrase = parsed.reasonPhrase;
            response->headers = parsed.headers;
            response->body = parsed.body;
            exchange.response = std::move(response);
            co_return exchange;
        }

        /**
         * @brief 建一条明文连接
         * @param loop 所属事件循环
         * @param key 目标身份（主机与端口；TLS 位由调用方按 scheme 定）
         * @return std::unique_ptr<HttpOutboundConnection> 连上了就交出连接，连接失败返回空
         */
        Core::Task<std::unique_ptr<HttpOutboundConnection>> establishPlainConnection(Core::EventLoop &loop,
                                                                                    const HttpOutboundEndpointKey &key)
        {
            auto stream = co_await TcpClient::connect(loop, key.host, key.port);
            if (!stream)
            {
                co_return nullptr;
            }
            co_return HttpOutboundConnection::forPlain(key, std::move(*stream));
        }

        /**
         * @brief 建一条 TLS 连接：解析 → 连上 → 建 SSL → 设 SNI 与主机名校验 → 握手
         * @details 握手成功之前这条连接还不属于池：它可能停在「对端不说话」上，所以握手自己受时限
         *          约束，超时就把 SSL 与底层描述符一起丢掉（原实现也是这一段一路管到响应收完，这里
         *          只是把它拆成「握手」与「交换」两段，各拿剩余的预算）。
         * @param loop 所属事件循环
         * @param u 已拆开的 URL
         * @param handshakeTimeout 握手一段的时限
         * @return std::unique_ptr<HttpOutboundConnection> 握手成功就交出连接；任一步失败返回空
         */
        Core::Task<std::unique_ptr<HttpOutboundConnection>> establishSecureConnection(
                Core::EventLoop &loop, const ParsedUrl &u, const std::chrono::milliseconds handshakeTimeout)
        {
            HttpOutboundEndpointKey key{u.host, u.port, true};

            auto addrs = co_await Core::AsyncResolver::resolve(loop, u.host, u.port);
            if (addrs.empty())
            {
                co_return nullptr;
            }
            Core::AsyncSocket sock = Core::AsyncSocket::create(loop);
            try { co_await sock.asyncConnect(addrs[0]); }
            catch (const Base::Exception &) { co_return nullptr; }

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

            // 设 SNI 与**主机名校验**：链校验只证明「证书由受信 CA 签发」，不证明「签发的对象就是我们
            // 要访问的那台主机」——少了这一步，任何受信 CA 给他域签的证书都能冒充目标（CWE-297）。
            // IP 字面量与 DNS 名的匹配规则不同，必须分开设置
            SSL_set_tlsext_host_name(ssl, u.host.c_str());
            if (isIpLiteral(u.host))
            {
                X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), u.host.c_str());
            } else
            {
                SSL_set1_host(ssl, u.host.c_str());
            }

            // 带 ALPN：h2 只能靠协商结果识别，不在 ClientHello 里声明就永远只会收到 HTTP/1.1 的答。
            // 两个名字都提，本端偏好写在前面（服务端按自己的偏好在这份列表里挑）；列表的线格式是
            // 「单字节长度 + 协议名」的串联，不是逗号分隔——填错不会报错，只会对端一条都匹配不上
            static constexpr unsigned char kAlpnProtocolList[] = {
                    2U, 'h', '2',
                    8U, 'h', 't', 't', 'p', '/', '1', '.', '1',
            };
            // OpenSSL 这一支的返回值是反的：0 才是成功。名字用 SSL_set_alpn_protos 而不是 ...protocols：
            // 前者从 1.0.2 起就是真身并一直保留，后者只是新版本里加的兼容写法，按旧名调两边都在
            if (::SSL_set_alpn_protos(ssl, kAlpnProtocolList,
                                      static_cast<unsigned int>(sizeof(kAlpnProtocolList))) != 0)
            {
                SSL_free(ssl);
                co_return nullptr;
            }

            auto tlsSocket = std::make_unique<Core::TlsSocket>(ssl, loop, std::move(sock), Core::TlsSocket::Role::Client);
            const RequestDeadlineGuard<Core::TlsSocket> handshakeDeadline(loop, *tlsSocket, handshakeTimeout,
                                                            "HttpClient");
            try
            {
                co_await tlsSocket->handshake();
            }
            catch (const Base::Exception &)
            {
                co_return nullptr;
            }
            co_return HttpOutboundConnection::forSecure(key, std::move(tlsSocket));
        }

        /**
         * @brief 一次 h2 交换的结论：响应，以及「这条流上有没有收到过对端的任何帧」
         * @details 后一位与 HTTP/1.1 那边同名同义：只在「一个字节都没回来」时允许换一条连接重来一次
         *          （空闲连接被对端收掉是 keep-alive 的固有竞态），收到过就算响应本身出了问题，重发
         *          会把非幂等请求做两遍。
         */
        struct Http2Exchange
        {
            std::unique_ptr<HttpClientResponse> response;
            bool isAnyByteReceived{false};
        };

        /**
         * @brief 在一条已经协商好的 h2 连接上走完一次请求
         * @details 连接的生命周期不在这里：新建那条要走前奏（start），复用这条前奏早就走完了——
         *          把两件事分开，池才能拿同一段代码服务「刚建好的」与「留着待命的」两种连接。
         * @param client 已完成前奏的 h2 连接（由调用方持有；它自己认得归属的循环）
         * @param u 已拆开的 URL
         * @param method 方法
         * @param contentType 正文媒体类型，只随非空正文写出
         * @param body 正文
         * @param startedAt 本次请求的开始时刻，用于把整体时限摊到剩下的那一段上
         * @param requestTimeout 整体时限
         * @return Http2Exchange 响应；失败时 response 为空。reasonPhrase 恒为空——HTTP/2 没有原因
         *         短语这一项，状态语义只靠 :status
         */
        Core::Task<Http2Exchange> exchangeOnHttp2(
                Http2ClientConnection &client, const ParsedUrl &u,
                const std::string_view method, const std::string_view contentType, const std::string_view body,
                const std::chrono::steady_clock::time_point startedAt, const std::chrono::milliseconds requestTimeout)
        {
            Http2Exchange exchange;
            std::vector<std::pair<std::string, std::string>> extraHeaders;
            if (!body.empty())
            {
                extraHeaders.emplace_back("content-type", std::string(contentType));
            }
            // 主机文本与协议名都要先落到具名对象上：co_await 挂起期间 string_view 指着的临时串会先析构
            const std::string authority = authorityText(u);
            const std::string_view scheme = u.scheme == "https" ? "https" : "http";
            const std::optional<std::chrono::milliseconds> exchangeBudget = remainingBudget(startedAt, requestTimeout);
            if (!exchangeBudget.has_value())
            {
                co_return exchange;
            }
            Http2ClientResponse response = co_await client.request(
                    scheme, authority, method, u.path, extraHeaders, body, *exchangeBudget);
            exchange.isAnyByteReceived = response.isAnyByteReceived;
            if (!response.isOk())
            {
                // 状态码为 0（没收到响应头）或被对端中途 RST 掉：都不算一次成功的出站
                co_return exchange;
            }
            auto result = std::make_unique<HttpClientResponse>();
            result->statusCode = response.statusCode;
            result->headers = std::move(response.headers);
            result->body = std::move(response.body);
            exchange.response = std::move(result);
            co_return exchange;
        }

        /**
         * @brief 走完一次出站请求：有池就先复用、用完还回去，没池就按一次一条连接的老口径
         * @param loop 所属事件循环
         * @param method 方法
         * @param u 已拆开的 URL
         * @param contentType 正文媒体类型
         * @param body 正文
         * @param requestTimeout 整体时限（握手、发送、收完响应三段之和）
         * @param pool 空闲连接池；为空即一次一条连接
         * @return std::unique_ptr<HttpClientResponse> 响应；失败（含超时）返回空
         */
        Core::Task<std::unique_ptr<HttpClientResponse>> performRequest(
                Core::EventLoop &loop, const std::string_view method, const ParsedUrl &u,
                const std::string_view contentType, const std::string_view body,
                const std::chrono::milliseconds requestTimeout, HttpOutboundConnectionPool *pool)
        {
            const std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now();
            const bool isKeepAlive = pool != nullptr;
            // 请求文按要再拼：h2 那一支用不上它（帧里没有请求行），提前拼一份等于把正文整块多拷一次
            const auto makeRequestText = [&]
            {
                return buildRequestText(method, u, contentType, body, isKeepAlive);
            };
            const bool isHeadRequest = method == "HEAD";

            if (pool != nullptr)
            {
                const HttpOutboundEndpointKey key{u.host, u.port, u.scheme == "https"};
                // h2 的待命连接问在 h1 的空闲表之前：一台主机的 ALPN 结果是稳定的，两处不会同时有货
                if (auto cachedHttp2 = pool->acquireHttp2(key); cachedHttp2 != nullptr)
                {
                    Http2Exchange cachedExchange = co_await exchangeOnHttp2(
                            *cachedHttp2, u, method, contentType, body, startedAt, requestTimeout);
                    if (cachedExchange.response)
                    {
                        pool->releaseHttp2(std::move(cachedHttp2));
                        co_return std::move(cachedExchange.response);
                    }
                    if (cachedExchange.isAnyByteReceived)
                    {
                        co_return nullptr;
                    }
                    // 一个字节都没回来：多半是对端在我们手里把这条连接收了（与 h1 的 keep-alive 同一
                    // 条竞态）。这条就此作废——不作废也没有下一句，HPACK 动态表跟着连接一起丢——
                    // 直接往下重开一条重来一次，对调用方仍是一次成功请求
                }
                if (auto reused = pool->acquire(key))
                {
                    const std::optional<std::chrono::milliseconds> reusedBudget = remainingBudget(startedAt, requestTimeout);
                    if (reusedBudget.has_value())
                    {
                        const std::string requestText = makeRequestText();
                        OutboundExchange exchange = co_await exchangeOnConnection(loop, *reused, requestText, isHeadRequest,
                                                                                 *reusedBudget);
                        if (exchange.response)
                        {
                            if (isResponseReusable(exchange.response->headers))
                            {
                                reused->prepareForNextRequest();
                                pool->release(std::move(reused));
                            }
                            co_return std::move(exchange.response);
                        }
                        // 复用来的连接上连一个字节都没读到：这多半是对端在我们手里空闲期间把它关掉了
                        // （keep-alive 的经典竞态）。换一条新连接重来一次，对调用方仍是一次成功请求；
                        // 读到过字节才失败的不能重来——那已经是「响应本身有问题」，重发会把非幂等请求做两遍
                        if (exchange.isAnyByteReceived)
                        {
                            co_return nullptr;
                        }
                        reused->close();
                    }
                }
            }

            std::unique_ptr<HttpOutboundConnection> connection;
            if (u.scheme == "https")
            {
                const std::optional<std::chrono::milliseconds> handshakeBudget = remainingBudget(startedAt, requestTimeout);
                if (!handshakeBudget.has_value())
                {
                    co_return nullptr;
                }
                connection = co_await establishSecureConnection(loop, u, *handshakeBudget);
            }
            else
            {
                connection = co_await establishPlainConnection(loop, HttpOutboundEndpointKey{u.host, u.port, false});
            }
            if (!connection)
            {
                co_return nullptr;
            }
            if (connection->selectedAlpnProtocol() == kHttp2AlpnProtocolName)
            {
                // ALPN 选到了 h2：换一种说话方式。前奏在这里走——从池里拿回来的那条早就走过了，
                // 所以这一步只属于「刚建好的」这一支
                const std::optional<std::chrono::milliseconds> startBudget = remainingBudget(startedAt, requestTimeout);
                auto http2Connection = std::make_unique<Http2ClientConnection>(loop, std::move(connection));
                if (!startBudget.has_value() || !co_await http2Connection->start(*startBudget))
                {
                    co_return nullptr;
                }
                Http2Exchange freshExchange = co_await exchangeOnHttp2(
                        *http2Connection, u, method, contentType, body, startedAt, requestTimeout);
                if (pool != nullptr)
                {
                    if (http2Connection->isHealthy())
                    {
                        pool->releaseHttp2(std::move(http2Connection));
                    }
                }
                else
                {
                    // 没有池就是一次性的：主动 shutdown 而不是任其析构，否则对端把这次收口记成 abrupt
                    co_await http2Connection->shutdown();
                }
                co_return std::move(freshExchange.response);
            }

            const std::optional<std::chrono::milliseconds> exchangeBudget = remainingBudget(startedAt, requestTimeout);
            if (!exchangeBudget.has_value())
            {
                co_return nullptr;
            }
            const std::string requestText = makeRequestText();
            OutboundExchange exchange = co_await exchangeOnConnection(loop, *connection, requestText, isHeadRequest,
                                                                     *exchangeBudget);
            if (exchange.response && pool != nullptr && isResponseReusable(exchange.response->headers))
            {
                connection->prepareForNextRequest();
                pool->release(std::move(connection));
            }
            co_return std::move(exchange.response);
        }
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::get(Core::EventLoop &loop, std::string_view url,
                                                                   const std::chrono::milliseconds requestTimeout)
    {
        auto u = parseUrl(url);
        co_return co_await performRequest(loop, "GET", u, {}, {}, requestTimeout, nullptr);
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::post(
            Core::EventLoop &loop, std::string_view url, std::string_view contentType, std::string_view body,
            const std::chrono::milliseconds requestTimeout)
    {
        auto u = parseUrl(url);
        co_return co_await performRequest(loop, "POST", u, contentType, body, requestTimeout, nullptr);
    }

    HttpClient::HttpClient(Core::EventLoop &loop, const HttpOutboundConnectionPool::Config poolConfig) noexcept
        : m_loop(&loop)
        , m_pool(poolConfig)
    {
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::get(std::string_view url,
                                                                   const std::chrono::milliseconds requestTimeout)
    {
        auto u = parseUrl(url);
        co_return co_await performRequest(*m_loop, "GET", u, {}, {}, requestTimeout, &m_pool);
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::post(
            std::string_view url, std::string_view contentType, std::string_view body,
            const std::chrono::milliseconds requestTimeout)
    {
        auto u = parseUrl(url);
        co_return co_await performRequest(*m_loop, "POST", u, contentType, body, requestTimeout, &m_pool);
    }

    std::size_t HttpClient::idleConnectionCount() const noexcept
    {
        return m_pool.idleConnectionCount();
    }

    std::size_t HttpClient::idleHttp2ConnectionCount() const noexcept
    {
        return m_pool.idleHttp2ConnectionCount();
    }

    void HttpClient::closeIdleConnections() noexcept
    {
        m_pool.closeAll();
    }
} // namespace AsynGyanis::Net
#include "Net/Http/Client/HttpClient.h"
#include "Core/Tls/TlsContext.h"
#include "Net/Http/Client/HttpContentCoding.h"
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
#include "Net/Tcp/TcpStream.h"

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
            std::string host = url.host.find(':') != std::string::npos ? '[' + url.host + ']' : url.host;
            // 端口只在**不等于该协议的默认端口**时写：RFC 9110 §7.2 对 Host、RFC 9113 §8.3.1 对
            // :authority 是同一条法。连到哪个端口由 URL 的端口决定，这里漏掉端口的代价是**站点选择**：
            // 按 Host 分虚拟主机的对端会把 8443 上的服务认成默认那一个，严格的实现直接回 421
            const bool isSchemeDefaultPort = (url.scheme == "https" && url.port == 443U)
                                             || (url.scheme != "https" && url.port == 80U);
            if (!isSchemeDefaultPort)
            {
                host += ':' + std::to_string(url.port);
            }
            return host;
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

        /// 这三个头部由客户端按本次请求的实际情况写，调用方给了就拒收而不是覆盖或并存
        bool isClientOwnedHeaderName(const std::string_view name)
        {
            return equalsIgnoreAsciiCase(name, "host") || equalsIgnoreAsciiCase(name, "content-length") ||
                   equalsIgnoreAsciiCase(name, "connection");
        }

        /**
         * @brief 文本里是否含 CR、LF、NUL 或其它控制字符
         * @return true 含控制字符（会结束或撕裂请求行）
         */
        [[nodiscard]] bool hasControlChar(const std::string_view text)
        {
            for (const char character: text)
            {
                // 按无符号取值判：头部与正文允许 UTF-8（≥0x80），只有 0x00..0x1F 与 0x7F 是控制字符
                const auto rawByte = static_cast<unsigned int>(static_cast<unsigned char>(character));
                if (rawByte < 0x20U || rawByte == 0x7fU)
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 名字是不是 HTTP token（RFC 9110 §5.6.2）
         * @details 方法名与头部名都受这条约束：token 之外只有空格与分隔符，把它们放进来就等于让调用方
         *          自己拼出第二个请求行或第二个字段
         */
        [[nodiscard]] bool isTokenText(const std::string_view text)
        {
            if (text.empty())
            {
                return false;
            }
            constexpr std::string_view kTokenSeparatorsAllowed = "!#$%&'*+-.^_`|~";
            for (const char character: text)
            {
                const bool isAlnum = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                                     (character >= '0' && character <= '9');
                if (!isAlnum && kTokenSeparatorsAllowed.find(character) == std::string_view::npos)
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 校验调用方给的请求参数（方法名与附加头部）
         * @details 必须在动套接字之前判完：拖到写出时才发现，等于白开一条连接、还把对端已经发回的响应
         *          丢掉。三种写法一律拒绝而不是转义或静默删掉——它们都会改变请求行的结构。
         * @param request 方法、正文与附加头部
         * @throws Base::InvalidArgumentException 方法名或头部名不合 token、头部值含 CR/LF 等控制字符，
         *         或占用了 Host、Content-Length、Connection 这三项由客户端负责的头部
         */
        void validateRequest(const HttpClientRequest &request)
        {
            if (!isTokenText(request.method))
            {
                throw Base::InvalidArgumentException(R"(HttpClient：请求方法「)" + request.method +
                                                     R"(」不合 HTTP token（只允许字母、数字与 "!#$%&'*+-.^_`|~"）：那会撕裂请求行)");
            }
            for (const HttpClientHeaderField &field: request.headers)
            {
                if (!isTokenText(field.first))
                {
                    throw Base::InvalidArgumentException(R"(HttpClient：附加头部的名字「)" + field.first +
                                                         R"(」为空或含控制字符、冒号之类分隔符：头部名必须是 HTTP token)");
                }
                if (hasControlChar(field.second))
                {
                    // 值里有 CR/LF 就能自己结束这一行并插入新字段——这是请求分裂，不转义也不删除
                    throw Base::InvalidArgumentException(R"(HttpClient：附加头部 ")" + field.first +
                                                         R"(" 的值里有控制字符（会撕裂请求行）：请去掉 CR/LF 后再发)");
                }
                if (isClientOwnedHeaderName(field.first))
                {
                    throw Base::InvalidArgumentException(R"(HttpClient：附加头部 ")" + field.first +
                                                         R"(" 由客户端按这次请求自己写，不能由调用方给：请删掉这一项)");
                }
            }
        }

        /**
         * @brief 拼出请求文（请求行 + 头部 + 正文）
         * @details 两条连路（明文与 TLS）与 HTTP/1.1 这一侧的复用共用这一份，避免各支长歪。附加头部排在
         *          Host 之后、Content-* 之前；字段顺序对语义无影响。写法不合规范的那几类由
         *          validateRequest 在动套接字之前就把住，这里不再重复判。
         * @param request 方法、正文与附加头部
         * @param u 已拆开的 URL
         * @param isKeepAlive 这次请求是否走复用连接：走复用就明写 keep-alive，不走则沿用 close
         * @return std::string 完整的请求文（含正文）
         */
        std::string buildRequestText(const HttpClientRequest &request, const ParsedUrl &u, const bool isKeepAlive)
        {
            std::string text;
            text.reserve(256 + request.body.size());
            text += request.method; text += ' ';
            text += u.path; text += " HTTP/1.1\r\n";
            appendHostHeader(text, u);
            for (const HttpClientHeaderField &field: request.headers)
            {
                text += field.first; text += ": "; text += field.second; text += "\r\n";
            }
            // 代为声明本端真能解回来的编码：调用方自己写过 accept-encoding 就整个不管正文（同 h2 那一支）
            if (shouldAdvertiseAcceptEncoding(request.headers))
            {
                text += "Accept-Encoding: "; text += kOutboundAcceptEncodingValue; text += "\r\n";
            }
            if (!request.body.empty())
            {
                if (!request.contentType.empty())
                {
                    text += "Content-Type: "; text += request.contentType; text += "\r\n";
                }
                text += "Content-Length: "; text += std::to_string(request.body.size()); text += "\r\n";
            }
            // 明写 keep-alive：HTTP/1.1 本就是它，但对端与中间盒都可能按 1.0 的默认值办事
            text += isKeepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
            text += "\r\n";
            text += request.body;
            return text;
        }

        /**
         * @brief 把解析器的结论折成响应；不成或未解完时写出分得清的原因
         * @details 「没解完」在这一层分不清是对端提前收线还是被请求时限掐断（两者都只表现为读到 0），
         *          所以措辞把两种可能都点出来，让调用方按现象去查时限而不是猜。
         * @param parser 已喂完字节的响应解析器
         * @param target 失败原因里要点出的目标（主机名）
         * @param failureReason 输出：失败原因，成功时不动它
         * @return std::unique_ptr<HttpClientResponse> 响应；没解完或解错即为空
         */
        std::unique_ptr<HttpClientResponse> takeParsedResponse(HttpResponseParser &parser, const std::string_view target,
                                                              std::string &failureReason)
        {
            if (parser.hasFailed())
            {
                failureReason = "响应不合规范或超出本端上限（状态行、头部与正文三条里的一项）：目标 " + std::string(target);
                return nullptr;
            }
            if (!parser.isComplete())
            {
                failureReason = "响应没读完就断了：对端提前收线，或本次请求已到时限（目标 " + std::string(target) + "）";
                return nullptr;
            }
            auto response = std::make_unique<HttpClientResponse>();
            response->statusCode   = parser.result().statusCode;
            response->reasonPhrase = parser.result().reasonPhrase;
            response->headers      = parser.result().headers;
            response->body         = parser.result().body;
            return response;
        }


        /**
         * @brief 在一条已建立的连接上走完一次请求/响应
         * @param loop 所属事件循环（看门狗的定时器用它）
         * @param connection 已连上、TLS 也已握手完的连接
         * @param requestText 完整请求文
         * @param isHeadRequest 这是不是 HEAD：HEAD 的应答一律在头块之后结束（RFC 9112 §6.3 第 1 条），
         *        不标记的话「不带 Content-Length 的 HEAD 应答」会被当成读到关闭，客户端只能干等
         * @param requestTimeout 本次交换的时限（调用方已把前面几段花掉的时间扣掉）
         * @param failureReason 输出：这次交换失败在哪一段（写出 / 读中断 / 响应不合规范）
         * @return OutboundExchange 响应与「有没有读到过字节」；响应为空即失败
         */
        Core::Task<OutboundExchange> exchangeOnConnection(Core::EventLoop &loop, HttpOutboundConnection &connection,
                                                         const std::string &requestText, const bool isHeadRequest,
                                                         const std::chrono::milliseconds requestTimeout,
                                                         std::string &failureReason)
        {
            const RequestDeadlineGuard<HttpOutboundConnection> deadline(loop, connection, requestTimeout, "HttpClient");
            OutboundExchange exchange;
            const std::string host = connection.endpointKey().host;
            if (isHeadRequest)
            {
                connection.parser().markAsHeadResponse();
            }
            try
            {
                if (!co_await connection.send(requestText))
                {
                    failureReason = "写出请求失败：对端在收完请求前收线，或本次请求已到时限（主机 " + host + "）";
                    co_return exchange;
                }

                std::array<char, 4096> buffer{};
                while (true)
                {
                    const ssize_t receivedByteCount = co_await connection.receive(buffer.data(), buffer.size());
                    if (receivedByteCount < 0)
                    {
                        failureReason = "读响应失败：通路报出不合规范的读取（主机 " + host + "）";
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
            catch (const std::exception &failure)
            {
                // 等待中套接字被关掉（超时掐断、对端收尾）时底层抛异常：本接口的契约是「失败返回空响应」，
                // 异常不许逃给调用方。抛出点之后的字节序已经不可信，这条连接当场收口——绝不还回池里
                LOG_WARN_FMT("HttpClient: 请求中断。底层原因：{}", failure.what());
                failureReason = "请求中断：套接字在等待中被关掉（时限掐断或对端收线，主机 " + host + "）";
                connection.close();
                co_return exchange;
            }
            if (!connection.parser().isComplete())
            {
                connection.parser().endOfStream();
            }
            exchange.response = takeParsedResponse(connection.parser(), host, failureReason);
            co_return exchange;
        }

        /**
         * @brief 带时限地连一条 TCP 通路：解析 → 非阻塞 connect → 到点就把套接字关掉
         * @details 这一段为什么必须由本层盯时限，而不是交给内核的 SYN 重试兜底：Windows 的完成端口后端
         *          上「连不上」并没有可写事件可等——实测对刚关掉的监听端口，系统 2 秒内就把连接拒了，
         *          而挂在可写上的协程永远等不到那一次唤醒，调用方就此无限期停在这里。Linux 那边靠可写
         *          事件带出 SO_ERROR，能自己醒。两条平台要按同一个契约走，所以时限统一由看门狗掐。
         * @param loop 所属事件循环
         * @param host 目标主机名或 IP 字面量
         * @param port 目标端口
         * @param connectTimeout 连接一段的时限
         * @param failureReason 输出：断在哪一段（解析地址 / 连不上或已到时限）
         * @return std::optional<Core::AsyncSocket> 连上了交出套接字，失败为空
         */
        Core::Task<std::optional<Core::AsyncSocket>> connectWithDeadline(
                Core::EventLoop &loop, const std::string &host, const std::uint16_t port,
                const std::chrono::milliseconds connectTimeout, std::string &failureReason)
        {
            const std::vector<Core::InetAddress> addresses = co_await Core::AsyncResolver::resolve(loop, host, port);
            if (addresses.empty())
            {
                failureReason = "解析地址失败：没能把「" + host + "」解析成可用地址";
                co_return std::nullopt;
            }
            for (const Core::InetAddress &address: addresses)
            {
                Core::AsyncSocket socket = Core::AsyncSocket::create(loop);
                try
                {
                    const RequestDeadlineGuard<Core::AsyncSocket> deadline(loop, socket, connectTimeout, "HttpClient");
                    co_await socket.asyncConnect(address);
                }
                catch (const Base::Exception &failure)
                {
                    // 底层原文只进日志（what() 里带抛出点，不该交回调用方），交出去的那句要能指出断在哪一段
                    LOG_WARN_FMT("HttpClient: 连接 {}:{} 失败。底层原因：{}", host, port, failure.what());
                    continue;
                }
                co_return socket;
            }
            failureReason = "建立 TCP 连接失败：对端拒绝、不可达或还没连上就超时（目标 " + host + ":"
                            + std::to_string(port) + "）";
            co_return std::nullopt;
        }

        /**
         * @brief 建一条明文连接
         * @param loop 所属事件循环
         * @param key 目标身份（主机与端口；TLS 位由调用方按 scheme 定）
         * @param failureReason 输出：连不上时点明断在哪一段
         * @param startedAt 本次请求的开始时刻，用于把整体时限摊到剩下那一段
         * @param requestTimeout 整体时限
         * @return std::unique_ptr<HttpOutboundConnection> 连上了就交出连接，连接失败返回空
         */
        Core::Task<std::unique_ptr<HttpOutboundConnection>> establishPlainConnection(
                Core::EventLoop &loop, const HttpOutboundEndpointKey &key,
                const std::chrono::steady_clock::time_point startedAt, const std::chrono::milliseconds requestTimeout,
                std::string &failureReason)
        {
            const std::optional<std::chrono::milliseconds> connectBudget = remainingBudget(startedAt, requestTimeout);
            if (!connectBudget.has_value())
            {
                failureReason = "本次请求已到时限：还没开始连接（目标 " + key.host + ":" + std::to_string(key.port) + "）";
                co_return nullptr;
            }
            auto socket = co_await connectWithDeadline(loop, key.host, key.port, *connectBudget, failureReason);
            if (!socket)
            {
                co_return nullptr;
            }
            co_return HttpOutboundConnection::forPlain(key, TcpStream(std::move(*socket)));
        }

        /**
         * @brief 建一条 TLS 连接：带时限连上 → 建 SSL → 设 SNI 与主机名校验 → 带 ALPN 握手
         * @details 握手成功之前这条连接还不属于池：它可能停在「对端不说话」上，所以连接与握手两段各自
         *          受剩余时限约束，超时就把 SSL 与底层描述符一起丢掉（原实现是这一段一路管到响应收完，
         *          这里只是把它拆成「连接」「握手」「交换」三段，各拿剩余的预算）。
         * @param loop 所属事件循环
         * @param u 已拆开的 URL
         * @param startedAt 本次请求的开始时刻
         * @param requestTimeout 整体时限
         * @param failureReason 输出：断在哪一段（解析地址 / 连接 / TLS 那几步 / 握手）
         * @return std::unique_ptr<HttpOutboundConnection> 握手成功就交出连接；任一步失败返回空
         */
        Core::Task<std::unique_ptr<HttpOutboundConnection>> establishSecureConnection(
                Core::EventLoop &loop, const ParsedUrl &u, const std::chrono::steady_clock::time_point startedAt,
                const std::chrono::milliseconds requestTimeout, std::string &failureReason,
                const Core::TlsContext *clientTls)
        {
            HttpOutboundEndpointKey key{u.host, u.port, true};

            const std::optional<std::chrono::milliseconds> connectBudget = remainingBudget(startedAt, requestTimeout);
            if (!connectBudget.has_value())
            {
                failureReason = "本次请求已到时限：还没开始连接（主机 " + u.host + "）";
                co_return nullptr;
            }
            auto socket = co_await connectWithDeadline(loop, u.host, u.port, *connectBudget, failureReason);
            if (!socket)
            {
                co_return nullptr;
            }
            Core::AsyncSocket sock = std::move(*socket);

            // 带池那一路用本实例自己的上下文（策略、信任库、客户端证书都在那上面）；
            // 静态那一路没有承载策略的地方，仍用进程级默认上下文
            auto *ctx = clientTls != nullptr ? clientTls->nativeHandle() : clientSslCtx();
            if (!ctx)
            {
                failureReason = "TLS 上下文创建失败：OpenSSL 没能给出 client method，通常是库未正确初始化";
                co_return nullptr;
            }
            SSL *ssl = SSL_new(ctx);
            if (!ssl)
            {
                failureReason = "TLS 会话对象创建失败：OpenSSL 内存不足";
                co_return nullptr;
            }

            // SSL 必须绑上底层描述符才有 BIO 可用：少了这一步 SSL_connect 立刻失败，
            // 而且错误队列是空的（error:00000000），现场只剩「握手失败」四个字
            if (::SSL_set_fd(ssl, sock.fileDescriptor()) == 0)
            {
                SSL_free(ssl);
                failureReason = "TLS 无法绑定套接字描述符：OpenSSL 的 BIO 创建失败";
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
                failureReason = "TLS 没能设置 ALPN 协议列表：OpenSSL 拒绝了这份写法";
                co_return nullptr;
            }

            auto tlsSocket = std::make_unique<Core::TlsSocket>(ssl, loop, std::move(sock), Core::TlsSocket::Role::Client);
            const std::optional<std::chrono::milliseconds> handshakeBudget = remainingBudget(startedAt, requestTimeout);
            if (!handshakeBudget.has_value())
            {
                tlsSocket->close();
                failureReason = "本次请求已到时限：还没做 TLS 握手（主机 " + u.host + "）";
                co_return nullptr;
            }
            const RequestDeadlineGuard<Core::TlsSocket> handshakeDeadline(loop, *tlsSocket, *handshakeBudget,
                                                            "HttpClient");
            try
            {
                co_await tlsSocket->handshake();
            }
            catch (const Base::Exception &failure)
            {
                LOG_WARN_FMT("HttpClient: 与 {} 的 TLS 握手失败。底层原因：{}", u.host, failure.what());
                failureReason = "TLS 握手失败：证书没通过校验、协议或密码套件不匹配，或对端在握手中途收线（主机 "
                                + u.host + "）";
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
            /// 请求有没有写上过通路。h2 一条连接上跑几条流，一条被时限掐掉会把同连接的兄弟一起带走：
            /// 只看「有没有收到字节」会把「已整个发出、只等答复」的那条也判成可以重来一次，
            /// 于是非幂等请求被悄悄做两遍。两个位一起看才分得开「空闲期被对端收了」与「发出去没回」
            bool isAnyByteSent{false};
        };

        /**
         * @brief 在一条已经协商好的 h2 连接上走完一次请求
         * @details 连接的生命周期不在这里：新建那条要走前奏（start），复用这条前奏早就走完了——
         *          把两件事分开，池才能拿同一段代码服务「刚建好的」与「留着待命的」两种连接。
         * @param client 已完成前奏的 h2 连接（由调用方持有；它自己认得归属的循环）
         * @param u 已拆开的 URL
         * @param request 方法、正文、媒体类型与附加头部
         * @param startedAt 本次请求的开始时刻，用于把整体时限摊到剩下的那一段上
         * @param requestTimeout 整体时限
         * @param failureReason 输出：这次交换失败的原因（对端 RST、时限、响应本身不合规范）
         * @return Http2Exchange 响应；失败时 response 为空。reasonPhrase 恒为空——HTTP/2 没有原因
         *         短语这一项，状态语义只靠 :status
         */
        Core::Task<Http2Exchange> exchangeOnHttp2(
                Http2ClientConnection &client, const ParsedUrl &u, const HttpClientRequest &request,
                const std::chrono::steady_clock::time_point startedAt, const std::chrono::milliseconds requestTimeout,
                std::string &failureReason)
        {
            Http2Exchange exchange;
            std::vector<std::pair<std::string, std::string>> extraHeaders;
            if (!request.body.empty() && !request.contentType.empty())
            {
                extraHeaders.emplace_back("content-type", std::string(request.contentType));
            }
            for (const HttpClientHeaderField &field: request.headers)
            {
                // h2 的头部名必须是全小写 ASCII（RFC 7540 §8.1.2），调用方给的大写名字在这里折下去；
                // 值原样保留，大小写对值语义有影响
                std::string foldedName = field.first;
                std::transform(foldedName.begin(), foldedName.end(), foldedName.begin(),
                               [](const unsigned char byte)
                               {
                                   return (byte >= 'A' && byte <= 'Z') ? static_cast<char>(byte + ('a' - 'A')) : byte;
                               });
                extraHeaders.emplace_back(std::move(foldedName), field.second);
            }
            // 与 h1 那一支同一条判据：代加声明才透明解压，两边问的是同一个函数（见 HttpContentCoding）
            if (shouldAdvertiseAcceptEncoding(request.headers))
            {
                extraHeaders.emplace_back("accept-encoding", std::string(kOutboundAcceptEncodingValue));
            }
            // 主机文本与协议名都要先落到具名对象上：co_await 挂起期间 string_view 指着的临时串会先析构
            const std::string authority = authorityText(u);
            const std::string_view scheme = u.scheme == "https" ? "https" : "http";
            const std::string method = request.method;
            const std::string body(request.body);
            const std::optional<std::chrono::milliseconds> exchangeBudget = remainingBudget(startedAt, requestTimeout);
            if (!exchangeBudget.has_value())
            {
                failureReason = "本次请求已到时限：还没把请求写上通路（主机 " + u.host + "）";
                co_return exchange;
            }
            Http2ClientResponse response = co_await client.request(
                    scheme, authority, method, u.path, extraHeaders, body, *exchangeBudget);
            exchange.isAnyByteReceived = response.isAnyByteReceived;
            exchange.isAnyByteSent = response.isAnyByteSent;
            if (!response.isOk())
            {
                // 状态码为 0（没收到响应头）或被对端中途 RST 掉：都不算一次成功的出站
                failureReason = response.errorMessage.empty()
                                    ? "HTTP/2 这一侧没拿到有效响应：状态码缺失或流被对端收尾（主机 " + u.host + "）"
                                    : std::move(response.errorMessage);
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
         * @param request 方法、正文、媒体类型与附加头部（调用方给的写法已由 validateRequest 把住）
         * @param u 已拆开的 URL
         * @param requestTimeout 整体时限（握手、发送、收完响应三段之和）
         * @param pool 空闲连接池；为空即一次一条连接
         * @param failureReason 输出：失败发生在哪一段，供 send() 折成 expected 的失败值
         * @return std::unique_ptr<HttpClientResponse> 响应；失败（含超时）返回空
         */
        Core::Task<std::unique_ptr<HttpClientResponse>> performRequest(
                Core::EventLoop &loop, const HttpClientRequest &request, const ParsedUrl &u,
                const std::chrono::milliseconds requestTimeout, HttpOutboundConnectionPool *pool,
                std::string &failureReason, const Core::TlsContext *clientTls)
        {
            const std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now();
            const bool isKeepAlive = pool != nullptr;
            const HttpOutboundEndpointKey endpointKey{u.host, u.port, u.scheme == "https"};
            // 请求文按要再拼：h2 那一支用不上它（帧里没有请求行），提前拼一份等于把正文整块多拷一次
            const auto makeRequestText = [&]
            {
                return buildRequestText(request, u, isKeepAlive);
            };
            const bool isHeadRequest = request.method == "HEAD";

            if (pool != nullptr)
            {
                // h2 的待命连接问在 h1 的空闲表之前：一台主机的 ALPN 结果是稳定的，两处不会同时有货
                if (auto cachedHttp2 = pool->acquireHttp2(endpointKey); cachedHttp2 != nullptr)
                {
                    Http2Exchange cachedExchange = co_await exchangeOnHttp2(
                            *cachedHttp2, u, request, startedAt, requestTimeout, failureReason);
                    if (cachedExchange.response)
                    {
                        co_return std::move(cachedExchange.response);
                    }
                    if (cachedExchange.isAnyByteReceived || cachedExchange.isAnyByteSent)
                    {
                        // 要么对端答过话（响应本身出了问题），要么我们已把请求整个交上通路（发出去的
                        // 请求没有回音）——两种都不该重来：后一种会把非幂等请求做两遍
                        co_return nullptr;
                    }
                    // 一个字节没发出、也没收到：多半是对端在我们手里把这条连接收了（与 h1 的
                    // keep-alive 同一条竞态）。这条就此作废——不作废也没有下一句，HPACK 动态表跟着
                    // 连接一起丢——直接往下重开一条重来一次，对调用方仍是一次成功请求
                }
                if (auto reused = pool->acquire(endpointKey))
                {
                    const std::optional<std::chrono::milliseconds> reusedBudget = remainingBudget(startedAt, requestTimeout);
                    if (reusedBudget.has_value())
                    {
                        const std::string requestText = makeRequestText();
                        OutboundExchange exchange = co_await exchangeOnConnection(loop, *reused, requestText, isHeadRequest,
                                                                                 *reusedBudget, failureReason);
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
                connection = co_await establishSecureConnection(loop, u, startedAt, requestTimeout, failureReason, clientTls);
            }
            else
            {
                connection = co_await establishPlainConnection(loop, endpointKey, startedAt, requestTimeout,
                                                               failureReason);
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
                auto http2Connection = std::make_shared<Http2ClientConnection>(loop, std::move(connection));
                if (!startBudget.has_value() || !co_await http2Connection->start(*startBudget))
                {
                    failureReason = "HTTP/2 前奏没走完：对端没接我们的 SETTINGS，或时限先到（主机 " + u.host + "）";
                    co_return nullptr;
                }
                Http2Exchange freshExchange = co_await exchangeOnHttp2(
                        *http2Connection, u, request, startedAt, requestTimeout, failureReason);
                if (pool == nullptr)
                {
                    // 没有池就是一次性的：主动 shutdown 而不是任其析构，否则对端把这次收口记成 abrupt
                    co_await http2Connection->shutdown();
                }
                else if (http2Connection->isHealthy())
                {
                    // 收进池里留着待命；不健康的那条不收，跟着最后一个持有者一起收口
                    pool->adoptHttp2(endpointKey, std::move(http2Connection));
                }
                co_return std::move(freshExchange.response);
            }

            const std::optional<std::chrono::milliseconds> exchangeBudget = remainingBudget(startedAt, requestTimeout);
            if (!exchangeBudget.has_value())
            {
                failureReason = "本次请求已到时限：连接建好了却没剩下写请求的预算（主机 " + u.host + "）";
                co_return nullptr;
            }
            const std::string requestText = makeRequestText();
            OutboundExchange exchange = co_await exchangeOnConnection(loop, *connection, requestText, isHeadRequest,
                                                                     *exchangeBudget, failureReason);
            if (exchange.response && pool != nullptr && isResponseReusable(exchange.response->headers))
            {
                connection->prepareForNextRequest();
                pool->release(std::move(connection));
            }
            co_return std::move(exchange.response);
        }
    }

    Core::Task<std::expected<HttpClientResponse, std::string>>
    HttpClient::send(Core::EventLoop &loop, const std::string_view url, HttpClientRequest request,
                     const std::chrono::milliseconds requestTimeout)
    {
        // 畸形 URL 与不合规范的头部都是用法错误，按 parseUrl 的既有口径抛出，不折进 expected 的失败值
        const ParsedUrl parsed = parseUrl(url);
        validateRequest(request);
        std::string failureReason;
        std::unique_ptr<HttpClientResponse> response = co_await performRequest(
                loop, request, parsed, requestTimeout, nullptr, failureReason, nullptr);
        if (!response)
        {
            // 每条失败路径都会先写下原因；这里兜住的是「哪天新增了忘了写的出口」，而不是让调用方拿到空原因
            co_return std::unexpected(failureReason.empty() ? "请求失败：没拿到响应，也没有记下原因（目标 " + std::string(url) + "）"
                                                            : std::move(failureReason));
        }
        // 解压放在这里：两条承载（h1 与 h2）的响应都汇到这一处，代加声明与解回来的判据只写一遍
        if (!applyContentEncoding(request, *response, failureReason))
        {
            co_return std::unexpected(std::move(failureReason));
        }
        co_return std::move(*response);
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::get(Core::EventLoop &loop, std::string_view url,
                                                                   const std::chrono::milliseconds requestTimeout)
    {
        const HttpClientRequest request;
        auto sent = co_await send(loop, url, request, requestTimeout);
        if (!sent.has_value())
        {
            LOG_ERROR_FMT("HttpClient: GET {} 失败。原因：{}", url, sent.error());
            co_return nullptr;
        }
        co_return std::make_unique<HttpClientResponse>(std::move(*sent));
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::post(
            Core::EventLoop &loop, std::string_view url, std::string_view contentType, std::string_view body,
            const std::chrono::milliseconds requestTimeout)
    {
        HttpClientRequest request;
        request.method = "POST";
        request.body = body;
        // 声明过「正文按表单编码」却没给类型时补上那个类型：交一个空 Content-Type 出去等于没声明，
        // 对端只能按 application/octet-stream 猜（RFC 9110 §8.4）
        request.contentType = contentType.empty() ? std::string_view{"application/x-www-form-urlencoded"} : contentType;
        auto sent = co_await send(loop, url, request, requestTimeout);
        if (!sent.has_value())
        {
            LOG_ERROR_FMT("HttpClient: POST {} 失败。原因：{}", url, sent.error());
            co_return nullptr;
        }
        co_return std::make_unique<HttpClientResponse>(std::move(*sent));
    }

    HttpClient::HttpClient(Core::EventLoop &loop, const HttpOutboundConnectionPool::Config poolConfig)
        : HttpClient(loop, poolConfig, Core::TlsPolicy{})
    {
    }

    // 唯一要做的事是放掉那份 TLS 上下文（头文件里它只是前向声明）；池由自己的成员收尾
    HttpClient::~HttpClient() = default;

    HttpClient::HttpClient(Core::EventLoop &loop, const HttpOutboundConnectionPool::Config poolConfig, const Core::TlsPolicy &tlsPolicy)
        : m_loop(&loop)
        , m_pool(poolConfig)
        , m_clientTls(std::make_unique<Core::TlsContext>(tlsPolicy, Core::TlsContext::Role::Client))
    {
        // 出站一侧恒要校验对端证书：不校验等于任何受信 CA 给他域签的证书都能冒充目标主机（CWE-297）。
        // 这一句放在构造而不是每条连接里，是为了让「策略里自带 CA」与「用系统信任库」两种配置的取舍
        // 只有一处判据（见 TlsContext::enableClientPeerVerification）
        m_clientTls->enableClientPeerVerification();
    }

    bool HttpClient::setClientCertificate(const std::string &certificateFile, const std::string &keyFile)
    {
        // 装载失败保持原状态：本客户端继续以「不带身份」出站，调用方拿到 false 自己决定要不要放弃
        return m_clientTls->loadCertificate(certificateFile, keyFile);
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::get(std::string_view url,
                                                                   const std::chrono::milliseconds requestTimeout)
    {
        const HttpClientRequest request;
        co_return co_await sendPooled(url, request, requestTimeout);
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::post(
            std::string_view url, std::string_view contentType, std::string_view body,
            const std::chrono::milliseconds requestTimeout)
    {
        HttpClientRequest request;
        request.method = "POST";
        request.body = body;
        // 与静态的 post() 同一口径：没给媒体类型的表单正文补上那个类型，两条连路不该有两种写法
        request.contentType = contentType.empty() ? std::string_view{"application/x-www-form-urlencoded"} : contentType;
        co_return co_await sendPooled(url, request, requestTimeout);
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::sendPooled(
            const std::string_view url, const HttpClientRequest &request, const std::chrono::milliseconds requestTimeout)
    {
        const ParsedUrl parsed = parseUrl(url);
        validateRequest(request);
        std::string failureReason;
        std::unique_ptr<HttpClientResponse> response = co_await performRequest(
                *m_loop, request, parsed, requestTimeout, &m_pool, failureReason, m_clientTls.get());
        if (!response)
        {
            LOG_ERROR_FMT("HttpClient: {} {} 失败。原因：{}", request.method, url, failureReason);
            co_return nullptr;
        }
        if (!applyContentEncoding(request, *response, failureReason))
        {
            LOG_ERROR_FMT("HttpClient: {} {} 失败。原因：{}", request.method, url, failureReason);
            co_return nullptr;
        }
        co_return response;
    }

    std::size_t HttpClient::idleConnectionCount() const noexcept
    {
        return m_pool.idleConnectionCount();
    }

    std::size_t HttpClient::idleHttp2ConnectionCount() const noexcept
    {
        return m_pool.idleHttp2ConnectionCount();
    }

    std::size_t HttpClient::http2MaximumInFlightStreamCount() const noexcept
    {
        return m_pool.http2MaximumInFlightStreamCount();
    }

    void HttpClient::closeIdleConnections() noexcept
    {
        m_pool.closeAll();
    }
} // namespace AsynGyanis::Net
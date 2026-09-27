#include "Net/Http/Client/HttpClient.h"
#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/DeadlineGuard.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/AsyncResolver.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/ConnectionRace.h"
#include "Core/Tls/TlsContext.h"
#include "Core/Tls/TlsPolicy.h"
#include "Core/Tls/TlsSocket.h"
#include "Net/Http/Client/HttpContentCoding.h"
#include "Net/Http/Client/HttpCookieJar.h"
#include "Net/Http/Client/HttpOutboundConnectionPool.h"
#include "Net/Http2/Http2ClientConnection.h"
#include "Net/Http2/Http2Session.h"
#include "Net/Http3/Http3ClientConnection.h"
#include "Net/Tcp/TcpStream.h"
#include "Platform/IO/Socket.h"

#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <set>
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
            return converted.ec == std::errc{} && converted.ptr == text.data() + text.size() && port >= 1U && port <= 65535U;
        }
    } // namespace

    ParsedUrl parseUrl(const std::string_view url)
    {
        // 请求行是把 path 原样拼出来的：里面若有 CR/LF 或空白，等于让调用方自己结束请求行、
        // 甚至插进新的头部（请求分裂）。这不是「请求失败」，是用法错误，当场报出来
        if (url.find_first_of("\r\n\t ") != std::string_view::npos)
        {
            throw Base::InvalidArgumentException("HttpClient：URL 里不允许出现空白或控制字符（会撕裂请求行）：「" + std::string(url) + "」");
        }

        ParsedUrl         parsed;
        std::string_view  remainder       = url;
        const std::size_t schemeSeparator = remainder.find("://");
        if (schemeSeparator != std::string_view::npos)
        {
            const std::string_view schemeText = remainder.substr(0, schemeSeparator);
            // 协议名大小写无关（RFC 3986 §6.2.3）：HTTPS:// 悄悄当成 http 就是把 TLS 整段降级
            if (equalsIgnoreAsciiCase(schemeText, "https"))
            {
                parsed.scheme = "https";
            } else if (equalsIgnoreAsciiCase(schemeText, "http"))
            {
                parsed.scheme = "http";
            } else
            {
                throw Base::InvalidArgumentException(R"(HttpClient：只支持 "http" 与 "https" 两种协议，收到的是「)" + std::string(schemeText) + R"(」：换成 http(s):// 开头再试)");
            }
            remainder.remove_prefix(schemeSeparator + 3);
        }

        const std::size_t      pathSeparator = remainder.find('/');
        const std::string_view authority     = pathSeparator == std::string_view::npos ? remainder : remainder.substr(0, pathSeparator);
        const std::string_view pathPart      = pathSeparator == std::string_view::npos ? std::string_view{} : remainder.substr(pathSeparator);

        // 方括号里的是 IP 字面量（RFC 3986 §3.2.2）：IPv6 自带冒号，不这样区分就分不清哪段是端口
        std::string_view hostText = authority;
        std::string_view portText;
        bool             hasExplicitPort = false;
        if (!authority.empty() && authority.front() == '[')
        {
            const std::size_t closingBracket = authority.find(']');
            if (closingBracket == std::string_view::npos)
            {
                throw Base::InvalidArgumentException(R"(HttpClient：URL 的主机部分方括号没闭合（IPv6 字面量要写成 "[::1]:8080" 的形式）：「)" + std::string(authority) + R"(」)");
            }
            hostText                            = authority.substr(1, closingBracket - 1);
            const std::string_view afterBracket = authority.substr(closingBracket + 1);
            if (!afterBracket.empty())
            {
                if (afterBracket.front() != ':')
                {
                    throw Base::InvalidArgumentException(R"(HttpClient：URL 的主机部分在 "]" 之后还跟着多余字符（端口要写成 ":8080"）：「)" + std::string(authority) + R"(」)");
                }
                hasExplicitPort = true;
                portText        = afterBracket.substr(1);
            }
        } else
        {
            const std::size_t portSeparator = authority.rfind(':');
            if (portSeparator != std::string_view::npos)
            {
                // 冒号不止一个又没有方括号：那是没按规范包起来的 IPv6，拆出来的「主机」会是半截地址
                if (authority.find(':') != portSeparator)
                {
                    throw Base::InvalidArgumentException(R"(HttpClient：URL 的主机是 IPv6 时必须写成方括号形式（"[::1]:8080"）：「)" + std::string(authority) + R"(」)");
                }
                hasExplicitPort = true;
                hostText        = authority.substr(0, portSeparator);
                portText        = authority.substr(portSeparator + 1);
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
                throw Base::InvalidArgumentException(R"(HttpClient：URL 的端口「)" + std::string(portText) + R"(」不是 1..65535 的十进制数：完整 URL 是「)" + std::string(url) +
                                                     R"(」)");
            }
            parsed.port = static_cast<uint16_t>(port);
        } else
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
            const bool isSchemeDefaultPort = (url.scheme == "https" && url.port == 443U) || (url.scheme != "https" && url.port == 80U);
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
                if (!c)
                    return nullptr;
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
            return ::inet_pton(AF_INET, host.c_str(), addressBytes.data()) == 1 || ::inet_pton(AF_INET6, host.c_str(), addressBytes.data()) == 1;
        }

        /**
         * @brief 一次出站交换的结论：响应，以及「有没有读到过对端的第一个字节」「请求有没有整个
         *        写上通路」
         * @details 三条通路（h1 一条连接一个请求、h2 与 h3 按流复用）共用这一份结论，因此重来的判据
         *          在谁那里都一样：读到过字节＝响应本身出了问题，换连接不换一个答案；没读到也没写出＝
         *          通路本来就死着，重来一次不涉及重复执行；已整个写出却没回音＝本端分不清「对端没见过
         *          它」与「对端正慢」，于是只按方法幂等性决定要不要重来（见 isIdempotentRequestMethod，
         *          RFC 9112 §9.3.2 的自动重试许可只覆盖幂等方法）。
         *          复用型通路上「写成功」尤其要紧：一条连接上跑几条流，一条被时限掐掉会把同一条通路上的
         *          兄弟一起带走，只看「有没有收到字节」判不出该不该重发，非幂等请求就可能被悄悄做两遍。
         */
        struct OutboundExchange
        {
            std::unique_ptr<HttpClientResponse> response;
            bool                                isAnyByteReceived{false};
            /// 请求有没有被通路完整收下。写成功才算发出：通路本来就死着时 send 返回 false，
            /// 那一支仍是「对端在我们手里把连接收了」，重来不涉及重复执行
            bool isAnyByteSent{false};
        };

        /**
         * @brief 这个方法原文按规范是不是幂等的（同一条请求做两遍与做一遍，对服务器状态的影响相同）
         * @details 用来决定「请求已整个写上通路、却没等到回音」时要不要换一条连接重来一次：
         *          RFC 9112 §9.3.2 允许在连接故障之后自动重试**幂等**方法，幂等集合由 RFC 9110 §9.2.2
         *          给出（GET/HEAD/OPTIONS/PUT/DELETE/TRACE；PATCH 明确不算）。方法 token 大小写敏感
         *          （RFC 9110 §9），这里就不折叠；表外原文一律按不幂等处理——猜错方向的代价是把非幂等
         *          请求做两遍，比少恢复一次贵得多
         * @param method 报文里的方法原文
         * @return true 重来一次是安全的
         */
        bool isIdempotentRequestMethod(const std::string_view method)
        {
            return method == "GET" || method == "HEAD" || method == "OPTIONS" || method == "PUT" || method == "DELETE" || method == "TRACE";
        }

        /**
         * @brief 从整体时限里扣掉已经花掉的那一段
         * @details 契约是「握手、发送、收完响应三段之和」：分两段各给一次完整时限，等于把契约放宽
         *          一倍。返回空表示预算已经用完，调用方据此直接判失败。
         * @param startedAt 本次请求的开始时刻
         * @param requestTimeout 整体时限
         * @return std::optional<std::chrono::milliseconds> 还剩多少；已经用尽则为空
         */
        std::optional<std::chrono::milliseconds> remainingBudget(const std::chrono::steady_clock::time_point startedAt, const std::chrono::milliseconds requestTimeout)
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);
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
            return equalsIgnoreAsciiCase(name, "host") || equalsIgnoreAsciiCase(name, "content-length") || equalsIgnoreAsciiCase(name, "connection");
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
                const bool isAlnum = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9');
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
            if (!request.body.empty() && request.bodySource)
            {
                // 两种正文写法同时给：一份 Content-Length 加一份 chunked 是让对端挑一个信，
                // 而挑哪个由中间盒决定——当场拒绝，不猜
                throw Base::InvalidArgumentException("HttpClient：body 与 bodySource 只能选一种：前者一次性交整份正文，后者按段拉出去");
            }
            if (!isTokenText(request.method))
            {
                throw Base::InvalidArgumentException(R"(HttpClient：请求方法「)" + request.method + R"(」不合 HTTP token（只允许字母、数字与 "!#$%&'*+-.^_`|~"）：那会撕裂请求行)");
            }
            for (const HttpClientHeaderField &field: request.headers)
            {
                if (!isTokenText(field.first))
                {
                    throw Base::InvalidArgumentException(R"(HttpClient：附加头部的名字「)" + field.first + R"(」为空或含控制字符、冒号之类分隔符：头部名必须是 HTTP token)");
                }
                if (hasControlChar(field.second))
                {
                    // 值里有 CR/LF 就能自己结束这一行并插入新字段——这是请求分裂，不转义也不删除
                    throw Base::InvalidArgumentException(R"(HttpClient：附加头部 ")" + field.first + R"(" 的值里有控制字符（会撕裂请求行）：请去掉 CR/LF 后再发)");
                }
                if (isClientOwnedHeaderName(field.first))
                {
                    throw Base::InvalidArgumentException(R"(HttpClient：附加头部 ")" + field.first + R"(" 由客户端按这次请求自己写，不能由调用方给：请删掉这一项)");
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
            text += request.method;
            text += ' ';
            text += u.path;
            text += " HTTP/1.1\r\n";
            appendHostHeader(text, u);
            for (const HttpClientHeaderField &field: request.headers)
            {
                text += field.first;
                text += ": ";
                text += field.second;
                text += "\r\n";
            }
            // 代为声明本端真能解回来的编码：调用方自己写过 accept-encoding 就整个不管正文（同 h2 那一支）
            if (shouldAdvertiseAcceptEncoding(request.headers))
            {
                text += "Accept-Encoding: ";
                text += kOutboundAcceptEncodingValue;
                text += "\r\n";
            }
            if (request.bodySource)
            {
                // 流式正文：长度事先不知道，按 chunked 定界（RFC 9112 §7.1）。这一支不写
                // Content-Length——两份定界同时挂在一条报文上，对端挑哪一份信由中间盒决定
                if (!request.contentType.empty())
                {
                    text += "Content-Type: ";
                    text += request.contentType;
                    text += "\r\n";
                }
                text += "Transfer-Encoding: chunked\r\n";
            } else if (!request.body.empty())
            {
                if (!request.contentType.empty())
                {
                    text += "Content-Type: ";
                    text += request.contentType;
                    text += "\r\n";
                }
                text += "Content-Length: ";
                text += std::to_string(request.body.size());
                text += "\r\n";
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
        std::unique_ptr<HttpClientResponse> takeParsedResponse(HttpResponseParser &parser, const std::string_view target, std::string &failureReason)
        {
            if (parser.hasFailed())
            {
                if (parser.isBodyOverLimit())
                {
                    // 「对端发了不合规范的报文」与「本端胃口有限」是两件事：后者报成前者会让人
                    // 去查对端，而解法其实在自己手里（把上限调高或填 0 放开）
                    failureReason = "响应正文超过本端上限 " + std::to_string(parser.maximumBodySize()) +
                                    " 字节（HttpOutboundConnectionPool::Config::maximumResponseBodyBytes，"
                                    "填 0 表示不限）：目标 " +
                                    std::string(target);
                    return nullptr;
                }
                failureReason = "响应不合规范或超出本端上限（状态行、头部与正文三条里的一项）：目标 " + std::string(target);
                return nullptr;
            }
            if (!parser.isComplete())
            {
                failureReason = "响应没读完就断了：对端提前收线，或本次请求已到时限（目标 " + std::string(target) + "）";
                return nullptr;
            }
            auto response          = std::make_unique<HttpClientResponse>();
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
        Core::Task<OutboundExchange> exchangeOnConnection(Core::EventLoop &loop, HttpOutboundConnection &connection, const std::string &requestText,
                                                          const HttpBodyChunkSource &bodySource, const bool isHeadRequest, const std::chrono::milliseconds requestTimeout,
                                                          std::string &failureReason)
        {
            const Core::DeadlineGuard<HttpOutboundConnection> deadline(loop, connection, requestTimeout, "HttpClient");
            OutboundExchange                                  exchange;
            const std::string                                 host = connection.endpointKey().host;
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
                exchange.isAnyByteSent = true;
                if (bodySource)
                {
                    // 拉一段、发一段：上一段没写上通路就不叫下一段，这就是上传的背压。零长的段按
                    // §7.1 就是终止块，不能当正文发出去（那等于提前把正文判完，后面的段落没人认领）
                    while (true)
                    {
                        const std::optional<std::string> chunk = co_await bodySource();
                        if (!chunk.has_value())
                        {
                            break;
                        }
                        if (chunk->empty())
                        {
                            continue;
                        }
                        std::string frame;
                        frame.reserve(chunk->size() + 16U);
                        frame += std::format("{:x}\r\n", chunk->size());
                        frame += *chunk;
                        frame += "\r\n";
                        if (!co_await connection.send(frame))
                        {
                            failureReason = "写出流式正文失败：对端在收完正文前收线，或本次请求已到时限（主机 " + host + "）";
                            co_return exchange;
                        }
                    }
                    const std::string terminator = "0\r\n\r\n";
                    if (!co_await connection.send(terminator))
                    {
                        failureReason = "收尾流式正文失败：终止块没能写上通路（对端收线或已到时限，主机 " + host + "）";
                        co_return exchange;
                    }
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
            } catch (const std::exception &failure)
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
         * @brief 带时限地连一条 TCP 通路：解析 → 排序 → 并发试候选 → 到点就把没连上的都关掉
         * @details 这一段为什么必须由本层盯时限，而不是交给内核的 SYN 重试兜底：Windows 的完成端口后端
         *          上「连不上」并没有可写事件可等——实测对刚关掉的监听端口，系统 2 秒内就把连接拒了，
         *          而挂在可写上的协程永远等不到那一次唤醒，调用方就此无限期停在这里。Linux 那边靠可写
         *          事件带出 SO_ERROR，能自己醒。两条平台要按同一个契约走，所以时限统一由看门狗掐。
         *
         *          候选地址交给 Core::connectCandidates() 并发起，而不是按顺序一条条试：顺序试法里一条
         *          黑洞地址（发出去没人应答，常见于 IPv6 链路已断却仍路由得出去）会把整段预算吃干净，
         *          后面的候选根本轮不到——表现就是「双栈主机连不上、纯 IPv4 主机连得上」。排序按
         *          RFC 8305 §4 的族间交错，两族都有候选时谁都不许占满前几席。
         * @param loop 所属事件循环
         * @param host 目标主机名或 IP 字面量
         * @param port 目标端口
         * @param connectTimeout 连接一段的时限
         * @param failureReason 输出：断在哪一段（解析地址 / 连不上或已到时限）
         * @return std::optional<Core::AsyncSocket> 连上了交出套接字，失败为空
         */
        Core::Task<std::optional<Core::AsyncSocket>> connectWithDeadline(Core::EventLoop &loop, const std::string &host, const std::uint16_t port,
                                                                         const std::chrono::milliseconds connectTimeout, std::string &failureReason)
        {
            const std::vector<Core::InetAddress> addresses = co_await Core::AsyncResolver::resolve(loop, host, port);
            if (addresses.empty())
            {
                failureReason = "解析地址失败：没能把「" + host + "」解析成可用地址";
                co_return std::nullopt;
            }

            auto connected = co_await Core::connectCandidates(loop, Core::orderForConnectionRace(addresses), connectTimeout);
            if (!connected)
            {
                failureReason = "建立 TCP 连接失败：对端拒绝、不可达或还没连上就超时（目标 " + host + ":" + std::to_string(port) + "）";
                co_return std::nullopt;
            }
            co_return std::move(connected->socket);
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
        Core::Task<std::unique_ptr<HttpOutboundConnection>> establishPlainConnection(Core::EventLoop &loop, const HttpOutboundEndpointKey &key,
                                                                                     const std::chrono::steady_clock::time_point startedAt,
                                                                                     const std::chrono::milliseconds requestTimeout, std::string &failureReason)
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
        Core::Task<std::unique_ptr<HttpOutboundConnection>> establishSecureConnection(Core::EventLoop &loop, const ParsedUrl &u,
                                                                                      const std::chrono::steady_clock::time_point startedAt,
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
                    2U, 'h', '2', 8U, 'h', 't', 't', 'p', '/', '1', '.', '1',
            };
            // OpenSSL 这一支的返回值是反的：0 才是成功。名字用 SSL_set_alpn_protos 而不是 ...protocols：
            // 前者从 1.0.2 起就是真身并一直保留，后者只是新版本里加的兼容写法，按旧名调两边都在
            if (::SSL_set_alpn_protos(ssl, kAlpnProtocolList, static_cast<unsigned int>(sizeof(kAlpnProtocolList))) != 0)
            {
                SSL_free(ssl);
                failureReason = "TLS 没能设置 ALPN 协议列表：OpenSSL 拒绝了这份写法";
                co_return nullptr;
            }

            auto                                           tlsSocket       = std::make_unique<Core::TlsSocket>(ssl, loop, std::move(sock), Core::TlsSocket::Role::Client);
            const std::optional<std::chrono::milliseconds> handshakeBudget = remainingBudget(startedAt, requestTimeout);
            if (!handshakeBudget.has_value())
            {
                tlsSocket->close();
                failureReason = "本次请求已到时限：还没做 TLS 握手（主机 " + u.host + "）";
                co_return nullptr;
            }
            const Core::DeadlineGuard<Core::TlsSocket> handshakeDeadline(loop, *tlsSocket, *handshakeBudget, "HttpClient");
            try
            {
                co_await tlsSocket->handshake();
            } catch (const Base::Exception &failure)
            {
                LOG_WARN_FMT("HttpClient: 与 {} 的 TLS 握手失败。底层原因：{}", u.host, failure.what());
                failureReason = "TLS 握手失败：证书没通过校验、协议或密码套件不匹配，或对端在握手中途收线（主机 " + u.host + "）";
                co_return nullptr;
            }
            co_return HttpOutboundConnection::forSecure(key, std::move(tlsSocket));
        }

        /**
         * @brief 按复用型通路（HTTP/2 与 HTTP/3）的要求备齐附加字段
         * @details 两条通路的字段段形状一样（小写 ASCII 名 + 原文值），差别只在线上是 HPACK 还是
         *          QPACK，因此这里只留一份。名字折小写是协议要求（RFC 9113 §8.1.2、RFC 9114 §4.2），
         *          值原样保留——大小写对值语义有影响。
         * @param request 方法、正文、媒体类型与附加头部
         * @param isAnyBody 这次是否带正文，整块与流式来源都算：媒体类型只随正文上线，
         *        口径与 h1 那一支的 `buildRequestText` 一致（少这一条会让流式上传在 h2/h3 上丢掉
         *        Content-Type，而服务端按缺省媒体类型处理这份正文）
         * @return std::vector<std::pair<std::string, std::string>> 按给出的顺序排好的字段
         */
        std::vector<std::pair<std::string, std::string>> buildVersionedExtraFields(const HttpClientRequest &request, const bool isAnyBody)
        {
            std::vector<std::pair<std::string, std::string>> extraFields;
            if (isAnyBody && !request.contentType.empty())
            {
                extraFields.emplace_back("content-type", std::string(request.contentType));
            }
            for (const HttpClientHeaderField &field: request.headers)
            {
                std::string foldedName = field.first;
                std::transform(foldedName.begin(), foldedName.end(), foldedName.begin(),
                               [](const unsigned char byte) { return (byte >= 'A' && byte <= 'Z') ? static_cast<char>(byte + ('a' - 'A')) : byte; });
                extraFields.emplace_back(std::move(foldedName), field.second);
            }
            // 与 h1 那一支同一条判据：代加声明才透明解压，三条通路问的是同一个函数（见 HttpContentCoding）
            if (shouldAdvertiseAcceptEncoding(request.headers))
            {
                extraFields.emplace_back("accept-encoding", std::string(kOutboundAcceptEncodingValue));
            }
            return extraFields;
        }

        /**
         * @brief 把复用型通路交回的字段段搬进对外的响应形状
         * @details reasonPhrase 留空是协议形状决定的：h2 与 h3 的状态只有 :status 这一个数，
         *          没有原因短语那一项。
         * @param statusCode 状态码
         * @param headers 响应字段（含尾段），按收到的顺序
         * @param body 正文
         * @return std::unique_ptr<HttpClientResponse> 对外的响应
         */
        std::unique_ptr<HttpClientResponse> makeClientResponse(const int statusCode, std::vector<std::pair<std::string, std::string>> headers, std::string body)
        {
            auto result        = std::make_unique<HttpClientResponse>();
            result->statusCode = statusCode;
            result->headers    = std::move(headers);
            result->body       = std::move(body);
            return result;
        }

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
         * @return OutboundExchange 响应；失败时 response 为空。reasonPhrase 恒为空——HTTP/2 没有原因
         *         短语这一项，状态语义只靠 :status
         */
        Core::Task<OutboundExchange> exchangeOnHttp2(Http2ClientConnection &client, const ParsedUrl &u, const HttpClientRequest &request,
                                                     const std::chrono::steady_clock::time_point startedAt, const std::chrono::milliseconds requestTimeout,
                                                     std::string &failureReason)
        {
            OutboundExchange exchange;
            // 主机文本与协议名都要先落到具名对象上：co_await 挂起期间 string_view 指着的临时串会先析构
            const std::string authority = authorityText(u);
            const std::string method    = request.method;
            const std::string body(request.body);
            // 流式来源也落到本帧的对象上：它要跨过 co_await 活着。空的 std::function 拷贝不分配，
            // 所以这一句对不用流式上传的调用方是零成本
            const HttpBodyChunkSource                              bodySource     = request.bodySource;
            const std::vector<std::pair<std::string, std::string>> extraFields    = buildVersionedExtraFields(request, !body.empty() || static_cast<bool>(bodySource));
            const std::optional<std::chrono::milliseconds>         exchangeBudget = remainingBudget(startedAt, requestTimeout);
            if (!exchangeBudget.has_value())
            {
                failureReason = "本次请求已到时限：还没把请求写上通路（主机 " + u.host + "）";
                co_return exchange;
            }
            const std::string_view scheme   = u.scheme == "https" ? "https" : "http";
            Http2ClientResponse    response = bodySource ? co_await client.requestStreamed(scheme, authority, method, u.path, extraFields, bodySource, *exchangeBudget)
                                                         : co_await client.request(scheme, authority, method, u.path, extraFields, body, *exchangeBudget);
            exchange.isAnyByteReceived      = response.isAnyByteReceived;
            exchange.isAnyByteSent          = response.isAnyByteSent;
            if (!response.isOk())
            {
                // 状态码为 0（没收到响应头）或被对端中途 RST 掉：都不算一次成功的出站
                failureReason = response.errorMessage.empty() ? "HTTP/2 这一侧没拿到有效响应：状态码缺失或流被对端收尾（主机 " + u.host + "）" : std::move(response.errorMessage);
                co_return exchange;
            }
            exchange.response = makeClientResponse(response.statusCode, std::move(response.headers), std::move(response.body));
            co_return exchange;
        }

        /**
         * @brief 在一条已握手的 h3 链路上走完一次请求
         * @details 与 h2 那一支逐条同形：字段折小写的口径、时限摊到剩余预算、失败时带上「有没有答过话
         *          / 有没有写上通路」两位判据。差别只有一处、也是选路要管它的原因：h3 的出站入口
         *          只收整份正文，没有流式出口，所以带 `bodySource` 的请求根本不会走到这里
         *          （见 canUseHttp3 那条判据）。
         * @param client 已 start() 的 h3 会话
         * @param u 已拆开的 URL
         * @param request 方法、正文与附加头部
         * @param startedAt 本次请求的开始时刻
         * @param requestTimeout 整体时限
         * @param failureReason 输出：这次交换失败的原因
         * @return OutboundExchange 响应；失败时 response 为空
         */
        Core::Task<OutboundExchange> exchangeOnHttp3(Http3ClientConnection &client, const ParsedUrl &u, const HttpClientRequest &request,
                                                     const std::chrono::steady_clock::time_point startedAt, const std::chrono::milliseconds requestTimeout,
                                                     std::string &failureReason)
        {
            OutboundExchange                                       exchange;
            const std::vector<std::pair<std::string, std::string>> extraFields = buildVersionedExtraFields(request, !request.body.empty());
            const std::string                                      authority   = authorityText(u);
            const std::string                                      method      = request.method;
            const std::string                                      body(request.body);
            const std::optional<std::chrono::milliseconds>         exchangeBudget = remainingBudget(startedAt, requestTimeout);
            if (!exchangeBudget.has_value())
            {
                failureReason = "本次请求已到时限：还没把请求写上通路（主机 " + u.host + "）";
                co_return exchange;
            }
            // QUIC 的 TLS 是强制的，:scheme 因此在 h3 上恒为 https（能走到这里已由 canUseHttp3 保证）
            Http3ClientResponse response = co_await client.request("https", authority, method, u.path, extraFields, body, *exchangeBudget);
            exchange.isAnyByteReceived   = response.isAnyByteReceived;
            exchange.isAnyByteSent       = response.isAnyByteSent;
            if (!response.isOk())
            {
                failureReason = response.errorMessage.empty() ? "HTTP/3 这一侧没拿到有效响应：状态码缺失或这条流被收尾（主机 " + u.host + "）" : std::move(response.errorMessage);
                co_return exchange;
            }
            exchange.response = makeClientResponse(response.statusCode, std::move(response.headers), std::move(response.body));
            co_return exchange;
        }

        /// 一次「复用型通路」（HTTP/2 或 HTTP/3）上的交换之后，这条请求该怎么收场
        enum class ReuseDisposition
        {
            Serve, ///< 拿到了响应，直接交出去
            Fail,  ///< 不许重来：failureReason 已写明是对端答过话还是幂等性不允许
            Retry  ///< 通路在我们手里被收了：换一条（甚至换一种协议）重来一次
        };

        /**
         * @brief 复用型通路上一次交换的收尾判据
         * @details 三条出口的理由：对端答过话就换不换答案（响应本身出了问题，换连接也是白换）；
         *          请求已整个写上通路而非幂等方法不能重来（可能已经执行过，RFC 9112 §9.3.2 的自动
         *          重试许可只覆盖幂等方法）；其余都当作 keep-alive 的固有竞态——对端在我们手里把这条
         *          连接收了，换一条重来对调用方仍是一次成功请求。h2 与 h3、待命链路和新起的链路
         *          共用这一份判据，四条通路各抄一遍迟早抄出四种口径。
         * @param exchange 刚跑完的那次交换的结论（Serve 时它的 response 仍在里面，由调用方取走）
         * @param method 请求方法原文，判幂等性用
         * @param host 目标主机，只进文案
         * @param failureReason 输出：两条终止出口都要写下原因
         * @return ReuseDisposition 该走哪条出口
         */
        ReuseDisposition classifyMultiplexedExchange(const OutboundExchange &exchange, const std::string_view method, const std::string &host, std::string &failureReason)
        {
            if (exchange.response)
            {
                return ReuseDisposition::Serve;
            }
            if (exchange.isAnyByteReceived)
            {
                // 对端答过话：响应本身出了问题，换一条连接重来不会换一个答案
                return ReuseDisposition::Fail;
            }
            if (exchange.isAnyByteSent && !isIdempotentRequestMethod(method))
            {
                failureReason = "请求已整个写上通路而对端没答话：方法 " + std::string{method} + " 不在幂等集合里，本端不重发（主机 " + host + "）";
                return ReuseDisposition::Fail;
            }
            return ReuseDisposition::Retry;
        }

        /**
         * @brief 出站 h3 的本端设置：由带池的 HttpClient 备一份，静态那一路传空即永不走 h3
         * @details 存的是指针而不是值拷贝：这份设置每条请求都要读一次，而策略里带着 CA 路径等字符串，
         *          按值传等于给每次出站多加几份拷贝。它的真身是 HttpClient 的成员，活到本客户端析构，
         *          比任何一条请求都长。
         */
        struct Http3OutboundContext
        {
            bool                               isEnabled{false};               ///< setHttp3Enabled() 的结果
            const Core::TlsPolicy             *tlsPolicy{nullptr};             ///< QUIC 连接自己建 TLS 上下文，吃的是策略而不是现成的上下文
            const std::string                 *clientCertificateFile{nullptr}; ///< 双向 TLS 的客户端身份，与私钥同时给才带
            const std::string                 *clientPrivateKeyFile{nullptr};  ///< 配套私钥
            std::set<HttpOutboundEndpointKey> *rejectedEndpoints{nullptr};     ///< 探败过的端点：一个端点只探一次
        };

        /// 探一条 h3 链路的时限上界。UDP 被中间网络黑洞时「连不上」没有快速回音、只能等时限到，
        /// 因此这一笔不能由整条请求的预算出：到点就交回 false，剩下的预算仍归那次真正的 TCP 请求用
        constexpr std::chrono::milliseconds kHttp3ProbeTimeout{3000};

        /**
         * @brief 这次请求能不能走 h3：三个条件各挡一件事
         * @param u 已拆开的 URL
         * @param request 方法、正文与流式来源
         * @param http3 本端设置；为空即这一路没有 h3 可用
         * @return true 允许试 h3（不代表一定能成）
         */
        bool canUseHttp3(const ParsedUrl &u, const HttpClientRequest &request, const Http3OutboundContext *http3)
        {
            // ①QUIC 的 TLS 是强制的：明文 http:// 没有 h3 这一说；
            // ②h3 的出站入口只收整份正文，没有流式出口。带 bodySource 的请求改走 TCP，
            //   而不是把来源整块缓冲下来——那会悄悄改变这条请求的内存账；
            // ③开关没开，或这一路根本没带设置（静态的 get/post/send）；策略缺失时同样不走——
            //   拿不到信任库的连接等于不校验对端证书
            return http3 != nullptr && http3->isEnabled && http3->tlsPolicy != nullptr && u.scheme == "https" && !static_cast<bool>(request.bodySource);
        }

        /// 这个端点此前是否已经探败过 h3：一个端点在本客户端的存活期内只探一次
        bool isHttp3ProbeRejected(const HttpOutboundEndpointKey &endpointKey, const Http3OutboundContext &http3)
        {
            return http3.rejectedEndpoints != nullptr && http3.rejectedEndpoints->contains(endpointKey);
        }

        /// 记下这个端点的 h3 没走通，此后同端点的请求直接走 TCP（理由见 setHttp3Enabled 的第④条）
        void rejectHttp3Probe(const HttpOutboundEndpointKey &endpointKey, const Http3OutboundContext &http3)
        {
            if (http3.rejectedEndpoints != nullptr)
            {
                http3.rejectedEndpoints->insert(endpointKey);
            }
        }

        /**
         * @brief 探一条 h3 链路：解析地址 → 握 QUIC → 起 h3 层
         * @details 候选地址取排序后的第一个：并发试多条候选是 TCP 那一路的做法（每条候选各占一个
         *          套接字），QUIC 一侧要把 Initial 发到一条确定的路径上，本层不替调用方猜该试哪条；
         *          排序仍按 RFC 8305 §4 的族间交错，双栈主机不会一律压在 IPv6 上。
         * @param loop 所属事件循环
         * @param u 已拆开的 URL（主机与端口）
         * @param pool 连接池：正文上限要从它的配置落到这条链路的 h3 本端能力上
         * @param http3 本端设置
         * @param failureReason 输出：断在哪一段
         * @return std::shared_ptr<Http3OutboundLink> 已可提请求的链路；任何一段没成返回空
         */
        Core::Task<std::shared_ptr<Http3OutboundLink>> establishHttp3Link(Core::EventLoop &loop, const ParsedUrl &u, const HttpOutboundConnectionPool &pool,
                                                                          const Http3OutboundContext &http3, std::string &failureReason)
        {
            const std::vector<Core::InetAddress> addresses = co_await Core::AsyncResolver::resolve(loop, u.host, u.port);
            if (addresses.empty())
            {
                failureReason = "解析地址失败：没能把「" + u.host + "」解析成可用地址";
                co_return nullptr;
            }

            QuicClientConnection::Configuration configuration;
            configuration.hostName              = u.host; ///< SNI 与证书校验目标，h3 上没有「不带身份的连接」这一档
            configuration.tlsPolicy             = *http3.tlsPolicy;
            configuration.handshakeTimeout      = kHttp3ProbeTimeout;
            configuration.clientCertificateFile = http3.clientCertificateFile != nullptr ? *http3.clientCertificateFile : std::string{};
            configuration.clientPrivateKeyFile  = http3.clientPrivateKeyFile != nullptr ? *http3.clientPrivateKeyFile : std::string{};

            Http3ClientConnection::Config http3Config;
            // 同一条胃口换成 h3 那一侧的说法：协商出哪条协议不该改变本端愿意收多少正文
            http3Config.maximumResponseBodyBytes = pool.config().maximumResponseBodyBytes;

            const std::vector<Core::InetAddress> orderedCandidates = Core::orderForConnectionRace(addresses);
            auto                                 link              = std::make_shared<Http3OutboundLink>(loop, std::move(configuration), http3Config);
            try
            {
                // connect() 一次走完两段：QUIC 握手 + h3 层起步（三条单向流与 SETTINGS），任一没成回 false
                if (!co_await link->connect(orderedCandidates.front()))
                {
                    failureReason = "HTTP/3 这一侧没走通：对端没在这个 UDP 端口上听 h3、证书没通过校验、控制流开不出来，"
                                    "或这条网络路径把 UDP 黑洞了（主机 " +
                                    u.host + "）";
                    co_return nullptr;
                }
            } catch (const Base::Exception &failure)
            {
                // UDP 套接字建不起来是这条链路上唯一的抛出点（见 QuicClientConnection::connect 的 @throws）：
                // 与其它几段一样折成返回值，不让异常穿过这一段
                LOG_WARN_FMT("HttpClient: 与 {} 建 HTTP/3 链路失败。底层原因：{}", u.host, failure.what());
                failureReason = "HTTP/3 这一侧没走通：本端建不起 UDP 套接字（主机 " + u.host + "）";
                co_return nullptr;
            }
            co_return link;
        }

        /**
         * @brief 走完一次出站请求：有池就先复用、用完还回去，没池就按一次一条连接的老口径
         * @param loop 所属事件循环
         * @param request 方法、正文、媒体类型与附加头部（调用方给的写法已由 validateRequest 把住）
         * @param u 已拆开的 URL
         * @param requestTimeout 整体时限（握手、发送、收完响应三段之和）
         * @param pool 空闲连接池；为空即一次一条连接
         * @param failureReason 输出：失败发生在哪一段，供 send() 折成 expected 的失败值
         * @param clientTls 本客户端的出站 TLS 上下文；为空即静态那一路的进程级默认档
         * @param http3 出站 h3 的本端设置；为空、开关关着、明文 URL 或流式上传都不走 h3
         * @return std::unique_ptr<HttpClientResponse> 响应；失败（含超时）返回空
         */
        Core::Task<std::unique_ptr<HttpClientResponse>> performRequest(Core::EventLoop &loop, const HttpClientRequest &request, const ParsedUrl &u,
                                                                       const std::chrono::milliseconds requestTimeout, HttpOutboundConnectionPool *pool, std::string &failureReason,
                                                                       const Core::TlsContext *clientTls, const Http3OutboundContext *http3)
        {
            const std::chrono::steady_clock::time_point startedAt   = std::chrono::steady_clock::now();
            const bool                                  isKeepAlive = pool != nullptr;
            const HttpOutboundEndpointKey               endpointKey{u.host, u.port, u.scheme == "https"};
            // 请求文按要再拼：h2 与 h3 那一支用不上它（帧里没有请求行），提前拼一份等于把正文整块多拷一次
            const auto makeRequestText = [&] { return buildRequestText(request, u, isKeepAlive); };
            const bool isHeadRequest   = request.method == "HEAD";
            // 这次请求有没有资格走 h3：开关与 URL 形状之外，还要这个端点没被探败过
            const bool isHttp3Eligible = pool != nullptr && canUseHttp3(u, request, http3) && !isHttp3ProbeRejected(endpointKey, *http3);

            // 复用→建连这一整段要能重跑一遍：等同一端点建连的人醒来之后，第一件该做的
            // 还是回池里看有没有现成的连接，而不是就地假设「有」或「没有」。
            while (true)
            {
                if (pool != nullptr)
                {
                    // h3 的待命链路问在最前：开关开着时它优先，走不通才落到 TCP 那两张表
                    if (isHttp3Eligible)
                    {
                        if (auto cachedHttp3 = pool->acquireHttp3(endpointKey); cachedHttp3 != nullptr)
                        {
                            OutboundExchange cachedHttp3Exchange = co_await exchangeOnHttp3(cachedHttp3->http3(), u, request, startedAt, requestTimeout, failureReason);
                            switch (classifyMultiplexedExchange(cachedHttp3Exchange, request.method, u.host, failureReason))
                            {
                                case ReuseDisposition::Serve:
                                    co_return std::move(cachedHttp3Exchange.response);
                                case ReuseDisposition::Fail:
                                    co_return nullptr;
                                case ReuseDisposition::Retry:
                                    // 这条链路一个字节都没答过：多半是空闲期间被对端收了（QUIC 的 idle
                                    // timeout 或对端直接走掉）。往下重开一条，对调用方仍是一次成功请求
                                    break;
                            }
                        }
                    }
                    // h2 的待命连接问在 h1 的空闲表之前：一台主机的 ALPN 结果是稳定的，两处不会同时有货
                    if (auto cachedHttp2 = pool->acquireHttp2(endpointKey); cachedHttp2 != nullptr)
                    {
                        OutboundExchange cachedExchange = co_await exchangeOnHttp2(*cachedHttp2, u, request, startedAt, requestTimeout, failureReason);
                        switch (classifyMultiplexedExchange(cachedExchange, request.method, u.host, failureReason))
                        {
                            case ReuseDisposition::Serve:
                                co_return std::move(cachedExchange.response);
                            case ReuseDisposition::Fail:
                                co_return nullptr;
                            case ReuseDisposition::Retry:
                                // 一个字节没发出、也没收到，或是幂等方法发出去没了回音：多半是对端在我们手里把这条
                                // 连接收了（与 h1 的 keep-alive 同一条竞态）。这条就此作废——不作废也没有下一句，
                                // HPACK 动态表跟着连接一起丢——往下重开一条重来一次，对调用方仍是一次成功请求
                                break;
                        }
                    }
                    if (auto reused = pool->acquire(endpointKey))
                    {
                        const std::optional<std::chrono::milliseconds> reusedBudget = remainingBudget(startedAt, requestTimeout);
                        if (reusedBudget.has_value())
                        {
                            const std::string requestText = makeRequestText();
                            OutboundExchange  exchange = co_await exchangeOnConnection(loop, *reused, requestText, request.bodySource, isHeadRequest, *reusedBudget, failureReason);
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
                            // 读到过字节才失败的不能重来——那已经是「响应本身有问题」，重发也不换一个答案
                            if (exchange.isAnyByteReceived)
                            {
                                co_return nullptr;
                            }
                            if (exchange.isAnyByteSent && !isIdempotentRequestMethod(request.method))
                            {
                                // 请求已整个写上通路却没有回音：本端分不清「对端没见过它」与「对端正慢」，
                                // 非幂等的按可能已经执行过处置（RFC 9112 §9.3.2 的重试许可只给幂等方法）
                                failureReason = "请求已整个写上通路而对端没答话：方法 " + std::string{request.method} + " 不在幂等集合里，本端不重发（主机 " + u.host + "）";
                                co_return nullptr;
                            }
                            reused->close();
                        }
                    }
                }

                // 同一端点同时只允许一次建连：占不到资格的人等领导者结算，醒来重新跑一遍上面那段
                // （h3 与 h2 的结论都已进池，直接用；h1 是独占的，等不到也不该等，自己建一条）。
                bool isEstablishmentLeader = false;
                while (pool != nullptr)
                {
                    if (pool->tryBeginEstablishment(endpointKey))
                    {
                        isEstablishmentLeader = true;
                        break;
                    }
                    co_await pool->awaitEstablishment(endpointKey, loop);
                    if (!remainingBudget(startedAt, requestTimeout).has_value())
                    {
                        failureReason = "本次请求已到时限：等同一端点的建连把预算用尽（主机 " + u.host + "）";
                        co_return nullptr;
                    }
                    // 领导者建好的通路都在池里：h3 与 h2 两条待命表重新问一遍，谁有货就用谁
                    if (isHttp3Eligible)
                    {
                        if (auto sharedHttp3 = pool->acquireHttp3(endpointKey); sharedHttp3 != nullptr)
                        {
                            OutboundExchange sharedHttp3Exchange = co_await exchangeOnHttp3(sharedHttp3->http3(), u, request, startedAt, requestTimeout, failureReason);
                            switch (classifyMultiplexedExchange(sharedHttp3Exchange, request.method, u.host, failureReason))
                            {
                                case ReuseDisposition::Serve:
                                    co_return std::move(sharedHttp3Exchange.response);
                                case ReuseDisposition::Fail:
                                    co_return nullptr;
                                case ReuseDisposition::Retry:
                                    continue; // 这条已经废了：回去抢资格
                            }
                        }
                    }
                    // 领导者建成了可共享的 h2 连接：直接拿它，一次握手都不用再付
                    if (auto shared = pool->acquireHttp2(endpointKey); shared != nullptr)
                    {
                        OutboundExchange sharedExchange = co_await exchangeOnHttp2(*shared, u, request, startedAt, requestTimeout, failureReason);
                        switch (classifyMultiplexedExchange(sharedExchange, request.method, u.host, failureReason))
                        {
                            case ReuseDisposition::Serve:
                                co_return std::move(sharedExchange.response);
                            case ReuseDisposition::Fail:
                                // 与第一次复用上同样的判据：对端答过话、或幂等性不允许，重开也不换一个答案
                                co_return nullptr;
                            case ReuseDisposition::Retry:
                                continue; // 这条已经废了：回去抢资格，抢到了就自己建一条
                        }
                    }
                }

                // 领导者归还资格的地方只有这几处：结论进池之后、走 h1 之前、以及每条失败出口。
                // 不用作用域守卫是因为本框架的协程帧**不在 co_return 时销毁**（FinalAwaiter 是 no-op），
                // 帧要等调用方放手才回收——那时才结算，等待方白等一整段请求时间。
                // 也不设兜底的异常出口：连接、握手与 h2 三层各自把通路异常折成了返回值（见
                // connectWithDeadline、establishSecureConnection 的 catch，与 Http2ClientConnection
                // 「绝不让异常穿过协程帧」那条），这一段里能逃给调用方的只剩分配失败
                const auto settleEstablishmentNow = [&]
                {
                    if (isEstablishmentLeader)
                    {
                        isEstablishmentLeader = false;
                        pool->settleEstablishment(endpointKey);
                    }
                };

                // 占到建连资格的人先探 h3：探成就用这条通路，探不通再落到下面的 TCP。
                // 探测的时限单独设上界（kHttp3ProbeTimeout），UDP 被黑洞时也只吃掉这一小段，
                // 剩下的预算仍归那次真正的 TCP 请求用——这是「失败回落」这条承诺能成立的地方
                if (isHttp3Eligible)
                {
                    std::shared_ptr<Http3OutboundLink> link = co_await establishHttp3Link(loop, u, *pool, *http3, failureReason);
                    if (link == nullptr)
                    {
                        // 这个端点的 h3 没走通：记一笔，此后同端点的请求直接走 TCP，不再各付一次探测时限
                        rejectHttp3Probe(endpointKey, *http3);
                    } else
                    {
                        OutboundExchange freshHttp3 = co_await exchangeOnHttp3(link->http3(), u, request, startedAt, requestTimeout, failureReason);
                        switch (classifyMultiplexedExchange(freshHttp3, request.method, u.host, failureReason))
                        {
                            case ReuseDisposition::Serve:
                                // 还能提请求才收进池：出过问题的那条跟着最后一个持有者一起收口
                                if (link->isHealthy())
                                {
                                    pool->adoptHttp3(endpointKey, std::move(link));
                                }
                                settleEstablishmentNow();
                                co_return std::move(freshHttp3.response);
                            case ReuseDisposition::Fail:
                                settleEstablishmentNow();
                                co_return nullptr;
                            case ReuseDisposition::Retry:
                                // 握手与起步都成了、这条请求却一个字节都没走通：这个端点的 h3 记下别再探，
                                // 这条链路跟着放手收口，下面按 TCP 重来一次
                                rejectHttp3Probe(endpointKey, *http3);
                                break;
                        }
                    }
                    // h3 这一路没给出结论：把探测阶段的措辞清掉，让下面那一段写它自己的失败原因
                    failureReason.clear();
                }

                std::unique_ptr<HttpOutboundConnection> connection;
                if (u.scheme == "https")
                {
                    connection = co_await establishSecureConnection(loop, u, startedAt, requestTimeout, failureReason, clientTls);
                } else
                {
                    connection = co_await establishPlainConnection(loop, endpointKey, startedAt, requestTimeout, failureReason);
                }
                if (!connection)
                {
                    settleEstablishmentNow();
                    co_return nullptr;
                }
                // 正文上限按池的配置落在这条新连接上：解析器是连接的成员初值（默认档）建的，而「这台
                // 客户端允许收多大的响应」是使用方定的。从池里取回来的那条在建好时就落过同一个数
                if (pool != nullptr)
                {
                    connection->parser().setMaximumBodySize(pool->config().maximumResponseBodyBytes);
                }
                if (connection->selectedAlpnProtocol() == kHttp2AlpnProtocolName)
                {
                    // ALPN 选到了 h2：换一种说话方式。前奏在这里走——从池里拿回来的那条早就走过了，
                    // 所以这一步只属于「刚建好的」这一支
                    const std::optional<std::chrono::milliseconds> startBudget = remainingBudget(startedAt, requestTimeout);
                    Http2ClientConnection::Config                  http2Config;
                    if (pool != nullptr)
                    {
                        // 同一条胃口换成 h2 那一侧的说法：协商出哪条协议不该改变本端愿意收多少正文
                        http2Config.maximumResponseBodyBytes = pool->config().maximumResponseBodyBytes;
                    }
                    auto http2Connection = std::make_shared<Http2ClientConnection>(loop, std::move(connection), std::move(http2Config));
                    if (!startBudget.has_value() || !co_await http2Connection->start(*startBudget))
                    {
                        failureReason = "HTTP/2 前奏没走完：对端没接我们的 SETTINGS，或时限先到（主机 " + u.host + "）";
                        settleEstablishmentNow();
                        co_return nullptr;
                    }
                    OutboundExchange freshExchange = co_await exchangeOnHttp2(*http2Connection, u, request, startedAt, requestTimeout, failureReason);
                    if (pool == nullptr)
                    {
                        // 没有池就是一次性的：主动 shutdown 而不是任其析构，否则对端把这次收口记成 abrupt
                        co_await http2Connection->shutdown();
                    } else if (http2Connection->isHealthy())
                    {
                        // 收进池里留着待命；不健康的那条不收，跟着最后一个持有者一起收口
                        pool->adoptHttp2(endpointKey, std::move(http2Connection));
                    }
                    // 交出结论之后再归还建连资格：等待者醒来时要么能拿到这条连接，要么看到它已废、
                    // 自己去建一条。试过「前奏一完就交池并结算」让等待者只等一次握手（而不是等完
                    // 领导者整条请求），那一版在 keep-alive 竞态的恢复用例上红：第二条新建的连接
                    // 立刻读失败，机理没查清，因此这里取正确的那一种
                    settleEstablishmentNow();
                    co_return std::move(freshExchange.response);
                }

                // 走 h1 之前归还建连资格：h1 的连接是独占的，领导者不会把它交进池给别人用，
                // 等待者继续等的意义只有一条——等一个不会来的复用。让它们立刻各自去建
                settleEstablishmentNow();

                const std::optional<std::chrono::milliseconds> exchangeBudget = remainingBudget(startedAt, requestTimeout);
                if (!exchangeBudget.has_value())
                {
                    failureReason = "本次请求已到时限：连接建好了却没剩下写请求的预算（主机 " + u.host + "）";
                    co_return nullptr;
                }
                const std::string requestText = makeRequestText();
                OutboundExchange  exchange    = co_await exchangeOnConnection(loop, *connection, requestText, request.bodySource, isHeadRequest, *exchangeBudget, failureReason);
                if (exchange.response && pool != nullptr && isResponseReusable(exchange.response->headers))
                {
                    connection->prepareForNextRequest();
                    pool->release(std::move(connection));
                }
                co_return std::move(exchange.response);
            }
        }
    } // namespace

    Core::Task<std::expected<HttpClientResponse, std::string>> HttpClient::send(Core::EventLoop &loop, const std::string_view url, HttpClientRequest request,
                                                                                const std::chrono::milliseconds requestTimeout)
    {
        // 畸形 URL 与不合规范的头部都是用法错误，按 parseUrl 的既有口径抛出，不折进 expected 的失败值
        const ParsedUrl parsed = parseUrl(url);
        validateRequest(request);
        std::string failureReason;
        // 静态那一路不带池、也没有承载 TLS 策略与 h3 开关的地方，因此永不走 h3（两个空指针即此意）
        std::unique_ptr<HttpClientResponse> response = co_await performRequest(loop, request, parsed, requestTimeout, nullptr, failureReason, nullptr, nullptr);
        if (!response)
        {
            // 每条失败路径都会先写下原因；这里兜住的是「哪天新增了忘了写的出口」，而不是让调用方拿到空原因
            co_return std::unexpected(failureReason.empty() ? "请求失败：没拿到响应，也没有记下原因（目标 " + std::string(url) + "）" : std::move(failureReason));
        }
        // 解压放在这里：两条承载（h1 与 h2）的响应都汇到这一处，代加声明与解回来的判据只写一遍
        if (!applyContentEncoding(request, *response, failureReason))
        {
            co_return std::unexpected(std::move(failureReason));
        }
        co_return std::move(*response);
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::get(Core::EventLoop &loop, std::string_view url, const std::chrono::milliseconds requestTimeout)
    {
        const HttpClientRequest request;
        auto                    sent = co_await send(loop, url, request, requestTimeout);
        if (!sent.has_value())
        {
            LOG_ERROR_FMT("HttpClient: GET {} 失败。原因：{}", url, sent.error());
            co_return nullptr;
        }
        co_return std::make_unique<HttpClientResponse>(std::move(*sent));
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::post(Core::EventLoop &loop, std::string_view url, std::string_view contentType, std::string_view body,
                                                                     const std::chrono::milliseconds requestTimeout)
    {
        HttpClientRequest request;
        request.method = "POST";
        request.body   = body;
        // 声明过「正文按表单编码」却没给类型时补上那个类型：交一个空 Content-Type 出去等于没声明，
        // 对端只能按 application/octet-stream 猜（RFC 9110 §8.4）
        request.contentType = contentType.empty() ? std::string_view{"application/x-www-form-urlencoded"} : contentType;
        auto sent           = co_await send(loop, url, request, requestTimeout);
        if (!sent.has_value())
        {
            LOG_ERROR_FMT("HttpClient: POST {} 失败。原因：{}", url, sent.error());
            co_return nullptr;
        }
        co_return std::make_unique<HttpClientResponse>(std::move(*sent));
    }

    HttpClient::HttpClient(Core::EventLoop &loop, const HttpOutboundConnectionPool::Config poolConfig) : HttpClient(loop, poolConfig, Core::TlsPolicy{})
    {
    }

    // 唯一要做的事是放掉那份 TLS 上下文（头文件里它只是前向声明）；池由自己的成员收尾
    HttpClient::~HttpClient() = default;

    HttpClient::HttpClient(Core::EventLoop &loop, const HttpOutboundConnectionPool::Config poolConfig, const Core::TlsPolicy &tlsPolicy) :
        m_loop(&loop), m_pool(poolConfig), m_clientTls(std::make_unique<Core::TlsContext>(tlsPolicy, Core::TlsContext::Role::Client)),
        m_tlsPolicy(std::make_unique<Core::TlsPolicy>(tlsPolicy)), m_circuitBreaker(std::make_shared<OutboundCircuitBreaker>())
    {
        // 出站一侧恒要校验对端证书：不校验等于任何受信 CA 给他域签的证书都能冒充目标主机（CWE-297）。
        // 这一句放在构造而不是每条连接里，是为了让「策略里自带 CA」与「用系统信任库」两种配置的取舍
        // 只有一处判据（见 TlsContext::enableClientPeerVerification）
        m_clientTls->enableClientPeerVerification();
    }

    bool HttpClient::setClientCertificate(const std::string &certificateFile, const std::string &keyFile)
    {
        // 装载失败保持原状态：本客户端继续以「不带身份」出站，调用方拿到 false 自己决定要不要放弃
        // 路径也一并记下：开了 HTTP/3 时 QUIC 连接按同样的路径带身份，两条通路不能一条带一条不带
        if (!m_clientTls->loadCertificate(certificateFile, keyFile))
        {
            return false;
        }
        m_clientCertificateFile = certificateFile;
        m_clientPrivateKeyFile  = keyFile;
        return true;
    }

    void HttpClient::setHttp3Enabled(const bool isEnabled) noexcept
    {
        m_isHttp3Enabled = isEnabled;
    }

    bool HttpClient::isHttp3Enabled() const noexcept
    {
        return m_isHttp3Enabled;
    }

    std::size_t HttpClient::idleHttp3LinkCount() const noexcept
    {
        return m_pool.idleHttp3LinkCount();
    }

    std::size_t HttpClient::http3MaximumInFlightStreamCount() const noexcept
    {
        return m_pool.http3MaximumInFlightStreamCount();
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::get(std::string_view url, const std::chrono::milliseconds requestTimeout)
    {
        const HttpClientRequest request;
        co_return co_await sendPooled(url, request, requestTimeout);
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::post(std::string_view url, std::string_view contentType, std::string_view body,
                                                                     const std::chrono::milliseconds requestTimeout)
    {
        HttpClientRequest request;
        request.method = "POST";
        request.body   = body;
        // 与静态的 post() 同一口径：没给媒体类型的表单正文补上那个类型，两条连路不该有两种写法
        request.contentType = contentType.empty() ? std::string_view{"application/x-www-form-urlencoded"} : contentType;
        co_return co_await sendPooled(url, request, requestTimeout);
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::sendPooled(const std::string_view url, const HttpClientRequest &request,
                                                                           const std::chrono::milliseconds requestTimeout)
    {
        const ParsedUrl parsed = parseUrl(url);

        // 挂了罐子才拷一份请求出来加头部：没挂时开销与行为都和原来一样
        std::optional<HttpClientRequest> requestWithCookie;
        const HttpClientRequest         *effectiveRequest = &request;
        if (m_cookieJar != nullptr)
        {
            // 调用方自己写了 cookie 头就以他为准：替他改成罐子里的那份，等于静默覆盖明确给出的头部
            const bool callerHasCookieHeader =
                    std::ranges::any_of(request.headers, [](const HttpClientHeaderField &field) { return equalsIgnoreAsciiCase(field.first, "cookie"); });
            if (!callerHasCookieHeader)
            {
                if (const auto cookieHeader = m_cookieJar->buildRequestHeader(parsed.host, parsed.scheme == "https", parsed.path); cookieHeader.has_value())
                {
                    requestWithCookie = request;
                    requestWithCookie->headers.emplace_back("cookie", *cookieHeader);
                    effectiveRequest = &requestWithCookie.value();
                }
            }
        }

        validateRequest(*effectiveRequest);
        std::string failureReason;

        const HttpOutboundEndpointKey endpointKey{parsed.host, parsed.port, parsed.scheme == "https"};
        if (m_circuitBreaker != nullptr && !m_circuitBreaker->allowRequest(endpointKey))
        {
            // 开闸期间连套接字都不建：上游塌掉时这笔钱原本每个请求重付一遍，而调用方只看到「超时」
            failureReason = std::format("{} {} 被本地熔断器挡下：该端点处于开闸期，不解析地址、不建连接也不握手（到点会自动放一条探测请求）", request.method, url);
            LOG_ERROR_FMT("HttpClient: {}", failureReason);
            co_return nullptr;
        }

        // 出站 h3 的本端设置：几份引用都指向本客户端的成员，活到这次 co_await 完成之后。
        // 按引用而不是拷贝交给选路那一段，是为了不给每条出站请求添几份字符串拷贝
        Http3OutboundContext http3Context;
        http3Context.isEnabled             = m_isHttp3Enabled;
        http3Context.tlsPolicy             = m_tlsPolicy.get();
        http3Context.clientCertificateFile = &m_clientCertificateFile;
        http3Context.clientPrivateKeyFile  = &m_clientPrivateKeyFile;
        http3Context.rejectedEndpoints     = &m_http3RejectedEndpoints;

        std::unique_ptr<HttpClientResponse> response =
                co_await performRequest(*m_loop, *effectiveRequest, parsed, requestTimeout, &m_pool, failureReason, m_clientTls.get(), &http3Context);
        if (!response)
        {
            if (m_circuitBreaker != nullptr)
            {
                m_circuitBreaker->reportFailure(endpointKey);
            }
            LOG_ERROR_FMT("HttpClient: {} {} 失败。原因：{}", request.method, url, failureReason);
            co_return nullptr;
        }
        if (m_circuitBreaker != nullptr)
        {
            // 传输层失败与 5xx 记失败；4xx 是使用方的请求有问题，记到上游头上会让一次错误的
            // 调用把所有人挡在门外
            if (response->statusCode >= 500)
            {
                m_circuitBreaker->reportFailure(endpointKey);
            } else
            {
                m_circuitBreaker->reportSuccess(endpointKey);
            }
        }
        if (!applyContentEncoding(*effectiveRequest, *response, failureReason))
        {
            LOG_ERROR_FMT("HttpClient: {} {} 失败。原因：{}", request.method, url, failureReason);
            co_return nullptr;
        }

        if (m_cookieJar != nullptr)
        {
            std::vector<std::string> setCookieValues;
            for (const HttpClientHeaderField &field: response->headers)
            {
                if (equalsIgnoreAsciiCase(field.first, "set-cookie"))
                {
                    setCookieValues.push_back(field.second);
                }
            }
            if (!setCookieValues.empty())
            {
                m_cookieJar->storeFromResponse(parsed.host, parsed.scheme == "https", parsed.path, setCookieValues);
            }
        }
        co_return response;
    }

    Core::Task<std::unique_ptr<HttpClientResponse>> HttpClient::send(const std::string_view url, const HttpClientRequest &request, const std::chrono::milliseconds requestTimeout)
    {
        co_return co_await sendPooled(url, request, requestTimeout);
    }

    void HttpClient::setCookieJar(std::shared_ptr<HttpCookieJar> cookieJar) noexcept
    {
        m_cookieJar = std::move(cookieJar);
    }

    std::shared_ptr<HttpCookieJar> HttpClient::cookieJar() const noexcept
    {
        return m_cookieJar;
    }

    void HttpClient::setCircuitBreaker(std::shared_ptr<OutboundCircuitBreaker> circuitBreaker) noexcept
    {
        m_circuitBreaker = std::move(circuitBreaker);
    }

    std::shared_ptr<OutboundCircuitBreaker> HttpClient::circuitBreaker() const noexcept
    {
        return m_circuitBreaker;
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

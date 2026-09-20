// Net HTTP/1.1 服务端自检：路由与中间件、解析上限、分块与 SSE、WebSocket、限额、观测端点与优雅收口
//
// 客户端全部走主线程的阻塞套接字并带 SO_RCVTIMEO 硬时限，不用协程探针：
// 一来「期望没有响应」的用例本就必须自带时限（挂在 asyncReceive 上没有退路），
// 二来一个塞着几十次 await 的协程帧会让 GCC 13 在编译期直接 internal compiler error。
// 只有两处用协程：取服务器统计并 stop()/drain()（只能在服务所属循环上做），
// 以及跑一遍框架自带的 Net::HttpClient（跑在另一条循环上，不与被测服务抢线程）。
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/ConnectionDistributor.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoContext.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/FileSender.h"
#include "Net/Http/HttpParserLimits.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/HttpServer.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/HttpServerStats.h"
#include "Net/Http/Middleware.h"
#include "Net/Http/Router.h"
#include "Net/Tcp/PerIpConnectionLimiter.h"
#include "Net/Tcp/TcpServer.h"
#include "Net/WebSocket/WebSocketPeer.h"
#include "Platform/Platform.h"
#include "Platform/System/ProcessInfo.h"
#include "common/SampleSupport.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if ASYN_PLATFORM_WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

using namespace AsynGyanis;

namespace
{
    /**
     * @brief 一条已解析的响应
     */
    struct ResponseView
    {
        int                                              status{0};         ///< 状态码
        std::string                                      reason;            ///< 原因短语
        std::vector<std::pair<std::string, std::string>> headers;           ///< 头字段，按收到顺序
        std::string                                      body;              ///< 正文（chunked 时是原始分块字节）
        std::size_t                                      consumedLength{0}; ///< 这条响应在流里占掉的字节数
    };

    /**
     * @brief 一次会话的读取结果
     */
    struct SessionRead
    {
        std::string bytes;             ///< 收到的全部字节
        bool        isClosedByPeer{false}; ///< 是否等到对端收口（与「只是超时」区分开）
        std::string stopReason;        ///< 读循环为何停下：eof / timeout / reset / error-N，见 describeReadStopReason
    };

    /// 大小写不敏感比较（HTTP 头名按规范不区分大小写）
    bool equalsIgnoringCase(const std::string_view left, const std::string_view right)
    {
        return left.size() == right.size() &&
               std::equal(left.begin(), left.end(), right.begin(),
                          [](const char leftChar, const char rightChar)
                          {
                              return std::tolower(static_cast<unsigned char>(leftChar)) == std::tolower(static_cast<unsigned char>(rightChar));
                          });
    }

    /**
     * @brief 取某个头字段的值
     * @param response 目标响应
     * @param name 头名（大小写不敏感）
     * @return std::string 值；没有则空串
     */
    std::string headerOf(const ResponseView &response, const std::string_view name)
    {
        for (const auto &[field, value]: response.headers)
        {
            if (equalsIgnoringCase(field, name))
            {
                return value;
            }
        }
        return {};
    }

    /**
     * @brief 从字节流开头解析一条响应
     * @param stream 连接上已收到的字节
     * @return std::optional<ResponseView> 头都凑不齐时返回空；正文没到齐时给已收到的部分
     */
    std::optional<ResponseView> parseResponse(const std::string_view stream)
    {
        const std::size_t headerEnd = stream.find("\r\n\r\n");
        if (headerEnd == std::string_view::npos)
        {
            return std::nullopt;
        }

        ResponseView response;
        response.consumedLength = headerEnd + 4;
        const std::string_view head = stream.substr(0, headerEnd);

        const std::size_t statusSpace = head.find(' ');
        if (head.substr(0, 5) != "HTTP/" || statusSpace == std::string_view::npos)
        {
            return std::nullopt;
        }
        response.status = std::atoi(std::string{head.substr(statusSpace + 1, 3)}.c_str());
        const std::size_t reasonEnd = head.find("\r\n");
        response.reason = head.substr(statusSpace + 4, reasonEnd == std::string_view::npos ? std::string_view::npos : reasonEnd - statusSpace - 4);

        std::size_t lineStart = head.find("\r\n");
        while (lineStart != std::string_view::npos && lineStart + 2 < head.size())
        {
            const std::size_t lineEnd = head.find("\r\n", lineStart + 2);
            const std::string_view line = head.substr(lineStart + 2, (lineEnd == std::string_view::npos ? head.size() : lineEnd) - lineStart - 2);
            const std::size_t      colon = line.find(':');
            if (colon != std::string_view::npos)
            {
                std::string name{line.substr(0, colon)};
                std::string value{line.substr(colon + 1)};
                while (!value.empty() && value.front() == ' ')
                {
                    value.erase(value.begin());
                }
                response.headers.emplace_back(std::move(name), std::move(value));
            }
            if (lineEnd == std::string_view::npos)
            {
                break;
            }
            lineStart = lineEnd;
        }

        const std::string_view body = stream.substr(headerEnd + 4);
        if (const std::string lengthText = headerOf(response, "Content-Length"); !lengthText.empty())
        {
            const std::size_t bodyLength = static_cast<std::size_t>(std::stoull(lengthText));
            if (body.size() < bodyLength)
            {
                // 正文没到齐：状态与头仍算解析出来了（HEAD 就常年落在这条路上）
                response.body           = std::string{body};
                response.consumedLength = stream.size();
                return response;
            }
            response.body = std::string{body.substr(0, bodyLength)};
            response.consumedLength += bodyLength;
            return response;
        }
        // 没有 Content-Length：分块与「关连接收尾」都按「收到什么就是什么」处理
        response.body           = std::string{body};
        response.consumedLength = stream.size();
        return response;
    }

    /**
     * @brief 把一条连接上收到的字节流按响应切开
     * @param stream 收到的字节
     * @return std::vector<ResponseView> 能解析出来的响应，按顺序
     */
    std::vector<ResponseView> splitResponses(const std::string &stream)
    {
        std::vector<ResponseView> responses;
        std::string_view          remaining{stream};
        while (!remaining.empty())
        {
            const auto parsed = parseResponse(remaining);
            if (!parsed.has_value() || parsed->consumedLength == 0)
            {
                break;
            }
            responses.push_back(*parsed);
            remaining = remaining.substr(std::min(parsed->consumedLength, remaining.size()));
        }
        return responses;
    }

    /// 第一条响应的状态码；一条都解析不出来时返回 0
    int firstStatusOf(const SessionRead &reading)
    {
        const auto responses = splitResponses(reading.bytes);
        return responses.empty() ? 0 : responses.front().status;
    }

    /**
     * @brief 组装一条请求
     * @param method 方法
     * @param target 请求目标
     * @param extraHeaders 额外头字段
     * @param body 正文
     * @return std::string 可直接写进套接字的报文
     */
    std::string makeRequest(const std::string_view method, const std::string_view target, const std::vector<std::string> &extraHeaders,
                            const std::string_view body = {})
    {
        std::string request{method};
        request += ' ';
        request += target;
        request += " HTTP/1.1\r\n";
        for (const std::string &header: extraHeaders)
        {
            request += header;
            request += "\r\n";
        }
        request += "\r\n";
        request += body;
        return request;
    }

    /**
     * @brief 一条普通明文请求的头
     * @param isKeepAlive 是否要求复用连接；false 时带 Connection: close
     * @param extraHeader 额外一条头字段（可空）
     * @return std::vector<std::string> 头字段列表
     */
    std::vector<std::string> plainHeaders(const bool isKeepAlive = false, const std::string_view extraHeader = {})
    {
        std::vector<std::string> headers{std::string("Host: 127.0.0.1"),
                                         isKeepAlive ? "Connection: keep-alive" : "Connection: close"};
        if (!extraHeader.empty())
        {
            headers.emplace_back(extraHeader);
        }
        return headers;
    }

    /// 一次请求的完整报文（默认带 Connection: close）
    std::string plainRequest(const std::string_view method, const std::string_view target, const bool isKeepAlive = false,
                             const std::string_view extraHeader = {}, const std::string_view body = {})
    {
        return makeRequest(method, target, plainHeaders(isKeepAlive, extraHeader), body);
    }

    /**
     * @brief 按 RFC 6455 造一个客户端掩码帧
     * @param opcode 帧类型（0x1 文本、0x8 关闭）
     * @param payload 载荷
     * @return std::string 帧字节
     */
    std::string makeClientFrame(const std::uint8_t opcode, const std::string_view payload)
    {
        std::string       frame;
        const std::size_t length = payload.size();
        frame.push_back(static_cast<char>(0x80 | opcode));
        if (length < 126)
        {
            frame.push_back(static_cast<char>(0x80 | length));
        }
        else
        {
            frame.push_back(static_cast<char>(0x80 | 126));
            frame.push_back(static_cast<char>((length >> 8) & 0xFF));
            frame.push_back(static_cast<char>(length & 0xFF));
        }
        const char maskKey[4]{'S', 'a', 'm', 'p'};
        frame.append(maskKey, 4);
        for (std::size_t index = 0; index < length; ++index)
        {
            frame.push_back(static_cast<char>(payload[index] ^ maskKey[index % 4]));
        }
        return frame;
    }

    /// WebSocket 握手请求：升级三件套加上一个合法的 Sec-WebSocket-Key
    std::string webSocketHandshake(const std::string_view extraHeader = {})
    {
        std::vector<std::string> headers{std::string("Host: 127.0.0.1"), "Connection: Upgrade", "Upgrade: websocket",
                                         "Sec-WebSocket-Version: 13", "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ=="};
        if (!extraHeader.empty())
        {
            headers.emplace_back(extraHeader);
        }
        return makeRequest("GET", "/ws", headers);
    }

    /**
     * @brief 示例自己的 HTTP/1.1 路由
     * @details 挂法照抄 samples/main.cpp（那是部署形态），这里只放自检要的端点。
     */
    void setupRoutes(Net::Router &router)
    {
        router.get("/", [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
        {
            response.setStatus(200);
            response.setHeader("Content-Type", "text/plain");
            response.setBody("Hello World");
            co_return;
        });

        // 大正文：压缩中间件对太小的响应不值得动手，这一条才是压缩的正题
        router.get("/big", [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
        {
            response.setStatus(200);
            response.setHeader("Content-Type", "text/plain");
            std::string payload;
            for (int repeat = 0; repeat < 300; ++repeat)
            {
                payload += "compressible text payload. ";
            }
            response.setBody(std::move(payload));
            co_return;
        });

        router.get("/json", [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
        {
            response.setStatus(200);
            response.setHeader("Content-Type", "application/json");
            response.setBody(R"({"status":"ok"})");
            co_return;
        });

        // 回显正文：把「解析出请求 → 交给处理器 → 响应写回」这条链一整段钉住
        router.post("/echo", [](Net::HttpRequest &request, Net::HttpResponse &response) -> Core::Task<void>
        {
            response.setStatus(200);
            response.setHeader("Content-Type", "text/plain");
            response.setBody(std::string{request.body()});
            co_return;
        });

        // 路径与查询串：确认解析器把请求目标拆开了
        router.get("/inspect", [](Net::HttpRequest &request, Net::HttpResponse &response) -> Core::Task<void>
        {
            response.setStatus(200);
            response.setHeader("Content-Type", "text/plain");
            const auto parameters = request.queryParams();
            const auto found      = parameters.find("needle");
            response.setBody(std::string{request.path()} + "#" + (found == parameters.end() ? "missing" : found->second));
            co_return;
        });

        // 处理器抛异常：框架该把它折成 500，而不是让连接无声断掉
        router.get("/boom", [](Net::HttpRequest &, Net::HttpResponse &) -> Core::Task<void>
        {
            throw std::runtime_error("示例：处理器故意抛出");
            co_return;
        });

        router.get("/sse", [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
        {
            response.startChunkedResponse(200);
            response.setHeader("Content-Type", "text/event-stream");
            if (!co_await response.writeChunk("data: one\n\n"))
            {
                co_return;
            }
            static_cast<void>(co_await response.writeChunk("data: two\n\n"));
            co_return;
        });

        // WebSocket 回显：收到什么就原样回什么，帧进与帧出一次验完
        router.get("/ws", [](Net::HttpRequest &, Net::HttpResponse &response) -> Core::Task<void>
        {
            response.upgradeToWebSocket(
                    [](Net::WebSocketPeer &peer) -> Core::Task<>
                    {
                        while (const auto message = co_await peer.receive())
                        {
                            if (!co_await peer.sendText(message->payload))
                            {
                                co_return;
                            }
                        }
                        co_return;
                    });
            co_return;
        });
    }

    /// 自检结论：全部在主线程写、主线程读，不涉及跨线程可见性
    struct Observations
    {
        SessionRead root;               ///< GET /
        SessionRead big;                ///< 不带 Accept-Encoding 的大正文
        SessionRead bigGzip;            ///< 带 Accept-Encoding: gzip 的大正文
        SessionRead head;               ///< HEAD /
        SessionRead json;               ///< GET /json
        SessionRead missing;            ///< 未知路由
        SessionRead wrongMethod;        ///< POST /json
        SessionRead echo;               ///< 定长正文回显
        SessionRead chunkedUpload;      ///< 分块上传
        SessionRead expectContinue;     ///< Expect: 100-continue
        SessionRead oversizeBody;       ///< 越过正文上限
        SessionRead longUri;            ///< 越过请求目标上限
        SessionRead manyHeaders;        ///< 越过头字段条数上限
        SessionRead malformed;          ///< 不是 HTTP 的请求行
        SessionRead thrown;             ///< 处理器抛异常
        SessionRead inspect;            ///< 路径与查询串
        SessionRead echoedRequestId;    ///< 带 X-Request-Id 的一次
        SessionRead generatedRequestId; ///< 不带 X-Request-Id 的一次
        SessionRead sse;                ///< 分块流式响应
        SessionRead webSocket;          ///< 握手 + 回显 + 关闭
        SessionRead extension;          ///< 带 permessage-deflate 提议的握手
        SessionRead metricsFirst;       ///< 第一次 /metrics
        SessionRead metricsSecond;      ///< 第二次 /metrics
        SessionRead health;             ///< /healthz
        SessionRead pipelined;          ///< 一条连接上连发三条（限额服务器）
        SessionRead idleClosed;         ///< 空闲超时收口的连接（受护服务器）
        SessionRead rateFirst;          ///< 令牌桶第一条
        SessionRead rateSecond;         ///< 令牌桶第二条
        SessionRead rateThird;          ///< 令牌桶第三条
        SessionRead perIpProbe;         ///< 单来源额度被占满后的那一条
        SessionRead maxConnectionProbe; ///< 超过全局并发上限的那一条
        SessionRead afterStop;          ///< stop() 之后再连一条
        SessionRead staticPage;         ///< GET /page.html（静态文件目录）
        SessionRead staticMissing;      ///< 静态目录里没有的文件
        SessionRead staticUppercase;    ///< 大写扩展名的静态文件
        SessionRead dispatchedFirst;    ///< 接受分发链路第一条请求
        SessionRead dispatchedSecond;   ///< 接受分发链路第二条请求
        std::size_t statsTotalRequests{0};   ///< 服务器统计：累计请求数
        std::size_t statsBadRequests{0};     ///< 服务器统计：坏请求数
        bool        isHttpClientOkay{false}; ///< 走 Net::HttpClient 的一条真请求
    };

    Observations g_observations;

    /// 统计快照与 stop()/drain() 是否已在服务器的循环上做完
    std::atomic<bool> g_isChoreFinished{false};

    /// 框架自带客户端的探针是否跑完
    std::atomic<bool> g_isClientProbeFinished{false};

    /**
     * @brief 造一个只放两个文件的临时静态目录（一个小写扩展名、一个大写扩展名）
     * @return std::filesystem::path 目录路径；建不出来时为空
     */
    std::filesystem::path prepareStaticDirectory()
    {
        const auto directory = std::filesystem::temp_directory_path() /
                               ("asyn-sample-net_http-" + std::to_string(Platform::ProcessInfo::currentProcessId()));
        std::error_code makeError;
        std::filesystem::create_directories(directory, makeError);
        if (makeError)
        {
            return {};
        }
        for (const char *fileName: {"page.html", "PAGE.HTML"})
        {
            std::ofstream stream(directory / fileName, std::ios::binary);
            stream << "<p>static payload from disk</p>";
            stream.close();
            if (!stream)
            {
                return {};
            }
        }
        return directory;
    }

#if ASYN_PLATFORM_WIN32
    using socket_handle_t = SOCKET;
    constexpr socket_handle_t kInvalidSocketHandle = INVALID_SOCKET;

    /// 让后续 recv 最多等这些毫秒
    void applyReceiveTimeout(const socket_handle_t socketHandle, const int millisecondCount)
    {
        ::setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&millisecondCount), sizeof(millisecondCount));
    }

    void closeSocketHandle(const socket_handle_t socketHandle) noexcept
    {
        ::closesocket(socketHandle);
    }
#else
    using socket_handle_t = int;
    constexpr socket_handle_t kInvalidSocketHandle = -1;

    /// 让后续 recv 最多等这些毫秒
    void applyReceiveTimeout(const socket_handle_t socketHandle, const int millisecondCount)
    {
        const timeval timeout{millisecondCount / 1000, static_cast<suseconds_t>((millisecondCount % 1000) * 1000)};
        ::setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }

    void closeSocketHandle(const socket_handle_t socketHandle) noexcept
    {
        ::close(socketHandle);
    }
#endif

    /**
     * @brief 把「recv 没拿到字节」归因，供读循环记录停止原因
     * @details 「探针没等够」（timeout）与「服务端把连接复位了」（reset）在断言与排查时是两回事：
     *          后者通常是服务端在仍有未读入站数据时关闭连接，内核直接发 RST，连已写出的响应都可能一起丢
     * @param byteCount recv 的返回值，0 表示对端正常收口
     * @return std::string eof / timeout / reset / error-N 之一，直接进日志
     */
    [[nodiscard]] std::string describeReadStopReason(const int byteCount)
    {
        if (byteCount == 0)
        {
            return "eof";
        }
#if ASYN_PLATFORM_WIN32
        const int errorCode = static_cast<int>(::WSAGetLastError());
        if (errorCode == WSAETIMEDOUT)
        {
            return "timeout";
        }
        if (errorCode == WSAECONNRESET)
        {
            return "reset";
        }
#else
        const int errorCode = errno;
        if (errorCode == EAGAIN || errorCode == EWOULDBLOCK)
        {
            return "timeout";
        }
        if (errorCode == ECONNRESET)
        {
            return "reset";
        }
#endif
        return "error-" + std::to_string(errorCode);
    }

    /// 连到本机的示例端口；失败时返回无效句柄
    socket_handle_t connectToLocalhost(const std::uint16_t port)
    {
        const socket_handle_t socketHandle = ::socket(AF_INET, SOCK_STREAM, 0);
        if (socketHandle == kInvalidSocketHandle)
        {
            return kInvalidSocketHandle;
        }
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port   = htons(port);
#if ASYN_PLATFORM_WIN32
        target.sin_addr.S_un.S_addr = inet_addr("127.0.0.1");
#else
        target.sin_addr.s_addr = inet_addr("127.0.0.1");
#endif
        if (::connect(socketHandle, reinterpret_cast<sockaddr *>(&target), sizeof(target)) != 0)
        {
            closeSocketHandle(socketHandle);
            return kInvalidSocketHandle;
        }
        return socketHandle;
    }

    /**
     * @brief 一条手动管发生命周期的客户端会话
     * @details 析构自动关描述符。可以分多次写出、读到「对端收口」或「一次 recv 超时」为止，
     *          因此既能测 keep-alive 与管线化，也能测「被拒的连接拿不到响应」而不会挂住。
     */
    class Session
    {
    public:
        /**
         * @brief 连到端口
         * @param port 目标端口
         * @param receiveTimeoutMilliseconds 每次 recv 的最长等待
         */
        explicit Session(const std::uint16_t port, const int receiveTimeoutMilliseconds = 600) :
            m_socketHandle(connectToLocalhost(port))
        {
            if (m_socketHandle != kInvalidSocketHandle)
            {
                applyReceiveTimeout(m_socketHandle, receiveTimeoutMilliseconds);
            }
        }

        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;

        ~Session()
        {
            if (m_socketHandle != kInvalidSocketHandle)
            {
                closeSocketHandle(m_socketHandle);
            }
        }

        /// 是否连上了
        [[nodiscard]] bool isConnected() const noexcept
        {
            return m_socketHandle != kInvalidSocketHandle;
        }

        /**
         * @brief 写一段字节
         * @param payload 要发的字节
         * @return bool 全部写出去
         */
        bool send(const std::string &payload)
        {
            if (!isConnected())
            {
                return false;
            }
            return ::send(m_socketHandle, payload.data(), static_cast<int>(payload.size()), 0) == static_cast<int>(payload.size());
        }

        /**
         * @brief 读到「对端收口」或「一次 recv 超时」为止
         * @param maximumRounds 最多读几轮，留一个硬上界防止永不收口
         * @return SessionRead 收到的字节与是否被对端收口
         */
        SessionRead readUntilIdle(const int maximumRounds = 40)
        {
            SessionRead reading;
            if (!isConnected())
            {
                return reading;
            }
            char buffer[8192]{};
            for (int round = 0; round < maximumRounds; ++round)
            {
                const int byteCount = static_cast<int>(::recv(m_socketHandle, buffer, sizeof(buffer), 0));
                if (byteCount > 0)
                {
                    reading.bytes.append(buffer, static_cast<std::size_t>(byteCount));
                    continue;
                }
                reading.isClosedByPeer = byteCount == 0;
                reading.stopReason = describeReadStopReason(byteCount);
                break;
            }
            return reading;
        }

    private:
        socket_handle_t m_socketHandle; ///< 连接句柄
    };

    /**
     * @brief 一次请求一条连接的完整往返
     * @param port 目标端口
     * @param request 完整请求报文
     * @return SessionRead 收到的字节与是否被对端收口
     */
    SessionRead requestOnce(const std::uint16_t port, const std::string &request)
    {
        Session session(port);
        if (!session.send(request))
        {
            return {};
        }
        return session.readUntilIdle();
    }

    /**
     * @brief 在一条连接上按序发出多段字节再读回全部响应
     * @param port 目标端口
     * @param payloads 要按序写出的字节段
     * @return SessionRead 收到的字节与是否被对端收口
     */
    SessionRead requestChain(const std::uint16_t port, const std::vector<std::string> &payloads)
    {
        Session session(port);
        for (const std::string &payload: payloads)
        {
            if (!session.send(payload))
            {
                return {};
            }
        }
        return session.readUntilIdle();
    }

    /**
     * @brief 只连不断开的连接：用来把「单来源额度」与「全局并发上限」占满
     * @param port 目标端口
     * @param count 要挂住的条数
     * @return std::vector<socket_handle_t> 已连上的句柄
     */
    std::vector<socket_handle_t> openHeldConnections(const std::uint16_t port, const std::size_t count)
    {
        std::vector<socket_handle_t> held;
        for (std::size_t index = 0; index < count; ++index)
        {
            if (const socket_handle_t socketHandle = connectToLocalhost(port); socketHandle != kInvalidSocketHandle)
            {
                held.push_back(socketHandle);
            }
        }
        return held;
    }

    /// 释放 openHeldConnections 挂住的句柄
    void closeHeldConnections(std::vector<socket_handle_t> &held)
    {
        for (const socket_handle_t socketHandle: held)
        {
            closeSocketHandle(socketHandle);
        }
        held.clear();
    }

    /**
     * @brief 在服务器自己的循环上取统计快照，然后走优雅收口（stop + drain）
     * @details 统计与 stop()/drain() 动的都是那条循环正持有的对象，只能在那个线程上做
     * @param mainServer 主服务器
     * @return Core::Task<> 做完即返回
     */
    Core::Task<> collectStatsAndStop(Net::HttpServer &mainServer)
    {
        const Net::HttpServerStats statistics = mainServer.stats();
        g_observations.statsTotalRequests     = static_cast<std::size_t>(statistics.totalRequestCount);
        g_observations.statsBadRequests       = static_cast<std::size_t>(statistics.badRequestCount);

        mainServer.stop();
        co_await mainServer.drain(std::chrono::seconds{5});
        g_isChoreFinished.store(true, std::memory_order_release);
        co_return;
    }

    /**
     * @brief 框架自带客户端的一轮真请求：与上面手写的报文互为对照
     * @param loop 承载本次等待的循环
     * @param url 目标地址。**按值收**：惰性协程不能引用调用方的临时量
     * @return Core::Task<> 结论写进观测
     */
    Core::Task<> probeWithHttpClient(Core::EventLoop &loop, const std::string url)
    {
        const auto response = co_await Net::HttpClient::get(loop, url);
        g_observations.isHttpClientOkay = response != nullptr && response->statusCode == 200 && response->body == "Hello World";
        g_isClientProbeFinished.store(true, std::memory_order_release);
        co_return;
    }

    /// 等某台服务器把监听端口亮出来：只连不发，连上即说明内核已在听
    bool awaitListening(const std::uint16_t port)
    {
        return Samples::waitUntil(
                [port]
                {
                    const socket_handle_t probeHandle = connectToLocalhost(port);
                    if (probeHandle == kInvalidSocketHandle)
                    {
                        return false;
                    }
                    closeSocketHandle(probeHandle);
                    return true;
                },
                std::chrono::seconds{5}, std::chrono::milliseconds{20});
    }
} // namespace

int main(const int argc, char **argv)
{
    Samples::setupConsoleLogging();
    LOG_INFO("=== Net HTTP/1.1 服务端示例开始 ===");

    const std::uint16_t mainPort    = Samples::readPortArgument(argc, argv, 0);
    const std::uint16_t strictPort  = static_cast<std::uint16_t>(mainPort + 1);
    const std::uint16_t guardedPort = static_cast<std::uint16_t>(mainPort + 2);
    // 接受分发占两个端口：一个给「只接受与派发」的那台，一个给接手连接的 worker 自己占位
    const std::uint16_t dispatchPort       = static_cast<std::uint16_t>(mainPort + 3);
    const std::uint16_t dispatchWorkerPort = static_cast<std::uint16_t>(mainPort + 4);
    // 全局并发上限单独一台：受护那台的空闲时限被压到 300 毫秒去验空闲收口，挂住的静默连接
    // 会被清扫掉，名额随之空出来——探针就可能被正常服务，这条负向用例就成了赌调度
    const std::uint16_t cappedPort = static_cast<std::uint16_t>(mainPort + 5);
    auto &samples = Samples::checklist();

    Core::IoContext context(2);
    auto &          pool = context.threadPool();
    // 三条服务器都在第一条循环上；第二条只给 HttpClient 探针用——探针协程不与被测服务同循环，
    // 否则它挂在等响应上时分不清是服务端没处理还是这条循环没空去 accept
    Core::EventLoop &serverLoop = pool.eventLoop(0);
    Core::EventLoop &probeLoop  = pool.eventLoop(1);

    Net::HttpParserLimits parserLimits;
    parserLimits.maximumUriLength         = 1024;
    parserLimits.maximumHeaderCount       = 50;
    parserLimits.maximumHeaderBlockLength = 4096;
    parserLimits.maximumBodySize          = 1024;

    const auto buildServer = [&](const std::uint16_t port, const Net::HttpServerLimits &limits, const bool withExtras)
    {
        auto server = std::make_unique<Net::HttpServer>(serverLoop, *Core::InetAddress::resolve("127.0.0.1", port));
        setupRoutes(server->router());
        server->setLimits(limits);
        server->setParserLimits(parserLimits);
        if (withExtras)
        {
            server->router().addMiddleware(Net::compressionMiddleware());
            server->enableMetricsEndpoint();
            server->enableHealthEndpoint();
        }
        return server;
    };

    Net::HttpServerLimits defaultLimits;
    Net::HttpServerLimits strictLimits;
    strictLimits.maximumRequestsPerConnection = 2;
    // 空闲时限留默认的 75 秒：单来源上限那条要挂一条静默连接当占位，时限比探针跑完长才不会
    // 被清扫走、名额才确实占满
    Net::HttpServerLimits guardedLimits;
    guardedLimits.idleTimeout = std::chrono::milliseconds{300};

    auto mainServer    = buildServer(mainPort, defaultLimits, true);
    auto strictServer  = buildServer(strictPort, strictLimits, false);
    auto guardedServer = buildServer(guardedPort, guardedLimits, false);
    auto cappedServer  = buildServer(cappedPort, defaultLimits, false);

    // 静态文件目录：只挂在与业务路由同一台服务器上，配置必须在 start() 之前完成
    const std::filesystem::path staticDirectory = prepareStaticDirectory();
    const bool                  hasStaticDirectory = !staticDirectory.empty();
    if (hasStaticDirectory)
    {
        mainServer->staticFileDir(staticDirectory.string());
    }

    // 接受分发：acceptor 只接受与派发，连接对象与协议工作全落在另一条循环上的 worker 实例
    auto distributor     = std::make_shared<Core::ConnectionDistributor>();
    auto dispatchServer  = buildServer(dispatchWorkerPort, defaultLimits, false);
    auto acceptorServer  = buildServer(dispatchPort, defaultLimits, false);
    Net::HttpServer *rawDispatchServer = dispatchServer.get();
    distributor->addWorker(probeLoop, [rawDispatchServer](const int fileDescriptor)
    {
        rawDispatchServer->adoptConnection(fileDescriptor);
    });

    // 单来源上限 1：额度被占满时，再来一条连接不该拿到服务
    strictServer->setPerIpConnectionLimiter(std::make_shared<Net::PerIpConnectionLimiter>(1));
    // 全局并发上限 2：挂住两条之后第三条不该拿到服务。这台用默认 75 秒的空闲时限，
    // 挂住的静默连接才不会被清扫走、名额才确实占满
    cappedServer->setMaxConnections(2);
    // 令牌桶：容量 3、每秒补 1 个——空闲超时那条先占一枚，随后连发三条在第三条耗尽
    guardedServer->router().addMiddleware(
            Net::tokenBucketRateLimiterMiddleware(std::make_shared<Net::TokenBucket>(1.0, 3.0)));

    // 协程帧必须活到它跑完：本向量一路持有到 main 结束
    std::vector<Core::Task<>> tasks;
    tasks.push_back(mainServer->start());
    tasks.push_back(strictServer->start());
    tasks.push_back(guardedServer->start());
    tasks.push_back(cappedServer->start());
    tasks.push_back(acceptorServer->startAccepting(distributor));
    for (auto &task: tasks)
    {
        serverLoop.scheduler().schedule(task.handle());
    }
    pool.start();

    samples.check(awaitListening(mainPort) && awaitListening(strictPort) && awaitListening(guardedPort) && awaitListening(cappedPort) &&
                          awaitListening(dispatchPort),
                  "五条监听器都已在收连接（就绪等待有界，不靠睡一觉碰运气）");

    // —— 正向用例：一次请求一条连接，读循环以「对端收口」结束 ——
    g_observations.root        = requestOnce(mainPort, plainRequest("GET", "/"));
    g_observations.big         = requestOnce(mainPort, plainRequest("GET", "/big"));
    g_observations.bigGzip     = requestOnce(mainPort, plainRequest("GET", "/big", false, "Accept-Encoding: gzip"));
    g_observations.head        = requestOnce(mainPort, plainRequest("HEAD", "/"));
    g_observations.json        = requestOnce(mainPort, plainRequest("GET", "/json"));
    g_observations.missing     = requestOnce(mainPort, plainRequest("GET", "/nope"));
    g_observations.wrongMethod = requestOnce(mainPort, plainRequest("POST", "/json", false, "Content-Length: 0"));
    g_observations.echo        = requestOnce(mainPort, plainRequest("POST", "/echo", false, "Content-Length: 11", "hello echo?"));
    g_observations.chunkedUpload =
            requestOnce(mainPort, plainRequest("POST", "/echo", false, "Transfer-Encoding: chunked", "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"));
    g_observations.expectContinue = requestOnce(mainPort,
                                                makeRequest("POST", "/echo",
                                                            {"Host: 127.0.0.1", "Connection: close", "Expect: 100-continue",
                                                             "Content-Length: 4"},
                                                            "ping"));
    g_observations.oversizeBody = requestOnce(mainPort, plainRequest("POST", "/echo", false, "Content-Length: 4096", "short"));
    g_observations.longUri      = requestOnce(mainPort, plainRequest("GET", "/" + std::string(3000, 'a')));

    std::string manyHeadersRequest{"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"};
    for (int index = 0; index < 80; ++index)
    {
        manyHeadersRequest += "X-Filler-" + std::to_string(index) + ": v\r\n";
    }
    manyHeadersRequest += "\r\n";
    g_observations.manyHeaders = requestOnce(mainPort, manyHeadersRequest);

    g_observations.malformed = requestOnce(mainPort, "THIS IS NOT HTTP\r\n\r\n");
    g_observations.thrown    = requestOnce(mainPort, plainRequest("GET", "/boom"));
    g_observations.inspect   = requestOnce(mainPort, plainRequest("GET", "/inspect?needle=42"));

    g_observations.echoedRequestId    = requestOnce(mainPort, plainRequest("GET", "/", false, "X-Request-Id: probe-fixed-id"));
    g_observations.generatedRequestId = requestOnce(mainPort, plainRequest("GET", "/"));

    g_observations.sse       = requestOnce(mainPort, plainRequest("GET", "/sse"));
    g_observations.webSocket = requestChain(mainPort, {webSocketHandshake(), makeClientFrame(0x1, "ping-frame"), makeClientFrame(0x8, "")});
    g_observations.extension = requestChain(mainPort,
                                            {webSocketHandshake("Sec-WebSocket-Extensions: permessage-deflate"), makeClientFrame(0x8, "")});
    g_observations.metricsFirst  = requestOnce(mainPort, plainRequest("GET", "/metrics"));
    g_observations.health        = requestOnce(mainPort, plainRequest("GET", "/healthz"));
    g_observations.metricsSecond = requestOnce(mainPort, plainRequest("GET", "/metrics"));

    // —— 静态文件：请求路径不在路由表里时，兜底路由才去磁盘上找 ——
    g_observations.staticPage      = requestOnce(mainPort, plainRequest("GET", "/page.html"));
    g_observations.staticUppercase = requestOnce(mainPort, plainRequest("GET", "/PAGE.HTML"));
    g_observations.staticMissing   = requestOnce(mainPort, plainRequest("GET", "/nowhere.html"));

    // —— 接受分发：连接由 acceptor 收下，交给另一条循环上的 worker 实例服务 ——
    g_observations.dispatchedFirst  = requestOnce(dispatchPort, plainRequest("GET", "/"));
    g_observations.dispatchedSecond = requestOnce(dispatchPort, plainRequest("GET", "/json"));

    // —— 限额服务器：一条连接上按序管线化三条，第三条起连接该被收口（单连接请求数上限 2）——
    g_observations.pipelined = requestChain(strictPort,
                                           {plainRequest("GET", "/", true), plainRequest("GET", "/", true), plainRequest("GET", "/", true)});

    // —— 受护服务器：空闲超时那条先跑（它会吃掉桶里一枚令牌），随后三条限流探针正好在第三条耗尽 ——
    g_observations.idleClosed = requestOnce(guardedPort, plainRequest("GET", "/", true));
    g_observations.rateFirst  = requestOnce(guardedPort, plainRequest("GET", "/"));
    g_observations.rateSecond = requestOnce(guardedPort, plainRequest("GET", "/"));
    g_observations.rateThird  = requestOnce(guardedPort, plainRequest("GET", "/"));

    // —— 负向用例：期望「拿不到响应」，每次都带硬时限 ——
    {
        std::vector<socket_handle_t> held = openHeldConnections(strictPort, 1);
        g_observations.perIpProbe               = requestOnce(strictPort, plainRequest("GET", "/"));
        closeHeldConnections(held);
    }
    {
        std::vector<socket_handle_t> held = openHeldConnections(cappedPort, 2);
        g_observations.maxConnectionProbe       = requestOnce(cappedPort, plainRequest("GET", "/"));
        closeHeldConnections(held);
    }

    // —— 框架自带客户端：跑在第二条循环上 ——
    const std::string clientUrl = "http://127.0.0.1:" + std::to_string(mainPort) + "/";
    tasks.push_back(probeWithHttpClient(probeLoop, clientUrl));
    probeLoop.scheduler().scheduleRemote(tasks.back().handle());
    const bool isClientProbeDone = Samples::waitUntil([]
                                                      {
                                                          return g_isClientProbeFinished.load(std::memory_order_acquire);
                                                      },
                                                      std::chrono::seconds{20}, std::chrono::milliseconds{20});

    // —— 优雅收口：统计与 stop()/drain() 都投回服务器的循环 ——
    tasks.push_back(collectStatsAndStop(*mainServer));
    serverLoop.scheduler().scheduleRemote(tasks.back().handle());
    const bool isChoreDone = Samples::waitUntil([]
                                                {
                                                    return g_isChoreFinished.load(std::memory_order_acquire);
                                                },
                                                std::chrono::seconds{20}, std::chrono::milliseconds{20});

    g_observations.afterStop = requestOnce(mainPort, plainRequest("GET", "/"));

    samples.check(isClientProbeDone && isChoreDone, "客户端探针与统计/收口都在时限内跑完（没有挂死的 await）");

    const auto &observations = g_observations;

    const auto rootResponses = splitResponses(observations.root.bytes);
    samples.check(!rootResponses.empty() && rootResponses.front().status == 200 && rootResponses.front().body == "Hello World",
                  "GET / 回 200 与固定正文（路由分发与响应组装）");
    samples.check(!rootResponses.empty() && !headerOf(rootResponses.front(), "Content-Type").empty() &&
                          !headerOf(rootResponses.front(), "Content-Length").empty() && !headerOf(rootResponses.front(), "Date").empty(),
                  "响应带上 Content-Length 与 Date");
    samples.check(observations.root.isClosedByPeer, "带 Connection: close 的请求答完后由服务端收口连接");
    samples.check(firstStatusOf(observations.json) == 200 && observations.json.bytes.find("application/json") != std::string::npos,
                  "GET /json 走同一条链并给出 JSON 媒体类型");

    // HEAD 本就没有正文，按 Content-Length 等正文会永远等不到，因此这里直接看原始字节。
    // 响应头由服务端统一以小写名写出（升级响应除外），比对也跟着用小写
    samples.check(observations.head.bytes.find("HTTP/1.1 200") != std::string::npos &&
                          observations.head.bytes.find("Hello World") == std::string::npos &&
                          observations.head.bytes.find("content-length:") != std::string::npos,
                  "HEAD 只有头部没有正文，但 Content-Length 照给（RFC 9110 §9.3.2）");

    samples.check(firstStatusOf(observations.missing) == 404, "未知路由回 404");
    samples.check(firstStatusOf(observations.wrongMethod) == 405, "方法不匹配回 405");

    const auto echoResponses = splitResponses(observations.echo.bytes);
    samples.check(!echoResponses.empty() && echoResponses.front().body == "hello echo?", "定长正文按 Content-Length 收齐并交给处理器");
    const auto chunkedResponses = splitResponses(observations.chunkedUpload.bytes);
    samples.check(!chunkedResponses.empty() && chunkedResponses.front().body == "hello world",
                  "分块请求体逐块拼回后再交出去（Transfer-Encoding: chunked）");
    if (observations.expectContinue.bytes.find("HTTP/1.1 100") == std::string::npos)
    {
        LOG_WARN("h1 没有给出 100 Continue interim 响应（h2 与 h3 有）：协议侧缺口，不钉成契约");
    }
    samples.check(firstStatusOf(observations.expectContinue) == 200 && observations.expectContinue.bytes.find("ping") != std::string::npos,
                  "Expect: 100-continue 不打断正文收取与应答");
    samples.check(firstStatusOf(observations.oversizeBody) == 413, "正文越过 maximumBodySize 回 413");

    const int longUriStatus = firstStatusOf(observations.longUri);
    LOG_INFO_FMT("越过 maximumUriLength 的请求目标实际回 {}", longUriStatus);
    samples.check(longUriStatus == 414 || longUriStatus == 431, "请求目标越界被拒（414 或 431 这一类）");
    samples.check(firstStatusOf(observations.manyHeaders) == 431, "头字段条数越上限回 431");
    samples.check(firstStatusOf(observations.malformed) == 400, "不是 HTTP 的请求行回 400 而不是直接断线");
    samples.check(firstStatusOf(observations.thrown) == 500, "处理器抛出被折成 500，连接不至于无声断开");

    const auto inspectResponses = splitResponses(observations.inspect.bytes);
    samples.check(!inspectResponses.empty() && inspectResponses.front().body == "/inspect#42", "请求目标被拆成路径与查询串");

    samples.check(observations.big.bytes.find("content-encoding") == std::string::npos, "不声明编码时响应保持原样");
    samples.check(observations.bigGzip.bytes.find("content-encoding: gzip") != std::string::npos &&
                          observations.bigGzip.bytes.find("\x1f\x8b") != std::string::npos &&
                          observations.bigGzip.bytes.size() < observations.big.bytes.size(),
                  "Accept-Encoding: gzip 命中压缩中间件，给出的确是 gzip 字节且更小");

    const auto echoedIdResponses = splitResponses(observations.echoedRequestId.bytes);
    const auto generatedResponses = splitResponses(observations.generatedRequestId.bytes);
    const std::string supplied  = echoedIdResponses.empty() ? std::string{} : headerOf(echoedIdResponses.front(), "X-Request-Id");
    const std::string generated = generatedResponses.empty() ? std::string{} : headerOf(generatedResponses.front(), "X-Request-Id");
    samples.check(supplied == "probe-fixed-id", "客户端给的 X-Request-Id 被原样回显");
    samples.check(!generated.empty() && generated != "probe-fixed-id", "没给 X-Request-Id 时服务端自己生成一个");

    samples.check(observations.sse.bytes.find("data: one") != std::string::npos && observations.sse.bytes.find("data: two") != std::string::npos &&
                          observations.sse.bytes.find("0\r\n\r\n") != std::string::npos,
                  "SSE 分块写出：两段事件与收尾零长块都到齐");

    samples.check(observations.webSocket.bytes.find("101 Switching Protocols") != std::string::npos &&
                          observations.webSocket.bytes.find("Sec-WebSocket-Accept:") != std::string::npos,
                  "WebSocket 握手回 101 并给出 Sec-WebSocket-Accept");
    samples.check(observations.webSocket.bytes.find("ping-frame") != std::string::npos, "掩码文本帧被解出并原样回显");
    samples.check(observations.webSocket.bytes.find('\x88') != std::string::npos, "客户端关闭帧得到对端的关闭帧（关闭握手）");
    samples.check(observations.extension.bytes.find("permessage-deflate") != std::string::npos, "握手带上 permessage-deflate 提议时被协商进响应头");

    const auto metricsResponses = splitResponses(observations.metricsFirst.bytes);
    samples.check(!metricsResponses.empty() && metricsResponses.front().status == 200 && !metricsResponses.front().body.empty(),
                  "/metrics 有内容且是 200");
    samples.check(observations.metricsSecond.bytes != observations.metricsFirst.bytes, "两次抓取的指标数值随请求推进而变化");
    samples.check(firstStatusOf(observations.health) == 200, "/healthz 回 200");

    const auto pipelinedResponses = splitResponses(observations.pipelined.bytes);
    // 断言之外先留一行现场：这条探针历史上偶发不过，光看「1 步没过」分不出是探针没等够、
    // 还是服务端在仍有未读入站数据时关连接把响应带成了 RST
    LOG_INFO_FMT("管线化探针：解出 {} 条响应，读循环停止原因 {}，被对端收口 {}",
                 pipelinedResponses.size(), observations.pipelined.stopReason, observations.pipelined.isClosedByPeer);
    // 收口方式可以是 FIN 也可以是 RST：客户端一次写下三条请求，服务端答完两条就决定关连接，
    // 而第三条此刻可能还留在自己的接收队列里（是否已进队列取决于分段到达的时机），带未读数据
    // 关闭套接字时协议栈就会发 RST。两条响应都已完整到手，这才是本步要钉的契约；
    // 「不再服务第三条」由 size()==2 表达，「确实是被服务端收口而不是探针没等够」由停止原因表达
    const bool isClosedByServer = observations.pipelined.stopReason == "eof" || observations.pipelined.stopReason == "reset";
    samples.check(pipelinedResponses.size() == 2 && pipelinedResponses.front().status == 200 && pipelinedResponses.back().status == 200 &&
                          isClosedByServer,
                  "单连接请求数上限 2：前两条按序答完，第三条起连接被收口");

    const auto idleResponses = splitResponses(observations.idleClosed.bytes);
    samples.check(!idleResponses.empty() && idleResponses.front().status == 200 && observations.idleClosed.isClosedByPeer,
                  "空闲超时到点后连接被服务端收口，此前的响应已完整送达");

    samples.check(firstStatusOf(observations.rateFirst) == 200 && firstStatusOf(observations.rateSecond) == 200 &&
                          firstStatusOf(observations.rateThird) == 429,
                  "令牌桶耗尽后第三条回 429（桶容量 3、每秒补 1）");

    samples.check(observations.isHttpClientOkay, "框架自带的 Net::HttpClient 也能把这条服务打穿");
    samples.check(observations.statsTotalRequests > 20 && observations.statsBadRequests >= 2, "服务器统计把成功与坏请求分别计入");

    samples.check(observations.perIpProbe.bytes.empty(), "单来源并发上限生效：额度被占满时新连接拿不到服务");
    samples.check(observations.maxConnectionProbe.bytes.empty(), "全局并发上限生效：超出上限的连接拿不到服务");
    samples.check(observations.afterStop.bytes.empty(), "stop() 之后新建连接不再被服务（优雅收口第一步）");

    samples.check(hasStaticDirectory, "临时静态目录建起来了（后面三条的前提）");
    const auto staticResponses = splitResponses(observations.staticPage.bytes);
    samples.check(!staticResponses.empty() && staticResponses.front().status == 200 &&
                          staticResponses.front().body == "<p>static payload from disk</p>" &&
                          headerOf(staticResponses.front(), "Content-Type") == "text/html",
                  "静态文件按磁盘内容返回，并给出 text/html");
    samples.check(firstStatusOf(observations.staticUppercase) == 200 && observations.staticUppercase.bytes.find("text/html") != std::string::npos,
                  "大写扩展名也落到 text/html（MIME 查表不区分大小写）");
    samples.check(firstStatusOf(observations.staticMissing) == 404, "静态目录里没有的文件回 404");
    samples.check(std::string_view{Net::FileSender::contentTypeForFile("archive.unknownext")} == "application/octet-stream" &&
                          std::string_view{Net::FileSender::contentTypeForFile("page.HTML")} == "text/html",
                  "FileSender 的 MIME 查表：未知扩展名按二进制流，大小写同表");
    samples.check(firstStatusOf(observations.dispatchedFirst) == 200 && firstStatusOf(observations.dispatchedSecond) == 200,
                  "接受分发链路：acceptor 收下的连接交给另一条循环上的 worker，两条请求都答完");

    std::error_code removeError;
    std::filesystem::remove_all(staticDirectory, removeError);

    context.stop();
    return Samples::finishSample("net_http_demo");
}

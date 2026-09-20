// Net HTTP/1.1 服务端自检：路由与中间件、解析上限、分块与 SSE、WebSocket、限额、观测端点与优雅收口
//
// 正向用例由第二条循环上的协程探针跑（每条请求都要求对端最终关连接，读循环因此总会结束）；
// 「期望拿不到响应」的负向用例改由主线程的阻塞套接字跑，并带 SO_RCVTIMEO 硬时限——
// 被拒的连接可能根本不被 accept，在协程里等它就等于把整轮自检钉死在那一次 await 上。
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/IoContext.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/InetAddress.h"
#include "Net/Http/Client/HttpClient.h"
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
#include "common/SampleSupport.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
     * @brief 自检期间攒下来的观测
     * @details 正向部分在探针循环上写，负向部分在主线程写；主线程等标记置起后才读前者
     */
    struct Observations
    {
        std::string root;           ///< GET /
        std::string big;            ///< 不带 Accept-Encoding 的大正文
        std::string bigGzip;        ///< 带 Accept-Encoding: gzip 的大正文
        std::string head;           ///< HEAD /
        std::string json;           ///< GET /json
        std::string missing;        ///< 未知路由
        std::string wrongMethod;    ///< POST /json
        std::string echo;           ///< 定长正文回显
        std::string chunkedUpload;  ///< 分块上传
        std::string expectContinue; ///< Expect: 100-continue
        std::string oversizeBody;   ///< 越过正文上限
        std::string longUri;        ///< 越过请求目标上限
        std::string manyHeaders;    ///< 越过头字段条数上限
        std::string malformed;      ///< 不是 HTTP 的请求行
        std::string thrown;         ///< 处理器抛异常
        std::string inspect;        ///< 路径与查询串
        std::string requestIds;     ///< 带与不带 X-Request-Id 的两次响应，用 \0 分隔
        std::string sse;            ///< 分块流式响应
        std::string webSocket;      ///< 握手 + 回显 + 关闭
        std::string extension;      ///< 带 permessage-deflate 提议的握手
        std::string metricsFirst;   ///< 第一次 /metrics
        std::string metricsSecond;  ///< 第二次 /metrics
        std::string health;         ///< /healthz
        std::string pipelined;      ///< 一条连接上连发三条（限额服务器）
        std::string idleClosed;     ///< 空闲超时收口的连接（受护服务器）
        std::string rateFirst;      ///< 令牌桶第一条
        std::string rateSecond;     ///< 令牌桶第二条
        std::string rateThird;      ///< 令牌桶第三条
        std::string perIpProbe;     ///< 单来源额度被占满后的那一条
        std::string maxConnectionProbe;///< 超过全局并发上限的那一条
        std::string afterStop;      ///< stop() 之后再连一条
        std::size_t statsTotalRequests{0};   ///< 服务器统计：累计请求数
        std::size_t statsBadRequests{0};     ///< 服务器统计：坏请求数
        bool        isHttpClientOkay{false}; ///< 走 Net::HttpClient 的一条真请求
    };

    Observations g_observations;

    /// 正向探针是否跑完：观测值写完由这枚标记 release 出去
    std::atomic<bool> g_isProbeFinished{false};

    /// 统计快照与 stop()/drain() 是否已在服务器的循环上做完
    std::atomic<bool> g_isChoreFinished{false};

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
     * @return std::optional<ResponseView> 头都凑不齐时返回空；正文没到齐时给「已收到的部分」
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
                // 正文没到齐：状态与头仍算解析出来了，正文给已收到的部分（HEAD 就落在这条路上）
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
    std::vector<ResponseView> splitResponses(std::string_view stream)
    {
        std::vector<ResponseView> responses;
        while (!stream.empty())
        {
            const auto parsed = parseResponse(stream);
            if (!parsed.has_value() || parsed->consumedLength == 0)
            {
                break;
            }
            responses.push_back(*parsed);
            stream = stream.substr(std::min(parsed->consumedLength, stream.size()));
        }
        return responses;
    }

    /// 第一条响应的状态码；一条都解析不出来时返回 0
    int firstStatusOf(const std::string &stream)
    {
        const auto responses = splitResponses(stream);
        return responses.empty() ? 0 : responses.front().status;
    }

    /**
     * @brief 组装一条请求
     * @param method 方法
     * @param target 请求目标
     * @param extraHeaders 额外头字段（Host 与 Connection 由 plainHeaders 一并给出）
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
     * @param isKeepAlive 是否要求复用连接；false 时带 Connection: close，读循环靠它自然收尾
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

    /**
     * @brief 连到端口、按序写出若干段字节，然后读到对端收口
     * @details 读循环靠「对端总会关连接」自然结束：正向请求一律带 Connection: close，
     *          或落在明确会收口的限额上（单连接请求数上限、空闲超时）。
     * @param loop 所属循环
     * @param port 目标端口
     * @param payloads 要按序写出的字节段
     * @return Core::Task<std::string> 收到的全部字节
     */
    Core::Task<std::string> exchange(Core::EventLoop &loop, const std::uint16_t port, const std::vector<std::string> &payloads)
    {
        const auto  address = Core::InetAddress::resolve("127.0.0.1", port);
        std::string received;
        if (!address.has_value())
        {
            co_return received;
        }

        auto socket = Core::AsyncSocket::create(loop, AF_INET, SOCK_STREAM);
        static_cast<void>(co_await socket.asyncConnect(*address));
        for (const std::string &payload: payloads)
        {
            if (co_await socket.asyncSend(payload.data(), payload.size()) <= 0)
            {
                co_return received;
            }
        }

        std::uint8_t buffer[8192]{};
        for (int round = 0; round < 200; ++round)
        {
            const ssize_t byteCount = co_await socket.asyncReceive(buffer, sizeof(buffer));
            if (byteCount <= 0)
            {
                break;
            }
            received.append(reinterpret_cast<const char *>(buffer), static_cast<std::size_t>(byteCount));
        }
        co_return received;
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
     * @brief 主服务器上的正向探针：路由、解析上限、压缩、SSE、WebSocket 与观测端点
     * @param loop 探针自己的循环（与被测服务不抢同一条线程）
     * @param mainPort 主服务器端口
     * @return Core::Task<> 跑完即返回
     */
    Core::Task<> runMainServerProbes(Core::EventLoop &loop, const std::uint16_t mainPort)
    {
        g_observations.root        = co_await exchange(loop, mainPort, {plainRequest("GET", "/")});
        g_observations.big         = co_await exchange(loop, mainPort, {plainRequest("GET", "/big")});
        g_observations.bigGzip     = co_await exchange(loop, mainPort, {plainRequest("GET", "/big", false, "Accept-Encoding: gzip")});
        g_observations.head        = co_await exchange(loop, mainPort, {plainRequest("HEAD", "/")});
        g_observations.json        = co_await exchange(loop, mainPort, {plainRequest("GET", "/json")});
        g_observations.missing     = co_await exchange(loop, mainPort, {plainRequest("GET", "/nope")});
        std::vector<std::string> payloads_wrongMethod {plainRequest("POST", "/json", false, "Content-Length: 0")};
        g_observations.wrongMethod = co_await exchange(loop, mainPort, payloads_wrongMethod);
        std::vector<std::string> payloads_echo {plainRequest("POST", "/echo", false, "Content-Length: 11", "hello echo?")};
        g_observations.echo = co_await exchange(loop, mainPort, payloads_echo);
        g_observations.chunkedUpload =
                co_await exchange(loop, mainPort,
                                  {plainRequest("POST", "/echo", false, "Transfer-Encoding: chunked", "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n")});
        g_observations.expectContinue =
                co_await exchange(loop, mainPort, {makeRequest("POST", "/echo",
                                                               {"Host: 127.0.0.1", "Connection: close", "Expect: 100-continue",
                                                                "Content-Length: 4"},
                                                               "ping")});
        g_observations.oversizeBody =
                co_await exchange(loop, mainPort, {plainRequest("POST", "/echo", false, "Content-Length: 4096", "short")});
        std::vector<std::string> payloads_longUri {plainRequest("GET", "/" + std::string(3000, 'a'))};
        g_observations.longUri = co_await exchange(loop, mainPort, payloads_longUri);

        std::string manyHeadersRequest{"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"};
        for (int index = 0; index < 80; ++index)
        {
            manyHeadersRequest += "X-Filler-" + std::to_string(index) + ": v\r\n";
        }
        manyHeadersRequest += "\r\n";
        std::vector<std::string> payloads_manyHeaders {manyHeadersRequest};
        g_observations.manyHeaders = co_await exchange(loop, mainPort, payloads_manyHeaders);

        std::vector<std::string> payloads_malformed {"THIS IS NOT HTTP\r\n\r\n"};
        g_observations.malformed = co_await exchange(loop, mainPort, payloads_malformed);
        g_observations.thrown    = co_await exchange(loop, mainPort, {plainRequest("GET", "/boom")});
        g_observations.inspect   = co_await exchange(loop, mainPort, {plainRequest("GET", "/inspect?needle=42")});

        std::vector<std::string> payloads_requestIds {plainRequest("GET", "/", false, "X-Request-Id: probe-fixed-id")};
        g_observations.requestIds = co_await exchange(loop, mainPort, payloads_requestIds);
        g_observations.requestIds += '\0';
        g_observations.requestIds += co_await exchange(loop, mainPort, {plainRequest("GET", "/")});

        g_observations.sse       = co_await exchange(loop, mainPort, {plainRequest("GET", "/sse")});
        std::vector<std::string> payloads_webSocket {webSocketHandshake(), makeClientFrame(0x1, "ping-frame"), makeClientFrame(0x8, "")};
        g_observations.webSocket = co_await exchange(loop, mainPort, payloads_webSocket);
        std::vector<std::string> payloads_extension {webSocketHandshake("Sec-WebSocket-Extensions: permessage-deflate"), makeClientFrame(0x8, "")};
        g_observations.extension = co_await exchange(loop, mainPort, payloads_extension);
        g_observations.metricsFirst  = co_await exchange(loop, mainPort, {plainRequest("GET", "/metrics")});
        g_observations.health        = co_await exchange(loop, mainPort, {plainRequest("GET", "/healthz")});
        std::vector<std::string> payloads_metricsSecond {plainRequest("GET", "/metrics")};
        g_observations.metricsSecond = co_await exchange(loop, mainPort, payloads_metricsSecond);
    }

    /**
     * @brief 限额与受护服务器上的正向探针：单连接请求数、令牌桶、空闲超时
     * @param loop 探针自己的循环
     * @param strictPort 限额服务器端口（单连接请求数）
     * @param guardedPort 受护服务器端口（令牌桶、空闲超时、全局并发上限）
     * @return Core::Task<> 跑完即返回
     */
    Core::Task<> runLimitProbes(Core::EventLoop &loop, const std::uint16_t strictPort, const std::uint16_t guardedPort)
    {
        // 限额服务器：一条连接上按序管线化三条，第三条起连接该被收口（单连接请求数上限 2）
        std::vector<std::string> payloads_pipelined {plainRequest("GET", "/", true), plainRequest("GET", "/", true),
                                                      plainRequest("GET", "/", true)};
        g_observations.pipelined = co_await exchange(loop, strictPort, payloads_pipelined);

        // 受护服务器：空闲超时那条先跑（它会吃掉桶里一枚令牌），随后三条限流探针正好在第三条耗尽
        std::vector<std::string> payloads_idleClosed {plainRequest("GET", "/", true)};
        g_observations.idleClosed = co_await exchange(loop, guardedPort, payloads_idleClosed);
        g_observations.rateFirst  = co_await exchange(loop, guardedPort, {plainRequest("GET", "/")});
        std::vector<std::string> payloads_rateSecond {plainRequest("GET", "/")};
        g_observations.rateSecond = co_await exchange(loop, guardedPort, payloads_rateSecond);
        g_observations.rateThird  = co_await exchange(loop, guardedPort, {plainRequest("GET", "/")});
    }

    /**
     * @brief 正向探针总入口：分两段跑完，最后再走一遍框架自带的客户端
     * @param loop 探针自己的循环
     * @param mainPort 主服务器端口
     * @param strictPort 限额服务器端口（单连接请求数）
     * @param guardedPort 受护服务器端口（令牌桶、空闲超时、全局并发上限）
     * @return Core::Task<> 跑完即返回
     */
    Core::Task<> runProbes(Core::EventLoop &loop, const std::uint16_t mainPort, const std::uint16_t strictPort, const std::uint16_t guardedPort)
    {
        co_await runMainServerProbes(loop, mainPort);
        co_await runLimitProbes(loop, strictPort, guardedPort);

        const auto response = co_await Net::HttpClient::get(loop, "http://127.0.0.1:" + std::to_string(mainPort) + "/");
        g_observations.isHttpClientOkay = response != nullptr && response->statusCode == 200 && response->body == "Hello World";

        g_isProbeFinished.store(true, std::memory_order_release);
        co_return;
    }

#if ASYN_PLATFORM_WIN32
    using socket_handle_t = SOCKET;
    constexpr socket_handle_t kInvalidSocketHandle = INVALID_SOCKET;

    /// 让后续 recv 最多等这些毫秒：负向用例必须有硬时限，不能靠对端收口
    void applyReceiveTimeout(const socket_handle_t socketHandle, const int millisecondCount)
    {
        ::setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&millisecondCount), sizeof(millisecondCount));
    }
#else
    using socket_handle_t = int;
    constexpr socket_handle_t kInvalidSocketHandle = -1;

    /// 让后续 recv 最多等这些毫秒：负向用例必须有硬时限，不能靠对端收口
    void applyReceiveTimeout(const socket_handle_t socketHandle, const int millisecondCount)
    {
        const timeval timeout{millisecondCount / 1000, (millisecondCount % 1000) * 1000};
        ::setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }
#endif

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
            ::closesocket(socketHandle);
            return kInvalidSocketHandle;
        }
        return socketHandle;
    }

    /**
     * @brief 主线程上的阻塞请求：写完请求，最多等 receiveTimeout 收响应
     * @details 负向用例（期望「没有响应」）必须自带时限：被拒的连接可能压根没被 accept，
     *          挂在协程里等它就等于把整轮自检钉死在那一次 await 上。
     * @param port 目标端口
     * @param request 完整请求报文
     * @param receiveTimeoutMilliseconds recv 的最长等待
     * @return std::string 收到的字节；空表示时限内没有任何响应
     */
    std::string blockingRequest(const std::uint16_t port, const std::string &request, const int receiveTimeoutMilliseconds)
    {
        const socket_handle_t socketHandle = connectToLocalhost(port);
        if (socketHandle == kInvalidSocketHandle)
        {
            return {};
        }
        applyReceiveTimeout(socketHandle, receiveTimeoutMilliseconds);
        ::send(socketHandle, request.data(), static_cast<int>(request.size()), 0);

        std::string received;
        char        buffer[2048]{};
        for (int round = 0; round < 8; ++round)
        {
            const int byteCount = static_cast<int>(::recv(socketHandle, buffer, sizeof(buffer), 0));
            if (byteCount <= 0)
            {
                break;
            }
            received.append(buffer, static_cast<std::size_t>(byteCount));
        }
        ::closesocket(socketHandle);
        return received;
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
            ::closesocket(socketHandle);
        }
        held.clear();
    }
} // namespace

int main(const int argc, char **argv)
{
    Samples::setupConsoleLogging();
    LOG_INFO("=== Net HTTP/1.1 服务端示例开始 ===");

    const std::uint16_t mainPort    = Samples::readPortArgument(argc, argv, 0);
    const std::uint16_t strictPort  = static_cast<std::uint16_t>(mainPort + 1);
    const std::uint16_t guardedPort = static_cast<std::uint16_t>(mainPort + 2);
    auto &samples = Samples::checklist();

    Core::IoContext context(2);
    auto &          pool = context.threadPool();
    // 服务器全在第一条循环上，探针走第二条：客户端与被测服务不抢同一条线程，
    // 否则探针挂在「等响应」上时，分不清是服务端没处理还是这条循环没空去 accept
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
    strictLimits.idleTimeout                  = std::chrono::seconds{5};
    Net::HttpServerLimits guardedLimits;
    guardedLimits.idleTimeout = std::chrono::milliseconds{300};

    auto mainServer    = buildServer(mainPort, defaultLimits, true);
    auto strictServer  = buildServer(strictPort, strictLimits, false);
    auto guardedServer = buildServer(guardedPort, guardedLimits, false);

    // 单来源上限 1：额度被占满时，再来一条连接不该拿到服务
    strictServer->setPerIpConnectionLimiter(std::make_shared<Net::PerIpConnectionLimiter>(1));
    // 全局并发上限 2：挂住两条之后第三条不该拿到服务
    guardedServer->setMaxConnections(2);
    // 令牌桶：容量 3、每秒补 1 个——空闲超时那条先占一枚，随后连发三条在第三条耗尽
    guardedServer->router().addMiddleware(
            Net::tokenBucketRateLimiterMiddleware(std::make_shared<Net::TokenBucket>(1.0, 3.0)));

    // 协程帧必须活到它跑完：本向量一路持有到 main 结束
    std::vector<Core::Task<>> tasks;
    tasks.push_back(mainServer->start());
    tasks.push_back(strictServer->start());
    tasks.push_back(guardedServer->start());
    for (auto &task: tasks)
    {
        serverLoop.scheduler().schedule(task.handle());
    }
    pool.start();

    // 监听器要到循环线程跑起 accept 协程才真正 listen()：先用「只连不发」的有界等待确认三条端口
    // 都在收连接，再放协程探针出去。不发请求是为了不动令牌桶——限流那条用例的桶要按原样空着开始
    const auto awaitListening = [](const std::uint16_t port)
    {
        return Samples::waitUntil(
                [port]
                {
                    const socket_handle_t probeHandle = connectToLocalhost(port);
                    if (probeHandle == kInvalidSocketHandle)
                    {
                        return false;
                    }
                    ::closesocket(probeHandle);
                    return true;
                },
                std::chrono::seconds{5}, std::chrono::milliseconds{20});
    };
    const bool isListeningReady = awaitListening(mainPort) && awaitListening(strictPort) && awaitListening(guardedPort);

    tasks.push_back(runProbes(probeLoop, mainPort, strictPort, guardedPort));
    probeLoop.scheduler().scheduleRemote(tasks.back().handle());
    const bool isProbeDone = Samples::waitUntil([]
                                                {
                                                    return g_isProbeFinished.load(std::memory_order_acquire);
                                                },
                                                std::chrono::seconds{30}, std::chrono::milliseconds{20});

    tasks.push_back(collectStatsAndStop(*mainServer));
    serverLoop.scheduler().scheduleRemote(tasks.back().handle());
    const bool isChoreDone = Samples::waitUntil([]
                                                {
                                                    return g_isChoreFinished.load(std::memory_order_acquire);
                                                },
                                                std::chrono::seconds{20}, std::chrono::milliseconds{20});

    samples.check(isListeningReady && isProbeDone && isChoreDone, "三条监听器都起了，两轮探针也在时限内跑完（没有挂死的 await）");

    // —— 负向用例：主线程阻塞跑，每条都自带硬时限 ——
    {
        std::vector<socket_handle_t> held = openHeldConnections(strictPort, 1);
        g_observations.perIpProbe         = blockingRequest(strictPort, plainRequest("GET", "/"), 500);
        closeHeldConnections(held);
    }
    {
        std::vector<socket_handle_t> held   = openHeldConnections(guardedPort, 2);
        g_observations.maxConnectionProbe   = blockingRequest(guardedPort, plainRequest("GET", "/"), 500);
        closeHeldConnections(held);
    }
    // 优雅收口之后再连一条：应当拿不到任何响应
    g_observations.afterStop = blockingRequest(mainPort, plainRequest("GET", "/"), 500);

    const auto &observations = g_observations;

    const auto rootResponses = splitResponses(observations.root);
    samples.check(!rootResponses.empty() && rootResponses.front().status == 200 && rootResponses.front().body == "Hello World",
                  "GET / 回 200 与固定正文（路由分发与响应组装）");
    samples.check(!rootResponses.empty() && !headerOf(rootResponses.front(), "Content-Type").empty() &&
                          !headerOf(rootResponses.front(), "Content-Length").empty() && !headerOf(rootResponses.front(), "Date").empty(),
                  "响应带上 Content-Length 与 Date");
    samples.check(firstStatusOf(observations.json) == 200 && observations.json.find("application/json") != std::string::npos,
                  "GET /json 走同一条链并给出 JSON 媒体类型");

    // HEAD 本就没有正文，按 Content-Length 等正文会永远等不到，因此这里直接看原始字节。
    // 响应头由服务端统一以小写名写出，比对也跟着用小写（升级响应是另一条路径，保持原名）
    samples.check(observations.head.find("HTTP/1.1 200") != std::string::npos && observations.head.find("Hello World") == std::string::npos &&
                          observations.head.find("content-length:") != std::string::npos,
                  "HEAD 只有头部没有正文，但 Content-Length 照给（RFC 9110 §9.3.2）");

    samples.check(firstStatusOf(observations.missing) == 404, "未知路由回 404");
    samples.check(firstStatusOf(observations.wrongMethod) == 405, "方法不匹配回 405");

    const auto echoResponses = splitResponses(observations.echo);
    samples.check(!echoResponses.empty() && echoResponses.front().body == "hello echo?", "定长正文按 Content-Length 收齐并交给处理器");
    const auto chunkedResponses = splitResponses(observations.chunkedUpload);
    samples.check(!chunkedResponses.empty() && chunkedResponses.front().body == "hello world",
                  "分块请求体逐块拼回后再交出去（Transfer-Encoding: chunked）");
    if (observations.expectContinue.find("HTTP/1.1 100") == std::string::npos)
    {
        LOG_WARN("h1 没有给出 100 Continue interim 响应（h2 与 h3 有）：协议侧缺口，不钉成契约");
    }
    samples.check(firstStatusOf(observations.expectContinue) == 200 && observations.expectContinue.find("ping") != std::string::npos,
                  "Expect: 100-continue 不打断正文收取与应答");
    samples.check(firstStatusOf(observations.oversizeBody) == 413, "正文越过 maximumBodySize 回 413");

    const int longUriStatus = firstStatusOf(observations.longUri);
    LOG_INFO_FMT("越过 maximumUriLength 的请求目标实际回 {}", longUriStatus);
    samples.check(longUriStatus == 414 || longUriStatus == 431, "请求目标越界被拒（414 或 431 这一类）");
    samples.check(firstStatusOf(observations.manyHeaders) == 431, "头字段条数越上限回 431");
    samples.check(firstStatusOf(observations.malformed) == 400, "不是 HTTP 的请求行回 400 而不是直接断线");
    samples.check(firstStatusOf(observations.thrown) == 500, "处理器抛出被折成 500，连接不至于无声断开");

    const auto inspectResponses = splitResponses(observations.inspect);
    samples.check(!inspectResponses.empty() && inspectResponses.front().body == "/inspect#42", "请求目标被拆成路径与查询串");

    samples.check(observations.big.find("content-encoding") == std::string::npos, "不声明编码时响应保持原样");
    samples.check(observations.bigGzip.find("content-encoding: gzip") != std::string::npos &&
                          observations.bigGzip.find("\x1f\x8b") != std::string::npos && observations.bigGzip.size() < observations.big.size(),
                  "Accept-Encoding: gzip 命中压缩中间件，给出的确是 gzip 字节且更小");

    const std::size_t idSplit   = observations.requestIds.find('\0');
    const auto        echoedId  = splitResponses(observations.requestIds.substr(0, idSplit));
    const auto        generatedId = splitResponses(observations.requestIds.substr(idSplit == std::string::npos ? 0 : idSplit + 1));
    const std::string supplied  = echoedId.empty() ? std::string{} : headerOf(echoedId.front(), "X-Request-Id");
    const std::string generated = generatedId.empty() ? std::string{} : headerOf(generatedId.front(), "X-Request-Id");
    samples.check(supplied == "probe-fixed-id", "客户端给的 X-Request-Id 被原样回显");
    samples.check(!generated.empty() && generated != "probe-fixed-id", "没给 X-Request-Id 时服务端自己生成一个");

    samples.check(observations.sse.find("data: one") != std::string::npos && observations.sse.find("data: two") != std::string::npos &&
                          observations.sse.find("0\r\n\r\n") != std::string::npos,
                  "SSE 分块写出：两段事件与收尾零长块都到齐");

    samples.check(observations.webSocket.find("101 Switching Protocols") != std::string::npos &&
                          observations.webSocket.find("Sec-WebSocket-Accept:") != std::string::npos,
                  "WebSocket 握手回 101 并给出 Sec-WebSocket-Accept");
    samples.check(observations.webSocket.find("ping-frame") != std::string::npos, "掩码文本帧被解出并原样回显");
    samples.check(observations.webSocket.find('\x88') != std::string::npos, "客户端关闭帧得到对端的关闭帧（关闭握手）");
    samples.check(observations.extension.find("permessage-deflate") != std::string::npos, "握手带上 permessage-deflate 提议时被协商进响应头");

    const auto metricsResponses = splitResponses(observations.metricsFirst);
    samples.check(!metricsResponses.empty() && metricsResponses.front().status == 200 && !metricsResponses.front().body.empty(),
                  "/metrics 有内容且是 200");
    samples.check(observations.metricsSecond != observations.metricsFirst, "两次抓取的指标数值随请求推进而变化");
    samples.check(firstStatusOf(observations.health) == 200, "/healthz 回 200");

    const auto pipelinedResponses = splitResponses(observations.pipelined);
    samples.check(pipelinedResponses.size() == 2 && pipelinedResponses.front().status == 200 && pipelinedResponses.back().status == 200,
                  "单连接请求数上限 2：前两条按序答完，第三条起连接被收口");

    const auto idleResponses = splitResponses(observations.idleClosed);
    samples.check(!idleResponses.empty() && idleResponses.front().status == 200,
                  "空闲超时到点后连接被收口，此前的响应已完整送达");

    samples.check(firstStatusOf(observations.rateFirst) == 200 && firstStatusOf(observations.rateSecond) == 200 &&
                          firstStatusOf(observations.rateThird) == 429,
                  "令牌桶耗尽后第三条回 429（桶容量 3、每秒补 1）");

    samples.check(observations.isHttpClientOkay, "框架自带的 Net::HttpClient 也能把这条服务打穿");
    samples.check(observations.statsTotalRequests > 20 && observations.statsBadRequests >= 2, "服务器统计把成功与坏请求分别计入");

    samples.check(observations.perIpProbe.empty(), "单来源并发上限生效：额度被占满时新连接拿不到服务");
    samples.check(observations.maxConnectionProbe.empty(), "全局并发上限生效：超出上限的连接拿不到服务");
    samples.check(observations.afterStop.empty(), "stop() 之后新建连接不再被服务（优雅收口第一步）");

    context.stop();
    return Samples::finishSample("net_http_demo");
}

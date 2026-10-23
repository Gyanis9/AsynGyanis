#include "Net/Http/Client/HttpClient.h"
#include "Net/Http/Client/HttpClientUrl.h"
#include "Net/Http/Client/HttpClientSupport.h"
#include "Base/Exception/Exception.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/DeadlineGuard.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/AsyncResolver.h"
#include "Core/Socket/AsyncSocket.h"
#include "Core/Socket/ConnectionRace.h"
#include "Core/Socket/InetAddress.h"
#include "Core/Tls/TlsContext.h"
#include "Core/Tls/TlsPolicy.h"
#include "Core/Tls/TlsSocket.h"
#include "Net/Http/Client/HttpContentCoding.h"
#include "Net/Http/Message/HttpDate.h"
#include "Net/Http/Message/HttpHeaderRules.h"
#include "Net/Http/Client/HttpCookieJar.h"
#include "Net/Http/Client/HttpOutboundConnectionPool.h"
#include "Net/Http/Client/HttpProxyPumpedTls.h"
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
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

namespace AsynGyanis::Net
{

    std::optional<std::chrono::seconds> HttpClientResponse::retryAfter(const std::chrono::system_clock::time_point now) const
    {
        // 头名大小写不敏感这件事交给 headerValue 一处做（它按 ASCII 折叠两侧），这里不再自己比一遍
        const std::optional<std::string_view> header = headerValue("retry-after");
        if (!header.has_value())
        {
            return std::nullopt;
        }
        return parseRetryAfter(*header, now);
    }


    namespace
    {
        using HttpClientDetail::establishPlainConnection;
        using HttpClientDetail::establishSecureConnection;
        using HttpClientDetail::remainingBudget;
        using HttpClientUrl::appendHostHeader;
        using HttpClientUrl::authorityText;

        /**
         * @brief 在一组「名: 值」字段里按名字取第一条，名字大小写不敏感
         * @details `headerValue` 与 `trailerValue` 共用一处：两段的折叠规则必须是同一份，各写一遍就会漂移
         * @param fields 字段表（响应头部或尾部字段）
         * @param name 待查的字段名，大小写任意
         * @return std::optional<std::string_view> 第一条同名字段的值；没有则为空
         */
        std::optional<std::string_view> firstFieldValueNamed(const std::vector<HttpClientHeaderField> &fields, const std::string_view name)
        {
            // 比对走 Net::equalsIgnoringCase（只折 A-Z/a-z）：头部名大小写不敏感是 RFC 9110 §5.1 的规定，
            // 而字段按对端给什么留什么，因此这里必须折叠两侧。不用 std::tolower：它按 locale 折，
            // 同一份应答在不同机器上会比出不同结果——那正是本仓各处折叠都避开它的原因
            for (const HttpClientHeaderField &field: fields)
            {
                if (equalsIgnoringCase(field.first, name))
                {
                    return std::string_view(field.second);
                }
            }
            return std::nullopt;
        }
    } // namespace

    std::optional<std::string_view> HttpClientResponse::headerValue(const std::string_view name) const
    {
        return firstFieldValueNamed(headers, name);
    }

    std::optional<std::string_view> HttpClientResponse::trailerValue(const std::string_view name) const
    {
        return firstFieldValueNamed(trailers, name);
    }

    namespace
    {
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
            /// 对端有没有保证这条请求没被处理过（RFC 9113 §8.7 的两种机制：REFUSED_STREAM 与
            /// GOAWAY 的 last-stream-id）。为真时连非幂等方法也可以重来一次
            bool isGuaranteedUnprocessed{false};
        };

        /**
         * @brief 这个方法原文按规范是不是幂等的（同一条请求做两遍与做一遍，对服务器状态的影响相同）
         * @details 用来决定「请求已整个写上通路、却没等到回音」时要不要换一条连接重来一次：
         *          RFC 9112 §9.3.2 允许在连接故障之后自动重试幂等方法，幂等集合由 RFC 9110 §9.2.2
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
         * @brief 按请求里给的链路上下文写出 traceparent 头部
         * @details 线上形态只在这一处生成：调用方交来上下文，本函数负责那 55 字节的写法，
         *          于是「自己拼头部」的第二份实现不会出现（拼错的 traceparent 在对端是整条链路断掉）。
         * @param headers 要追加头部的容器（就地改）
         * @param context 本端这一跳的上下文
         * @throws Base::InvalidArgumentException headers 里已经手写过一条 traceparent：
         *         一处出站请求只能有一个上级，替调用方挑一个就是把两份意图混成一条头部
         */
        void appendTraceparentHeader(std::vector<HttpClientHeaderField> &headers, const TraceIdentifiers &context)
        {
            const bool callerWroteIt = std::ranges::any_of(headers, [](const HttpClientHeaderField &field) { return equalsIgnoringCase(field.first, kTraceparentHeaderName); });
            if (callerWroteIt)
            {
                throw Base::InvalidArgumentException("HttpClient：请求的 traceContext 与 headers 里手写的 traceparent 同时给了："
                                                     "一处出站请求只能有一个上级上下文。请去掉其中一处再试");
            }
            headers.emplace_back(kTraceparentHeaderName, Traceparent::value(context));
        }

        /**
         * @brief 按请求里给的上下文写出 traceparent 与可选的 tracestate
         * @param headers 要追加头部的容器（就地改）
         * @param context 本端这一跳的上下文
         * @param state 可选的 tracestate；空条目表不发送
         * @throws Base::InvalidArgumentException headers 里已手写过 traceparent 或 tracestate
         */
        void appendTraceHeaders(std::vector<HttpClientHeaderField> &headers, const TraceIdentifiers &context, const std::optional<TraceState> &state)
        {
            appendTraceparentHeader(headers, context);
            if (!state.has_value())
            {
                return;
            }
            const bool callerWroteIt = std::ranges::any_of(headers, [](const HttpClientHeaderField &field) { return equalsIgnoringCase(field.first, kTracestateHeaderName); });
            if (callerWroteIt)
            {
                throw Base::InvalidArgumentException("HttpClient：请求的 traceState 与 headers 里手写的 tracestate 同时给了："
                                                     "一处出站请求只能有一份 tracestate。请去掉其中一处再试");
            }
            std::string stateText;
            state->renderInto(stateText);
            if (!stateText.empty())
            {
                headers.emplace_back(kTracestateHeaderName, std::move(stateText));
            }
        }

        /**
         * @brief 从整体时限里扣掉已经花掉的那一段
         * @details 契约是「握手、发送、收完响应三段之和」：分两段各给一次完整时限，等于把契约放宽
         *          一倍。返回空表示预算已经用完，调用方据此直接判失败。
         * @param startedAt 本次请求的开始时刻
         * @param requestTimeout 整体时限
         * @return std::optional<std::chrono::milliseconds> 还剩多少；已经用尽则为空
         */
        /**
         * @brief 把请求的绝对截止时刻折进相对时限（取更严者）
         * @param request 请求（读 deadline）
         * @param baseTimeout 相对时限的剩余部分；<= 0 表示调用方本来就没给相对时限
         * @param nowMoment 折的时刻
         * @param failureReason 截止时刻已过时写入原因
         * @return std::optional<std::chrono::milliseconds> 有效时限；截止时刻已过时为空
         * @note 剩余预算不足 1 毫秒按 1 毫秒：0 在 requestTimeout 一侧是「立刻失败」，
         *       不把「快到期」翻成「完全不发」
         */
        std::optional<std::chrono::milliseconds> foldDeadlineIntoTimeout(const HttpClientRequest &request, const std::chrono::milliseconds baseTimeout,
                                                                         const std::chrono::steady_clock::time_point nowMoment, std::string &failureReason)
        {
            if (!request.deadline.has_value())
            {
                return baseTimeout;
            }
            if (*request.deadline <= nowMoment)
            {
                failureReason = "本次请求已到截止时刻：不再建连接、不再继续等待（调用方给的剩余预算已用尽）";
                return std::nullopt;
            }
            const auto remaining     = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(*request.deadline - nowMoment), std::chrono::milliseconds{1});
            const auto effectiveTime = baseTimeout > std::chrono::milliseconds::zero() ? std::min(baseTimeout, remaining) : remaining;
            return effectiveTime;
        }

        /**
         * @brief 发送前补上链路上下文；不需要改头时原样交出请求
         * @details Cookie 不在这里加：重定向链的每一跳要按当跳 URL 取罐子，加在这一层只能按首跳 URL 取一次
         * @param request 调用方的请求
         * @param storage 需要改头时的副本槽（调用方持有，活到本次发送结束）
         * @return const HttpClientRequest & 实际要发的请求
         */
        const HttpClientRequest &prepareOutboundRequest(const HttpClientRequest &request, std::optional<HttpClientRequest> &storage)
        {
            const HttpClientRequest *effectiveRequest = &request;

            // 链路上下文按「要改头部才拷」的规矩处理：不填时开销与行为都和原来一样
            if (request.traceContext.has_value())
            {
                if (!storage.has_value())
                {
                    storage          = request;
                    effectiveRequest = &storage.value();
                }
                appendTraceHeaders(storage->headers, *request.traceContext, request.traceState);
            }
            return *effectiveRequest;
        }

        /**
         * @brief 取一个端点的在途名额；Fail 立即失败，Wait 在时限/截止时刻内轮询
         * @param pool 出向连接池
         * @param loop 等待用的循环
         * @param key 端点键
         * @param request 读 deadline
         * @param method 请求方法（失败原因里点名）
         * @param url 目标地址（失败原因里点名）
         * @param requestTimeout 调用方给的相对时限
         * @param startedAt 本次请求开始时刻
         * @param failureReason 失败原因出参
         * @return true 已占到名额（调用方负责 releaseInFlight）
         */
        Core::Task<bool> acquireOutboundInFlightSlot(HttpOutboundConnectionPool &pool, Core::EventLoop &loop, const HttpOutboundEndpointKey &key, const HttpClientRequest &request,
                                                     const std::string_view method, const std::string_view url, const std::chrono::milliseconds requestTimeout,
                                                     const std::chrono::steady_clock::time_point startedAt, std::string &failureReason)
        {
            const bool failFast = pool.config().inFlightOverflowPolicy == InFlightOverflowPolicy::Fail;
            if (pool.tryAcquireInFlight(key, failFast))
            {
                co_return true;
            }
            if (failFast)
            {
                failureReason = std::format("{} {} 被出向并发闸挡下：端点在途已达上限 {}（处置为立即失败）", method, url, pool.config().maximumInFlightPerEndpoint);
                co_return false;
            }

            Core::Timer gateTimer(loop);
            for (;;)
            {
                const auto nowMoment = std::chrono::steady_clock::now();
                if (request.cancelToken.stop_requested())
                {
                    failureReason = "请求已取消：等待出向并发闸名额时调用方请求停止";
                    break;
                }
                if (request.deadline.has_value() && nowMoment >= *request.deadline)
                {
                    failureReason = "出向并发闸：等待端点名额时到达请求的截止时刻";
                    break;
                }
                if (requestTimeout > std::chrono::milliseconds::zero() && nowMoment - startedAt >= requestTimeout)
                {
                    failureReason = "出向并发闸：等待端点名额耗尽了请求时限";
                    break;
                }
                if (requestTimeout <= std::chrono::milliseconds::zero() && !request.deadline.has_value())
                {
                    failureReason = "出向并发闸：请求没有可用的等待预算（时限为 0 且未给截止时刻）";
                    break;
                }
                co_await gateTimer.waitFor(std::chrono::milliseconds{5});
                if (pool.tryAcquireInFlight(key, false))
                {
                    co_return true;
                }
            }
            // 轮询的中间失败不计数，最终判失败时记一次
            pool.noteGateRejection();
            co_return false;
        }

        /**
         * @brief 闸等待结束后重新折出有效时限：先扣掉等待花掉的时间，再与绝对截止取更严者
         * @param request 读 deadline
         * @param requestTimeout 调用方给的相对时限
         * @param startedAt 本次请求开始时刻
         * @param failureReason 失败原因出参
         * @return std::optional<std::chrono::milliseconds> 有效时限；预算已被等待耗尽或截止已过时为空
         */
        std::optional<std::chrono::milliseconds> foldPostWaitTimeout(const HttpClientRequest &request, std::chrono::milliseconds requestTimeout,
                                                                     const std::chrono::steady_clock::time_point startedAt, std::string &failureReason)
        {
            std::chrono::milliseconds remaining = requestTimeout;
            if (remaining > std::chrono::milliseconds::zero())
            {
                const auto spentWaiting = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);
                if (spentWaiting >= remaining)
                {
                    failureReason = "本次请求已到时限：出向并发闸的等待用尽了整条预算";
                    return std::nullopt;
                }
                remaining -= spentWaiting;
            }
            return foldDeadlineIntoTimeout(request, remaining, std::chrono::steady_clock::now(), failureReason);
        }

        /**
         * @brief 在途名额的 RAII 归还（构造即占用，析构即归还）
         */
        class OutboundInFlightGuard
        {
        public:
            OutboundInFlightGuard(HttpOutboundConnectionPool &pool, const HttpOutboundEndpointKey &key) noexcept : m_pool(pool), m_key(key)
            {
            }

            ~OutboundInFlightGuard()
            {
                m_pool.releaseInFlight(m_key);
            }

            OutboundInFlightGuard(const OutboundInFlightGuard &)            = delete;
            OutboundInFlightGuard &operator=(const OutboundInFlightGuard &) = delete;

        private:
            HttpOutboundConnectionPool &m_pool; ///< 名额所属的池
            HttpOutboundEndpointKey     m_key;  ///< 端点
        };

        /**
         * @brief 请求取消的看门狗：令牌被请求停止时关掉当前连接，把阻塞中的收发协程唤醒
         * @details 套接字的等待器没有取消接口，「关掉这条连接」是本框架唤醒挂起协程的既定手段。
         *          看门狗跑在所属循环上，只轮询 `stop_token` 这一个线程安全的读数；连接对象由
         *          请求协程持有，本对象的析构先于它，因此不会留下一个醒来关已销毁连接的帧。
         */
        class CancellationGuard
        {
        public:
            CancellationGuard(Core::EventLoop &loop, HttpOutboundConnection &connection, const std::stop_token token, const std::string_view description) :
                m_watchdog(watchCancellation(loop, connection, token, m_isCancelled, m_isTriggered, description))
            {
                m_watchdog->handle().resume();
            }

            ~CancellationGuard()
            {
                m_isCancelled = true;
                m_watchdog.reset();
            }

            CancellationGuard(const CancellationGuard &)            = delete;
            CancellationGuard &operator=(const CancellationGuard &) = delete;

            /// 这次交换是否真的被取消看门狗掐断
            [[nodiscard]] bool isTriggered() const noexcept
            {
                return m_isTriggered;
            }

        private:
            static Core::Task<> watchCancellation(Core::EventLoop &loop, HttpOutboundConnection &connection, const std::stop_token token, const bool &isCancelled,
                                                  bool &isTriggered, const std::string_view description)
            {
                if (!token.stop_possible())
                {
                    co_return;
                }
                Core::Timer timer(loop);
                while (!isCancelled)
                {
                    if (token.stop_requested())
                    {
                        isTriggered = true;
                        LOG_WARN_FMT("{}: 请求被调用方取消，已关闭当前连接", description);
                        connection.close();
                        co_return;
                    }
                    co_await timer.waitFor(std::chrono::milliseconds{5});
                }
            }

            bool                        m_isTriggered{false}; ///< 看门狗是否已关闭连接（只在本循环线程读写）
            bool                        m_isCancelled{false}; ///< 监视已结束（看门狗协程按引用持有）
            std::optional<Core::Task<>> m_watchdog;           ///< 看门狗协程帧：置空即撤销
        };

        /**
         * @brief 把响应里的 Set-Cookie 收进罐子（没挂罐子时什么都不做）
         * @param cookieJar Cookie 罐（可空）
         * @param parsed 目标 URL
         * @param response 响应
         */
        void storeResponseCookies(HttpCookieJar *cookieJar, const ParsedUrl &parsed, const HttpClientResponse &response)
        {
            if (cookieJar == nullptr)
            {
                return;
            }
            std::vector<std::string> setCookieValues;
            for (const HttpClientHeaderField &field: response.headers)
            {
                if (equalsIgnoringCase(field.first, "set-cookie"))
                {
                    setCookieValues.push_back(field.second);
                }
            }
            if (!setCookieValues.empty())
            {
                cookieJar->storeFromResponse(parsed.host, parsed.scheme == "https", parsed.path, setCookieValues);
            }
        }

        /**
         * @brief 按响应头判断这条连接还能不能接着用
         * @details 对端声明 close 就不能再用（RFC 9112 §9.6：那是对本条连接的收尾声明）。没写这条头
         *          就是 HTTP/1.1 的默认，可以继续用。1.0 的对端默认收完就关，而解析器不把版本号交出
         *          来：那一档走「用了才发现对端已关」的路径——读回来的是干净的 0 字节，调用方按
         *          「一个字节都没收到」重开一条，不会把上一条的尾巴接到下一条头上。
         *          头名与那个 token 都按大小写不敏感判（RFC 9110 §7.6.1：connection-option 是 token，
         *          §5.1 说字段名大小写不敏感），且每条 connection 头都要看——中转可能把它拆成两条写，
         *          而 close 出现在任何一条里都是收尾。取值用子串找而不是按逗号切 token：找错的方向是
         *          「把还能用的连接当成要关的」，代价只是另开一条，反过来则会把请求发进一条对端已收的通路
         * @param responseHeaders 已收齐响应的头部字段（名与值按收到的顺序原样留着）
         * @return true 这条连接还能再发一条请求
         */
        bool isResponseReusable(const std::vector<std::pair<std::string, std::string>> &responseHeaders)
        {
            constexpr std::string_view kConnectionHeaderName = "connection";
            constexpr std::string_view kCloseOption          = "close";
            for (const auto &header: responseHeaders)
            {
                if (!equalsIgnoringCase(header.first, kConnectionHeaderName))
                {
                    continue;
                }
                if (containsIgnoringCase(header.second, kCloseOption))
                {
                    return false;
                }
            }
            return true;
        }
        /// 这几个头部由客户端按本次请求的实际情况写，调用方给了就拒收而不是覆盖或并存。Transfer-Encoding
        /// 也在其中：本端对流式正文自己写 chunked、对缓冲正文自己写 Content-Length，调用方再塞一份就是
        /// 「一条报文两个正文边界」——那正是本端 intake 要拒的那一类；而 h2 上它是 RFC 9113 §8.2.2
        /// 禁止的连接特定头，发出去只会让对端把这条共享连接按协议错误收掉
        bool isClientOwnedHeaderName(const std::string_view name)
        {
            return equalsIgnoringCase(name, "host") || equalsIgnoringCase(name, "content-length") || equalsIgnoringCase(name, "connection") ||
                   equalsIgnoringCase(name, "transfer-encoding");
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
            // 字符表交给 HttpHeaderRules 那一份（RFC 9110 §5.6.2 的 tchar 定义只写一处）；
            // 空串仍然当场拒：那边把「没有反例」算成 true，而方法名与头部名都不允许为空
            return containsOnlyTokenCharacters(text);
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
            if (request.traceState.has_value() && !request.traceContext.has_value())
            {
                throw Base::InvalidArgumentException("HttpClient：给了 traceState 却没给 traceContext：没有 traceparent 的 tracestate 按 W3C §3.3 应被丢弃，"
                                                     "请同时给出本端这一跳的链路上下文");
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
         * @param useAbsoluteForm 经代理的明文请求用绝对形式请求目标（RFC 9112 §3.2.2）
         * @param proxyAuthorization 非空即把这条值写进 Proxy-Authorization（经代理的明文请求才给）
         * @return std::string 完整的请求文（含正文）
         */
        std::string buildRequestText(const HttpClientRequest &request, const ParsedUrl &u, const bool isKeepAlive, const bool useAbsoluteForm,
                                     const std::string *proxyAuthorization)
        {
            std::string text;
            text.reserve(256 + request.body.size());
            text += request.method;
            text += ' ';
            if (useAbsoluteForm)
            {
                text += u.scheme;
                text += "://";
                text += authorityText(u);
            }
            text += u.path;
            text += " HTTP/1.1\r\n";
            appendHostHeader(text, u);
            if (proxyAuthorization != nullptr && !proxyAuthorization->empty())
            {
                text += "Proxy-Authorization: ";
                text += *proxyAuthorization;
                text += "\r\n";
            }
            for (const HttpClientHeaderField &field: request.headers)
            {
                text += field.first;
                text += ": ";
                text += field.second;
                text += "\r\n";
            }
            // 代为声明本端真能解回来的编码：调用方自己写过 accept-encoding 就整个不管正文（同 h2 那一支）
            // 挂了接收口也不代加：交出去的必须是对端发的那一份原样字节，本端不替你逐段解压
            if (shouldAdvertiseAcceptEncoding(request.headers) && !request.responseBodyReceiver)
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
            // 正文按移动取走：这是每响应里唯一与正文长度成正比的一笔拷贝
            response->body     = parser.takeBody();
            response->trailers = parser.result().trailers;
            return response;
        }


        /**
         * @brief 把流式正文逐段编成分块帧写上通路，最后补终止块
         * @details 拉一段、发一段：上一段没写上通路就不叫下一段，这就是上传的背压。零长的段按
         *          §7.1 就是终止块，不能当正文发出去（那等于提前把正文判完，后面的段落没人认领）。
         * @param connection 已连上、TLS 也已握手完的连接
         * @param bodySource 正文来源：每调一次给一段，空可选即收尾
         * @param host 失败原因里要点出的主机名
         * @param failureReason 输出：失败原因，成功时不动它
         * @return Core::Task<bool> true 表示正文与终止块都已写出；false 表示已写出 failureReason
         */
        Core::Task<bool> sendStreamingBody(HttpOutboundConnection &connection, const HttpBodyChunkSource &bodySource, const std::string_view host, std::string &failureReason)
        {
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
                    failureReason = "写出流式正文失败：对端在收完正文前收线，或本次请求已到时限（主机 " + std::string(host) + "）";
                    co_return false;
                }
            }
            const std::string terminator = "0\r\n\r\n";
            if (!co_await connection.send(terminator))
            {
                failureReason = "收尾流式正文失败：终止块没能写上通路（对端收线或已到时限，主机 " + std::string(host) + "）";
                co_return false;
            }
            co_return true;
        }

        /**
         * @brief 收尾一次 h1 交换：处理接收口收口、补最后一批、把解析结果搬成对外响应
         * @param connection 这条交换所在的连接
         * @param exchange 已经记下收发事实的交换记录
         * @param responseBodyReceiver 调用方的响应正文接收口（可空）
         * @param deliveredBatch 最后一批正文的落地缓冲
         * @param isStoppedByReceiver 接收口主动收口
         * @param cancellation 取消看门狗（读是否被取消掐断）
         * @param cancelledText 取消时使用的失败文案
         * @param host 目标主机（失败文案用）
         * @param failureReason 失败原因输出
         * @return OutboundExchange 收尾后的交换记录
         */
        Core::Task<OutboundExchange> finishExchange(HttpOutboundConnection &connection, OutboundExchange exchange, const HttpResponseBodyReceiver &responseBodyReceiver,
                                                    std::string &deliveredBatch, const bool isStoppedByReceiver, const CancellationGuard &cancellation,
                                                    const std::string &cancelledText, const std::string &host, std::string &failureReason)
        {
            if (isStoppedByReceiver)
            {
                // 调用方主动收的口：头部仍是完整可信的响应，正文到此为止。剩下的字节留在通路里，
                // 这条连接当场关掉——留着它还回池，下一条请求会把剩下的正文当自己的响应头读
                connection.close();
                auto stoppedResponse          = std::make_unique<HttpClientResponse>();
                stoppedResponse->statusCode   = connection.parser().result().statusCode;
                stoppedResponse->reasonPhrase = connection.parser().result().reasonPhrase;
                stoppedResponse->headers      = connection.parser().result().headers;
                exchange.response             = std::move(stoppedResponse);
                co_return exchange;
            }
            if (!connection.parser().isComplete())
            {
                connection.parser().endOfStream();
            }
            // 最后一批要在收齐之后交出去（零长的收尾也要交），接收口由此能分清「走完了」与「断了」；
            // 中途失败时不再交——那批字节是残缺的，调用方从返回的响应为空就能看出这条没成
            if (responseBodyReceiver && connection.parser().isComplete())
            {
                connection.parser().takeBodyBytes(deliveredBatch);
                static_cast<void>(co_await responseBodyReceiver(connection.parser().result(), std::string_view(deliveredBatch), true));
            }
            exchange.response = takeParsedResponse(connection.parser(), host, failureReason);
            if (cancellation.isTriggered() && !exchange.response)
            {
                failureReason = cancelledText;
            }
            co_return exchange;
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
                                                          const HttpBodyChunkSource &bodySource, const HttpResponseBodyReceiver &responseBodyReceiver, const bool isHeadRequest,
                                                          const std::chrono::milliseconds requestTimeout, const std::stop_token cancelToken, std::string &failureReason)
        {
            const Core::DeadlineGuard<HttpOutboundConnection> deadline(loop, connection, requestTimeout, "HttpClient");
            const CancellationGuard                           cancellation(loop, connection, cancelToken, "HttpClient");
            OutboundExchange                                  exchange;
            const std::string                                 host          = connection.endpointKey().host;
            const std::string                                 cancelledText = "请求已取消：调用方在等待期间请求停止（主机 " + host + "）";
            // 接收口自己收的口：响应只交到那一为止，连接当场关掉（半条正文的连接不能还池）
            bool        isStoppedByReceiver = false;
            std::string deliveredBatch;
            if (cancelToken.stop_requested())
            {
                failureReason = cancelledText;
                co_return exchange;
            }
            if (isHeadRequest)
            {
                connection.parser().markAsHeadResponse();
            }
            try
            {
                if (!co_await connection.send(requestText))
                {
                    failureReason = cancellation.isTriggered() ? cancelledText : "写出请求失败：对端在收完请求前收线，或本次请求已到时限（主机 " + host + "）";
                    co_return exchange;
                }
                exchange.isAnyByteSent = true;
                if (bodySource)
                {
                    if (!co_await sendStreamingBody(connection, bodySource, host, failureReason))
                    {
                        co_return exchange;
                    }
                }

                std::array<char, 4096> buffer{};
                while (true)
                {
                    const ssize_t receivedByteCount = co_await connection.receive(buffer.data(), buffer.size());
                    if (receivedByteCount < 0)
                    {
                        failureReason = cancellation.isTriggered() ? cancelledText : "读响应失败：通路报出不合规范的读取（主机 " + host + "）";
                        co_return exchange;
                    }
                    if (receivedByteCount == 0)
                    {
                        break;
                    }
                    exchange.isAnyByteReceived = true;
                    // 走连接自己的入口喂字节：响应收齐而这一批还剩字节时它会把这条连接当场判死，
                    // 于是下面 isOpen() 那一判就把这种错位的连接挡在池外
                    connection.feedReceivedBytes({buffer.data(), static_cast<std::size_t>(receivedByteCount)});
                    // 没挂接收口就是原样：一直读到整条响应收齐
                    if (!responseBodyReceiver || !connection.parser().isHeadComplete())
                    {
                        if (connection.parser().isComplete())
                        {
                            break;
                        }
                        if (connection.parser().hasFailed())
                        {
                            // 解析已判死：对端多半还握着这条连接，畸形报文不等于对端会收线。继续读只会
                            // 白占着描述符等到对端关闭或看门狗到点。失败响应为空，本来就走不到还池
                            // 那一条；仍显式收口，因为协程帧要等调用方放手才销毁，析构里的关闭来得更晚
                            connection.close();
                            break;
                        }
                        continue;
                    }
                    // 头部一收齐就逐批交付：这一段交完之前不去读下一批通路字节，背压因此落在接收口上
                    if (const std::size_t batchByteCount = connection.parser().takeBodyBytes(deliveredBatch); batchByteCount > 0)
                    {
                        if (!co_await responseBodyReceiver(connection.parser().result(), std::string_view(deliveredBatch), false))
                        {
                            isStoppedByReceiver = true;
                            break;
                        }
                    }
                    if (connection.parser().isComplete() || connection.parser().hasFailed())
                    {
                        if (!connection.parser().isComplete())
                        {
                            // 与上面那处同一判据：解析失败时对端可能仍开着连接，这里显式收口
                            connection.close();
                        }
                        break;
                    }
                }
            } catch (const std::exception &failure)
            {
                // 等待中套接字被关掉（超时掐断、对端收尾）时底层抛异常：本接口的契约是「失败返回空响应」，
                // 异常不许逃给调用方。抛出点之后的字节序已经不可信，这条连接当场收口——绝不还回池里
                LOG_WARN_FMT("HttpClient: 请求中断。底层原因：{}", failure.what());
                failureReason = cancellation.isTriggered() ? cancelledText : "请求中断：套接字在等待中被关掉（时限掐断或对端收线，主机 " + host + "）";
                connection.close();
                co_return exchange;
            }
            co_return co_await finishExchange(connection, std::move(exchange), responseBodyReceiver, deliveredBatch, isStoppedByReceiver, cancellation, cancelledText, host,
                                              failureReason);
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

        /**
         * @brief 按复用型通路（HTTP/2 与 HTTP/3）的要求备齐附加字段
         * @details 两条通路的字段段形状一样（小写 ASCII 名 + 原文值），差别只在线上是 HPACK 还是
         *          QPACK，因此这里只留一份。名字折小写是协议要求（RFC 9113 §8.2、RFC 9114 §4.2），
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
            // 挂了接收口同样不加：理由与 h1 那一支一样，交出去的是对端发的原样字节
            if (shouldAdvertiseAcceptEncoding(request.headers) && !request.responseBodyReceiver)
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
        std::unique_ptr<HttpClientResponse> makeClientResponse(const int statusCode, std::vector<std::pair<std::string, std::string>> headers, std::string body,
                                                               std::vector<std::pair<std::string, std::string>> trailers = {})
        {
            auto result        = std::make_unique<HttpClientResponse>();
            result->statusCode = statusCode;
            result->headers    = std::move(headers);
            result->body       = std::move(body);
            result->trailers   = std::move(trailers);
            return result;
        }

        /**
         * @brief 把复用型通路（h2 与 h3）的逐批交付接到调用方那份响应接收口上
         * @details 两边交的是同一批字节，差别只在头部装在哪份记录里：协议侧交自己那份响应记录，
         *          本处折成 `HttpResponseInfo` 再交出去。折好的那份按「状态码 + 字段条数」缓存，
         *          尾部字段到齐才重折一次——一批一拷会让每条 SSE 事件都搭上一次头部分配。
         */
        class MultiplexedBodyDeliverer
        {
        public:
            /// @param receiver 调用方那份接收口（按值持有：它要跨过每一次 co_await 活着）
            explicit MultiplexedBodyDeliverer(HttpResponseBodyReceiver receiver) : m_receiver(std::move(receiver))
            {
            }

            /**
             * @brief 交一批出去
             * @tparam ResponseRecord 协议侧的响应记录（h2 与 h3 那两份都有 statusCode 与 headers）
             * @param record 这条流当前的响应记录
             * @param batch 本批正文（只在本次调用内有效）
             * @param isLastBatch 是否最后一批
             * @return true 接收口还要下一批
             */
            template<typename ResponseRecord>
            Core::Task<bool> deliver(const ResponseRecord &record, const std::string_view batch, const bool isLastBatch)
            {
                if (!m_head.has_value() || m_head->statusCode != record.statusCode || m_head->headers.size() != record.headers.size())
                {
                    HttpResponseInfo head;
                    head.statusCode = record.statusCode;
                    head.headers    = record.headers;
                    m_head          = std::move(head);
                }
                co_return co_await m_receiver(*m_head, batch, isLastBatch);
            }

        private:
            HttpResponseBodyReceiver        m_receiver; ///< 调用方的接收口
            std::optional<HttpResponseInfo> m_head{};   ///< 折好的响应头部副本
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
            // 接收口与它的适配器都落在本帧上：协议侧那份 std::function 要跨过每一次 co_await 活着，
            // 而这两个对象要到本协程返回才随帧销毁（空的那一份不分配，普通请求是零成本）
            std::optional<MultiplexedBodyDeliverer> deliverer;
            Http2ResponseBodyReceiver               h2Receiver;
            if (request.responseBodyReceiver)
            {
                deliverer.emplace(request.responseBodyReceiver);
                MultiplexedBodyDeliverer *delivererPointer = &*deliverer;
                h2Receiver = [delivererPointer](const Http2ClientResponse &record, const std::string_view batch, const bool isLastBatch) -> Core::Task<bool>
                { co_return co_await delivererPointer->deliver(record, batch, isLastBatch); };
            }
            const std::string_view scheme   = u.scheme == "https" ? "https" : "http";
            Http2ClientResponse    response = bodySource ? co_await client.requestStreamed(scheme, authority, method, u.path, extraFields, bodySource, *exchangeBudget, h2Receiver)
                                                         : co_await client.request(scheme, authority, method, u.path, extraFields, body, *exchangeBudget, h2Receiver);
            exchange.isAnyByteReceived      = response.isAnyByteReceived;
            exchange.isAnyByteSent          = response.isAnyByteSent;
            // h2 是三条通路里唯一能拿到「保证没处理过」这个信号的：§8.7 的两种机制都是帧级的
            exchange.isGuaranteedUnprocessed = response.isGuaranteedUnprocessed;
            if (!response.isOk())
            {
                // 状态码为 0（没收到响应头）或被对端中途 RST 掉：都不算一次成功的出站
                failureReason = response.errorMessage.empty() ? "HTTP/2 这一侧没拿到有效响应：状态码缺失或流被对端收尾（主机 " + u.host + "）" : std::move(response.errorMessage);
                co_return exchange;
            }
            // 主动收口那一条也走这一支：头部完整、body 为空（字节都交给了接收口），isAnyByteReceived 与
            // isAnyByteSent 两位照旧可信
            exchange.response = makeClientResponse(response.statusCode, std::move(response.headers), std::move(response.body), std::move(response.trailers));
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
            std::optional<MultiplexedBodyDeliverer> deliverer;
            Http3ResponseBodyReceiver               h3Receiver;
            if (request.responseBodyReceiver)
            {
                deliverer.emplace(request.responseBodyReceiver);
                MultiplexedBodyDeliverer *delivererPointer = &*deliverer;
                h3Receiver = [delivererPointer](const Http3ClientResponse &record, const std::string_view batch, const bool isLastBatch) -> Core::Task<bool>
                { co_return co_await delivererPointer->deliver(record, batch, isLastBatch); };
            }
            // QUIC 的 TLS 是强制的，:scheme 因此在 h3 上恒为 https（能走到这里已由 canUseHttp3 保证）
            Http3ClientResponse response = co_await client.request("https", authority, method, u.path, extraFields, body, *exchangeBudget, h3Receiver);
            exchange.isAnyByteReceived   = response.isAnyByteReceived;
            exchange.isAnyByteSent       = response.isAnyByteSent;
            // h3 的保证来自 §5.2 的 H3_REQUEST_REJECTED 与 GOAWAY 通告值及以上的那些流（§7）
            exchange.isGuaranteedUnprocessed = response.isGuaranteedUnprocessed;
            if (!response.isOk())
            {
                failureReason = response.errorMessage.empty() ? "HTTP/3 这一侧没拿到有效响应：状态码缺失或这条流被收尾（主机 " + u.host + "）" : std::move(response.errorMessage);
                co_return exchange;
            }
            // 主动收口那一条走同一支：头部完整、body 为空
            exchange.response = makeClientResponse(response.statusCode, std::move(response.headers), std::move(response.body), std::move(response.trailers));
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
            // 判据只有 HttpClient::isRetrySafeAfterFailure 一份：它把「对端答过话」「请求写上过通路」
            // 「对端保证没处理过」三态合起来判，h2 的 REFUSED_STREAM／GOAWAY 那两种保证也在那里生效
            if (!HttpClient::isRetrySafeAfterFailure(method, exchange.isAnyByteSent, exchange.isAnyByteReceived, exchange.isGuaranteedUnprocessed))
            {
                if (!exchange.isAnyByteReceived)
                {
                    failureReason = "请求已整个写上通路而对端没答话：方法 " + std::string{method} +
                                    " 不在幂等集合里，对端也没按 RFC 9113 §8.7 保证这条请求没被处理过，本端不重发（主机 " + host + "）";
                }
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
         * @brief 这次请求的剩余预算付不付得起一次 h3 探测的固定开销
         * @details 探测那 3 秒是这条通路上唯一由本端定的开销，而 `requestTimeout` 的契约是「握手、发送、
         *          收完响应三段之和」（见 remainingBudget）：只给 300 毫秒的请求原本会被一次探测撑到 3 秒，
         *          探测回来时预算已尽，那次真正的 TCP 请求连一次都不试就判失败。付不起就跳过探测，
         *          直接走 TCP——而不是把探测截短成 300 毫秒：截短的那次一定失败，而失败会被记成
         *          「这个端点没有 h3」，于是一条小时限的健康检查就能把这个端点的 h3 永久关掉。
         * @param startedAt 本次请求的开始时刻
         * @param requestTimeout 调用方给的整体时限
         * @return true 还剩至少一次探测的时限，可以去探
         */
        bool canAffordHttp3Probe(const std::chrono::steady_clock::time_point startedAt, const std::chrono::milliseconds requestTimeout)
        {
            const std::optional<std::chrono::milliseconds> remaining = remainingBudget(startedAt, requestTimeout);
            return remaining.has_value() && *remaining >= kHttp3ProbeTimeout;
        }

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
            // DNS 也归 h3 探测这一小段预算：解析卡住时不把整条请求拖过自己的时限
            const std::vector<Core::InetAddress> addresses = co_await Core::AsyncResolver::resolve(loop, u.host, u.port, kHttp3ProbeTimeout);
            if (addresses.empty())
            {
                failureReason = "解析地址失败或超时：没能把「" + u.host + "」解析成可用地址";
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
         * @brief 本次请求的代理决定：路由加上写报文用的两个派生标志
         */
        struct RequestProxyDecision
        {
            std::optional<HttpProxyRoute> route;                     ///< 空即直连
            bool                          useAbsoluteForm{false};    ///< 明文经代理：请求目标用绝对形式（RFC 9112 §3.2.2）
            const std::string            *requestProxyAuth{nullptr}; ///< 非空即往请求上写 Proxy-Authorization（指向 route 内的值）
        };

        /**
         * @brief 解析一次代理路由并派生标志；坏配置写原因交回 false（拒绝请求而不是静默直连）
         * @details 摘在 performRequest 之外：重定向每一跳都要按当跳 URL 重解析，而那个函数本来就贴着
         *          复杂度基线——解析属「决定」、执行属「动作」，分开也让逐跳语义一眼可见
         */
        bool resolveRequestProxyRoute(const HttpProxyPolicy &policy, const ParsedUrl &url, std::string &failureReason, RequestProxyDecision &decision)
        {
            const std::expected<std::optional<HttpProxyRoute>, std::string> resolution = resolveOutboundProxy(policy, url.scheme, url.host, url.port);
            if (!resolution.has_value())
            {
                failureReason = resolution.error();
                return false;
            }
            decision.route            = *resolution;
            decision.useAbsoluteForm  = decision.route.has_value() && url.scheme == "http";
            decision.requestProxyAuth = decision.useAbsoluteForm ? &decision.route->authorizationHeader : nullptr;
            return true;
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
         * @param decision 本次请求的代理决定（路由与两个派生标志）：由 performRequestFollowingRedirects 每跳解析一次后传入
         * @return std::unique_ptr<HttpClientResponse> 响应；失败（含超时）返回空
         */
        Core::Task<std::unique_ptr<HttpClientResponse>> performRequest(Core::EventLoop &loop, const HttpClientRequest &request, const ParsedUrl &u,
                                                                       const std::chrono::milliseconds requestTimeout, HttpOutboundConnectionPool *pool, std::string &failureReason,
                                                                       const Core::TlsContext *clientTls, const Http3OutboundContext *http3, const RequestProxyDecision &decision)
        {
            const std::chrono::steady_clock::time_point startedAt   = std::chrono::steady_clock::now();
            const bool                                  isKeepAlive = pool != nullptr;
            const HttpOutboundEndpointKey               endpointKey{u.host, u.port, u.scheme == "https"};
            // 请求文按要再拼：h2 与 h3 那一支用不上它（帧里没有请求行），提前拼一份等于把正文整块多拷一次
            const auto makeRequestText = [&] { return buildRequestText(request, u, isKeepAlive, decision.useAbsoluteForm, decision.requestProxyAuth); };
            const bool isHeadRequest   = request.method == "HEAD";
            // 这次请求有没有资格走 h3：开关与 URL 形状之外还要端点没被探败过；被代理命中一律跳过（QUIC 不经 HTTP 代理）
            const bool isHttp3Eligible = pool != nullptr && canUseHttp3(u, request, http3) && !isHttp3ProbeRejected(endpointKey, *http3) && !decision.route.has_value();

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
                            OutboundExchange  exchange = co_await exchangeOnConnection(loop, *reused, requestText, request.bodySource, request.responseBodyReceiver, isHeadRequest,
                                                                                       *reusedBudget, request.cancelToken, failureReason);
                            if (exchange.response)
                            {
                                // isOpen() 这一位是给「接收口半路收口、连接已被关掉」留的：那种连接
                                // 不能还池，否则下一条请求会把剩下的正文当响应头读
                                if (reused->isOpen() && isResponseReusable(exchange.response->headers))
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
                // 不用作用域守卫是因为本框架的协程帧不在 co_return 时销毁（FinalAwaiter 是 no-op），
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
                // 剩下的预算仍归那次真正的 TCP 请求用——这是「失败回落」这条承诺能成立的地方。
                // 前提调用方的时限付得起这一段（判据见 canAffordHttp3Probe）：付不起就不探，
                // 也不替这个端点记下「没有 h3」
                if (isHttp3Eligible && canAffordHttp3Probe(startedAt, requestTimeout))
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
                    connection = co_await establishSecureConnection(loop, u, startedAt, requestTimeout, failureReason, clientTls, decision.route);
                } else
                {
                    connection = co_await establishPlainConnection(loop, endpointKey, startedAt, requestTimeout, failureReason, decision.route, clientTls);
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
                OutboundExchange  exchange    = co_await exchangeOnConnection(loop, *connection, requestText, request.bodySource, request.responseBodyReceiver, isHeadRequest,
                                                                              *exchangeBudget, request.cancelToken, failureReason);
                if (exchange.response && pool != nullptr && connection->isOpen() && isResponseReusable(exchange.response->headers))
                {
                    connection->prepareForNextRequest();
                    pool->release(std::move(connection));
                }
                co_return std::move(exchange.response);
            }
        }

        /// 可自动跟随的重定向：300 要人来选，304 根本不是跳转（它说的是「你手里那份还有效」）
        [[nodiscard]] constexpr bool isAutoFollowableRedirectStatus(const int statusCode) noexcept
        {
            return statusCode == 301 || statusCode == 302 || statusCode == 303 || statusCode == 307 || statusCode == 308;
        }

        /// 头部名按 ASCII 折小写（RFC 9110 §5.1 大小写不敏感，且不跟 locale 走）
        [[nodiscard]] std::string lowerFieldName(const std::string_view name)
        {
            std::string folded(name);
            std::transform(folded.begin(), folded.end(), folded.begin(),
                           [](const unsigned char byte) { return (byte >= 'A' && byte <= 'Z') ? static_cast<char>(byte + ('a' - 'A')) : byte; });
            return folded;
        }

        /// 折成 GET 之后正文没了，留着那些声明等于让对端按一份不存在的正文处理
        void stripBodyDescribingFields(HttpClientRequest &request)
        {
            std::erase_if(request.headers,
                          [](const HttpClientHeaderField &field)
                          {
                              // 名单写在闭包里：GCC 不许闭包捕获静态存储期的对象（-Werror 直接拒），而这张表只服务这一处判断
                              static constexpr std::array<std::string_view, 4> kBodyFieldNames{"content-length", "content-type", "content-encoding", "transfer-encoding"};
                              const std::string                                folded = lowerFieldName(field.first);
                              return std::any_of(kBodyFieldNames.begin(), kBodyFieldNames.end(), [&folded](const std::string_view name) { return folded == name; });
                          });
        }

        /// 跨源时剥掉凭据与 Cookie：Authorization 跟着对端挑的主机走，等于把口令交给另一台机器（RFC 9110 §11.5.3）；
        /// Cookie 是原主机域下的状态，跟着跳过去就是把原主机的会话交给新主机
        void stripCrossOriginCredentials(HttpClientRequest &request)
        {
            std::erase_if(request.headers,
                          [](const HttpClientHeaderField &field)
                          {
                              const std::string folded = lowerFieldName(field.first);
                              return folded == "authorization" || folded == "cookie";
                          });
        }

        /// 一条 3xx 折算出来的下一跳：URL 与请求两份都要换
        struct RedirectStep
        {
            ParsedUrl         url;
            HttpClientRequest request;
        };

        /// 协议、主机、端口任一变了就算跨源
        [[nodiscard]] constexpr bool isCrossOrigin(const ParsedUrl &from, const ParsedUrl &to) noexcept
        {
            return from.scheme != to.scheme || from.host != to.host || from.port != to.port;
        }

        /**
         * @brief 按 RFC 9110 §15.4 把一条 3xx 折算成下一跳；折不出来交回空并把停在哪儿写进 stopReason
         * @details 交回空不等于失败：调用方仍拿得到对端真实给过的那条 3xx，看得境停在哪一格。
         */
        [[nodiscard]] std::optional<RedirectStep> planRedirectStep(const HttpClientResponse &response, const HttpClientRequest &current, const ParsedUrl &currentUrl,
                                                                   std::string &stopReason)
        {
            // 接收口是对「这一条响应」的约定：跟随会让同一个口子先收一段 3xx 正文、再收一段终局正文，
            // 两种读法都不对，所以宁可不跟
            if (current.responseBodyReceiver)
            {
                stopReason = "设了响应正文接收口，跟随重定向会让同一个接收口收两段互不相干的正文";
                return std::nullopt;
            }
            const auto location = response.headerValue("location");
            if (!location.has_value())
            {
                stopReason = "对端没给 Location";
                return std::nullopt;
            }
            const auto absolute = resolveUrlReference(currentUrl, *location);
            if (!absolute.has_value())
            {
                stopReason = "Location 不是能解析成 http(s) 的引用";
                return std::nullopt;
            }
            RedirectStep step;
            try
            {
                step.url = parseUrl(*absolute);
            } catch (const Base::Exception &)
            {
                // resolveUrlReference 已经核过写法，这里正常到不了；到了也不能让异常穿过这条协程
                stopReason = "Location 解出的绝对 URL 仍读不出（对端给的引用超出可跟随的范围）";
                return std::nullopt;
            }
            // 303 见 GET，HEAD 例外（它本来就是「只要头」的问法）；301/302 只折 POST，那是历史上唯一
            // 被普遍改写成 GET 的方法；307/308 明写不许改方法——它们要的重放就是同一个请求
            const bool isFoldToGet = response.statusCode == 303 ? current.method != "HEAD" : (response.statusCode == 301 || response.statusCode == 302) && current.method == "POST";
            step.request           = current;
            if (isFoldToGet)
            {
                step.request.method      = "GET";
                step.request.body        = {};
                step.request.contentType = {};
                step.request.bodySource  = {};
                stripBodyDescribingFields(step.request);
            } else if (current.bodySource)
            {
                // 方法要保住就意味着正文也要保住，而拉取来源是一次性的：再拉一遍交出去的不是同一份字节
                stopReason = "下一跳要原样重放正文，而这份正文是一次性的拉取来源";
                return std::nullopt;
            }
            if (isCrossOrigin(currentUrl, step.url))
            {
                stripCrossOriginCredentials(step.request);
            }
            return step;
        }

        /**
         * @brief `performRequest()` 的重定向包一层：单次与连接池两条出口共用这一份跟随逻辑
         * @details 时限按整条请求算：每一跳只拿此刻剩下的预算，所以跟几跳都不会把 requestTimeout
         *          乘上跳数（这条纪律与 h3 探测吃预算那处同一个口径）。开关关着时第一趟就原样透传。
         */
        Core::Task<std::unique_ptr<HttpClientResponse>> performRequestFollowingRedirects(Core::EventLoop &loop, const HttpClientRequest &request, const ParsedUrl &url,
                                                                                         const std::chrono::milliseconds requestTimeout, HttpOutboundConnectionPool *pool,
                                                                                         HttpCookieJar *cookieJar, std::string &failureReason, const Core::TlsContext *clientTls,
                                                                                         const Http3OutboundContext *http3, const HttpProxyPolicy *proxyPolicy)
        {
            const auto        deadline   = std::chrono::steady_clock::now() + requestTimeout;
            HttpClientRequest current    = request;
            ParsedUrl         currentUrl = url;
            // 调用方自己写了 cookie 头就以他为准：罐子在任何一跳都不补，也不覆盖
            const bool callerSuppliedCookie = std::ranges::any_of(request.headers, [](const HttpClientHeaderField &field) { return equalsIgnoringCase(field.first, "cookie"); });
            for (std::size_t hop = 0;; ++hop)
            {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (remaining <= std::chrono::milliseconds::zero())
                {
                    failureReason = "本次请求已到时限：前面 " + std::to_string(hop) + " 跳重定向把整条预算用尽（最后一条来自 " + currentUrl.host + "）";
                    co_return nullptr;
                }
                std::unique_ptr<HttpClientResponse> response;
                // 代理决定按当跳 URL 解析：重定向换主机/换 scheme 时选的代理与豁免表随之切换
                {
                    RequestProxyDecision proxyDecision;
                    if (!resolveRequestProxyRoute(*proxyPolicy, currentUrl, failureReason, proxyDecision))
                    {
                        co_return nullptr;
                    }
                    // current 本身不承载罐子注入的 Cookie：每一跳都从干净副本上按当跳 URL 补，跨源剥离也不会
                    // 留下上一跳的注入结果
                    HttpClientRequest hopRequest = current;
                    if (!callerSuppliedCookie && cookieJar != nullptr)
                    {
                        std::erase_if(hopRequest.headers, [](const HttpClientHeaderField &field) { return equalsIgnoringCase(field.first, "cookie"); });
                        if (const auto cookieHeader = cookieJar->buildRequestHeader(currentUrl.host, currentUrl.scheme == "https", currentUrl.path); cookieHeader.has_value())
                        {
                            hopRequest.headers.emplace_back("cookie", *cookieHeader);
                        }
                    }
                    response = co_await performRequest(loop, hopRequest, currentUrl, remaining, pool, failureReason, clientTls, http3, proxyDecision);
                }
                if (response != nullptr)
                {
                    // Set-Cookie 属于发出这条响应的那一跳：按当跳 URL 入库，终跳的才不会被记到首跳的域下
                    storeResponseCookies(cookieJar, currentUrl, *response);
                }
                if (response == nullptr || !request.followRedirects || !isAutoFollowableRedirectStatus(response->statusCode))
                {
                    co_return std::move(response);
                }
                if (hop >= request.maximumRedirectCount)
                {
                    // 交回最后那条 3xx 而不是编一个失败：它是对端真实给过的响应，调用方看得见停在第几跳
                    LOG_WARN_FMT("HttpClient: 停在第 {} 跳重定向（上限 {}，主机 {}）：跳数到顶，继续跟下去多半是环", hop, request.maximumRedirectCount, currentUrl.host);
                    co_return std::move(response);
                }
                std::string stopReason;
                auto        step = planRedirectStep(*response, current, currentUrl, stopReason);
                if (!step.has_value())
                {
                    LOG_WARN_FMT("HttpClient: 没有跟随这条 {} 重定向（主机 {}）：{}", response->statusCode, currentUrl.host, stopReason);
                    co_return std::move(response);
                }
                LOG_DEBUG_FMT("HttpClient: 跟随 {} 重定向到 {}{}（第 {} 跳）", response->statusCode, step->url.host, step->url.path, hop + 1);
                current    = std::move(step->request);
                currentUrl = std::move(step->url);
            }
        }
    } // namespace

    Core::Task<std::expected<HttpClientResponse, std::string>> HttpClient::send(Core::EventLoop &loop, const std::string_view url, HttpClientRequest request,
                                                                                const std::chrono::milliseconds requestTimeout)
    {
        // 畸形 URL 与不合规范的头部都是用法错误，按 parseUrl 的既有口径抛出，不折进 expected 的失败值
        const ParsedUrl parsed = parseUrl(url);
        validateRequest(request);
        if (request.cancelToken.stop_requested())
        {
            co_return std::unexpected("请求已取消：调用方在发送前已请求停止（目标 " + std::string(url) + "）");
        }
        if (request.traceContext.has_value())
        {
            // request 是按下标传进来的副本：改它的头部不影响调用方，也不必再有一份实现去拼那条头部
            appendTraceHeaders(request.headers, *request.traceContext, request.traceState);
        }
        std::string failureReason;
        // 绝对截止时刻折进相对时限：没有 deadline 时原样透传，行为与加这一项之前逐字相同
        const std::optional<std::chrono::milliseconds> effectiveTimeout = foldDeadlineIntoTimeout(request, requestTimeout, std::chrono::steady_clock::now(), failureReason);
        if (!effectiveTimeout.has_value())
        {
            co_return std::unexpected(std::move(failureReason));
        }
        // 静态那一路不带池、也没有承载 TLS 策略与 h3 开关的地方，因此永不走 h3（两个空指针即此意）；
        // 代理策略同样没有实例承载，每次调用按默认策略现读一次环境（对齐 curl 每进程语义）。
        // 静态入口也不带 Cookie 罐：没有可承载罐子的实例状态
        const HttpProxyPolicy               proxyPolicy = applyProxyEnvironment(HttpProxyPolicy{});
        std::unique_ptr<HttpClientResponse> response =
                co_await performRequestFollowingRedirects(loop, request, parsed, *effectiveTimeout, nullptr, nullptr, failureReason, nullptr, nullptr, &proxyPolicy);
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

    bool HttpClient::isRetrySafeAfterFailure(const std::string_view method, const bool isAnyByteSent, const bool isAnyByteReceived, const bool isGuaranteedUnprocessed) noexcept
    {
        if (isAnyByteReceived)
        {
            // 对端答过话：那是响应本身出了问题，换一条通路重来不会换一个答案
            return false;
        }
        if (!isAnyByteSent)
        {
            // 请求压根没写上通路：对端无从执行它，重来不涉及把一件事做两遍
            return true;
        }
        // 写上了通路而对端没答话：默认按「可能已经执行过」处置，重来只许幂等方法（RFC 9112 §9.3.2 给的
        // 自动重试许可只覆盖幂等方法）；但 RFC 9113 §8.7 那两种保证是另一回事——REFUSED_STREAM 与 GOAWAY
        // 的 last-stream-id 都说明对端没处理过这条请求，那时连非幂等方法也可以重来
        return isIdempotentRequestMethod(method) || isGuaranteedUnprocessed;
    }

    HttpClient::HttpClient(Core::EventLoop &loop, const HttpOutboundConnectionPool::Config poolConfig) : HttpClient(loop, poolConfig, Core::TlsPolicy{})
    {
    }

    // 唯一要做的事是放掉那份 TLS 上下文（头文件里它只是前向声明）；池由自己的成员收尾
    HttpClient::~HttpClient() = default;

    HttpClient::HttpClient(Core::EventLoop &loop, const HttpOutboundConnectionPool::Config poolConfig, const Core::TlsPolicy &tlsPolicy) :
        HttpClient(loop, poolConfig, tlsPolicy, HttpProxyPolicy{})
    {
    }

    HttpClient::HttpClient(Core::EventLoop &loop, const HttpOutboundConnectionPool::Config poolConfig, const Core::TlsPolicy &tlsPolicy, HttpProxyPolicy proxyPolicy) :
        m_loop(&loop), m_pool(poolConfig), m_clientTls(std::make_unique<Core::TlsContext>(tlsPolicy, Core::TlsContext::Role::Client)),
        m_tlsPolicy(std::make_unique<Core::TlsPolicy>(tlsPolicy)), m_proxyPolicy(applyProxyEnvironment(std::move(proxyPolicy))),
        m_circuitBreaker(std::make_shared<OutboundCircuitBreaker>())
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

        // 填了 traceContext 才拷一份请求出来加头部：不填时开销与行为都和原来一样
        std::optional<HttpClientRequest> requestWithTrace;
        const HttpClientRequest         &effectiveRequest = prepareOutboundRequest(request, requestWithTrace);
        validateRequest(effectiveRequest);
        std::string failureReason;
        if (effectiveRequest.cancelToken.stop_requested())
        {
            failureReason = "请求已取消：调用方在发送前已请求停止（目标 " + std::string(url) + "）";
            LOG_ERROR_FMT("HttpClient: {}", failureReason);
            co_return nullptr;
        }

        const HttpOutboundEndpointKey endpointKey{parsed.host, parsed.port, parsed.scheme == "https"};

        // 出向并发闸：每个端点的在途请求数上限。Fail（默认）立即失败；Wait 在时限/截止时刻内轮询等名额。
        // 计数在闸内、释放靠守卫，覆盖整条请求（含重定向），不与空闲连接池的那条账混用
        const auto requestStartedAt = std::chrono::steady_clock::now();
        if (!co_await acquireOutboundInFlightSlot(m_pool, *m_loop, endpointKey, effectiveRequest, request.method, url, requestTimeout, requestStartedAt, failureReason))
        {
            LOG_ERROR_FMT("HttpClient: {}", failureReason);
            co_return nullptr;
        }
        OutboundInFlightGuard inFlightGuard{m_pool, endpointKey};

        // 闸等待吃掉的预算要从相对时限里扣掉，再与绝对截止时刻取更严者
        const std::optional<std::chrono::milliseconds> effectiveTimeout = foldPostWaitTimeout(effectiveRequest, requestTimeout, requestStartedAt, failureReason);
        if (!effectiveTimeout.has_value())
        {
            LOG_ERROR_FMT("HttpClient: {}", failureReason);
            co_return nullptr;
        }

        // 熔断放行排在这两道本地闸之后：半开态下 allowRequest 会占掉一个探测名额，而它只有走到
        // reportSuccess/reportFailure 才归还。前面两条早退（并发闸没拿到名额、预算已耗尽）都不会
        // 报告任何结果，先问熔断就等于把名额永远漏在账上，端点从此在半开态里被永久拒绝。
        // 排在这里仍有原意：开闸期间连套接字都不建，上游塌掉时这笔钱不会每个请求重付一遍
        if (m_circuitBreaker != nullptr && !m_circuitBreaker->allowRequest(endpointKey))
        {
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

        // 罐子要被跟随重定向那段协程跨每一跳的 co_await 使用：先取一份快照，让它活在本协程帧里。
        // 直接把 m_cookieJar.get() 传下去时，同一条循环上有人 setCookieJar() 换罐，旧罐析构后这里就悬垂
        const std::shared_ptr<HttpCookieJar> cookieJarSnapshot = m_cookieJar;

        std::unique_ptr<HttpClientResponse> response = co_await performRequestFollowingRedirects(
                *m_loop, effectiveRequest, parsed, *effectiveTimeout, &m_pool, cookieJarSnapshot.get(), failureReason, m_clientTls.get(), &http3Context, &m_proxyPolicy);
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
        if (!applyContentEncoding(effectiveRequest, *response, failureReason))
        {
            LOG_ERROR_FMT("HttpClient: {} {} 失败。原因：{}", request.method, url, failureReason);
            co_return nullptr;
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

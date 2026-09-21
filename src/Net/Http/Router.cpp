#include "Net/Http/Router.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 已收录方法的固定顺序，用于生成确定性的 Allow 头
         *
         * @details 不随注册顺序变化，客户端每次拿到的 Allow 字面量一致，便于做断言与缓存。
         *          这里刻意不含 UNKNOWN：它不是一个可以申请的方法，写进 Allow 等于自曝实现细节。
         */
        constexpr std::array<HttpMethod, 7> kRecognizedMethodOrder{
                HttpMethod::GET, HttpMethod::HEAD, HttpMethod::POST,
                HttpMethod::PUT, HttpMethod::DELETE, HttpMethod::PATCH, HttpMethod::OPTIONS};

        /**
         * @brief 拼 Allow 头时方法之间的分隔符（ASCII，遵循 RFC 9110 §10.4 的 "#rulelist" 写法）
         */
        constexpr std::string_view kAllowedMethodSeparator = ", ";
    } // namespace

    // ============================================================================
    // PatternRoute
    // ============================================================================

    Router::PatternRoute::PatternRoute(const HttpMethod routeMethod, const bool matchAnyMethod, std::string routePattern,
                                       Handler routeHandler, const bool isStreaming) :
        method(routeMethod), isAnyMethod(matchAnyMethod), pattern(std::move(routePattern)), streaming(isStreaming),
        handler(std::move(routeHandler))
    {
        // 预解析放在构造里：模式在路由生命周期内不变，没必要每请求再拆一遍
        precomputeSegments(pattern);
    }

    void Router::PatternRoute::precomputeSegments(const std::string &routePattern)
    {
        segments.clear();
        wildcardPrefix.clear();
        isWildcard = false;

        // 只有「模式最后一个字符是 '*'」才按通配处理；"/a/*/b" 里中间的 '*' 按字面段比对，
        // 免得悄悄把不合法的模式解释成两层通配（llhttp 送进来的路径里本就不该有这种东西）
        const bool endsWithWildcardCharacter = !routePattern.empty() && routePattern.back() == '*';
        if (endsWithWildcardCharacter)
        {
            // 去掉结尾的 '*' 后剩下的就是前缀部分："/static/*" -> "/static/"
            const std::string_view prefixPart(routePattern.data(), routePattern.size() - 1);

            // 单独一个 "*"（或 "/*"）表示「任意路径」，固定段为空，天然无需边界对齐
            const bool isBareWildcard = prefixPart.empty() || prefixPart == "/";

            // 其余写法必须以 '/' 结尾："/static*" 这种「前缀贴着 '*'」的写法会把匹配退化成
            // 字符串前缀比较，于是 "/static" 能命中 "/staticevil"——目录穿越与越权读文件的入口。
            // 这里直接拒绝把它当通配，按字面路径处理，宁可 404 也不放行。
            const bool isBoundaryAligned = !prefixPart.empty() && prefixPart.back() == '/';
            if (isBareWildcard || isBoundaryAligned)
            {
                isWildcard     = true;
                wildcardPrefix = std::string(prefixPart); // 含结尾 '/'，是「边界对齐」的关键形态
            }
        }

        // 取出参与逐段比对的固定部分：通配模式取 wildcardPrefix 去掉结尾 '/'，其余取整个模式。
        // 两种形态随后都统一剥掉开头的 '/'，得到不含首斜杠的待拆串
        const std::string_view fixedPart = isWildcard
                                               ? std::string_view(wildcardPrefix.data(), wildcardPrefix.empty() ? 0 : wildcardPrefix.size() - 1)
                                               : std::string_view(routePattern);
        std::string_view remainder = (!fixedPart.empty() && fixedPart.front() == '/') ? fixedPart.substr(1) : fixedPart;

        while (!remainder.empty())
        {
            const std::size_t slashPosition = remainder.find('/');
            segments.emplace_back(remainder.substr(0, slashPosition));

            // 没有下一个 '/' 说明刚拆完最后一段；否则越过分隔符继续
            if (slashPosition == std::string_view::npos)
            {
                break;
            }
            remainder = remainder.substr(slashPosition + 1);
        }
    }

    // ============================================================================
    // 注册入口
    // ============================================================================

    void Router::get(const std::string &path, Handler handler)
    {
        addRoute(HttpMethod::GET, false, path, std::move(handler));
    }

    void Router::post(const std::string &path, Handler handler)
    {
        addRoute(HttpMethod::POST, false, path, std::move(handler));
    }

    void Router::put(const std::string &path, Handler handler)
    {
        addRoute(HttpMethod::PUT, false, path, std::move(handler));
    }

    void Router::postStreaming(const std::string &path, Handler handler)
    {
        addRoute(HttpMethod::POST, false, path, std::move(handler), true);
    }

    void Router::putStreaming(const std::string &path, Handler handler)
    {
        addRoute(HttpMethod::PUT, false, path, std::move(handler), true);
    }

    void Router::del(const std::string &path, Handler handler)
    {
        addRoute(HttpMethod::DELETE, false, path, std::move(handler));
    }

    void Router::patch(const std::string &path, Handler handler)
    {
        addRoute(HttpMethod::PATCH, false, path, std::move(handler));
    }

    void Router::head(const std::string &path, Handler handler)
    {
        addRoute(HttpMethod::HEAD, false, path, std::move(handler));
    }

    void Router::options(const std::string &path, Handler handler)
    {
        addRoute(HttpMethod::OPTIONS, false, path, std::move(handler));
    }

    void Router::any(const std::string &path, Handler handler)
    {
        // method 位置填 UNKNOWN 仅作占位：能否放行完全由 isAnyMethod 决定，
        // 而 UNKNOWN 作为「请求方法」在本类里根本不参与匹配（见 route() 的识别性判定）
        addRoute(HttpMethod::UNKNOWN, true, path, std::move(handler));
    }

    void Router::addMiddleware(MiddlewareFunc middleware)
    {
        m_pipeline.use(std::move(middleware));
    }

    bool Router::isLiteralPath(const std::string_view path)
    {
        // ':' 与 '*' 是本路由器仅有的两个模式记号，出现任一即不能进 O(1) 精确索引
        return path.find(':') == std::string_view::npos && path.find('*') == std::string_view::npos;
    }

    std::string_view Router::methodName(const HttpMethod method)
    {
        switch (method)
        {
            case HttpMethod::GET:
                return "GET";
            case HttpMethod::HEAD:
                return "HEAD";
            case HttpMethod::POST:
                return "POST";
            case HttpMethod::PUT:
                return "PUT";
            case HttpMethod::DELETE:
                return "DELETE";
            case HttpMethod::PATCH:
                return "PATCH";
            case HttpMethod::OPTIONS:
                return "OPTIONS";
            case HttpMethod::UNKNOWN:
                // 未收录方法不进 Allow：它既不是本服务器支持的能力，也不该被客户端拿去重试
                return {};
        }
        return {};
    }

    void Router::addRoute(const HttpMethod method, const bool isAnyMethod, const std::string &path, Handler handler,
                          const bool streaming)
    {
        // 空路径按根路径处理：注册 "" 的人本意就是「访问站点根」，留着一个永远匹配不上的键只会让人困惑
        const std::string normalizedPath = path.empty() ? std::string("/") : path;

        if (isLiteralPath(normalizedPath))
        {
            std::vector<ExactRoute> &candidates = m_exactRoutes[normalizedPath];

            // 同 (方法集合, 路径) 重复注册：就地替换处理函数且不改变条目位置，
            // 这样「重复注册」既幂等，又不会把先到先得的顺序悄悄挪到最后
            for (ExactRoute &candidate: candidates)
            {
                const bool isSameBinding = candidate.isAnyMethod == isAnyMethod && (isAnyMethod || candidate.method == method);
                if (isSameBinding)
                {
                    candidate.handler   = std::move(handler);
                    candidate.streaming = streaming;
                    return;
                }
            }
            candidates.push_back(ExactRoute{method, isAnyMethod, streaming, std::move(handler)});
            return;
        }

        // 模式路由同理先找同键条目替换；找不到才追加到末尾（注册顺序即优先级顺序）
        for (PatternRoute &existingRoute: m_patternRoutes)
        {
            const bool isSameBinding = existingRoute.isAnyMethod == isAnyMethod && (isAnyMethod || existingRoute.method == method);
            if (isSameBinding && existingRoute.pattern == normalizedPath)
            {
                existingRoute.handler   = std::move(handler);
                existingRoute.streaming = streaming;
                return;
            }
        }
        m_patternRoutes.emplace_back(method, isAnyMethod, normalizedPath, std::move(handler), streaming);
    }

    // ============================================================================
    // 匹配
    // ============================================================================

    bool Router::matchesPattern(const PatternRoute &route, const std::string_view requestPath, PathParameters &collectedParameters,
                                const bool collectParameters)
    {
        // 请求路径不以 '/' 开头就不是合法Origin-form（OPTIONS 的 "*" 除外，它由通配路由整体吃掉），
        // 这里直接判不匹配，避免把 "etc/passwd" 这类畸形路径与 "/etc/passwd" 当成同一条
        if (requestPath.empty() || requestPath.front() != '/')
        {
            return false;
        }

        // 剩余路径统一保持「段首不含 '/'」的形态："/a/b" 剥掉首字符后是 "a/b"
        std::string_view remainingPath = requestPath.substr(1);

        for (const std::string &patternSegment: route.segments)
        {
            // 模式的固定段还没走完而路径已经没有段了：段数不足，直接失败
            // （这条判据同时堵住了「模式比路径长」的整类误匹配）
            if (remainingPath.empty())
            {
                return false;
            }

            const std::size_t slashPosition = remainingPath.find('/');
            const std::string_view currentSegment = remainingPath.substr(0, slashPosition);

            if (!patternSegment.empty() && patternSegment.front() == ':')
            {
                // ":name" 段不接受空值："/user/" 不该被 "/user/:id" 命中并给出空的 id
                if (currentSegment.empty())
                {
                    return false;
                }
                // 流式探测只关心「命没命中 + 是不是流式」，参数随即丢弃，故按 collectParameters 跳过
                // 键与值各一次 std::string 构造（长值还是堆分配）
                if (collectParameters)
                {
                    collectedParameters[std::string(patternSegment.substr(1))] = std::string(currentSegment);
                }
            } else if (patternSegment != currentSegment)
            {
                // 字面段不等即失败，不做前缀比较：这正是 "/static/*" 不再命中 "/staticevil" 的原因
                return false;
            }

            // 越过刚消费掉的分隔符；没有下一个 '/' 说明这是最后一段
            remainingPath = (slashPosition == std::string_view::npos) ? std::string_view{} : remainingPath.substr(slashPosition + 1);
        }

        // 通配路由：固定段全部命中即可，剩下的整段（可含多级 '/'）都算捕获值
        if (route.isWildcard)
        {
            if (collectParameters)
            {
                collectedParameters[std::string(kWildcardParameterName)] = std::string(remainingPath);
            }
            return true;
        }

        // 非通配路由要求「段数恰好相等」：remainingPath 非空说明路径比模式长
        return remainingPath.empty();
    }

    void Router::commitPathParameters(HttpRequest &request, const PathParameters &collectedParameters)
    {
        // 匹配阶段一律只往临时容器里写（见 matchesPattern 的 collectedParameters），
        // 到这里才一次性提交：否则一条候选路由匹配到一半失败，它写进请求的 ":id"
        // 会残留在最终命中的另一条路由上，handler 读到的是别条路由的参数。
        // 提交策略为「先到先得、不覆盖」：同一个请求只会被一条路由处理，
        // 保留既有值只是为了让外部提前塞入的调试参数不被抹掉。
        for (const auto &[parameterName, parameterValue]: collectedParameters)
        {
            if (request.param(parameterName).has_value())
            {
                continue;
            }
            request.setParam(parameterName, parameterValue);
        }
    }

    void Router::writeNotFoundOrNotAllowed(const HttpRequest &request, HttpResponse &response, const bool isMethodNotAllowed, const std::string &allowedMethods)
    {
        // 不重置响应：本函数现在由中间件管道的终点回调调用，而 CORS 与访问日志这类横切
        // 中间件的头部正是在进入终点之前写下的，重置会把它们一起抹掉。响应对象在会话里
        // 是每请求新建的局部量（见 httpKeepAliveLoop），不存在跨请求的残留需要清理。
        // 协议版本仍显式交代一次：状态行必须跟随请求的版本
        const std::string requestVersion = request.httpVersion();
        response.setHttpVersion(requestVersion);

        if (!isMethodNotAllowed)
        {
            response.setStatus(404);
            response.setBody("Not Found");
        } else
        {
            // 405 必须带 Allow：RFC 9110 §15.5.7 要求源服务器在 405 响应里 MUST 生成 Allow 头
            response.setStatus(405);
            response.setBody("Method Not Allowed");
            if (!allowedMethods.empty())
            {
                response.setHeader("allow", allowedMethods);
            }
        }
        response.setHeader("content-type", "text/plain");

        // 版本回填：请求是 HTTP/1.0 时响应状态行也必须写 1.0，否则客户端按 1.1 语义解读连接复用
        if (!requestVersion.empty())
        {
            response.setHttpVersion(requestVersion);
        }
    }

    void Router::finalizeResponse(HttpResponse &response)
    {
        // RFC 9110 §15.3.3 / §15.4.5：204 与 304 的响应都不能带正文。
        //
        // HEAD 刻意不在这里处理：它的正文要留到序列化时才用得上——会话按完整正文序列化头部，
        // 才能得到与同一路径 GET 逐字节一致的一份头部（含自动补齐的 content-length 与
        // content-type），随后在发送时把正文换成空（见 detail::httpKeepAliveLoop）。
        //
        // 流式响应不做任何正文收尾：它的正文由分块帧逐段写出、长度对路由层未知，既不补
        // content-length 也不清整块正文（setBody() 在流式模式下会报错）。HEAD 上使用流式模式
        // 属于调用方的误用，已在 HttpResponse::startChunkedResponse 的 @warning 里说明
        if (response.isChunkedResponse())
        {
            return;
        }

        const bool isNoContent   = response.status() == 204;
        const bool isNotModified = response.status() == 304;
        if (isNoContent || isNotModified)
        {
            // 只清正文。严格线上语义下这两类响应连 content-length 都不该出现（RFC 7230 §3.3.2），
            // 但 HttpResponse::toString() 无条件补一条 content-length，抑制它需要响应层开口子，
            // 不在本文件的职责范围内
            response.setBody(std::string_view{});
        }
    }

    // ============================================================================
    // 路由入口
    // ============================================================================

    Core::Task<> Router::route(HttpRequest &request, HttpResponse &response)
    {
        // 路径取视图而不是副本：request.path() 返回指向请求对象的视图，路由这里只读不改，
        // 每请求因此省掉一次路径串拷贝（精确路由的查找靠下面的透明哈希做到零分配）
        const std::string_view requestPath = request.path();
        const HttpMethod  requestMethod = request.method();

        // 方法是否被本框架收录：未收录（CONNECT/TRACE/M-SEARCH 等）一律不进业务匹配。
        // UNKNOWN 不参与通配匹配：放行它等于让任何畸形方法都能蹭到兜底路由上。
        const bool isRequestMethodRecognized = requestMethod != HttpMethod::UNKNOWN;

        // 本条路径上允许的方法集合，用于路径命中而方法不合时生成 405 的 Allow 头。
        // 用定长数组而不是 vector：收录的方法一共 7 个，any() 路由至多把它们全列一遍，
        // 8 个位置足够——为它每请求分配一次堆内存不值当
        constexpr std::size_t kMaximumAllowedMethodCount = kRecognizedMethodOrder.size() + 1;
        std::array<HttpMethod, kMaximumAllowedMethodCount> allowedMethodSet{};
        std::size_t allowedMethodCount = 0;
        const auto  isMethodAllowed = [&allowedMethodSet, &allowedMethodCount](const HttpMethod method)
        {
            const auto end = allowedMethodSet.begin() + static_cast<std::ptrdiff_t>(allowedMethodCount);
            return std::find(allowedMethodSet.begin(), end, method) != end;
        };
        const auto rememberAllowedMethod = [&](const HttpMethod routeMethod, const bool routeMatchesAnyMethod)
        {
            if (routeMatchesAnyMethod)
            {
                // any() 路由等价于把全部收录方法都声明一遍
                for (const HttpMethod recognizedMethod: kRecognizedMethodOrder)
                {
                    if (!isMethodAllowed(recognizedMethod))
                    {
                        allowedMethodSet[allowedMethodCount] = recognizedMethod;
                        ++allowedMethodCount;
                    }
                }
                return;
            }
            if (!isMethodAllowed(routeMethod))
            {
                allowedMethodSet[allowedMethodCount] = routeMethod;
                ++allowedMethodCount;
            }
        };

        // 选中的处理函数按指针引用持有：路由期间绝不拷贝 std::function。
        // 一次拷贝意味着把大 lambda 的捕获（正则、模板、配置表）按请求复制一遍，
        // 那是纯粹的每请求堆分配，而容器在整段 co_await 期间都不会被改动，引用始终有效。
        const Handler *selectedHandler = nullptr;
        PathParameters selectedParameters;

        // HEAD 复用 GET（RFC 9110 §9.1：通用服务器必须同时支持 GET 与 HEAD）的判据落在「同一级之内
        // 先严格匹配、这一级全都没中才按 GET 复用」，而不是「整张表先按 HEAD 跑一遍、没中再按 GET
        // 跑一遍」：后者会让下一级的兜底 any("*")（HttpServer 的静态目录就是这么挂的）在 HEAD 那一遍
        // 就把请求抢走，同路径的 GET 业务路由反而永远轮不到。级别之间的优先级因此与 GET 完全一致
        const auto selectHandlerForMethod = [&](const HttpMethod matchMethod) -> bool
        {
            // 只有 HEAD 有第二遍：第一遍认「显式放行本方法」（head() 与 any()），第二遍才复用 GET。
            // 因此显式注册的 head() 无论先注册还是后注册都赢过同路径的 get()
            const int passCount = matchMethod == HttpMethod::HEAD ? 2 : 1;

            // ---- 一级：字面路径索引。命中即完成本层候选收集 ----
            if (const auto exactIterator = m_exactRoutes.find(requestPath); exactIterator != m_exactRoutes.end())
            {
                for (int pass = 0; pass < passCount; ++pass)
                {
                    for (const ExactRoute &candidate: exactIterator->second)
                    {
                        // Allow 只在第一遍记：第二遍的候选是它的子集，重复记没有新信息
                        if (pass == 0)
                        {
                            rememberAllowedMethod(candidate.method, candidate.isAnyMethod);
                        }

                        if (!isRequestMethodRecognized)
                        {
                            continue;
                        }
                        // 先到先得：同一路径上的多个条目按注册顺序取第一个放行本方法的
                        const bool isStrictMatch = candidate.isAnyMethod || candidate.method == matchMethod;
                        const bool isGetReuse    = pass == 1 && candidate.method == HttpMethod::GET;
                        if (isStrictMatch || isGetReuse)
                        {
                            selectedHandler = &candidate.handler;
                            return true;
                        }
                    }
                }
            }

            // ---- 二级：模式路由线性扫描。仅在一级没选中处理函数时才继续，规则同样是先到先得 ----
            for (int pass = 0; pass < passCount; ++pass)
            {
                for (const PatternRoute &route: m_patternRoutes)
                {
                    // 参数只在本条路由成立时才留下：每轮都换一个新的临时容器，
                    // 失败候选攒下的 ":id" 就此被整体丢弃，不会串到别的路由上
                    PathParameters candidateParameters;
                    if (!matchesPattern(route, requestPath, candidateParameters))
                    {
                        continue;
                    }

                    if (pass == 0)
                    {
                        rememberAllowedMethod(route.method, route.isAnyMethod);
                    }

                    const bool isStrictMatch = route.isAnyMethod || route.method == matchMethod;
                    const bool isGetReuse    = pass == 1 && route.method == HttpMethod::GET;
                    if (isRequestMethodRecognized && (isStrictMatch || isGetReuse))
                    {
                        selectedHandler    = &route.handler;
                        selectedParameters = std::move(candidateParameters);
                        return true;
                    }
                    // 路径命中而方法不合：记下事实，扫完全部候选再决定 405，Allow 也才凑得齐
                }
            }
            return false;
        };

        // 按请求方法跑一遍两级匹配。未收录方法是唯一不放行的例外：UNKNOWN 不能蹭上 any() 路由，
        // 这条判据由 isRequestMethodRecognized 把关
        static_cast<void>(selectHandlerForMethod(requestMethod));

        if (selectedHandler != nullptr)
        {
            commitPathParameters(request, selectedParameters);

            // 终点回调把「请求 + 响应 + 命中的 handler」绑成管道要求的无参可调用对象。
            // 它只在下面这次 co_await 期间存在，故引用捕获即可，无需 shared_ptr 续命。
            const TerminalHandler terminalHandler = [&request, &response, selectedHandler]() -> Core::Task<void>
            {
                co_await (*selectedHandler)(request, response);
            };

            // 中间件与 handler 的异常一律向上传播，由会话统一重置成 500，路由器不吞也不翻译
            co_await m_pipeline.run(request, response, terminalHandler);
            finalizeResponse(response);
            co_return;
        }

        // 路径压根没注册过 → 404；注册过但方法都不合 → 405 + Allow
        const bool isMethodNotAllowed = allowedMethodCount != 0;

        // RFC 9110 §9.1 + §15.5.7：本框架按 GET 复用 HEAD（见上面的匹配逻辑），因此路径支持 GET 时
        // 资源实际也支持 HEAD，Allow 必须一并列出——只回显显式注册的方法会让客户端以为 HEAD 不可用，
        // 与「HEAD 命中 GET 处理器」的实际行为自相矛盾。输出顺序由下面的固定序保证（GET 在 HEAD 前）
        if (isMethodAllowed(HttpMethod::GET) && !isMethodAllowed(HttpMethod::HEAD))
        {
            allowedMethodSet[allowedMethodCount] = HttpMethod::HEAD;
            ++allowedMethodCount;
        }

        std::string allowedMethods;
        if (isMethodNotAllowed)
        {
            // 按固定顺序输出，保证 Allow 头的字面量与注册顺序无关
            for (const HttpMethod recognizedMethod: kRecognizedMethodOrder)
            {
                if (!isMethodAllowed(recognizedMethod))
                {
                    continue;
                }
                if (!allowedMethods.empty())
                {
                    allowedMethods.append(kAllowedMethodSeparator);
                }
                allowedMethods.append(methodName(recognizedMethod));
            }
        }

        // 未命中也走同一条中间件管道：CORS、访问日志、限流这类横切逻辑必须对 404/405 一视同仁，
        // 否则跨域请求撞到 404 会退化成浏览器侧的 opaque 错误，日志里也看不到任何未命中的请求。
        //
        // 重置与写入分两步：先在这里清掉调用方可能残留的状态（本函数是公开 API，调用方可以
        // 复用同一个 HttpResponse），再让中间件写头部，最后由终点补状态与 Allow。
        // 若把重置放到终点里，中间件刚写下的 CORS 头会被一起抹掉
        response.reset();

        const TerminalHandler unmatchedTerminalHandler =
                [this, &request, &response, isMethodNotAllowed, &allowedMethods]() -> Core::Task<void>
        {
            writeNotFoundOrNotAllowed(request, response, isMethodNotAllowed, allowedMethods);
            co_return;
        };

        co_await m_pipeline.run(request, response, unmatchedTerminalHandler);
        finalizeResponse(response);
        co_return;
    }

    // ============================================================================
    // 流式派发判定
    // ============================================================================

    bool Router::matchedRouteIsStreaming(const HttpMethod matchMethod, const std::string_view requestPath) const
    {
        // 与 route() 的匹配保持同一优先级：精确索引 → 模式列表，两侧都是先到先得。
        // 这里只需回答「命中的那条是不是流式注册」，因此不收集参数、不记 Allow
        if (const auto exactIterator = m_exactRoutes.find(requestPath); exactIterator != m_exactRoutes.end())
        {
            for (const ExactRoute &candidate: exactIterator->second)
            {
                if (candidate.isAnyMethod || candidate.method == matchMethod)
                {
                    return candidate.streaming;
                }
            }
        }

        // 本探测只要「命中与否 + 是不是流式注册」，参数一律不收集：省去通配剩余路径与每个
        // ":name" 段的 std::string 构造（这条判定每条非精确请求都会跑到）
        PathParameters unusedParameters;
        for (const PatternRoute &route: m_patternRoutes)
        {
            if (!matchesPattern(route, requestPath, unusedParameters, false))
            {
                continue;
            }
            if (route.isAnyMethod || route.method == matchMethod)
            {
                return route.streaming;
            }
        }
        return false;
    }

    bool Router::hasStreamingRoute(const HttpMethod method, const std::string_view uri) const
    {
        // 流式注册只有 postStreaming()/putStreaming() 两个入口，二者都绑死 POST/PUT 且非 any 方法，
        // 故除这两个方法外任何请求都不可能被判定为流式（UNKNOWN 一并落在此处挡掉）。
        // GET/HEAD（静态与只读流量的大头）据此直接返回 false，免去整张路由表的空转扫描
        if (method != HttpMethod::POST && method != HttpMethod::PUT)
        {
            return false;
        }

        // 路径截取与 HttpRequest::path() 同一口径：第一个 '?' 之前算路径
        const std::size_t queryPosition = uri.find('?');
        const std::string_view requestPath = queryPosition == std::string_view::npos ? uri : uri.substr(0, queryPosition);

        // HEAD 复用 GET 的兜底不需要镜像：本类只提供 postStreaming()/putStreaming() 两种
        // 流式注册，HEAD 请求在方法层就与它们不相干
        return matchedRouteIsStreaming(method, requestPath);
    }

} // namespace AsynGyanis::Net

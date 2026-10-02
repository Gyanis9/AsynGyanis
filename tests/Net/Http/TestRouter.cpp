// Router 单元测试：两级匹配优先级、方法与 404/405 判定、HEAD 复用 GET、参数提交、响应收尾与按 Host 选站
#include "Net/Http/Router.h"

#include "Base/Exception/InvalidArgumentException.h"
#include "Core/Coroutine/Task.h"
#include "Net/Http/HttpMethod.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 构造一条路由测试用的请求：只填方法、URI、版本与可选的 Host
         * @param method 请求方法
         * @param uri    原始 URI（含查询串时由 path() 自行裁剪）
         * @param host   Host 头部原文，可为空（空表示这条请求不带 Host，用来验默认站点回落）
         * @return HttpRequest 可直接交给 route() 的请求对象
         */
        HttpRequest makeRequest(const HttpMethod method, std::string uri, std::string_view host = {})
        {
            HttpRequest request;
            request.setMethod(method);
            request.setUri(std::move(uri));
            request.setHttpVersion("HTTP/1.1");
            if (!host.empty())
            {
                request.addHeader("host", host);
            }
            return request;
        }

        /**
         * @brief 同步跑完一次路由
         * @details 测试里的 handler 全部即时完成，因此协程必然在首次 resume() 内跑完；
         *          若没跑完说明有人在测试链里偷偷挂起，直接判失败而不是把用例吊死。
         * @param router   被测路由器
         * @param request  请求对象
         * @param response 响应对象
         */
        void routeRequest(Router &router, HttpRequest &request, HttpResponse &response)
        {
            Core::Task<> routeTask = router.route(request, response);
            routeTask.handle().resume();
            ASSERT_TRUE(routeTask.isReady()) << "路由协程未在同步路径上跑完：测试 handler 不得真实挂起";
            routeTask.handle().promise().result();
        }

        /**
         * @brief 生成一个「写下固定正文并计数」的处理函数
         * @param bodyText    响应正文
         * @param callCounter 调用次数计数器，可为空指针
         * @return Router::Handler 处理函数
         */
        Router::Handler textHandler(std::string bodyText, std::atomic<int> *callCounter = nullptr)
        {
            return [bodyText = std::move(bodyText), callCounter]([[maybe_unused]] HttpRequest &request, HttpResponse &response) -> Core::Task<void>
            {
                if (callCounter != nullptr)
                {
                    callCounter->fetch_add(1);
                }
                response.setBody(bodyText);
                co_return;
            };
        }

        /// 全部被框架收录的方法，用于验证 any() 的放行集合
        constexpr std::array<HttpMethod, 7> kAllRecognizedMethods{HttpMethod::GET,    HttpMethod::HEAD,  HttpMethod::POST,   HttpMethod::PUT,
                                                                  HttpMethod::DELETE, HttpMethod::PATCH, HttpMethod::OPTIONS};

        /// any() 路由命中失败时，405 的 Allow 头应有的固定顺序文本
        constexpr std::string_view kAllRecognizedMethodNames = "GET, HEAD, POST, PUT, DELETE, PATCH, OPTIONS";
    } // namespace

    // ============================================================================
    // 精确路径与未命中
    // ============================================================================

    TEST(Router, DeliversRequestToHandlerRegisteredForExactPath)
    {
        Router           router;
        std::atomic<int> callCount{0};
        router.get("/hello", textHandler("world", &callCount));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/hello");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(callCount.load(), 1);
        EXPECT_EQ(response.body(), "world");
        EXPECT_EQ(response.status(), 200);
    }

    TEST(Router, CutsRoutingPathOffBeforeQueryDelimiter)
    {
        Router           router;
        std::atomic<int> callCount{0};
        router.get("/search", textHandler("found", &callCount));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/search?q=router&page=2");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(callCount.load(), 1);
        EXPECT_EQ(response.body(), "found");
    }

    TEST(Router, NormalizesEmptyRegistrationPathToRoot)
    {
        Router router;
        router.get("", textHandler("root"));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(response.body(), "root");
    }

    /**
     * @brief OPTIONS 的 asterisk-form 按根路径派发——服务器级的应答就住在 options("/") 上
     * @details RFC 9112 §3.2.3 里 "*" 指的不是某个资源而是整台服务器。本路由器没有「服务器整体」这一层：
     *          不映射就是 404（旧行为，也是 Router::matchesPattern 那条注释曾经声称「通配路由整体吃掉」
     *          却并不存在的路径）；映射错方向更糟——把 "*" 当成一段通配去匹配模式路由，等于让
     *          options("*") 这种「任意一段」的注册冒充服务器级应答。
     *          反向对照取一条单段路由：GET 的 "*" 落到哪儿都不该是它。
     */
    TEST(Router, RoutesAsteriskFormOptionsRequestToTheRootPath)
    {
        Router           router;
        std::atomic<int> serverOptionsCount{0};
        std::atomic<int> helloCount{0};
        router.options("/", textHandler("allow", &serverOptionsCount));
        router.get("/hello", textHandler("world", &helloCount));

        HttpRequest  optionsRequest = makeRequest(HttpMethod::OPTIONS, "*");
        HttpResponse optionsResponse;
        routeRequest(router, optionsRequest, optionsResponse);
        EXPECT_EQ(serverOptionsCount.load(), 1) << "OPTIONS * 没有落到服务器级的 options(\"/\") 处理器";
        EXPECT_EQ(optionsResponse.body(), "allow");

        // 反向对照：语法层之外的第二道闸——路由器被直接调用时，非 OPTIONS 的 "*" 不得匹配任何资源路由
        HttpRequest  getRequest = makeRequest(HttpMethod::GET, "*");
        HttpResponse getResponse;
        routeRequest(router, getRequest, getResponse);
        EXPECT_EQ(helloCount.load(), 0) << "单段路由被 asterisk-form 冒充命中";
        EXPECT_EQ(getResponse.status(), 404);
    }

    TEST(Router, WritesNotFoundWhenNoRouteMatchesPath)
    {
        Router router;
        router.get("/registered", textHandler("never"));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/missing");
        HttpResponse response;
        response.setStatus(201);
        response.setBody("上一轮残留的正文");
        ASSERT_TRUE(response.setHeader("x-prev", "1"));

        routeRequest(router, request, response);

        EXPECT_EQ(response.status(), 404);
        EXPECT_EQ(response.body(), "Not Found");
        EXPECT_EQ(response.getHeader("content-type").value_or(""), "text/plain");
        // 先 reset 再填：上一轮的痕迹不得跟着 404 一起发出去
        EXPECT_FALSE(response.getHeader("x-prev").has_value());
        EXPECT_EQ(response.headerValues("x-prev").size(), 0U);
    }

    TEST(Router, CarriesRequestProtocolVersionIntoErrorResponse)
    {
        Router      router;
        HttpRequest request = makeRequest(HttpMethod::GET, "/missing");
        request.setHttpVersion("HTTP/1.0");

        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_TRUE(response.toString().starts_with("HTTP/1.0 404 Not Found\r\n"));
    }

    TEST(Router, WritesMethodNotAllowedWithDeterministicAllowHeader)
    {
        Router           router;
        std::atomic<int> callCount{0};
        // 故意先注册 POST：Allow 的顺序由固定表决定，与注册先后无关
        router.post("/item", textHandler("created", &callCount));
        router.get("/item", textHandler("listed", &callCount));

        HttpRequest  request = makeRequest(HttpMethod::DELETE, "/item");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(callCount.load(), 0);
        EXPECT_EQ(response.status(), 405);
        EXPECT_EQ(response.body(), "Method Not Allowed");
        // GET 被允许时 Allow 另补隐含可用的 HEAD（RFC 9110 §9.1/§15.5.7）：资源实际支持 HEAD，
        // 只列显式注册的方法会与「HEAD 命中 GET 处理器」的实际行为自相矛盾
        EXPECT_EQ(response.getHeader("allow").value_or(""), "GET, HEAD, POST");
    }

    TEST(Router, ReusesGetRouteForHeadRequestOnExactPath)
    {
        Router           router;
        std::atomic<int> callCount{0};
        router.get("/only-get", textHandler("body", &callCount));

        HttpRequest  request = makeRequest(HttpMethod::HEAD, "/only-get");
        HttpResponse response;
        routeRequest(router, request, response);

        // RFC 9110 §9.1：通用服务器必须同时支持 GET 与 HEAD，没有显式注册 head() 时按 GET 复用。
        // 正文保留到会话发送前才抑制，头部因此能与同一路径的 GET 逐字节一致
        EXPECT_EQ(callCount.load(), 1);
        EXPECT_EQ(response.status(), 200);
        EXPECT_EQ(response.body(), "body");
        EXPECT_FALSE(response.getHeader("allow").has_value());
        EXPECT_NE(response.serializeHead().find("content-length: 4"), std::string::npos);
    }

    TEST(Router, ReusesGetPatternRouteAndCommitsItsParameterForHeadRequest)
    {
        Router           router;
        std::atomic<int> callCount{0};
        router.get("/user/:id",
                   [&callCount](HttpRequest &request, HttpResponse &response) -> Core::Task<void>
                   {
                       callCount.fetch_add(1);
                       response.setBody("user-" + request.param("id").value_or(""));
                       co_return;
                   });

        HttpRequest  request = makeRequest(HttpMethod::HEAD, "/user/42");
        HttpResponse response;
        routeRequest(router, request, response);

        // 模式路由同样复用："id" 必须与走 GET 时一样被收集并提交给请求
        EXPECT_EQ(callCount.load(), 1);
        EXPECT_EQ(response.body(), "user-42");
    }

    TEST(Router, PrefersExplicitHeadRouteOverGetRouteOnSamePath)
    {
        Router           router;
        std::atomic<int> getCalls{0};
        std::atomic<int> headCalls{0};
        // 故意先注册 GET：显式 head() 优先是「方法命中」而非「注册先后」的结果
        router.get("/probe", textHandler("from-get", &getCalls));
        router.head("/probe", textHandler("from-head", &headCalls));

        HttpRequest  request = makeRequest(HttpMethod::HEAD, "/probe");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(headCalls.load(), 1);
        EXPECT_EQ(getCalls.load(), 0);
        EXPECT_EQ(response.body(), "from-head");
    }

    TEST(Router, KeepsExactGetRouteAheadOfPatternGetRouteForHeadRequest)
    {
        Router           router;
        std::atomic<int> patternCalls{0};
        std::atomic<int> exactCalls{0};
        // 模式路由先注册：按 GET 复用 HEAD 时也必须遵守「精确路径永远优先于模式路径」
        router.get("/a/*", textHandler("pattern", &patternCalls));
        router.get("/a/b", textHandler("exact", &exactCalls));

        HttpRequest  request = makeRequest(HttpMethod::HEAD, "/a/b");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(exactCalls.load(), 1);
        EXPECT_EQ(patternCalls.load(), 0);
        EXPECT_EQ(response.body(), "exact");
    }

    TEST(Router, KeepsExactGetRouteAheadOfAnyMethodCatchAllForHeadRequest)
    {
        Router           router;
        std::atomic<int> catchAllCalls{0};
        std::atomic<int> exactCalls{0};
        // 静态目录就是这么挂的（HttpServer::staticFileDir 注册 any("*")）。HEAD 必须与 GET 走同一套
        // 优先级：若先按 HEAD 匹配一遍、没中再按 GET 匹配一遍，any() 会在第一遍就把请求抢走，
        // 同路径的 GET 业务路由反而轮不到——那是「HEAD 与 GET 只差有没有正文」这条语义的直接违反
        router.any("*", textHandler("catch-all", &catchAllCalls));
        router.get("/page", textHandler("business", &exactCalls));

        HttpRequest  request = makeRequest(HttpMethod::HEAD, "/page");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(exactCalls.load(), 1);
        EXPECT_EQ(catchAllCalls.load(), 0);
        EXPECT_EQ(response.body(), "business");
    }

    TEST(Router, Reports405ForHeadRequestOnPostOnlyPath)
    {
        Router           router;
        std::atomic<int> callCount{0};
        router.post("/submit", textHandler("created", &callCount));

        HttpRequest  request = makeRequest(HttpMethod::HEAD, "/submit");
        HttpResponse response;
        routeRequest(router, request, response);

        // 路径上没有 GET 也没有 HEAD，复用 GET 无从谈起：仍是 405。Allow 只列显式注册的方法，
        // 不把「靠 GET 隐式可用」的 HEAD 补进去
        EXPECT_EQ(callCount.load(), 0);
        EXPECT_EQ(response.status(), 405);
        EXPECT_EQ(response.getHeader("allow").value_or(""), "POST");
    }

    TEST(Router, RunsMiddlewarePipelineForUnmatchedRoute)
    {
        Router           router;
        std::atomic<int> middlewareCalls{0};
        router.addMiddleware(
                [&middlewareCalls](HttpRequest &, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
                {
                    middlewareCalls.fetch_add(1);
                    response.setHeader("x-middleware", "1");
                    co_await next();
                });

        HttpRequest  request = makeRequest(HttpMethod::GET, "/missing");
        HttpResponse response;
        routeRequest(router, request, response);

        // 横切逻辑对未命中一视同仁：中间件必须执行，且它写下的头部不能被 404 的写入抹掉
        EXPECT_EQ(middlewareCalls.load(), 1);
        EXPECT_EQ(response.getHeader("x-middleware").value_or(""), "1");
        EXPECT_EQ(response.status(), 404);
        EXPECT_EQ(response.body(), "Not Found");
    }

    // ============================================================================
    // 命中的路由模式（按路由打点的键）
    // ============================================================================

    /**
     * @brief 钉住：命中的路由模式原文进了请求，且中间件在管道里读得到同一份
     * @details 只有 `path()` 时，按路由分组的键基数等于访问过的 URL 数——一轮 404 扫描就能把采集端撑爆。
     *          模式原文是有限集合（注册多少条就有多少个值）。中间件那一半单独钉：横切逻辑（访问日志、
     *          打点）只在管道里跑，模式落晚一步它们就什么都读不到
     */
    TEST(Router, MatchedRouteIsVisibleToMiddlewareAndHandler)
    {
        std::vector<std::string> seenByHandler;
        std::vector<std::string> seenByMiddleware;
        const auto recordPattern = [&seenByHandler](HttpRequest &request, HttpResponse &response) -> Core::Task<void>
        {
            seenByHandler.emplace_back(request.matchedRoute());
            response.setBody("ok");
            co_return;
        };

        Router router;
        router.addMiddleware(
                [&seenByMiddleware](HttpRequest &request, HttpResponse &, const std::function<Core::Task<void>()> next) -> Core::Task<>
                {
                    seenByMiddleware.emplace_back(request.matchedRoute());
                    co_await next();
                });
        router.get("/hello", recordPattern);

        HttpRequest  request = makeRequest(HttpMethod::GET, "/hello");
        HttpResponse response;
        routeRequest(router, request, response);

        ASSERT_EQ(response.status(), 200);
        ASSERT_EQ(seenByHandler.size(), 1U);
        EXPECT_EQ(seenByHandler[0], "/hello") << "精确路由交回的应是注册原文";
        ASSERT_EQ(seenByMiddleware.size(), 1U);
        EXPECT_EQ(seenByMiddleware[0], "/hello") << "中间件跑在处理器之前，读到的必须已是这一条的命中";
    }

    /**
     * @brief 钉住：三条通路的模式各回各的注册原文，未命中回空
     */
    TEST(Router, MatchedRouteDistinguishesPatternWildcardAndNoMatch)
    {
        std::vector<std::string> seenByHandler;
        const auto               recordPattern = [&seenByHandler](HttpRequest &request, HttpResponse &response) -> Core::Task<void>
        {
            seenByHandler.emplace_back(request.matchedRoute());
            response.setBody("ok");
            co_return;
        };

        Router router;
        router.get("/user/:id", recordPattern);
        router.get("/static/*", recordPattern);
        router.get("/registered", textHandler("never", nullptr));

        for (const std::string_view path: {"/user/42", "/static/a/b.png"})
        {
            HttpRequest  request = makeRequest(HttpMethod::GET, std::string(path));
            HttpResponse response;
            routeRequest(router, request, response);
            EXPECT_EQ(response.status(), 200) << path;
        }
        // HEAD 复用 GET 时键仍是那条 GET 注册的模式——打点分组不该因为方法换了就多出一个桶
        HttpRequest  headRequest = makeRequest(HttpMethod::HEAD, "/user/7");
        HttpResponse headResponse;
        routeRequest(router, headRequest, headResponse);
        // 未命中：模式这一档必须是空，404 不能冒进任何注册路由的桶
        HttpRequest  missingRequest = makeRequest(HttpMethod::GET, "/missing");
        HttpResponse missingResponse;
        routeRequest(router, missingRequest, missingResponse);

        ASSERT_EQ(seenByHandler.size(), 3U);
        EXPECT_EQ(seenByHandler[0], "/user/:id") << "命中的是模式路由，交回的必须是模式而不是实际路径";
        EXPECT_EQ(seenByHandler[1], "/static/*") << "通配路由的键应含结尾的星号，与实际子路径区分开";
        EXPECT_EQ(seenByHandler[2], "/user/:id") << "HEAD 复用 GET 时键应仍是那条 GET 的模式";
        EXPECT_TRUE(missingRequest.matchedRoute().empty()) << "404 那一档带着上一条的模式，未命中就被打进了命中桶";
    }

    /**
     * @brief 钉住：请求对象跨报文复用时命中的模式必须跟着换
     * @details 会话按连接复用同一个请求对象；模式不清回去，下一条报文会冒用上一条的分组键，
     *          而 404 留着上一条的模式会把未命中打进命中路由的桶里——两头都得钉
     */
    TEST(Router, ClearsMatchedRouteAcrossRequestsOnOneConnection)
    {
        Router router;
        router.get("/hello", textHandler("world"));

        HttpRequest  request  = makeRequest(HttpMethod::GET, "/hello");
        HttpResponse response{};
        routeRequest(router, request, response);
        EXPECT_EQ(request.matchedRoute(), "/hello");

        request.reset();
        EXPECT_TRUE(request.matchedRoute().empty()) << "reset() 没清命中的模式：下一条报文会冒用上一条的分组键";

        request = makeRequest(HttpMethod::GET, "/missing");
        routeRequest(router, request, response);
        EXPECT_EQ(response.status(), 404);
        EXPECT_TRUE(request.matchedRoute().empty()) << "未命中却留着上一条的模式：404 会被打进命中路由的桶里";
    }

    // ============================================================================
    // 方法判定：UNKNOWN 不按通配处理
    // ============================================================================

    TEST(Router, RefusesUnrecognizedMethodEvenOnAnyMethodRoute)
    {
        Router           router;
        std::atomic<int> callCount{0};
        router.any("/probe", textHandler("wildcard", &callCount));

        // CONNECT / TRACE / M-SEARCH 经解析器都会落到 UNKNOWN
        HttpRequest  request = makeRequest(HttpMethod::UNKNOWN, "/probe");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(callCount.load(), 0);
        EXPECT_EQ(response.status(), 405);
        EXPECT_EQ(response.getHeader("allow").value_or(""), std::string(kAllRecognizedMethodNames));
    }

    TEST(Router, RunsAnyMethodRouteForEveryRecognizedMethod)
    {
        Router           router;
        std::atomic<int> callCount{0};
        router.any("/any", textHandler("answered", &callCount));

        for (const HttpMethod method: kAllRecognizedMethods)
        {
            HttpRequest  request = makeRequest(method, "/any");
            HttpResponse response;
            routeRequest(router, request, response);

            EXPECT_EQ(response.status(), 200) << "方法枚举值 " << static_cast<int>(method);
        }
        EXPECT_EQ(callCount.load(), static_cast<int>(kAllRecognizedMethods.size()));
    }

    TEST(Router, MatchesExplicitlyRegisteredMethodOnly)
    {
        Router           router;
        std::atomic<int> putCalls{0};
        std::atomic<int> optionsCalls{0};
        router.put("/resource", textHandler("replaced", &putCalls));
        router.options("/resource", textHandler("allowed", &optionsCalls));

        HttpRequest  putRequest = makeRequest(HttpMethod::PUT, "/resource");
        HttpResponse putResponse;
        routeRequest(router, putRequest, putResponse);
        EXPECT_EQ(putCalls.load(), 1);
        EXPECT_EQ(optionsCalls.load(), 0);

        HttpRequest  optionsRequest = makeRequest(HttpMethod::OPTIONS, "/resource");
        HttpResponse optionsResponse;
        routeRequest(router, optionsRequest, optionsResponse);
        EXPECT_EQ(optionsCalls.load(), 1);
        EXPECT_EQ(putCalls.load(), 1);
    }

    /**
     * @brief 钉住 del() 与 patch() 两个注册别名各自落在 DELETE 与 PATCH 上
     * @details 它们是注册入口里唯一没有直测的两个（del 因 delete 是 C++ 关键字而改名，更不容易看出
     *          登记到了哪个方法上）。写错方法枚举不会编译报错，而症状是「注册了却永远匹配不上」，
     *          连 405 的 Allow 都会跟着说错话
     */
    TEST(Router, RegistersDeleteAndPatchThroughTheirAliases)
    {
        Router           router;
        std::atomic<int> deleteCalls{0};
        std::atomic<int> patchCalls{0};
        router.del("/item", textHandler("removed", &deleteCalls));
        router.patch("/item", textHandler("merged", &patchCalls));

        HttpRequest  deleteRequest = makeRequest(HttpMethod::DELETE, "/item");
        HttpResponse deleteResponse;
        routeRequest(router, deleteRequest, deleteResponse);
        EXPECT_EQ(deleteResponse.status(), 200) << "del() 没有把路由登记到 DELETE 上";
        EXPECT_EQ(deleteResponse.body(), "removed");
        EXPECT_EQ(deleteCalls.load(), 1);
        EXPECT_EQ(patchCalls.load(), 0);

        HttpRequest  patchRequest = makeRequest(HttpMethod::PATCH, "/item");
        HttpResponse patchResponse;
        routeRequest(router, patchRequest, patchResponse);
        EXPECT_EQ(patchResponse.status(), 200) << "patch() 没有把路由登记到 PATCH 上";
        EXPECT_EQ(patchResponse.body(), "merged");
        EXPECT_EQ(patchCalls.load(), 1);

        // 对照：同一路径上没注册的方法必须 405，Allow 只列这两个方法。少了这条，
        // 「两个别名都注册成了 any()」这种错也会被前面的命中判成通过
        HttpRequest  getRequest = makeRequest(HttpMethod::GET, "/item");
        HttpResponse getResponse;
        routeRequest(router, getRequest, getResponse);
        EXPECT_EQ(getResponse.status(), 405) << "别名把路由放到了别的方法上，或注册成了任意方法";
        EXPECT_EQ(getResponse.getHeader("allow").value_or(std::string{}), "DELETE, PATCH");
        EXPECT_EQ(deleteCalls.load(), 1) << "未注册的 GET 打到了 DELETE 路由上";
    }

    // ============================================================================
    // 模式路由：具名参数
    // ============================================================================

    TEST(Router, ExtractsNamedSegmentIntoRequestParameter)
    {
        Router      router;
        std::string capturedIdentifier;
        router.get("/user/:id",
                   [&capturedIdentifier](HttpRequest &request, HttpResponse &response) -> Core::Task<void>
                   {
                       capturedIdentifier = request.param("id").value_or("");
                       response.setBody("profile");
                       co_return;
                   });

        HttpRequest  request = makeRequest(HttpMethod::GET, "/user/42");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(capturedIdentifier, "42");
        EXPECT_EQ(response.body(), "profile");
    }

    TEST(Router, RejectsNamedSegmentWhenPathSegmentIsEmpty)
    {
        Router           router;
        std::atomic<int> callCount{0};
        router.get("/user/:id", textHandler("profile", &callCount));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/user/");
        HttpResponse response;
        routeRequest(router, request, response);

        // "/user/" 不该被 "/user/:id" 命中并给出一个空的 id
        EXPECT_EQ(callCount.load(), 0);
        EXPECT_EQ(response.status(), 404);
    }

    TEST(Router, RejectsPatternShorterThanRequestPath)
    {
        Router           router;
        std::atomic<int> callCount{0};
        router.get("/files/:name", textHandler("download", &callCount));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/files/a/b");
        HttpResponse response;
        routeRequest(router, request, response);

        // 段数必须恰好相等，多出来的一段不再被默默吃掉
        EXPECT_EQ(callCount.load(), 0);
        EXPECT_EQ(response.status(), 404);
    }

    TEST(Router, DoesNotLeakParametersFromFailedCandidateRoute)
    {
        Router           router;
        std::atomic<int> failingCalls{0};
        std::string      capturedGamma;
        // 第一条候选会先收下 :alpha=7 再在下一段失配；它攒的参数绝不能留在请求上
        router.get("/p/:alpha/:beta", textHandler("never", &failingCalls));
        router.get("/p/:gamma",
                   [&capturedGamma](HttpRequest &request, HttpResponse &response) -> Core::Task<void>
                   {
                       capturedGamma = request.param("gamma").value_or("");
                       response.setBody("matched");
                       co_return;
                   });

        HttpRequest  request = makeRequest(HttpMethod::GET, "/p/7");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(failingCalls.load(), 0);
        EXPECT_EQ(capturedGamma, "7");
        EXPECT_FALSE(request.param("alpha").has_value());
    }

    TEST(Router, KeepsPreexistingParameterInsteadOfOverwritingIt)
    {
        Router router;
        router.get("/user/:id", textHandler("profile"));

        HttpRequest request = makeRequest(HttpMethod::GET, "/user/42");
        request.setParam("id", "preset");
        HttpResponse response;
        routeRequest(router, request, response);

        // 提交策略是「先到先得、不覆盖」，便于外部提前塞入调试参数
        EXPECT_EQ(request.param("id").value_or(""), "preset");
        EXPECT_EQ(response.body(), "profile");
    }

    // ============================================================================
    // 模式路由：通配
    // ============================================================================

    TEST(Router, CapturesWildcardRemainderUnderWildcardParameterName)
    {
        Router      router;
        std::string capturedRemainder;
        router.get("/static/*",
                   [&capturedRemainder](HttpRequest &request, HttpResponse &response) -> Core::Task<void>
                   {
                       capturedRemainder = request.param(std::string(kWildcardParameterName)).value_or("");
                       response.setBody("asset");
                       co_return;
                   });

        HttpRequest  request = makeRequest(HttpMethod::GET, "/static/css/main.css");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(capturedRemainder, "css/main.css");
        EXPECT_EQ(response.body(), "asset");
    }

    TEST(Router, RefusesWildcardRouteWhenSlashBoundaryMissing)
    {
        Router           router;
        std::atomic<int> callCount{0};
        router.get("/static/*", textHandler("asset", &callCount));

        // 前缀匹配必须停在段边界，否则 "/static" 会命中 "/staticevil"：目录穿越的入口
        HttpRequest  request = makeRequest(HttpMethod::GET, "/staticevil/secret");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(callCount.load(), 0);
        EXPECT_EQ(response.status(), 404);
    }

    TEST(Router, TreatsWildcardWithoutSlashBoundaryAsLiteralSegment)
    {
        Router           router;
        std::atomic<int> callCount{0};
        router.get("/static*", textHandler("asset", &callCount));

        HttpRequest  evilRequest = makeRequest(HttpMethod::GET, "/staticevil");
        HttpResponse evilResponse;
        routeRequest(router, evilRequest, evilResponse);
        EXPECT_EQ(callCount.load(), 0);
        EXPECT_EQ(evilResponse.status(), 404);

        // 不肯把「前缀贴着 *」解释成通配，宁可整条按字面处理
        HttpRequest  literalRequest = makeRequest(HttpMethod::GET, "/static*");
        HttpResponse literalResponse;
        routeRequest(router, literalRequest, literalResponse);
        EXPECT_EQ(callCount.load(), 1);
        EXPECT_EQ(literalResponse.body(), "asset");
    }

    TEST(Router, MatchesBareWildcardRouteAgainstAnyPath)
    {
        Router      router;
        std::string capturedRemainder;
        router.get("*",
                   [&capturedRemainder](HttpRequest &request, HttpResponse &response) -> Core::Task<void>
                   {
                       capturedRemainder = request.param(std::string(kWildcardParameterName)).value_or("");
                       response.setBody("fallback");
                       co_return;
                   });

        HttpRequest  request = makeRequest(HttpMethod::GET, "/anything/at/all");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(capturedRemainder, "anything/at/all");
        EXPECT_EQ(response.body(), "fallback");
    }

    // ============================================================================
    // 优先级
    // ============================================================================

    TEST(Router, PrefersLiteralRouteOverPatternRegisteredEarlier)
    {
        Router           router;
        std::atomic<int> patternCalls{0};
        std::atomic<int> literalCalls{0};
        router.get("/user/:id", textHandler("pattern", &patternCalls));
        router.get("/user/me", textHandler("literal", &literalCalls));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/user/me");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(literalCalls.load(), 1);
        EXPECT_EQ(patternCalls.load(), 0);
        EXPECT_EQ(response.body(), "literal");
    }

    TEST(Router, UsesFirstRegisteredPatternWhenBothCandidatesMatch)
    {
        Router           router;
        std::atomic<int> genericCalls{0};
        std::atomic<int> specificCalls{0};
        router.get("/:first/:second", textHandler("generic", &genericCalls));
        router.get("/x/:second", textHandler("specific", &specificCalls));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/x/1");
        HttpResponse response;
        routeRequest(router, request, response);

        // 同一层内先到先得，不做「谁更具体谁优先」的第二套仲裁
        EXPECT_EQ(genericCalls.load(), 1);
        EXPECT_EQ(specificCalls.load(), 0);
        EXPECT_EQ(request.param("first").value_or(""), "x");
        EXPECT_EQ(request.param("second").value_or(""), "1");
    }

    TEST(Router, ReplacesHandlerForSameMethodAndPathInPlace)
    {
        Router           router;
        std::atomic<int> firstCalls{0};
        std::atomic<int> secondCalls{0};
        router.get("/dup", textHandler("first", &firstCalls));
        router.get("/dup", textHandler("second", &secondCalls));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/dup");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(firstCalls.load(), 0);
        EXPECT_EQ(secondCalls.load(), 1);
        EXPECT_EQ(response.body(), "second");
    }

    TEST(Router, KeepsRegistrationPositionWhenPatternRouteIsReplaced)
    {
        Router           router;
        std::atomic<int> replacedCalls{0};
        std::atomic<int> laterCalls{0};
        router.get("/a/:shared", textHandler("first", &replacedCalls));
        router.get("/a/:other", textHandler("second", &laterCalls));

        // 就地替换第一条：它仍在第二条之前，先到先得关系不变
        router.get("/a/:shared", textHandler("replaced", &replacedCalls));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/a/1");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(replacedCalls.load(), 1);
        EXPECT_EQ(laterCalls.load(), 0);
        EXPECT_EQ(response.body(), "replaced");
    }

    // ============================================================================
    // 响应收尾：HEAD 正文交发送路径 / 204 / 304
    // ============================================================================

    TEST(Router, LeavesHeadResponseBodyForSessionToSuppress)
    {
        Router router;
        router.head("/document", textHandler("1234567"));

        HttpRequest  request = makeRequest(HttpMethod::HEAD, "/document");
        HttpResponse response;
        routeRequest(router, request, response);

        // 正文刻意不在路由层剥：头部要按完整正文序列化，才能与同一路径的 GET 逐字节一致。
        // 「不取正文地问一次 GET 会给多大」也仍成立——content-length 由序列化层按正文真实长度补齐
        EXPECT_EQ(response.body(), "1234567");
        EXPECT_NE(response.serializeHead().find("content-length: 7"), std::string::npos);
    }

    TEST(Router, KeepsContentLengthDeclaredByHeadHandler)
    {
        Router router;
        router.head("/document",
                    [](HttpRequest &, HttpResponse &response) -> Core::Task<void>
                    {
                        response.setHeader("content-length", "999");
                        response.setBody("ignored");
                        co_return;
                    });

        HttpRequest  request = makeRequest(HttpMethod::HEAD, "/document");
        HttpResponse response;
        routeRequest(router, request, response);

        // 处理函数自己声明的 content-length 不被覆盖：静态文件服务正是靠它省掉读一遍文件
        EXPECT_EQ(response.getHeader("content-length").value_or(""), "999");
        EXPECT_EQ(response.body(), "ignored");
    }

    TEST(Router, ClearsBodyForNoContentResponse)
    {
        Router router;
        router.get("/no-content",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<void>
                   {
                       response.setStatus(204);
                       response.setBody("leftover");
                       co_return;
                   });

        HttpRequest  request = makeRequest(HttpMethod::GET, "/no-content");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(response.status(), 204);
        EXPECT_TRUE(response.body().empty());
    }

    TEST(Router, ClearsBodyForNotModifiedResponse)
    {
        Router router;
        router.get("/cached",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<void>
                   {
                       response.setStatus(304);
                       response.setBody("leftover");
                       co_return;
                   });

        HttpRequest  request = makeRequest(HttpMethod::GET, "/cached");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(response.status(), 304);
        EXPECT_TRUE(response.body().empty());
    }

    // ============================================================================
    // 中间件串联
    // ============================================================================

    TEST(Router, WrapsMatchedHandlerWithGlobalMiddleware)
    {
        Router                   router;
        std::vector<std::string> executionOrder;
        router.addMiddleware(
                [&executionOrder](HttpRequest &, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
                {
                    executionOrder.emplace_back("before");
                    response.setHeader("x-trace", "set-by-middleware");
                    co_await next();
                    executionOrder.emplace_back("after");
                });
        router.get("/traced",
                   [&executionOrder](HttpRequest &, HttpResponse &response) -> Core::Task<void>
                   {
                       executionOrder.emplace_back("handler");
                       response.setBody("traced");
                       co_return;
                   });

        HttpRequest  request = makeRequest(HttpMethod::GET, "/traced");
        HttpResponse response;
        routeRequest(router, request, response);

        const std::vector<std::string> expectedOrder{"before", "handler", "after"};
        EXPECT_EQ(executionOrder, expectedOrder);
        EXPECT_EQ(response.getHeader("x-trace").value_or(""), "set-by-middleware");
        EXPECT_EQ(response.body(), "traced");
    }

    TEST(Router, PropagatesHandlerExceptionToCaller)
    {
        Router router;
        router.get("/throws",
                   [](HttpRequest &, HttpResponse &) -> Core::Task<void>
                   {
                       throw std::runtime_error("handler failed");
                       co_return;
                   });

        HttpRequest  request = makeRequest(HttpMethod::GET, "/throws");
        HttpResponse response;

        // 路由器不吞也不翻译异常，由会话统一转成 500
        auto routeTask = router.route(request, response);
        routeTask.handle().resume();
        ASSERT_TRUE(routeTask.isReady());
        EXPECT_THROW(static_cast<void>(routeTask.handle().promise().result()), std::runtime_error);
    }

    TEST(Router, DetectsStreamingRouteAcrossParameterizedAndWildcardPaths)
    {
        // 钉住 hasStreamingRoute 走 matchesPattern 的两条参数化分支（":id" 与 '*'）：
        // 流式探测刻意不收集参数（省掉剩余路径与每段 ":name" 的 std::string 构造），
        // 本用例保证「跳过收集」不会改变命中与否、以及是否流式的判定结果
        Router router;
        router.postStreaming("/upload/:id", textHandler("uploading"));
        router.putStreaming("/replace/:id", textHandler("replacing"));
        router.postStreaming("/assets/*", textHandler("streaming-assets"));
        router.get("/plain", textHandler("plain")); // 非流式，作对照

        EXPECT_TRUE(router.hasStreamingRoute(HttpMethod::POST, "/upload/42", {})) << ":id 段命中的流式路由必须被探测为流式";
        EXPECT_TRUE(router.hasStreamingRoute(HttpMethod::PUT, "/replace/42", {})) << "PUT 绑定的流式路由同样要探测为流式";
        EXPECT_TRUE(router.hasStreamingRoute(HttpMethod::POST, "/assets/css/main.css", {})) << "通配路由命中即算流式，剩余路径不收集也不影响判定";
        EXPECT_TRUE(router.hasStreamingRoute(HttpMethod::POST, "/upload/42?token=abc", {})) << "带查询串时要先按 route() 同口径裁出路径部分再判定";
        // 流式只绑 POST/PUT：其余方法在方法层即不可能命中，直接 false（不扫表）
        EXPECT_FALSE(router.hasStreamingRoute(HttpMethod::GET, "/upload/42", {})) << "GET 不该命中只按 POST 注册的流式路由";
        EXPECT_FALSE(router.hasStreamingRoute(HttpMethod::HEAD, "/assets/x", {})) << "HEAD 复用 GET 是针对普通路由的，流式判定不涉及";
        EXPECT_FALSE(router.hasStreamingRoute(HttpMethod::GET, "/plain", {})) << "非流式路由不得被探测为流式";
        EXPECT_FALSE(router.hasStreamingRoute(HttpMethod::POST, "/nowhere", {})) << "未注册路径一律非流式";
        EXPECT_FALSE(router.hasStreamingRoute(HttpMethod::UNKNOWN, "/assets/any", {})) << "未收录方法不参与匹配，更不能被判为流式";
    }
    // ============================================================================
    // 虚拟主机（按 Host 选站）
    // ============================================================================

    /**
     * @brief 命中的主机只走自己的表，根表的路由不参与
     * @details 「api.example.com 上有没有 /private」与「默认站上有没有 /private」是两回事：
     *          两张表混在一起比优先级，等于让一个域名拿到另一个域名的路由
     */
    TEST(RouterVirtualHost, ServesHostRoutesAndKeepsRootTableAsDefaultSite)
    {
        Router router;
        router.get("/site", textHandler("default-site"));
        router.get("/only-root", textHandler("root-only"));
        router.virtualHost("api.example.com").get("/site", textHandler("api-site"));

        {
            HttpRequest  request = makeRequest(HttpMethod::GET, "/site", "api.example.com");
            HttpResponse response;
            routeRequest(router, request, response);
            EXPECT_EQ(response.body(), "api-site");
        }
        {
            // 没匹配上任何主机的 Host 交回根表：单站点行为原样保留，「先起服务再配域名」的中间态不该红
            HttpRequest  request = makeRequest(HttpMethod::GET, "/site", "other.example.org");
            HttpResponse response;
            routeRequest(router, request, response);
            EXPECT_EQ(response.body(), "default-site");
        }
        {
            // 主机表上没有这条路径 → 404，而不是回落去问根表
            HttpRequest  request = makeRequest(HttpMethod::GET, "/only-root", "api.example.com");
            HttpResponse response;
            routeRequest(router, request, response);
            EXPECT_EQ(response.status(), 404) << "命中的主机没这条路径却由根表应答，等于把站点隔离漏掉一半";
        }
    }

    /**
     * @brief 精确主机名优先于通配主机名
     */
    TEST(RouterVirtualHost, ExactHostNameWinsOverWildcard)
    {
        Router router;
        router.virtualHost("*.example.com").get("/who", textHandler("wildcard"));
        router.virtualHost("api.example.com").get("/who", textHandler("exact"));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/who", "api.example.com");
        HttpResponse response;
        routeRequest(router, request, response);
        EXPECT_EQ(response.body(), "exact") << "通配条先注册就赢，选站就变成看注册顺序而不是看具体程度";
    }

    /**
     * @brief 通配主机之间取后缀最长者
     */
    TEST(RouterVirtualHost, LongestWildcardSuffixWins)
    {
        Router router;
        router.virtualHost("*.example.com").get("/who", textHandler("broad"));
        router.virtualHost("*.co.example.com").get("/who", textHandler("narrow"));

        HttpRequest  broad = makeRequest(HttpMethod::GET, "/who", "x.example.com");
        HttpResponse broadResponse;
        routeRequest(router, broad, broadResponse);
        EXPECT_EQ(broadResponse.body(), "broad");

        HttpRequest  narrow = makeRequest(HttpMethod::GET, "/who", "x.co.example.com");
        HttpResponse narrowResponse;
        routeRequest(router, narrow, narrowResponse);
        EXPECT_EQ(narrowResponse.body(), "narrow");
    }

    /**
     * @brief 通配不收顶点域名本身
     * @details "*.example.com" 的 '*' 要吃掉至少一个标签：example.com 自己得单独登记
     */
    TEST(RouterVirtualHost, WildcardDoesNotMatchTheApexItself)
    {
        Router router;
        router.virtualHost("*.example.com").get("/who", textHandler("wildcard"));
        router.get("/who", textHandler("default-site"));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/who", "example.com");
        HttpResponse response;
        routeRequest(router, request, response);
        EXPECT_EQ(response.body(), "default-site") << "把顶点域名也收进通配，等于凭空多出一个没人登记的站点";
    }

    /**
     * @brief 比对键不看大小写、端口与结尾的根点
     */
    TEST(RouterVirtualHost, HostKeyIgnoresCasePortAndTrailingDot)
    {
        Router router;
        router.virtualHost("API.Example.COM").get("/who", textHandler("api"));
        router.virtualHost("[::1]").get("/who", textHandler("loopback-v6"));

        for (const std::string_view host: {"api.example.com", "API.Example.COM:443", "api.example.com.", "Api.Example.Com."})
        {
            HttpRequest  request = makeRequest(HttpMethod::GET, "/who", host);
            HttpResponse response;
            routeRequest(router, request, response);
            EXPECT_EQ(response.body(), "api") << "同一个站点因写法不同落到了默认站上：" << host;
        }

        // IPv6 字面量的端口写在方括号之后，剥端口不能把括号里第一个冒号当端口分隔符
        HttpRequest  v6 = makeRequest(HttpMethod::GET, "/who", "[::1]:8443");
        HttpResponse v6Reply;
        routeRequest(router, v6, v6Reply);
        EXPECT_EQ(v6Reply.body(), "loopback-v6") << "IPv6 字面量被当成带端口的名字切了一半";
    }

    /**
     * @brief 没带 Host 的请求走默认站点
     * @details HTTP/1.0 式请求与直连 IP 的探活都不带 Host，判 404 会让健康检查先炸
     */
    TEST(RouterVirtualHost, RequestWithoutHostHeaderFallsBackToDefaultSite)
    {
        Router router;
        router.get("/who", textHandler("default-site"));
        router.virtualHost("api.example.com").get("/who", textHandler("api"));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/who");
        HttpResponse response;
        routeRequest(router, request, response);
        EXPECT_EQ(response.body(), "default-site");
        EXPECT_TRUE(router.hasVirtualHosts());
    }

    /**
     * @brief 根中间件对所有主机都生效，且套在主机中间件的外层
     * @details 鉴权、CORS、访问日志登记在根路由上，加一个主机就把它们绕过去是安全事故的形状
     */
    TEST(RouterVirtualHost, RootMiddlewareWrapsHostMiddleware)
    {
        Router                   router;
        std::vector<std::string> executionOrder;
        router.addMiddleware(
                [&executionOrder](HttpRequest &, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
                {
                    executionOrder.emplace_back("root-before");
                    response.setHeader("x-root", "1");
                    co_await next();
                    executionOrder.emplace_back("root-after");
                });
        Router &apiSite = router.virtualHost("api.example.com");
        apiSite.addMiddleware(
                [&executionOrder](HttpRequest &, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
                {
                    executionOrder.emplace_back("host-before");
                    response.setHeader("x-host", "api");
                    co_await next();
                    executionOrder.emplace_back("host-after");
                });
        apiSite.get("/who",
                    [&executionOrder](HttpRequest &, HttpResponse &response) -> Core::Task<void>
                    {
                        executionOrder.emplace_back("handler");
                        response.setBody("api");
                        co_return;
                    });

        HttpRequest  request = makeRequest(HttpMethod::GET, "/who", "api.example.com");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(response.body(), "api");
        EXPECT_EQ(executionOrder, (std::vector<std::string>{"root-before", "host-before", "handler", "host-after", "root-after"}));
        EXPECT_TRUE(response.hasHeader("x-root")) << "外层中间件写的头部没出来，说明选站把根管道跳过了";
        EXPECT_TRUE(response.hasHeader("x-host"));
    }

    /**
     * @brief 主机表回 404 时，根中间件写下的头部必须留下
     * @details 这是「里层不许复位响应」那条规则的落点：复位一擦，跨域请求撞到 404 就退化成
     *          浏览器侧的 opaque 错误，日志里也看不到 CORS 头部
     */
    TEST(RouterVirtualHost, NotFoundOnHostSiteKeepsRootMiddlewareHeaders)
    {
        Router router;
        router.addMiddleware(
                [](HttpRequest &, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<>
                {
                    response.setHeader("access-control-allow-origin", "*");
                    co_await next();
                });
        router.virtualHost("api.example.com").get("/known", textHandler("api"));

        HttpRequest  request = makeRequest(HttpMethod::GET, "/unknown", "api.example.com");
        HttpResponse response;
        routeRequest(router, request, response);

        EXPECT_EQ(response.status(), 404);
        EXPECT_EQ(response.getHeader("access-control-allow-origin").value_or(""), "*") << "里层表复位响应把外层中间件的 CORS 头抹了";
    }

    /**
     * @brief 注册侧的两条拒绝：子表上不再选站、没有意义的主机名
     */
    TEST(RouterVirtualHost, RejectsNestedVirtualHostsAndMeaninglessNames)
    {
        Router  router;
        Router &apiSite = router.virtualHost("api.example.com");
        EXPECT_THROW(apiSite.virtualHost("inner.example.com"), Base::InvalidArgumentException) << "两层选站要扯清中间件谁套谁，那不是能推出来的答案";

        for (const std::string_view meaningless: {"", "*", "*.", ":", "  "})
        {
            Router plain;
            EXPECT_THROW(plain.virtualHost(std::string(meaningless)), Base::InvalidArgumentException) << "收下「" << meaningless << "」就是一条永不命中的规则";
        }

        // 重复登记交回同一张表：与 get() 的「同 (方法, 路径) 重复注册幂等」同口径
        Router           counted;
        std::atomic<int> callCount{0};
        counted.virtualHost("api.example.com").get("/who", textHandler("first", &callCount));
        counted.virtualHost("API.EXAMPLE.COM").get("/who", textHandler("second", &callCount));
        HttpRequest  request = makeRequest(HttpMethod::GET, "/who", "api.example.com:8080");
        HttpResponse response;
        routeRequest(counted, request, response);
        EXPECT_EQ(response.body(), "second") << "同站重复登记开出两张表，前一张就成了没人走的路由";
        EXPECT_EQ(callCount.load(), 1);
    }

    /**
     * @brief 流式派发判定要跟着选站走
     * @details 主机表注册的流式路由若只查根表就被判成普通路由：会话等整份正文收齐才派发，
     *          而处理函数只读 bodyStream()，那条请求的正文就没人收了
     */
    TEST(RouterVirtualHost, StreamingRouteIsDetectedOnlyForItsOwnHost)
    {
        Router router;
        router.post("/upload", [](HttpRequest &, HttpResponse &) -> Core::Task<void> { co_return; });
        router.virtualHost("api.example.com").postStreaming("/upload", [](HttpRequest &, HttpResponse &) -> Core::Task<void> { co_return; });

        EXPECT_TRUE(router.hasStreamingRoute(HttpMethod::POST, "/upload", "api.example.com")) << "主机表上的流式路由没被探测出来，正文会被整份缓冲";
        EXPECT_TRUE(router.hasStreamingRoute(HttpMethod::POST, "/upload", "api.example.com:443")) << "带端口的 Host 归一化后应是同一个站点";
        EXPECT_FALSE(router.hasStreamingRoute(HttpMethod::POST, "/upload", "other.example.org")) << "别的 Host 命中根表，那里注册的是普通路由";
        EXPECT_FALSE(router.hasStreamingRoute(HttpMethod::POST, "/upload", {})) << "没带 Host 的请求走默认站点，那里也是普通路由";
    }
} // namespace AsynGyanis::Net

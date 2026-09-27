// tracingSpanMiddleware 用例：一节 HTTP 服务端链路该有的样子，以及头部改写的边界
//
// 中间件本身不涉及网络，所以这里直接按同步路径驱动它（链上没有任何真实挂起点），
// 断言落在「交出去的那一条节」与「请求上那条 traceparent」两处——它们分别是下游能看见的
// 上半与下半。

#include "Net/Tracing/HttpTracingMiddleware.h"

#include "Base/Exception/LogicException.h"
#include "Core/Coroutine/Task.h"
#include "Net/Http/HttpMethod.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Middleware.h"
#include "Net/Http/TraceContext.h"
#include "Net/Tracing/Span.h"
#include "Net/Tracing/Tracer.h"
#include "TracingTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using AsynGyanis::Core::Task;
    using AsynGyanis::Net::extractTraceContext;
    using AsynGyanis::Net::HttpMethod;
    using AsynGyanis::Net::HttpRequest;
    using AsynGyanis::Net::HttpResponse;
    using AsynGyanis::Net::MiddlewareFunc;
    using AsynGyanis::Net::SpanAttribute;
    using AsynGyanis::Net::SpanKind;
    using AsynGyanis::Net::SpanRecord;
    using AsynGyanis::Net::SpanStatusCode;
    using AsynGyanis::Net::TraceIdentifiers;
    using AsynGyanis::Net::Tracer;
    using AsynGyanis::Net::tracingSpanMiddleware;
    using AsynGyanis::Net::TestSupport::CapturingSpanExporter;

    /// 造一条只有方法、路径与（可选）上游上下文的请求
    HttpRequest makeRequest(const HttpMethod method, const std::string &uri, const std::string &traceparent = {})
    {
        HttpRequest request;
        request.setMethod(method);
        request.setUri(uri);
        if (!traceparent.empty())
        {
            static_cast<void>(request.setHeader(AsynGyanis::Net::kTraceparentHeaderName, traceparent));
        }
        return request;
    }

    /// 建一具全采的编排器并挂上记录型出口
    std::shared_ptr<Tracer> makeTracer(const std::shared_ptr<CapturingSpanExporter> &exporter)
    {
        Tracer::Configuration configuration;
        configuration.serviceName          = "middleware-test";
        configuration.exportBatchSpanCount = 1U;
        configuration.exportInterval       = std::chrono::milliseconds{60000};
        auto tracer                        = Tracer::create(configuration);
        tracer->addExporter(exporter);
        return tracer;
    }

    /**
     * @brief 同步跑完一次中间件调用
     * @param middleware 被测中间件
     * @param request 请求对象（跑完之后用例还要读它身上的头部）
     * @param response 响应对象
     * @param next 下游：不调用它就等于处理器短路
     * @return true 下游正常返回；false 下游抛出，异常已按原样传到这里
     */
    bool runMiddleware(const MiddlewareFunc &middleware, HttpRequest &request, HttpResponse &response, const std::function<Task<void>()> &next)
    {
        Task<void> chain = middleware(request, response, next);
        static_cast<void>(chain.handle().resume());
        EXPECT_TRUE(chain.isReady()) << "中间件链未在同步路径上跑完：测试链里不得真实挂起";
        try
        {
            chain.handle().promise().result();
        } catch (const std::runtime_error &)
        {
            // 会话层原本就会接住处理器的异常；这里只把它回传给用例
            return false;
        }
        return true;
    }

    /// 在响应上写一个状态码然后正常结束的下游
    std::function<Task<void>()> terminalWithStatus(HttpResponse &response, const int statusCode)
    {
        return [&response, statusCode]() -> Task<void>
        {
            response.setStatus(statusCode);
            co_return;
        };
    }

    /// 取那条已交出去的节（本文件的用例都只跑一条请求）
    SpanRecord singleRecordOf(const std::shared_ptr<CapturingSpanExporter> &exporter)
    {
        EXPECT_EQ(exporter->recordCount(), 1U);
        return exporter->records().front();
    }

    /// 在一节的维度里找一个键
    const SpanAttribute *findAttribute(const SpanRecord &record, const std::string_view key)
    {
        for (const SpanAttribute &attribute: record.attributes)
        {
            if (attribute.key == key)
            {
                return &attribute;
            }
        }
        return nullptr;
    }
} // namespace

/**
 * @brief 一条被采的请求落成服务端一节：方法、路径、状态码各成一条维度，头部指回本节
 */
TEST(HttpTracingMiddleware, SampledRequestYieldsAServerSpanAndRepointsTheHeader)
{
    const auto exporter   = std::make_shared<CapturingSpanExporter>();
    const auto tracer     = makeTracer(exporter);
    const auto middleware = tracingSpanMiddleware(tracer);

    HttpResponse response;
    HttpRequest  request = makeRequest(HttpMethod::GET, "/orders?page=2");
    ASSERT_TRUE(runMiddleware(middleware, request, response, terminalWithStatus(response, 201)));
    tracer->flush();

    const SpanRecord record = singleRecordOf(exporter);
    EXPECT_EQ(record.kind, SpanKind::Server);
    EXPECT_EQ(record.name, "GET") << "名字只取方法原文：路径是无界的，进名字会把检索侧的分组打碎";
    const SpanAttribute *methodAttribute = findAttribute(record, "http.request.method");
    ASSERT_TRUE(methodAttribute != nullptr);
    EXPECT_EQ(std::get<std::string>(methodAttribute->value), "GET");
    const SpanAttribute *pathAttribute = findAttribute(record, "url.path");
    ASSERT_TRUE(pathAttribute != nullptr);
    EXPECT_EQ(std::get<std::string>(pathAttribute->value), "/orders") << "查询串不进路径维度";
    const SpanAttribute *statusAttribute = findAttribute(record, "http.response.status_code");
    ASSERT_TRUE(statusAttribute != nullptr);
    EXPECT_EQ(std::get<std::int64_t>(statusAttribute->value), 201);
    EXPECT_EQ(record.status, SpanStatusCode::Ok);

    // 头部指回本节：下游从头部读到的段标识就是这一节的标识，嵌套因此是自动的
    const std::optional<TraceIdentifiers> repointed = extractTraceContext(request);
    ASSERT_TRUE(repointed.has_value());
    EXPECT_EQ(repointed->traceIdText(), record.identity.traceIdText());
    EXPECT_EQ(repointed->parentIdText(), record.identity.spanIdText());
    EXPECT_TRUE(repointed->isSampled());
}

/**
 * @brief 上游给了上下文就照办：沿用同一条链路，把上游的段标识当作上一节
 */
TEST(HttpTracingMiddleware, UpstreamContextBecomesTheParent)
{
    const auto exporter   = std::make_shared<CapturingSpanExporter>();
    const auto tracer     = makeTracer(exporter);
    const auto middleware = tracingSpanMiddleware(tracer);

    const TraceIdentifiers upstream = AsynGyanis::Net::Traceparent::generate(true);
    HttpResponse           response;
    HttpRequest            request = makeRequest(HttpMethod::POST, "/orders", AsynGyanis::Net::Traceparent::value(upstream));
    ASSERT_TRUE(runMiddleware(middleware, request, response, terminalWithStatus(response, 200)));
    tracer->flush();

    const SpanRecord record = singleRecordOf(exporter);
    EXPECT_EQ(record.identity.traceIdText(), upstream.traceIdText());
    EXPECT_EQ(record.identity.parentSpanIdText(), upstream.parentIdText());
    EXPECT_NE(record.identity.spanIdText(), upstream.parentIdText()) << "本节必须换一个新段标识，否则与上游同节";
    // 改写之后的头部把本节当作下一跳的上级
    const std::optional<TraceIdentifiers> repointed = extractTraceContext(request);
    ASSERT_TRUE(repointed.has_value());
    EXPECT_EQ(repointed->parentIdText(), record.identity.spanIdText());
}

/**
 * @brief 上游判定为不采：一节也不交，但头部仍指向这一跳的上下文
 */
TEST(HttpTracingMiddleware, UnsampledUpstreamRecordsNothing)
{
    const auto exporter   = std::make_shared<CapturingSpanExporter>();
    const auto tracer     = makeTracer(exporter);
    const auto middleware = tracingSpanMiddleware(tracer);

    TraceIdentifiers upstream = AsynGyanis::Net::Traceparent::generate(false);
    HttpResponse     response;
    HttpRequest      request = makeRequest(HttpMethod::GET, "/healthz", AsynGyanis::Net::Traceparent::value(upstream));
    ASSERT_TRUE(runMiddleware(middleware, request, response, terminalWithStatus(response, 200)));
    tracer->flush();

    EXPECT_EQ(exporter->recordCount(), 0U);
    const std::optional<TraceIdentifiers> repointed = extractTraceContext(request);
    ASSERT_TRUE(repointed.has_value());
    EXPECT_EQ(repointed->traceIdText(), upstream.traceIdText());
    EXPECT_FALSE(repointed->isSampled()) << "采样位要在往下传的路上保持为 0";
    EXPECT_NE(repointed->parentIdText(), upstream.parentIdText());
}

/**
 * @brief 看不懂的更高版本原样转发：一个字都不改，代价是本节的标识不体现在头部里
 */
TEST(HttpTracingMiddleware, PassesAnUnknownVersionHeaderThroughUnchanged)
{
    const auto exporter   = std::make_shared<CapturingSpanExporter>();
    const auto tracer     = makeTracer(exporter);
    const auto middleware = tracingSpanMiddleware(tracer);

    const std::string futureVersion = "01-0123456789abcdef0123456789abcdef-fedcba9876543210-03-extra";
    HttpResponse      response;
    HttpRequest       request = makeRequest(HttpMethod::GET, "/any", futureVersion);
    ASSERT_TRUE(runMiddleware(middleware, request, response, terminalWithStatus(response, 200)));
    tracer->flush();

    EXPECT_EQ(request.firstHeaderValueView(AsynGyanis::Net::kTraceparentHeaderName).value_or(std::string_view{}), futureVersion) << "重排别人写的字段不是本中间件的权限";
    const SpanRecord record = singleRecordOf(exporter);
    EXPECT_EQ(record.identity.traceIdText(), "0123456789abcdef0123456789abcdef") << "链路标识照旧沿用";
    EXPECT_EQ(record.identity.parentSpanIdText(), "fedcba9876543210");
}

/**
 * @brief 5xx 判失败、4xx 不判：后者是按业务规则给出的正常答复
 */
TEST(HttpTracingMiddleware, MarksOnlyServerErrorAsFailure)
{
    for (const int statusCode: {404, 500})
    {
        const auto exporter   = std::make_shared<CapturingSpanExporter>();
        const auto tracer     = makeTracer(exporter);
        const auto middleware = tracingSpanMiddleware(tracer);

        HttpResponse response;
        HttpRequest  request = makeRequest(HttpMethod::GET, "/missing");
        ASSERT_TRUE(runMiddleware(middleware, request, response, terminalWithStatus(response, statusCode)));
        tracer->flush();

        const SpanRecord record = singleRecordOf(exporter);
        EXPECT_EQ(record.status, statusCode >= 500 ? SpanStatusCode::Error : SpanStatusCode::Ok) << "状态码 " << statusCode;
    }
}

/**
 * @brief 处理器抛出时这一节仍带着失败结局交出去，异常照原样往上回
 */
TEST(HttpTracingMiddleware, HandlerExceptionStillDeliversAnErrorSpan)
{
    const auto exporter   = std::make_shared<CapturingSpanExporter>();
    const auto tracer     = makeTracer(exporter);
    const auto middleware = tracingSpanMiddleware(tracer);

    HttpResponse response;
    HttpRequest  request          = makeRequest(HttpMethod::PUT, "/boom");
    const auto   throwingTerminal = []() -> Task<void>
    {
        throw std::runtime_error("处理器炸了");
        co_return;
    };
    EXPECT_FALSE(runMiddleware(middleware, request, response, throwingTerminal));
    tracer->flush();

    const SpanRecord record = singleRecordOf(exporter);
    EXPECT_EQ(record.status, SpanStatusCode::Error);
    EXPECT_EQ(record.statusMessage, "处理函数抛出异常");
    EXPECT_EQ(findAttribute(record, "http.response.status_code"), nullptr) << "没走到写状态码的那一步，就不该凭空补一条";
}

/**
 * @brief 空的编排器是配置错误：当场抛出而不是悄悄一条都不记
 */
TEST(HttpTracingMiddleware, RejectsANullTracerAtCreation)
{
    EXPECT_THROW(static_cast<void>(tracingSpanMiddleware(nullptr)), AsynGyanis::Base::LogicException);
    try
    {
        static_cast<void>(tracingSpanMiddleware(nullptr));
    } catch (const AsynGyanis::Base::LogicException &exception)
    {
        EXPECT_NE(std::string{exception.what()}.find("Tracer::create"), std::string::npos) << exception.what();
    }
}

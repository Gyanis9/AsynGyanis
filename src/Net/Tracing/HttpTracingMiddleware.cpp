#include "Net/Tracing/HttpTracingMiddleware.h"

#include "Base/Exception/LogicException.h"
#include "Net/Http/HttpMethod.h"
#include "Net/Http/TraceContext.h"
#include "Net/Tracing/Span.h"

#include <exception>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 语义约定里的维度名：方法与路径各自成一条，检索侧要能分开查
        constexpr std::string_view kMethodAttributeName     = "http.request.method";
        constexpr std::string_view kPathAttributeName       = "url.path";
        constexpr std::string_view kStatusCodeAttributeName = "http.response.status_code";
    } // namespace

    MiddlewareFunc tracingSpanMiddleware(std::shared_ptr<Tracer> tracer)
    {
        if (tracer == nullptr)
        {
            throw Base::LogicException("链路中间件无法创建：传进来的 tracer 是空的。"
                                       "请先用 Tracer::create() 拿到编排器再交给本工厂；不记链路就不要挂这条中间件");
        }

        return [tracer = std::move(tracer)](HttpRequest &request, HttpResponse &response, const std::function<Core::Task<void>()> next) -> Core::Task<void>
        {
            const std::optional<TraceIdentifiers> parent = extractTraceContext(request);
            // 名字只取方法原文（一段静态文本）：路径进不了名字是刻意的——它是无界的，把每条不同的
            // URL 都做成一个操作名等于把检索侧的分组打碎。路径走 url.path 维度，
            // 而知道路由模式之后的应用可以自行 setName("GET /orders/:id")
            Span span = tracer->startSpan(methodKeyword(request.method()), SpanKind::Server, parent);
            span.setAttribute(kMethodAttributeName, methodKeyword(request.method()));
            span.setAttribute(kPathAttributeName, request.path());

            // 头部指回本节：处理器与出站客户端从头部读到的上下文，就是它们自己的直接上级。
            // 看不懂的更高版本原样转发——重排别人写的字段不是本中间件的权限
            if (!parent.has_value() || parent->version == 0U)
            {
                thread_local std::string traceparentText;
                Traceparent::renderInto(traceparentText, span.identifiers());
                static_cast<void>(request.setHeader(kTraceparentHeaderName, traceparentText));
            }

            try
            {
                co_await next();
            } catch (...)
            {
                // 异常原样交回会话去回 500；这里只负责把这一节判成失败并交货，
                // 否则处理器一抛，本节就带着「没人判定过结局」的初值离开
                span.setStatus(SpanStatusCode::Error, "处理函数抛出异常");
                span.finish();
                throw;
            }

            const int statusCode = response.status();
            span.setAttribute(kStatusCodeAttributeName, static_cast<std::int64_t>(statusCode));
            // 5xx 是服务端这一侧的失败，判 Error；4xx 不是——那是按业务规则给出的正常答复
            span.setStatus(statusCode >= 500 ? SpanStatusCode::Error : SpanStatusCode::Ok);
            span.finish();
            co_return;
        };
    }
} // namespace AsynGyanis::Net

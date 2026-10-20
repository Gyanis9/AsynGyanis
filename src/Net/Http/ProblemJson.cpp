/**
 * @file ProblemJson.cpp
 * @brief RFC 7807 `application/problem+json` 渲染器的实现
 * @author Gyanis
 * @date 2026-10-17
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Net/Http/ProblemJson.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Exception/InvalidArgumentException.h"

#include <optional>
#include <string>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// RFC 7807 §3.1：`type` 缺省就是 `about:blank`
        constexpr std::string_view kDefaultProblemType = "about:blank";

        /// 媒体类型按 RFC 7807 §3 的原文；JSON 本身恒为 UTF-8，不另加 charset 参数
        constexpr std::string_view kProblemMediaType = "application/problem+json";
    } // namespace

    std::string renderProblemJson(const ProblemDetails &problem)
    {
        if (problem.status < 100 || problem.status > 599)
        {
            throw Base::InvalidArgumentException("ProblemJson: status 必须在 100..599 之间，收到 " + std::to_string(problem.status) +
                                                 "——它同时要写进 HTTP 状态行，越界值发出去对端无法解释");
        }

        Base::ConfigObject object;
        object["type"]     = problem.type.empty() ? std::string(kDefaultProblemType) : problem.type;
        object["title"]    = problem.title;
        object["status"]   = problem.status;
        object["detail"]   = problem.detail;
        object["instance"] = problem.instance;

        const std::optional<std::string> rendered = Base::serializeConfigValue(object);
        if (!rendered.has_value())
        {
            // 五个字段都是文本：失败只可能是某个字符串带着非 UTF-8 字节。拒绝而不是替换，
            // 否则对端拿到一条看似成功、实际内容已被改写的错误体
            throw Base::InvalidArgumentException("ProblemJson: 字段含非 UTF-8 字节，无法序列化为合法的 JSON 错误体");
        }
        return *rendered;
    }

    bool setProblemJsonResponse(HttpResponse &response, const ProblemDetails &problem)
    {
        // 先渲染再动响应：序列化失败时响应保持原样，不给调用方留一条「状态改了、正文还是旧的」
        std::string body = renderProblemJson(problem);
        if (!response.setHeader("content-type", kProblemMediaType))
        {
            return false;
        }
        response.setStatus(static_cast<int>(problem.status));
        response.setOwnedBody(std::move(body));
        return true;
    }

} // namespace AsynGyanis::Net

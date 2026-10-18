// RFC 7807 problem+json 渲染器：字段齐全性、status 与 HTTP 状态码同解、
// detail 的 JSON 转义（中文/引号/反斜杠/换行）、type 缺省值，以及越界状态的拒绝。
#include "Net/Http/ProblemJson.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Exception/InvalidArgumentException.h"
#include "ExpectedAssertions.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 把渲染结果解析回 JSON 对象，解析失败时让当前用例直接失败
         * @param text 渲染器交出的正文
         * @return Base::ConfigValue 解析出的对象
         */
        [[nodiscard]] Base::ConfigValue parseRenderedProblem(const std::string &text)
        {
            const std::optional<Base::ConfigValue> parsed = Base::parseConfigValue(text);
            EXPECT_HAS_VALUE(parsed) << "渲染结果不是合法 JSON：" << text;
            return parsed.value_or(Base::ConfigValue{});
        }
    } // namespace

    /**
     * @brief 五个字段齐全，detail 里的中文与引号原样往返
     */
    TEST(ProblemJson, RendersEveryFieldAndRoundTripsDetailEscaping)
    {
        ProblemDetails problem;
        problem.type     = "https://example.com/probs/out-of-credit";
        problem.title    = "余额不足";
        problem.status   = 403;
        problem.detail   = "你的余额是 30，但需要 50。备注是 \"充值后重试\"，路径 C:\\tmp\\a.txt，换行在下一行\n结束。";
        problem.instance = "/account/12345/msgs/abc";

        const std::string       rendered = renderProblemJson(problem);
        const Base::ConfigValue parsed   = parseRenderedProblem(rendered);

        ASSERT_TRUE(parsed.is_object());
        EXPECT_EQ(parsed.size(), 5U) << "problem 对象必须恒有五个键：" << rendered;
        EXPECT_EQ(parsed.at("type").get<std::string>(), problem.type);
        EXPECT_EQ(parsed.at("title").get<std::string>(), problem.title);
        EXPECT_EQ(parsed.at("status").get<std::int64_t>(), problem.status);
        EXPECT_EQ(parsed.at("detail").get<std::string>(), problem.detail);
        EXPECT_EQ(parsed.at("instance").get<std::string>(), problem.instance);
    }

    /**
     * @brief type 缺省补 about:blank；显式给值时原样保留
     */
    TEST(ProblemJson, DefaultsMissingTypeToAboutBlank)
    {
        ProblemDetails problem;
        problem.title  = "请求无效";
        problem.status = 400;

        const Base::ConfigValue defaults = parseRenderedProblem(renderProblemJson(problem));
        EXPECT_EQ(defaults.at("type").get<std::string>(), "about:blank");

        problem.type                         = "urn:example:invalid-request";
        const Base::ConfigValue explicitType = parseRenderedProblem(renderProblemJson(problem));
        EXPECT_EQ(explicitType.at("type").get<std::string>(), problem.type);
    }

    /**
     * @brief 出口把 JSON 里的 status 与 HTTP 状态码写成同一个值，媒体类型是 problem+json
     */
    TEST(ProblemJson, SetsResponseStatusAndMediaTypeFromTheSameValue)
    {
        HttpResponse   response;
        ProblemDetails problem;
        problem.title    = "找不到";
        problem.status   = 404;
        problem.detail   = "资源不存在";
        problem.instance = "/missing";

        ASSERT_TRUE(setProblemJsonResponse(response, problem));

        EXPECT_EQ(response.status(), 404);
        ASSERT_TRUE(response.getHeader("content-type").has_value());
        EXPECT_EQ(*response.getHeader("content-type"), "application/problem+json");

        const Base::ConfigValue body = parseRenderedProblem(std::string(response.body()));
        EXPECT_EQ(body.at("status").get<std::int64_t>(), response.status()) << "JSON 里的 status 与状态行不是同一个值";
    }

    /**
     * @brief 越界状态码在渲染前就被拒，且原因说清它同时要进状态行
     */
    TEST(ProblemJson, RejectsStatusOutsideTheHttpRange)
    {
        ProblemDetails problem;
        problem.title  = "坏状态";
        problem.status = 700;

        try
        {
            static_cast<void>(renderProblemJson(problem));
            FAIL() << "status=700 没有被拒";
        } catch (const Base::InvalidArgumentException &error)
        {
            const std::string message = error.what();
            EXPECT_NE(message.find("100..599"), std::string::npos) << message;
            EXPECT_NE(message.find("HTTP 状态行"), std::string::npos) << message;
        }
    }

} // namespace AsynGyanis::Net

// OTLP 日志渲染器：severity 映射、纳秒时间戳（字符串）、resource/scope 形状、上下文字段与整批拒绝

#include "Net/Logging/OtlpLogJson.h"

#include "Net/Tracing/SpanExporter.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using nlohmann::json;

        /// 造一条内容固定的记录
        OtlpLogRecord makeRecord(const Base::LogLevel level, std::string message)
        {
            OtlpLogRecord record;
            record.level      = level;
            record.timestamp  = std::chrono::system_clock::time_point{std::chrono::seconds{1700000000}};
            record.message    = std::move(message);
            record.loggerName = "export-test";
            record.threadId   = "tid-1";
            record.attributes.emplace_back("trace_id", "4bf92f3577b34da6a3ce929d0e0e4736");
            return record;
        }
    } // namespace

    TEST(OtlpLogJson, RendersSeverityTimeBodyAndContextAttributes)
    {
        const TraceResource resource{.serviceName = "asyngyanis", .serviceVersion = "4.0.0"};
        const std::string   body = formatOtlpLogsJson(resource, {makeRecord(Base::LogLevel::Warn, "磁盘将满")});
        ASSERT_FALSE(body.empty());

        const json document = json::parse(body);
        ASSERT_TRUE(document.contains("resourceLogs"));
        const json &resourceLogs       = document.at("resourceLogs").at(0);
        const json &resourceAttributes = resourceLogs.at("resource").at("attributes");
        EXPECT_EQ(resourceAttributes.at(0).at("key"), "service.name");
        EXPECT_EQ(resourceAttributes.at(0).at("value").at("stringValue"), "asyngyanis");

        const json &scopeLogs = resourceLogs.at("scopeLogs").at(0);
        EXPECT_EQ(scopeLogs.at("scope").at("name"), "AsynGyanis.Logger");
        const json &record = scopeLogs.at("logRecords").at(0);
        EXPECT_EQ(record.at("severityText"), "WARN");
        EXPECT_EQ(record.at("severityNumber"), 13);
        EXPECT_EQ(record.at("timeUnixNano"), "1700000000000000000");
        EXPECT_EQ(record.at("body").at("stringValue"), "磁盘将满");

        bool sawTraceId = false;
        for (const json &attribute: record.at("attributes"))
        {
            if (attribute.at("key") == "trace_id")
            {
                EXPECT_EQ(attribute.at("value").at("stringValue"), "4bf92f3577b34da6a3ce929d0e0e4736");
                sawTraceId = true;
            }
        }
        EXPECT_TRUE(sawTraceId) << "上下文里的 trace_id 没进 attributes";
    }

    TEST(OtlpLogJson, MapsEveryLevelToItsOtlpSeverityNumber)
    {
        const TraceResource resource{.serviceName = "s"};
        const auto          severityOf = [&resource](const Base::LogLevel level)
        {
            const std::string body = formatOtlpLogsJson(resource, {makeRecord(level, "x")});
            return json::parse(body).at("resourceLogs").at(0).at("scopeLogs").at(0).at("logRecords").at(0).at("severityNumber").get<int>();
        };
        EXPECT_EQ(severityOf(Base::LogLevel::Trace), 1);
        EXPECT_EQ(severityOf(Base::LogLevel::Debug), 5);
        EXPECT_EQ(severityOf(Base::LogLevel::Info), 9);
        EXPECT_EQ(severityOf(Base::LogLevel::Warn), 13);
        EXPECT_EQ(severityOf(Base::LogLevel::Error), 17);
        EXPECT_EQ(severityOf(Base::LogLevel::Fatal), 21);
    }

    TEST(OtlpLogJson, ReturnsEmptyBodyWhenABatchContainsInvalidUtf8)
    {
        OtlpLogRecord broken = makeRecord(Base::LogLevel::Info, std::string{"bad \xFF byte"});
        EXPECT_TRUE(formatOtlpLogsJson(TraceResource{.serviceName = "s"}, {broken}).empty()) << "非法 UTF-8 应让整批成不了形，而不是发出一条被改过的记录";
    }

    /**
     * @brief 随机属性：几百组「必须被转义」的正文、上下文键值与服务名都要按原样待在正文里
     * @details 日志正文是三份渲染出口里最贴近攻击者输入的一格：被截断的请求路径、驱动报错原文、
     *          对端给的令牌片段都会落到这里面。上面那条手写用例钉的是一个形状，这一条钉整族形状——
     *          漏转义任何一个字符，要么整份正文解析失败，要么成员变形，两种都在这里露出来。
     * @note 固定种子是为了失败可复放。这一族刻意留在 gtest 侧而不是 fuzz-net.sh 那份闭包：
     *       traces 出口还要 Span.cpp 提供标识文本，而它牵进 Core::CoroutinePool 整条依赖链，
     *       那份闭包按 /MT 编、带不动项目的 /MD 库。
     */
    TEST(OtlpLogJson, RandomTrickyTextsRoundTripVerbatim)
    {
        // 整片取用而不是按字节挑：按字节会把「示」这类多字节序列拆开，造出的正是渲染器应当拒绝的
        // 非法 UTF-8——那是驱动自己的错，不是被测代码的错
        static constexpr std::array<std::string_view, 12> kTrickyPieces = {"\"", "\\", "\r", "\n", "\t", "\x01", "\x1f", "\x7f", " ", "a", "示", "例"};

        std::mt19937                               generator(20261019U);
        std::uniform_int_distribution<std::size_t> charPick(0U, kTrickyPieces.size() - 1U);
        const auto                                 makeText = [&](const std::size_t maximumLength) -> std::string
        {
            std::uniform_int_distribution<std::size_t> lengthPick(1U, maximumLength);
            std::string                                text;
            for (std::size_t count = lengthPick(generator); count > 0U; --count)
            {
                text += kTrickyPieces[charPick(generator)];
            }
            return text;
        };

        for (std::size_t round = 0; round < 300U; ++round)
        {
            const TraceResource resource{.serviceName = makeText(24U), .serviceVersion = makeText(12U)};
            OtlpLogRecord       record;
            record.level                   = Base::LogLevel::Error;
            record.timestamp               = std::chrono::system_clock::time_point{std::chrono::seconds{1700000000}};
            record.message                 = makeText(64U);
            record.loggerName              = makeText(16U);
            record.threadId                = makeText(8U);
            const std::string attributeKey = makeText(16U);
            record.attributes              = {{attributeKey, record.message}};

            const std::string body  = formatOtlpLogsJson(resource, {record});
            const std::string label = "第 " + std::to_string(round) + " 轮";
            ASSERT_FALSE(body.empty()) << label << "：全是合法 UTF-8 的取值不该让整批成不了形";

            const json document = json::parse(body, nullptr, false);
            if (document.is_discarded())
            {
                FAIL() << label << "：渲染结果不是合法 JSON，正文=" << body;
                return;
            }

            const json &logRecord = document.at("resourceLogs").at(0).at("scopeLogs").at(0).at("logRecords").at(0);
            EXPECT_EQ(logRecord.at("body").at("stringValue").get<std::string>(), record.message) << label;

            bool isContextAttributeIntact = false;
            for (const json &attribute: logRecord.at("attributes"))
            {
                if (attribute.at("key").get<std::string>() == attributeKey)
                {
                    isContextAttributeIntact = attribute.at("value").at("stringValue").get<std::string>() == record.message;
                    break;
                }
            }
            EXPECT_TRUE(isContextAttributeIntact) << label << "：上下文键值没按原样待在 attributes 里";

            bool isServiceNameIntact = false;
            for (const json &attribute: document.at("resourceLogs").at(0).at("resource").at("attributes"))
            {
                if (attribute.at("key").get<std::string>() == "service.name")
                {
                    isServiceNameIntact = attribute.at("value").at("stringValue").get<std::string>() == resource.serviceName;
                    break;
                }
            }
            EXPECT_TRUE(isServiceNameIntact) << label << "：服务名没按原样待在 resource 里";
        }
    }
} // namespace AsynGyanis::Net

// OTLP 日志渲染器：severity 映射、纳秒时间戳（字符串）、resource/scope 形状、上下文字段与整批拒绝

#include "Net/Logging/OtlpLogJson.h"

#include "Net/Tracing/SpanExporter.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
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
} // namespace AsynGyanis::Net

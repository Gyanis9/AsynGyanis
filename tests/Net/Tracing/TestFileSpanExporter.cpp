// 文件链路出口的用例：一节一行的形状、追加语义，以及整批成败的判法

#include "Net/Tracing/FileSpanExporter.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Exception/SystemException.h"
#include "Net/Tracing/Span.h"
#include "TracingTestSupport.h"

#include "CommonTestSupport.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using AsynGyanis::Base::ConfigValue;
    using AsynGyanis::Base::parseConfigValue;
    using AsynGyanis::Net::FileSpanExporter;
    using AsynGyanis::Net::SpanAttribute;
    using AsynGyanis::Net::SpanKind;
    using AsynGyanis::Net::SpanRecord;
    using AsynGyanis::Net::SpanStatusCode;
    using AsynGyanis::Net::TraceResource;
    using AsynGyanis::Net::TestSupport::jsonNumberOf;
    using AsynGyanis::Net::TestSupport::jsonTextOf;
    using AsynGyanis::Net::TestSupport::makeRecord;

    constexpr std::string_view kTraceId = "0123456789abcdef0123456789abcdef";
    constexpr std::string_view kSpanId  = "1111222233334444";

    /// 把整个文件按二进制读回来（逐行断言之外用，一次读满便于查 CR）
    std::string readWholeFile(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    /// 按 '\n' 切行，返回不含分行符的行列表（末尾的空行是最后一行的换行符切出来的，丢掉）
    std::vector<std::string> splitLines(std::string_view text)
    {
        std::vector<std::string> lines;
        while (!text.empty())
        {
            const std::size_t newlineIndex = text.find('\n');
            lines.emplace_back(text.substr(0, newlineIndex));
            if (newlineIndex == std::string_view::npos)
            {
                break;
            }
            text.remove_prefix(newlineIndex + 1U);
        }
        return lines;
    }

    /// 造一批两条节：一条根、一条子，维度与结局都齐
    std::vector<SpanRecord> makeTwoRecords()
    {
        SpanRecord root = makeRecord("root-operation", kTraceId, kSpanId);
        root.kind       = SpanKind::Server;
        root.status     = SpanStatusCode::Ok;

        SpanRecord child    = makeRecord("child-operation", kTraceId, "aaaabbbbccccdddd", kSpanId);
        child.kind          = SpanKind::Client;
        child.status        = SpanStatusCode::Error;
        child.statusMessage = "上游超时";
        child.attributes.push_back(SpanAttribute{.key = "peer.address", .value = std::string{"10.0.0.1:808"}});
        child.attributes.push_back(SpanAttribute{.key = "retry.count", .value = std::int64_t{2}});
        child.droppedAttributeCount = 1U;
        return {root, child};
    }
} // namespace

/**
 * @brief 一批两节落成两行自描述的 JSON，第二批往后追加而不是覆盖
 */
TEST(FileSpanExporter, WritesOneJsonObjectPerSpanAndAppendsAcrossBatches)
{
    const AsynGyanis::TestSupport::TemporaryDirectory temporaryDirectory{"FileSpanExporter"};
    const std::filesystem::path                       filePath = temporaryDirectory.path() / "spans.jsonl";
    FileSpanExporter                                  exporter{filePath};
    const TraceResource                               resource{.serviceName = "file-test-service", .serviceVersion = "0.1.0"};

    ASSERT_TRUE(exporter.exportSpans(resource, makeTwoRecords()));
    ASSERT_TRUE(exporter.exportSpans(resource, makeTwoRecords()));

    const std::string contents = readWholeFile(filePath);
    EXPECT_EQ(contents.find('\r'), std::string::npos) << "JSON Lines 的分行符就是 LF：Windows 上也不该翻成 CRLF";
    const std::vector<std::string> lines = splitLines(contents);
    ASSERT_EQ(lines.size(), 4U);

    const auto first  = parseConfigValue(lines[0]);
    const auto second = parseConfigValue(lines[1]);
    ASSERT_TRUE(first.has_value()) << lines[0];
    ASSERT_TRUE(second.has_value()) << lines[1];

    EXPECT_EQ(jsonTextOf(first->at("traceId")), kTraceId);
    EXPECT_EQ(jsonTextOf(first->at("spanId")), kSpanId);
    // 文件这一路始终带 parentSpanId 这一列（根节是空串）：逐行读的人不必处理字段缺席，
    // 而 OTLP 那边按 protojson 的规矩要省略缺省值——两个读者的习惯不同，形状也就不同
    EXPECT_EQ(jsonTextOf(first->at("parentSpanId")), "");
    EXPECT_EQ(jsonTextOf(first->at("name")), "root-operation");
    EXPECT_EQ(jsonTextOf(first->at("kind")), "server") << "文件这一路给人读，角色写文本而不是 OTLP 的数字";
    EXPECT_EQ(jsonTextOf(first->at("status")), "ok");
    EXPECT_EQ(jsonTextOf(first->at("serviceName")), "file-test-service");
    EXPECT_EQ(jsonTextOf(first->at("serviceVersion")), "0.1.0");
    EXPECT_EQ(jsonTextOf(first->at("startTimeUnixNano")), "1700000000123456700");
    EXPECT_EQ(jsonTextOf(first->at("durationNano")), "2500000000");
    EXPECT_FALSE(first->contains("droppedAttributesCount")) << "没丢维度时不带这一列";

    EXPECT_EQ(jsonTextOf(second->at("parentSpanId")), kSpanId);
    EXPECT_EQ(jsonTextOf(second->at("kind")), "client");
    EXPECT_EQ(jsonTextOf(second->at("status")), "error");
    EXPECT_EQ(jsonTextOf(second->at("statusMessage")), "上游超时");
    EXPECT_EQ(jsonNumberOf(second->at("droppedAttributesCount")), 1);
    // 维度写成对象而不是 KeyValue 数组：jq 里 .attributes["peer.address"] 直接可取
    EXPECT_EQ(jsonTextOf(second->at("attributes").at("peer.address")), "10.0.0.1:808");
    EXPECT_EQ(jsonNumberOf(second->at("attributes").at("retry.count")), 2);
    EXPECT_EQ(exporter.exporterName(), "file");
}

/**
 * @brief 父目录不存在时构造要把它建出来：改一行配置就能把链路写到没建过的子目录
 */
TEST(FileSpanExporter, CreatesMissingParentDirectories)
{
    const AsynGyanis::TestSupport::TemporaryDirectory temporaryDirectory{"FileSpanExporterMkDir"};
    const std::filesystem::path                       filePath = temporaryDirectory.path() / "nested" / "deeper" / "spans.jsonl";

    FileSpanExporter exporter{filePath};
    ASSERT_TRUE(exporter.exportSpans(TraceResource{.serviceName = "svc"}, {makeRecord("op", kTraceId, kSpanId)}));
    EXPECT_TRUE(std::filesystem::is_regular_file(filePath));
}

/**
 * @brief 有一行正文没能成形时整批判失败：不猜哪条坏了，也不静默跳过
 */
TEST(FileSpanExporter, RejectsTheWholeBatchWhenALineCannotBeRendered)
{
    const AsynGyanis::TestSupport::TemporaryDirectory temporaryDirectory{"FileSpanExporterBadUtf8"};
    const std::filesystem::path                       filePath = temporaryDirectory.path() / "spans.jsonl";
    FileSpanExporter                                  exporter{filePath};
    const TraceResource                               resource{.serviceName = "svc"};

    std::vector<SpanRecord> records = makeTwoRecords();
    records[1].name                 = std::string{"bad\xffname"};
    EXPECT_FALSE(exporter.exportSpans(resource, records));
}

/**
 * @brief shutdown 之后再交付一律拒收：出口已经不在了，不该假装收下
 */
TEST(FileSpanExporter, WritesNothingAfterShutdown)
{
    const AsynGyanis::TestSupport::TemporaryDirectory temporaryDirectory{"FileSpanExporterShutdown"};
    const std::filesystem::path                       filePath = temporaryDirectory.path() / "spans.jsonl";
    FileSpanExporter                                  exporter{filePath};
    const TraceResource                               resource{.serviceName = "svc"};

    ASSERT_TRUE(exporter.exportSpans(resource, makeTwoRecords()));
    exporter.shutdown();
    EXPECT_FALSE(exporter.exportSpans(resource, makeTwoRecords()));
    // 关掉之后再 shutdown 也必须安全：Tracer 与出口各自的收尾顺序不该由用例保证
    exporter.shutdown();
    EXPECT_EQ(splitLines(readWholeFile(filePath)).size(), 2U) << "第二次交付一行都不该落下去";
}

/**
 * @brief 路径不能落在文件上的构造要当场拒绝，而不是等到第一次交付才发现
 */
TEST(FileSpanExporter, ConstructorRejectsAnUnusablePath)
{
    const AsynGyanis::TestSupport::TemporaryDirectory temporaryDirectory{"FileSpanExporterBadPath"};
    const std::filesystem::path                       blocker = temporaryDirectory.path() / "blocker";
    std::ofstream                                     blockerStream{blocker};
    ASSERT_TRUE(blockerStream.is_open());
    blockerStream.close();

    EXPECT_THROW(static_cast<void>(FileSpanExporter(blocker / "spans.jsonl")), AsynGyanis::Base::SystemException) << "父路径是个普通文件：目录建不出来就该在构造期报错";
}

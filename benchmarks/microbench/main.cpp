// 热路径微基准：HPACK 编解码、h1 请求解析、h2 帧解码、HTTP 日期格式化/解析、request-id 生成、
// 头部单值查询与列表 token 判定、响应头序列化、h2/h3 组头块的两种走法。
//
// 用法：microbench [--json-out <文件>]
// 不给参数就跑全部用例并在控制台打表；给了 --json-out 再写一份 JSON，供 benchmarks/check-baseline.py 比对
// （用 --baseline benchmarks/microbench-baseline.json 指到微基准自己的基线）。
//
// 口径与注意事项：
//   · 计时只发生在每一轮的边界上：先用一小段校准把「一轮跑多少次」定下来，之后整轮计时。
//     每次操作都读一次时钟的话，对几十纳秒级的操作，计时本身的开销会盖过被测代码。
//   · 每个用例重复若干轮取**最快**的一轮。微基准问的是「这份代码最快能多快」，
//     而机器上的其它负载只会让某一轮变慢、不会让它变快，所以最小值是对能力最接近的估计。
//     这与服务端压测取中位数不冲突——那边 Python 客户端的开销占主导，取最优值等于美化数据。
//   · 数据形态尽量贴近稳态：HPACK 同一份头部反复编码/解码（同一连接复用编解码器）、
//     h1 请求带 10 个头与 64 字节正文、h2 解一帧 200 字节头块的 HEADERS。
//     换数据形态会改变结果，比较不同机器的数字前先确认两边用的是同一份输入。
#include "Net/Http/HttpDate.h"
#include "Net/Http/HttpHeaderFieldStore.h"
#include "Net/Http/HttpHeaderRules.h"
#include "Net/Http/HttpParser.h"
#include "Net/Http/HttpRequestId.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http2/Hpack.h"
#include "Net/Http2/Http2Frame.h"
#include "Net/Http3/Qpack.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 与 samples/main.cpp 同一处理：本文件只碰库自身的类型，全写根命名空间会把每行都拉长一倍
using namespace AsynGyanis;

namespace
{
    using SteadyClock = std::chrono::steady_clock;

    /// 校准时长：既当预热，也用来估算单次操作耗时以定下一轮的操作数
    constexpr auto kCalibrationDuration = std::chrono::milliseconds(20);

    /// 每一轮的目标时长；一轮太短则调度噪声占比过大，太长则整套基准等太久
    constexpr auto kRoundDuration = std::chrono::milliseconds(50);

    /// 每个用例测几轮取最快的一轮
    constexpr int kRoundCount = 5;

    /// 一个用例的测量结果
    struct CaseResult
    {
        std::string name;
        double nanosecondsPerOperation{0.0};
        double operationsPerSecond{0.0};
        std::uint64_t operationsPerRound{0};
    };

    /**
     * @brief 跑一个用例：自检 → 校准 → 若干轮定次测量 → 取最快的一轮
     * @param name 用例名，与基线 JSON 里的键一致
     * @param body 每调用一次算一次操作，**成功路径必须返回非 0**、失败路径返回 0
     * @param results 汇总表，本用例的结果追加在这里
     * @param checksumSink 校验和累加处，见下
     * @param failureCount 自检失败计数
     */
    template <typename Body>
    void measureCase(std::string name, Body body, std::vector<CaseResult> &results, std::uint64_t &checksumSink,
                     int &failureCount)
    {
        // 先确认一次走的是成功路径：这些入口在失败时返回 0（解析失败、解码失败、字段为空）。
        // 少了这一步，哪天输入写错成非法报文，量到的是失败这条冷路径，而输出只是一串更快的数字
        if (body() == 0)
        {
            std::printf("  %-24s 自检失败：被测入口没有走成功路径，本用例跳过\n", name.c_str());
            ++failureCount;
            return;
        }

        const auto calibrationBegin = SteadyClock::now();
        auto calibrationNow = calibrationBegin;
        std::uint64_t calibrationOperations = 0;
        do
        {
            checksumSink += static_cast<std::uint64_t>(body());
            ++calibrationOperations;
            calibrationNow = SteadyClock::now();
        } while (calibrationNow - calibrationBegin < kCalibrationDuration);

        const double calibrationNanoseconds =
                std::chrono::duration<double, std::nano>(calibrationNow - calibrationBegin).count();
        const double estimatedNanosecondsPerOperation = calibrationNanoseconds / static_cast<double>(calibrationOperations);
        const double roundNanoseconds = std::chrono::duration<double, std::nano>(kRoundDuration).count();

        // 至少跑一次：校准期间若碰上一次长时间调度延迟，除出来的单次耗时可能大到让这里算出 0
        const std::uint64_t operationsPerRound =
                std::max<std::uint64_t>(1, static_cast<std::uint64_t>(roundNanoseconds / estimatedNanosecondsPerOperation));

        CaseResult result;
        result.name = std::move(name);
        result.operationsPerRound = operationsPerRound;
        for (int round = 0; round < kRoundCount; ++round)
        {
            const auto begin = SteadyClock::now();
            for (std::uint64_t operation = 0; operation < operationsPerRound; ++operation)
            {
                checksumSink += static_cast<std::uint64_t>(body());
            }
            const auto end = SteadyClock::now();

            const double nanosecondsPerOperation =
                    std::chrono::duration<double, std::nano>(end - begin).count() / static_cast<double>(operationsPerRound);
            if (result.nanosecondsPerOperation == 0.0 || nanosecondsPerOperation < result.nanosecondsPerOperation)
            {
                result.nanosecondsPerOperation = nanosecondsPerOperation;
            }
        }
        result.operationsPerSecond = 1e9 / result.nanosecondsPerOperation;
        results.push_back(std::move(result));
    }

    /// 一份贴近真实请求的头部列表：伪头齐全 + 常见的 8 个实体头
    std::vector<Net::HpackHeaderField> makeRequestHeaderFields()
    {
        return {
            {":method", "GET"},
            {":scheme", "https"},
            {":authority", "www.example.com"},
            {":path", "/assets/app.bundle.js"},
            {"user-agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0"},
            {"accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8"},
            {"accept-encoding", "gzip, deflate, br, zstd"},
            {"accept-language", "zh-CN,zh;q=0.9,en;q=0.8"},
            {"cache-control", "no-cache"},
            {"cookie", "session=8f14e45fceea167a5a36dedd4bea2543; theme=dark; locale=zh-CN"},
            {"referer", "https://www.example.com/index.html"},
            {"if-none-match", "\"6f1a-0-18f2c4b7d80\""},
        };
    }

    /// 一条贴近真实的 h1 请求：10 个头部 + 64 字节正文
    std::string makeHttp1RequestText()
    {
        std::string text = "POST /api/v1/orders?trace=1 HTTP/1.1\r\n";
        text += "Host: api.example.com\r\n";
        text += "User-Agent: curl/8.7.1\r\n";
        text += "Accept: */*\r\n";
        text += "Content-Type: application/json\r\n";
        text += "Content-Length: 64\r\n";
        text += "Accept-Encoding: gzip, deflate, br\r\n";
        text += "Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9\r\n";
        text += "X-Request-Id: 0001-0000000000000abc\r\n";
        text += "Connection: keep-alive\r\n";
        text += "\r\n";
        text += "{\"orderId\":\"A-20260913-0001\",\"items\":[{\"sku\":\"X1\",\"quantity\":2}]}";
        return text;
    }

    /// 一帧带 200 字节头块的 HEADERS（END_HEADERS 置位，无优先级字段）
    std::string makeHttp2HeadersFrameText()
    {
        std::string headerBlock;
        headerBlock.reserve(200);
        // 拼一段长度固定的头块字节：内容本身不重要，重要的是让解码器走完「帧头 → 负载 → 剥 padding」
        for (std::size_t index = 0; index < 200; ++index)
        {
            headerBlock.push_back(static_cast<char>(index % 251));
        }

        Net::Http2HeadersPayload payload;
        payload.endHeaders = true;
        payload.headerBlockFragment = std::move(headerBlock);
        return Net::encodeHttp2HeadersFrame(payload, 1);
    }

    /// 控制台表格
    void printTable(const std::vector<CaseResult> &results, int failureCount, std::uint64_t checksum)
    {
        std::printf("\n== 微基准结果（每用例 %d 轮取最快一轮）==\n", kRoundCount);
        for (const CaseResult &result : results)
        {
            std::printf("  %-24s %10.1f ns/op %14.0f op/s（每轮 %llu 次）\n", result.name.c_str(),
                        result.nanosecondsPerOperation, result.operationsPerSecond,
                        static_cast<unsigned long long>(result.operationsPerRound));
        }
        // 校验和在这里露一次面：它在上面参与了累加，整段被测循环才不会被编译器整个丢掉
        std::printf("  自检失败 %d 例；校验和 %llu（仅用于阻止循环被优化掉，无业务含义）\n", failureCount,
                    static_cast<unsigned long long>(checksum));
    }

    /// 结果写成 JSON：键名与 benchmarks/microbench-baseline.json 对齐，好让同一个门禁脚本直接吃
    void writeJson(const std::string &path, const std::vector<CaseResult> &results, int failureCount)
    {
        std::string document;
        document += "{\n";
        document += "  \"benchmark\": \"microbench\",\n";
        document += std::format("  \"failures\": {},\n", failureCount);
        document += "  \"measurements\": {\n";
        for (std::size_t index = 0; index < results.size(); ++index)
        {
            const CaseResult &result = results[index];
            document += std::format(
                    "    \"{}\": {{\"throughputPerSecond\": {:.0f}, \"nanosecondsPerOperation\": {:.1f}}}",
                    result.name, result.operationsPerSecond, result.nanosecondsPerOperation);
            document += index + 1 == results.size() ? "\n" : ",\n";
        }
        document += "  }\n}\n";

        std::ofstream stream(path, std::ios::binary);
        if (!stream)
        {
            std::printf("  警告：结果文件 %s 打不开，JSON 未写出\n", path.c_str());
            return;
        }
        stream << document;
        std::printf("  结果已写入 %s\n", path.c_str());
    }
} // namespace

int main(int argumentCount, char **argumentValues)
{
    std::string jsonOutputPath;
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::string_view argument = argumentValues[index];
        if (argument == "--json-out" && index + 1 < argumentCount)
        {
            jsonOutputPath = argumentValues[++index];
            continue;
        }
        std::printf("用法：microbench [--json-out <文件>]\n未知参数：%s\n", argumentValues[index]);
        return 1;
    }

    std::vector<CaseResult> results;
    results.reserve(8);
    std::uint64_t checksum = 0;
    int failureCount = 0;

    // 请求头块的编/解码器在用例外建好并复用：被测的是稳态（同一连接反复用同一个编解码器），
    // 每次迭代都新建的话量到的是构造开销，不是热路径
    const std::vector<Net::HpackHeaderField> headerFields = makeRequestHeaderFields();
    Net::HpackEncoder encoder;
    Net::HpackDecoder decoder;
    std::vector<Net::HpackHeaderField> decodedHeaderFields;

    // HPACK 的动态表是收发两侧同步的：编码器把命名对插进自己的表之后，后续只发索引，
    // 解码器必须见过同一批插入才能解出同样的字段。这里先让两侧各走一轮同样的插入把表对齐——
    // 这正是真实连接上的样子（双方都从空表开始）——否则解码器拿到的是指向它自己空表的索引，
    // 必然判错，量的就成了失败路径（首次写这份基准时正是自检把这种情况挡了下来）
    const std::string hpackPrimingBlock = encoder.encode(headerFields);
    const bool isHpackTablePrimed = decoder.decode(hpackPrimingBlock, decodedHeaderFields, nullptr);
    if (!isHpackTablePrimed)
    {
        std::printf("  警告：HPACK 动态表对齐失败，解码用例的结果不可信\n");
    }

    // 编码输入改用视图（生产路径上 h2 就是按视图递的）：视图指向上面那批字符串，它们活到基准结束
    std::vector<Net::HpackHeaderFieldView> headerFieldViews;
    headerFieldViews.reserve(headerFields.size());
    for (const Net::HpackHeaderField &field: headerFields)
    {
        headerFieldViews.push_back(Net::HpackHeaderFieldView{.name = field.name, .value = field.value});
    }

    measureCase(
            "hpack-encode",
            [&encoder, &headerFieldViews]
            {
                const std::string block = encoder.encode(headerFieldViews);
                return block.size();
            },
            results, checksum, failureCount);

    const std::string encodedHeaderBlock = encoder.encode(headerFields);

    // 消融对照：h2 每条响应都要把「:status + 响应头」整表交给编码器。旧写法是再拷一份 owning
    // vector（每个头各构造名与值两个字符串），新写法只排一张视图表。这两例量的就是被去掉的那份拷贝
    measureCase(
            "h2-head-table-owning-copy",
            [&headerFields]
            {
                std::vector<Net::HpackHeaderField> fields;
                fields.reserve(headerFields.size() + 1U);
                fields.push_back(Net::HpackHeaderField{.name = ":status", .value = std::string("200")});
                fields.insert(fields.end(), headerFields.begin(), headerFields.end());
                return fields.size();
            },
            results, checksum, failureCount);

    measureCase(
            "h2-head-table-view-table",
            [&headerFields]
            {
                std::vector<Net::HpackHeaderFieldView> fields;
                fields.reserve(headerFields.size() + 1U);
                fields.push_back(Net::HpackHeaderFieldView{.name = std::string_view(":status"), .value = std::string_view("200")});
                for (const Net::HpackHeaderField &field: headerFields)
                {
                    fields.push_back(Net::HpackHeaderFieldView{.name = field.name, .value = field.value});
                }
                return fields.size();
            },
            results, checksum, failureCount);

    // h3 采集器两种走法的消融对照：旧写法先建单值视图（一张每次被写脏都要重建的哈希表）再按名回查
    // 每头的全部值（每头一个临时 vector），新写法按权威记录的设置顺序一次走完。
    // 两例每轮开头都做同一次 setHeader 把视图标脏——真实响应正是「业务刚写完」的样子，
    // 这份共同开销不参与差异；两条默认补齐行也不进本例，两侧一致且已由别的用例量过
    const auto makeH3ResponseFixture = []
    {
        Net::HttpResponse fixture;
        fixture.setStatus(200);
        fixture.setHeader("content-type", "text/plain; charset=utf-8");
        fixture.setHeader("set-cookie", "session=8f14e45fceea167a5a36dedd4bea2543; Path=/");
        fixture.setHeader("x-trace", "0001-0000000000000abc");
        fixture.setHeader("set-cookie", "theme=dark; Path=/");
        fixture.setHeader("connection", "keep-alive");
        fixture.setBody(std::string(64, 'x'));
        return fixture;
    };

    /// 把一张字段行表压成与顺序无关的内容指纹：两条走法给出的次序本就不同，
    /// 但字节总量与条数必须一致，否则比的就是「少拿了几个头」而不是拷贝开销
    const auto fieldLineFingerprint = [](const std::vector<Net::QpackHeaderField> &fields)
    {
        std::size_t fingerprint = 0;
        for (const Net::QpackHeaderField &field: fields)
        {
            fingerprint += field.name.size() + field.value.size() + 1U;
        }
        return fingerprint;
    };

    {
        // 先自检两种走法的产出等价，再开始计时（不等价说明用例本身写错了，数字没有意义）
        Net::HttpResponse selfCheckResponse = makeH3ResponseFixture();
        std::vector<Net::QpackHeaderField> legacyFields;
        legacyFields.reserve(selfCheckResponse.headers().size() + 3U);
        for (const auto &headerEntry: selfCheckResponse.headers())
        {
            for (const std::string &headerValue: selfCheckResponse.headerValues(headerEntry.first))
            {
                legacyFields.push_back(Net::QpackHeaderField{.name = headerEntry.first, .value = headerValue});
            }
        }
        std::vector<Net::QpackHeaderField> orderedFields;
        selfCheckResponse.forEachHeaderField(
                [&orderedFields](const std::string_view headerName, const std::string_view headerValue)
                { orderedFields.push_back(Net::QpackHeaderField{std::string(headerName), std::string(headerValue)}); });
        if (fieldLineFingerprint(legacyFields) != fieldLineFingerprint(orderedFields))
        {
            std::printf("  警告：h3 采集器两例的产出不等价，其结果不可信\n");
        }
    }

    Net::HttpResponse h3LegacyResponse = makeH3ResponseFixture();
    measureCase(
            "h3-head-collect-view-then-lookup",
            [&h3LegacyResponse, &fieldLineFingerprint]
            {
                static_cast<void>(h3LegacyResponse.setHeader("x-trace", "0001-0000000000000abc"));
                std::vector<Net::QpackHeaderField> fields;
                fields.reserve(h3LegacyResponse.headers().size() + 3U);
                fields.push_back(Net::QpackHeaderField{.name = ":status", .value = std::string("200")});
                for (const auto &headerEntry: h3LegacyResponse.headers())
                {
                    if (Net::isConnectionSpecificHeaderName(headerEntry.first))
                    {
                        continue;
                    }
                    for (const std::string &headerValue: h3LegacyResponse.headerValues(headerEntry.first))
                    {
                        fields.push_back(Net::QpackHeaderField{.name = headerEntry.first, .value = headerValue});
                    }
                }
                return fieldLineFingerprint(fields);
            },
            results, checksum, failureCount);

    Net::HttpResponse h3OrderedResponse = makeH3ResponseFixture();
    measureCase(
            "h3-head-collect-ordered-walk",
            [&h3OrderedResponse, &fieldLineFingerprint]
            {
                static_cast<void>(h3OrderedResponse.setHeader("x-trace", "0001-0000000000000abc"));
                std::vector<Net::QpackHeaderField> fields;
                fields.reserve(8U);
                fields.push_back(Net::QpackHeaderField{.name = ":status", .value = std::string("200")});
                h3OrderedResponse.forEachHeaderField(
                        [&fields](const std::string_view headerName, const std::string_view headerValue)
                        {
                            if (Net::isConnectionSpecificHeaderName(headerName))
                            {
                                return;
                            }
                            fields.push_back(Net::QpackHeaderField{std::string(headerName), std::string(headerValue)});
                        });
                return fieldLineFingerprint(fields);
            },
            results, checksum, failureCount);

    // QPACK 的头块编解码：h3 每条响应过一次编码器、每个请求过一次解码器。
    // 这里刻意用「整段都命中静态表」的字段行（三项都是 RFC 9204 附录 A 的整项）：一次插入都不产生，
    // 也就没有未确认段与引用的累积，量出来的才是稳态而不是越跑越慢的记账
    const std::vector<Net::QpackHeaderField> qpackStaticHitFields = {
        Net::QpackHeaderField{.name = ":status", .value = "200"},
        Net::QpackHeaderField{.name = "content-type", .value = "application/json"},
        Net::QpackHeaderField{.name = "content-length", .value = "0"},
    };
    Net::QpackEncoder qpackEncoder(4096, 100, 4096);
    std::string       qpackHeaderBlock;
    std::string       qpackEncoderStreamBytes;
    static_cast<void>(qpackEncoder.encodeFieldSection(0, std::span<const Net::QpackHeaderField>{qpackStaticHitFields},
                                                      qpackHeaderBlock, qpackEncoderStreamBytes));
    measureCase(
            "qpack-encode",
            [&qpackEncoder, &qpackStaticHitFields, &qpackHeaderBlock, &qpackEncoderStreamBytes]
            {
                static_cast<void>(qpackEncoder.encodeFieldSection(0, std::span<const Net::QpackHeaderField>{qpackStaticHitFields},
                                                                  qpackHeaderBlock, qpackEncoderStreamBytes));
                return qpackHeaderBlock.size();
            },
            results, checksum, failureCount);

    // 解码侧要按 h3 的口径把 Section Ack 还回去才算收口：这里每轮都调 noteFieldSectionDelivered，
    // 与真实连接上「解完一段就交付」一致，不至于让挂起记录越积越多
    const Net::QpackDecoderSettings qpackDecoderSettings{
        .maximumTableCapacityByteCount = 4096, .maximumBlockedStreamCount = 100, .maximumFieldSectionSizeByteCount = 16384};
    Net::QpackDecoder       qpackDecoder(qpackDecoderSettings);
    std::vector<Net::QpackHeaderField> qpackDecodedFields;
    std::string                        qpackDecoderStreamBytes;
    measureCase(
            "qpack-decode",
            [&qpackDecoder, &qpackHeaderBlock, &qpackDecodedFields, &qpackDecoderStreamBytes]
            {
                const std::span<const std::uint8_t> sectionBytes(reinterpret_cast<const std::uint8_t *>(qpackHeaderBlock.data()),
                                                                 qpackHeaderBlock.size());
                const auto decoded = qpackDecoder.decodeFieldSection(0, sectionBytes, qpackDecodedFields, qpackDecoderStreamBytes);
                if (!decoded || *decoded != Net::QpackFieldSectionDecodeStatus::Decoded)
                {
                    return std::size_t{0};
                }
                std::string deliveredBytes;
                static_cast<void>(qpackDecoder.noteFieldSectionDelivered(0, deliveredBytes));
                return qpackDecodedFields.size();
            },
            results, checksum, failureCount);

    measureCase(
            "hpack-decode",
            [&decoder, &encodedHeaderBlock, &decodedHeaderFields]
            {
                decodedHeaderFields.clear();
                if (!decoder.decode(encodedHeaderBlock, decodedHeaderFields, nullptr))
                {
                    return std::size_t{0};
                }
                return decodedHeaderFields.size();
            },
            results, checksum, failureCount);

    const std::string http1RequestText = makeHttp1RequestText();
    Net::HttpParser parser;
    measureCase(
            "http1-parse-request",
            [&parser, &http1RequestText]
            {
                // 解析器一条报文一份状态：拿到 Done 之后必须 reset() 才能接着解析下一条，
                // 不复位的话第二次喂入一个字节都不吃，量出来的是空转
                const Net::ParseStatus status = parser.parse(http1RequestText.data(), http1RequestText.size());
                const std::size_t consumed = status == Net::ParseStatus::Done ? parser.consumedByteCount() : 0;
                parser.reset();
                return consumed;
            },
            results, checksum, failureCount);

    const std::string http2HeadersFrameText = makeHttp2HeadersFrameText();
    Net::Http2FrameDecoder frameDecoder;
    measureCase(
            "http2-frame-decode",
            [&frameDecoder, &http2HeadersFrameText]
            {
                // 与解析器同理：产出帧必须取走、错误态必须复位，否则后续喂入一律不消费
                const Net::Http2FrameDecodeStatus status = frameDecoder.parse(http2HeadersFrameText.data(), http2HeadersFrameText.size());
                std::size_t payloadLength = 0;
                if (status == Net::Http2FrameDecodeStatus::Frame)
                {
                    const Net::Http2Frame frame = frameDecoder.takeFrame();
                    payloadLength = frame.payload.size();
                }
                frameDecoder.reset();
                return payloadLength;
            },
            results, checksum, failureCount);

    // 日期用固定时间点：把 now() 放进被测区间量到的是取时钟的开销，而真实的 Date 头是「一秒钟算一次、整秒复用」
    const auto fixedTimePoint = std::chrono::system_clock::time_point{std::chrono::seconds{1789000000}};
    measureCase(
            "http-date-format",
            [fixedTimePoint]
            {
                const std::string text = Net::formatHttpDate(fixedTimePoint);
                return text.size();
            },
            results, checksum, failureCount);

    // 与上一例对照就是「每条响应都重算」的浪费：这一例走按秒缓存的线程局部文本
    measureCase(
            "http-date-now",
            []
            {
                const std::string_view text = Net::currentHttpDateText();
                return text.size() + static_cast<std::size_t>(text[0]);
            },
            results, checksum, failureCount);

    constexpr std::string_view kSampleHttpDate = "Sun, 06 Nov 1994 08:49:37 GMT";
    measureCase(
            "http-date-parse",
            []
            {
                const std::optional<std::chrono::system_clock::time_point> parsed = Net::parseHttpDate(kSampleHttpDate);
                return parsed.has_value() ? std::size_t{1} : std::size_t{0};
            },
            results, checksum, failureCount);

    Net::HttpRequestIdGenerator requestIdGenerator;
    measureCase(
            "request-id-generate",
            [&requestIdGenerator]
            {
                const std::string requestId = requestIdGenerator.next();
                return requestId.size();
            },
            results, checksum, failureCount);

    // 头部单值查询：真实请求几乎每条都会读一两个头部（If-None-Match、CORS、WebSocket 握手……），
    // 而存储的单值视图是「按需重建」的——第一次查询的代价取决于重建是否被单个查询触发。
    // 这里按「装好 10 条头部 → 查 1 次」与「查 5 次」两种形态各测一例，
    // 装头部是两侧共同的本底开销，只用于把视图标脏，不参与差异
    const std::vector<std::pair<std::string, std::string>> headerFixtures = {
        {"host", "api.example.com"},
        {"user-agent", "curl/8.7.1"},
        {"accept", "*/*"},
        {"content-type", "application/json"},
        {"content-length", "64"},
        {"accept-encoding", "gzip, deflate, br"},
        {"authorization", "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"},
        {"x-request-id", "0001-0000000000000abc"},
        {"connection", "keep-alive, Upgrade"},
        {"cookie", "session=8f14e45fceea167a5a36dedd4bea2543; theme=dark"},
    };
    const auto refillHeaderStore = [&headerFixtures](Net::HttpHeaderFieldStore &store)
    {
        store.clear();
        for (const auto &[name, value]: headerFixtures)
        {
            store.append(name, value);
        }
    };

    Net::HttpHeaderFieldStore singleLookupStore;
    refillHeaderStore(singleLookupStore);
    measureCase(
            "header-refill-only",
            [&singleLookupStore, &refillHeaderStore]
            {
                // 对照例：只装 10 条头部不查，用来把下面两例的共同本底开销扣掉
                refillHeaderStore(singleLookupStore);
                return singleLookupStore.fields().size();
            },
            results, checksum, failureCount);

    measureCase(
            "header-get-once",
            [&singleLookupStore, &refillHeaderStore]
            {
                // 每轮都从「视图已过期」的起点开始，才量得到首查询的代价（真实请求正是如此）
                refillHeaderStore(singleLookupStore);
                const std::optional<std::string> value = singleLookupStore.get("accept-encoding");
                return value.has_value() ? value->size() : std::size_t{0};
            },
            results, checksum, failureCount);

    Net::HttpHeaderFieldStore multiLookupStore;
    refillHeaderStore(multiLookupStore);
    measureCase(
            "header-get-five",
            [&multiLookupStore, &refillHeaderStore, &headerFixtures]
            {
                refillHeaderStore(multiLookupStore);
                std::size_t totalLength = 0;
                for (std::size_t index = 0; index < 5; ++index)
                {
                    const std::optional<std::string> value = multiLookupStore.get(headerFixtures[index].first);
                    totalLength += value.has_value() ? value->size() : 0;
                }
                return totalLength;
            },
            results, checksum, failureCount);

    // 列表 token 判定与「取首条」这两条读路径走的是 HttpRequest/HttpSession 的 keep-alive 与
    // request-id 逻辑：每条请求都要跑几遍，判据本身只要布尔与首值，却常顺手把整列值拷出来
    Net::HttpHeaderFieldStore tokenStore;
    refillHeaderStore(tokenStore);
    measureCase(
            "header-token-hit",
            [&tokenStore, &refillHeaderStore]
            {
                refillHeaderStore(tokenStore);
                // 改后形态：在存储内部逐段比对，不构造取值列表
                return tokenStore.containsListToken("connection", "keep-alive")
                               ? std::size_t{1}
                               : std::size_t{0};
            },
            results, checksum, failureCount);

    Net::HttpHeaderFieldStore firstValueStore;
    refillHeaderStore(firstValueStore);
    measureCase(
            "header-first-value",
            [&firstValueStore, &refillHeaderStore]
            {
                refillHeaderStore(firstValueStore);
                // 改后形态与 HttpRequestIdGenerator::resolve() 一致：只取首条，不构造 vector
                const std::optional<std::string> clientRequestId = firstValueStore.firstValue("x-request-id");
                return clientRequestId.has_value() ? clientRequestId->size() : std::size_t{0};
            },
            results, checksum, failureCount);

    // 响应头序列化：每条响应一次，且它是「先按 headReserveLength 统计一遍、再 appendHead 写一遍」
    // 的两趟遍历所在。Date 走的是响应对象内的缓存值（与真实服务里同一秒内复用一致），
    // 格式化本身的代价另有 http-date-format 一例量着，这里不重复计
    const std::string responseBody(64, 'x');
    Net::HttpResponse responseFixture;
    responseFixture.setStatus(200);
    responseFixture.setHeader("content-type", "application/json");
    responseFixture.setHeader("server", "AsynGyanis/1.1");
    responseFixture.setHeader("cache-control", "no-store");
    responseFixture.setHeader("x-request-id", "0001-0000000000000abc");
    responseFixture.setBody(responseBody);
    measureCase(
            "response-head-serialize",
            [&responseFixture]
            {
                const std::string serializedHead = responseFixture.serializeHead();
                // 自检数的是「行数」而不是长度：setHeader 会拒掉非法名，只量长度看不出头部
                // 有没有真的进去。状态行 1 + 自设 4 + 自动 content-length 1 + 自动 Date 1 + 终止空行 1
                const std::size_t lineCount = static_cast<std::size_t>(std::ranges::count(serializedHead, '\n'));
                return lineCount >= 8 ? serializedHead.size() : std::size_t{0};
            },
            results, checksum, failureCount);

    // 长头部名（超过短字符串缓冲的 15 字符）查询：这类名字要现造一个归一化副本，
    // 「查一个值」里因此藏着一次堆分配。CORS 预检读 access-control-request-method、
    // WebSocket 握手读 sec-websocket-version 都落在这一格。
    // refill-long-name 是共同本底（比上面那组多装一条），两例相减才是读路径的代价
    std::vector<std::pair<std::string, std::string>> longNameFixtures = headerFixtures;
    longNameFixtures.emplace_back("access-control-request-method", "POST");
    const auto refillLongNameStore = [&longNameFixtures](Net::HttpHeaderFieldStore &store)
    {
        store.clear();
        for (const auto &[name, value]: longNameFixtures)
        {
            store.append(name, value);
        }
    };

    Net::HttpHeaderFieldStore longNameStore;
    refillLongNameStore(longNameStore);
    measureCase(
            "header-refill-long-name",
            [&longNameStore, &refillLongNameStore]
            {
                refillLongNameStore(longNameStore);
                return longNameStore.fields().size();
            },
            results, checksum, failureCount);

    measureCase(
            "header-get-long-name",
            [&longNameStore, &refillLongNameStore]
            {
                refillLongNameStore(longNameStore);
                const std::optional<std::string> value = longNameStore.get("access-control-request-method");
                return value.has_value() ? value->size() : std::size_t{0};
            },
            results, checksum, failureCount);

    // 写路径的同一格：覆盖一条既有的长名头部。改前一次 setHeader 要造「折小写的副本 + 传参副本」
    // 两份字符串，改后连名字都不再拷（值本来就要入库，那份拷贝省不掉）
    Net::HttpResponse corsResponse;
    corsResponse.setHeader("access-control-allow-origin", "https://example.com");
    measureCase(
            "response-set-header-long-name",
            [&corsResponse]
            {
                // setHeader 返回 false 表示被拒（自检据此判定用例没走成功路径）
                return corsResponse.setHeader("Access-Control-Allow-Origin", "https://example.com") ? std::size_t{1}
                                                                                                    : std::size_t{0};
            },
            results, checksum, failureCount);

    printTable(results, failureCount, checksum);
    if (!jsonOutputPath.empty())
    {
        writeJson(jsonOutputPath, results, failureCount);
    }
    // 自检失败要能从退出码看出来：门禁脚本也会按 JSON 里的 failures 判失败
    return failureCount == 0 ? 0 : 1;
}

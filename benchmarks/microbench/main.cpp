// 热路径微基准：HPACK 编解码、h1 请求解析、h2 帧解码、HTTP 日期格式化/解析、request-id 生成、
// 头部单值查询与列表 token 判定、响应头序列化、h2/h3 组头块的两种走法、响应压缩的一次性耗时、
// 事件循环的跨线程唤醒、连接池的取出与归还（稳态复用与每次新建两条通路）、SQLite 驱动的一条查询
// 与一条写入、QUIC 流层在长连接上的每包编帧。
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
#include "Core/Coroutine/Scheduler.h"
#include "Core/Coroutine/ThreadPool.h"
#include "Core/EventLoop/EventLoop.h"
#include "Database/Common/ConnectionConfig.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseResult.h"
#include "Database/Common/DatabaseValue.h"
#include "Database/Dialect/DialectRegistry.h"
#include "Database/Pool/ConnectionPool.h"
#include "Database/Pool/PoolConfig.h"
#include "Database/Pool/PooledConnection.h"
#include "Database/Queryable/Column.h"
#include "Database/Queryable/Expression.h"
#include "Database/Queryable/Queryable.h"
#include "Database/Queryable/TableSchema.h"
#include "Base/Config/ConfigManager.h"
#include "Base/Log/Formatters/DefaultFormatter.h"
#include "Base/Log/Formatters/JsonFormatter.h"
#include "Base/Log/LogEvent.h"
#include "Base/Log/Logger.h"
#include "Base/Log/Sinks/AsyncSink.h"
#include "Base/Log/Sinks/LogSink.h"
#include "Database/Sqlite/SqliteConnection.h"
#include "Net/Http/Compression.h"
#include "Net/Http/FileSender.h"
#include "Net/Http/Gzip.h"
#include "Net/Http/HttpDate.h"
#include "Net/Http/HttpHeaderFieldStore.h"
#include "Net/Http/HttpHeaderRules.h"
#include "Net/Http/HttpParser.h"
#include "Net/Http/HttpRequestId.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"
#include "Net/Http2/Hpack.h"
#include "Net/Http2/Http2Frame.h"
#include "Net/Http3/Qpack.h"
#include "Net/Http3/Http3Frame.h"
#include "Net/Quic/Streams/QuicStreamLayer.h"
#include "Net/WebSocket/PerMessageDeflate.h"
#include "Net/WebSocket/WebSocketFrame.h"
#include "Platform/FileSystem/FileBasicInfo.h"
#include "Platform/IO/EventNotifier.h"
#include "Platform/IO/FileContents.h"
#include "Platform/IO/MemoryMappedFile.h"
#include "Platform/System/PlatformTime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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

    /**
     * @brief 造一段接近真实响应的可压缩正文：词表按线性同余挑，数字与行长都在变
     * @details 把同一句话重复 N 遍会把压缩器送进退化路径（实测那样一份 256 KiB 正文能压到 0.05%），
     *          量出来的每字节耗时远低于真实 HTML/JSON 响应，照着它做取舍会做错决定。种子固定，
     *          因此各轮与多次运行拿到的都是逐字节相同的同一份正文（基准必须可重复）
     * @param minimumSize 正文至少要有的字节数
     * @return std::string 生成好的正文
     */
    [[nodiscard]] std::string makeResponseLikeBody(const std::size_t minimumSize)
    {
        static constexpr std::string_view kWords[] = {
                "request", "connection", "timeout", "payload", "etag", "server", "client", "window",
                "congestion", "stream", "header", "packet", "latency", "throughput", "backpressure", "route"};
        constexpr std::size_t kWordCount = sizeof(kWords) / sizeof(kWords[0]);

        std::string body;
        std::uint32_t state = 0x2545F491U;
        while (body.size() < minimumSize)
        {
            state = state * 1664525U + 1013904223U;
            body += "<";
            body += kWords[(state >> 7) % kWordCount];
            body += " id=\"";
            body += std::to_string(state % 1000003U);
            body += "\" ms=\"";
            body += std::to_string((state >> 11) % 997U);
            body += "\"/></row>\n";
        }
        return body;
    }

    /**
     * @brief 打桩的数据库连接：让连接池的取出/归还整条链路在没有真库的前提下跑起来
     * @details 池在获取与归还路径上只碰 connect / disconnect / isConnected / resetSessionState，
     *          且 isConnected() 被约定成纯状态查询（不发网络探活），因此这几个钩子就足以代表一条
     *          真实连接的稳态形态。execute() 不在本文件要量的通路上，返回空结果即可。
     */
    class StubConnection : public Database::DatabaseConnection
    {
    public:
        /**
         * @brief 模拟建连成功
         * @return 恒为 true
         */
        bool connect() override
        {
            // 状态标志由本端同步维护，与真实驱动的口径一致
            m_isConnected = true;
            return true;
        }

        /**
         * @brief 模拟断开
         */
        void disconnect() override
        {
            m_isConnected = false;
        }

        /**
         * @brief 纯状态查询：与真实驱动一样不发任何网络往返
         * @return 当前是否处于已连接状态
         */
        [[nodiscard]] bool isConnected() const override { return m_isConnected; }

        /**
         * @brief 占位执行：本文件不量查询通路
         * @param command 未被使用的命令文本
         * @return 恒为空结果
         */
        [[nodiscard]] std::unique_ptr<Database::DatabaseResult> execute(const std::string_view command) override
        {
            static_cast<void>(command);
            return nullptr;
        }

        /**
         * @brief 方言归属
         * @return 恒为 Sqlite（连接池不据此分支，仅为满足接口）
         */
        [[nodiscard]] Database::DatabaseType databaseType() const override { return Database::DatabaseType::Sqlite; }
    };

    /// 驱动侧用例的表行数：读写两侧都按主键命中，因此行数不随轮次增长
    inline constexpr int kBenchRowCount = 4096;

    /**
     * @brief 建一份填满的内存 SQLite 库，给驱动侧的耗时用例当底
     * @details 用 ":memory:"：这几例要量的是「编译 SQL + 绑定 + 推进游标」这条 CPU 通路，掺进真实
     *          文件 IO 会让读数随磁盘状态漂。行数固定且写侧只改不增，否则后面的轮次单调变慢，
     *          改动前后的两枚二进制就跑不到同一种数据规模上。
     * @return std::unique_ptr<Database::SqliteConnection> 已建好并填满的连接；建库失败时为空
     */
    [[nodiscard]] std::unique_ptr<Database::SqliteConnection> makePopulatedMemoryDatabase()
    {
        Database::ConnectionConfig configuration;
        configuration.database = ":memory:";

        auto connection = std::make_unique<Database::SqliteConnection>(configuration);
        if (!connection->connect())
        {
            return nullptr;
        }

        static_cast<void>(connection->execute("CREATE TABLE bench (id INTEGER PRIMARY KEY, label TEXT NOT NULL)"));
        static_cast<void>(connection->execute("BEGIN"));

        std::array<Database::DatabaseValue, 2> parameters{std::int64_t{0}, std::string("bench-label-0000")};
        for (int rowIndex = 0; rowIndex < kBenchRowCount; ++rowIndex)
        {
            parameters[0] = static_cast<std::int64_t>(rowIndex);
            static_cast<void>(connection->execute("INSERT INTO bench VALUES(?, ?)", std::span<const Database::DatabaseValue>(parameters)));
        }
        static_cast<void>(connection->execute("COMMIT"));

        return connection;
    }

    /// ORM 侧用例的底表行数：读 20 与 50 行都落在 SqliteResult 的快照物化上限（256 行）之内
    inline constexpr int kOrmBenchRowCount = 64;

    /// 方言批量渲染用例的行数：6 列 × 150 行 = 900 个占位符，留在 SQLite 999 个参数上限之内
    inline constexpr int kDialectBatchRowCount = 150;

    /**
     * @brief ORM 侧用例的行结构体：三列长文本，使每个单元格的取值必然落堆
     *
     * @details 文本长度刻意超过 SSO 阈值：短文本的拷贝不触发分配，会把「每格多拷一次整串」这类
     *          成本掩盖成常数。三列文本 + 一列整数 + 一列浮点也贴近真实业务行的形状。
     */
    struct BenchAccountRow
    {
        std::int64_t id;       ///< 主键
        std::string  label;    ///< 长文本列
        std::string  note;     ///< 长文本列
        std::string  payload;  ///< 长文本列
        std::int64_t quantity; ///< 整数列
        double       price;    ///< 浮点列
    };

    /**
     * @brief 建一个绑定内存 SQLite 的连接池，并填满 ORM 用例的底表
     * @details 池上限压到 1：":memory:" 库随连接生命周期存在，多条连接会各自持有一份空库。
     *          行数固定且写入不增长，否则改动前后的两枚二进制跑不到同一种数据规模上。
     * @return std::unique_ptr<Database::ConnectionPool> 建库并填满的池；任一步失败时为空
     */
    [[nodiscard]] std::unique_ptr<Database::ConnectionPool> makeOrmBenchPool()
    {
        Database::PoolConfig poolConfiguration;
        poolConfiguration.maximumPoolSize = 1;

        auto pool = std::make_unique<Database::ConnectionPool>(
                []() -> std::unique_ptr<Database::DatabaseConnection>
                {
                    auto connection = std::make_unique<Database::SqliteConnection>(
                            Database::ConnectionConfig::sqliteDefault(":memory:"));
                    // 池的工厂契约要求交出「已经 connect() 完成」的连接，池不会替调用方连接
                    connection->connect();
                    return connection;
                },
                poolConfiguration);

        Database::PooledConnection connection = pool->acquire();
        if (!connection)
        {
            return nullptr;
        }
        if (connection->execute("CREATE TABLE bench_accounts ("
                                "id INTEGER PRIMARY KEY, "
                                "label TEXT NOT NULL, "
                                "note TEXT NOT NULL, "
                                "payload TEXT NOT NULL, "
                                "quantity INTEGER NOT NULL, "
                                "price REAL NOT NULL)") == nullptr)
        {
            return nullptr;
        }

        // 每行 6 个占位符，150 行仍在 SQLite 的单语句参数上限之内；建底表走原生 SQL，不掺进 ORM 的成本
        std::array<Database::DatabaseValue, 6> parameters{std::int64_t{0},
                                                          std::string("bench-account-label-value"),
                                                          std::string("bench-account-note-value"),
                                                          std::string("bench-account-payload-value"),
                                                          std::int64_t{0},
                                                          1.5};
        static_cast<void>(connection->execute("BEGIN"));
        for (int rowIndex = 0; rowIndex < kOrmBenchRowCount; ++rowIndex)
        {
            parameters[0] = static_cast<std::int64_t>(rowIndex);
            parameters[4] = static_cast<std::int64_t>(rowIndex);
            if (connection->execute("INSERT INTO bench_accounts VALUES (?, ?, ?, ?, ?, ?)",
                                    std::span<const Database::DatabaseValue>(parameters)) == nullptr)
            {
                return nullptr;
            }
        }
        static_cast<void>(connection->execute("COMMIT"));

        return pool;
    }
    /**
     * @brief 造一条「已经跑了 completedRequestCount 条完整请求」的 QUIC 流层
     * @details 每条请求按 h3 的真实形状走完：对端发来带 FIN 的双向流、本端交出去并原数报回接收额度、
     *          写出响应并确认。这样两侧记录都满足回收条件——本用例问的就是「连接活了很久之后，
     *          编一帧还要不要为历史的流买单」。
     * @param completedRequestCount 先跑完多少条请求
     * @return std::unique_ptr<Net::QuicStreamLayer> 建好的流层
     */
    std::unique_ptr<Net::QuicStreamLayer> makeAgedQuicStreamLayer(const std::uint64_t completedRequestCount)
    {
        Net::QuicTransportParameters parameters;
        // 额度与流数上限都给足：这里关心的是扫描成本，不该被流量控制或流数上限挡住
        parameters.initialMaximumData = 1ULL << 40;
        parameters.initialMaximumStreamDataBidirectionalLocal = 1ULL << 30;
        parameters.initialMaximumStreamDataBidirectionalRemote = 1ULL << 30;
        parameters.initialMaximumStreamDataUnidirectional = 1ULL << 30;
        parameters.initialMaximumBidirectionalStreams = 100000;
        parameters.initialMaximumUnidirectionalStreams = 100000;

        auto layer = std::make_unique<Net::QuicStreamLayer>(parameters);
        layer->adoptPeerParameters(parameters);

        static const std::vector<std::uint8_t> requestBody = {'G', 'E', 'T', ' ', '/'};
        static const std::vector<std::uint8_t> responseBody = {'2', '0', '0'};
        for (std::uint64_t index = 0; index < completedRequestCount; ++index)
        {
            const std::uint64_t streamId = index * 4U;

            Net::QuicStreamFrame incoming;
            incoming.streamId = streamId;
            incoming.offset = 0;
            incoming.data = std::span<const std::uint8_t>(requestBody);
            incoming.isFinal = true;
            static_cast<void>(layer->onStreamFrame(incoming));
            while (layer->hasDeliveries())
            {
                const std::optional<Net::QuicStreamDelivery> delivery = layer->takeDelivery();
                if (!delivery.has_value())
                {
                    break;
                }
                // 原数报回接收额度：不报的话这条流的入站记录永远不算收口
                layer->releaseReceiveWindow(delivery->streamId, delivery->bytes.size());
            }

            static_cast<void>(layer->writeStreamData(streamId, std::span<const std::uint8_t>(responseBody), true));
            // 把响应编出去并逐轮确认，直到这条流两侧都收口（下一轮 collectFrames 时会摘掉记录）
            for (int round = 0; round < 4; ++round)
            {
                std::string frames;
                std::vector<Net::QuicStreamRange> sentRanges;
                std::vector<Net::QuicStreamAnnouncement> announcements;
                const bool hasFrames = layer->collectFrames(frames, 1200U, sentRanges, announcements);
                layer->onSendRangesAcknowledged(sentRanges);
                if (!announcements.empty())
                {
                    layer->onStreamAnnouncementsAcknowledged(announcements);
                }
                if (!hasFrames)
                {
                    break;
                }
            }
        }
        return layer;
    }
} // namespace

template<>
struct AsynGyanis::Database::Queryable::TableSchema<BenchAccountRow>
{
    static constexpr std::string_view kTableName = "bench_accounts";
    static constexpr auto kColumns = std::tuple{
        Column(&BenchAccountRow::id, "id"),
        Column(&BenchAccountRow::label, "label"),
        Column(&BenchAccountRow::note, "note"),
        Column(&BenchAccountRow::payload, "payload"),
        Column(&BenchAccountRow::quantity, "quantity"),
        Column(&BenchAccountRow::price, "price"),
    };
    static constexpr std::string_view kPrimaryKey = "id";
};

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
    // vector（每个头各构造名与值两个字符串），新写法只排一张视图表。这两例量的就是被去掉的那份拷贝。
    // 本例是纯分配器负载，读数随前序用例留下的堆上下文而变（同样的构造在干净进程的单文件探针里
    // 只要 283.7 ns），因此它只当「比视图表贵一个数量级」这把量级尺子用，别拿它读 1.2 倍以内的漂移
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

    // h3 出站 DATA 帧的两种排法（生产改动的正是这一处）：旧写法先把载荷拼进一份临时 string
    // 再整段搬进该流的待发缓冲，新写法直接拼进待发缓冲。两例都从「缓冲已清空」起步，
    // 差的只在那一趟载荷往返
    const std::vector<std::uint8_t> dataFramePayload(16 * 1024, 0x5a);
    const std::span<const std::uint8_t> dataFramePayloadView{dataFramePayload.data(), dataFramePayload.size()};
    std::string outboundBuffer;
    measureCase(
            "h3-data-frame-via-temporary",
            [&outboundBuffer, &dataFramePayloadView]
            {
                outboundBuffer.clear();
                std::string frameBytes;
                Net::appendHttp3Frame(frameBytes, Net::Http3DataFrame{dataFramePayloadView});
                outboundBuffer.append(frameBytes);
                return outboundBuffer.size();
            },
            results, checksum, failureCount);
    measureCase(
            "h3-data-frame-direct",
            [&outboundBuffer, &dataFramePayloadView]
            {
                outboundBuffer.clear();
                Net::appendHttp3FrameWithPayload(outboundBuffer, Net::Http3FrameType::Data, dataFramePayloadView);
                return outboundBuffer.size();
            },
            results, checksum, failureCount);

    // h2 出站 DATA 帧的同一对读法：旧写法先把载荷拼进一份临时帧串再整段搬进待发缓冲，
    // 新写法直接拼进去。载荷取对端默认通告的 SETTINGS_MAX_FRAME_SIZE（16 KiB），
    // 因为那趟拷贝正是随载荷线性放大的那一趟
    const std::string h2DataFramePayload(16 * 1024, 'q');
    measureCase(
            "h2-data-frame-via-temporary",
            [&outboundBuffer, &h2DataFramePayload]
            {
                outboundBuffer.clear();
                outboundBuffer.append(Net::encodeHttp2DataFrame(h2DataFramePayload, false, 1U));
                return outboundBuffer.size();
            },
            results, checksum, failureCount);
    measureCase(
            "h2-data-frame-direct",
            [&outboundBuffer, &h2DataFramePayload]
            {
                outboundBuffer.clear();
                Net::appendHttp2Frame(outboundBuffer, Net::Http2FrameType::Data, 0U, 1U, h2DataFramePayload);
                return outboundBuffer.size();
            },
            results, checksum, failureCount);

    // h2 一条响应的 HEADERS 帧两种排法的消融对照。载荷取「连接首帧上的响应头块」——另起一台没对齐
    // 动态表的编码器，字段才按字面量编出来（拿已对齐的编码器会得到 12 字节的纯索引块，短到落进
    // 短串内联，两例都一次分配也不碰，量的就不是生产形状）。旧写法是「substr 成片段 → 拼进 body
    // → 拼进临时帧串 → 搬进待发缓冲」，新写法直接拼进待发缓冲；两例产出必须逐字相同，否则量的就是两回事
    const std::vector<Net::HpackHeaderField> responseHeaderFields = {
            {":status", "200"},
            {"content-type", "application/json; charset=utf-8"},
            {"content-length", "1024"},
            {"date", "Sun, 20 Sep 2026 12:34:56 GMT"},
            {"server", "AsynGyanis/1.2"},
            {"x-request-id", "9f14e45fceea167a5a36dedd4bea2543"},
            {"cache-control", "no-store"},
    };
    Net::HpackEncoder firstResponseEncoder;
    const std::string responseHeaderBlock = firstResponseEncoder.encode(responseHeaderFields);
    if (responseHeaderBlock.size() <= 15U)
    {
        std::printf("  警告：响应头块只有 %zu 字节，落进短串内联，HEADERS 消融例量不到分配\n", responseHeaderBlock.size());
    }
    {
        Net::Http2HeadersPayload selfCheckPayload;
        selfCheckPayload.endStream = true;
        selfCheckPayload.endHeaders = true;
        selfCheckPayload.headerBlockFragment = responseHeaderBlock;
        std::string legacyRoute;
        legacyRoute.append(Net::encodeHttp2HeadersFrame(selfCheckPayload, 1U));
        std::string directRoute;
        Net::appendHttp2HeadersFrame(directRoute, responseHeaderBlock, true, true, 1U);
        if (legacyRoute != directRoute)
        {
            std::printf("  警告：h2 HEADERS 两种排法产出不一致，消融对照失去意义（旧 %zu 字节 / 新 %zu 字节）\n",
                        legacyRoute.size(), directRoute.size());
        }
        std::printf("  h2-head-frame-block-bytes = %zu\n", responseHeaderBlock.size());
    }
    measureCase(
            "h2-head-frame-via-temporary",
            [&outboundBuffer, &responseHeaderBlock]
            {
                // 改动前的完整形状：substr 成 owning 片段 → 攒进 body 串 → 拼进临时帧串 → 搬进待发缓冲
                outboundBuffer.clear();
                std::string fragment = responseHeaderBlock.substr(0, responseHeaderBlock.size());
                std::string body;
                body.append(fragment);
                // §6.2 的两位：END_STREAM = 0x1、END_HEADERS = 0x4（这里刻意不取框架常量，
                // 让这一例独立于被测代码描述线上形态）
                outboundBuffer.append(Net::encodeHttp2Frame(Net::Http2FrameType::Headers, 0x1 | 0x4, 1U, body));
                return outboundBuffer.size();
            },
            results, checksum, failureCount);
    measureCase(
            "h2-head-frame-direct",
            [&outboundBuffer, &responseHeaderBlock]
            {
                outboundBuffer.clear();
                Net::appendHttp2HeadersFrame(outboundBuffer, responseHeaderBlock, true, true, 1U);
                return outboundBuffer.size();
            },
            results, checksum, failureCount);

    // 静态文件每请求的元数据读取：三项分三样查（各开一次路径）还是一次查完。
    // 两侧读同一个临时文件，指纹不同也无妨——量的是取到这些数要付多少系统调用
    const std::filesystem::path metadataProbePath = std::filesystem::temp_directory_path() / "asyngyanis-microbench-meta.txt";
    {
        std::ofstream probeFile(metadataProbePath, std::ios::binary | std::ios::trunc);
        probeFile << "0123456789";
    }
    measureCase(
            "file-meta-three-queries",
            [&metadataProbePath]
            {
                std::error_code errorCode;
                std::size_t fingerprint = std::filesystem::is_regular_file(metadataProbePath, errorCode) ? 1U : 0U;
                fingerprint += static_cast<std::size_t>(std::filesystem::file_size(metadataProbePath, errorCode));
                const std::filesystem::file_time_type lastWriteTime = std::filesystem::last_write_time(metadataProbePath, errorCode);
                fingerprint += static_cast<std::size_t>(lastWriteTime.time_since_epoch().count() >> 7);
                return errorCode ? std::size_t{0} : fingerprint;
            },
            results, checksum, failureCount);
    measureCase(
            "file-meta-single-probe",
            [&metadataProbePath]
            {
                const std::optional<Platform::FileBasicInfo> info = Platform::queryFileBasicInfo(metadataProbePath);
                if (!info.has_value() || !info->isRegularFile)
                {
                    return std::size_t{0};
                }
                return info->sizeBytes + static_cast<std::size_t>(info->lastWriteSeconds >> 7);
            },
            results, checksum, failureCount);
    std::filesystem::remove(metadataProbePath);

    // 静态路径剩下的另一块每请求系统调用：把文件映射进来再解除。量它才知道「映射缓存」这项
    // 有意未做的事值不值——它与上面那次元数据查询相加，才是每请求在文件系统上的全部开销
    const std::filesystem::path mappingProbePath = std::filesystem::temp_directory_path() / "asyngyanis-microbench-mmap.txt";
    std::string mappingProbeBytes;
    {
        mappingProbeBytes.assign(64 * 1024, 'x');
        std::ofstream mappingFile(mappingProbePath, std::ios::binary | std::ios::trunc);
        mappingFile << mappingProbeBytes;
    }
    measureCase(
            "mmap-open-and-close",
            [&mappingProbePath]
            {
                // 每轮真的打开再解除映射：这正是一条静态响应在文件系统上的收尾开销，
                // 缓存命中时要省的也就是这一段
                Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(mappingProbePath);
                const std::size_t mappedLength = mappedFile.bytes().size();
                mappedFile = Platform::MemoryMappedFile{};
                return mappedLength;
            },
            results, checksum, failureCount);
    measureCase(
            "mmap-page-touch",
            [&mappingProbePath]
            {
                Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(mappingProbePath);
                // 首触缺页：映射成功后按页读一遍，等价于发送路径真的把这些字节交出去
                std::size_t touched = 0;
                const std::span<const std::byte> bytes = mappedFile.bytes();
                for (std::size_t offset = 0; offset < bytes.size(); offset += 4096)
                {
                    touched += std::to_integer<std::size_t>(bytes[offset]);
                }
                mappedFile = Platform::MemoryMappedFile{};
                return touched;
            },
            results, checksum, failureCount);
    std::filesystem::remove(mappingProbePath);

    // 静态正文的两条取法，一页大小（4 KiB）：小文件走一次顺序读还是建映射。
    // 服务端把正文交给套接字时两条路都要再拷一次，差别就在「建/解映射」对「读进堆缓冲」
    const std::filesystem::path smallFilePath = std::filesystem::temp_directory_path() / "asyngyanis-microbench-small.txt";
    {
        std::ofstream smallFile(smallFilePath, std::ios::binary | std::ios::trunc);
        smallFile << std::string(4096, 'q');
    }
    measureCase(
            "static-small-file-read",
            [&smallFilePath]
            {
                std::string body(4096, '\0');
                std::ifstream file(smallFilePath, std::ios::in | std::ios::binary);
                if (!file.is_open())
                {
                    return std::size_t{0};
                }
                file.read(body.data(), static_cast<std::streamsize>(body.size()));
                return static_cast<std::size_t>(file.gcount());
            },
            results, checksum, failureCount);
    measureCase(
            "static-small-file-mmap",
            [&smallFilePath]
            {
                Platform::MemoryMappedFile mappedFile = Platform::MemoryMappedFile::open(smallFilePath);
                const std::size_t mappedLength = mappedFile.bytes().size();
                mappedFile = Platform::MemoryMappedFile{};
                return mappedLength;
            },
            results, checksum, failureCount);
    std::filesystem::remove(smallFilePath);

    // 静态正文在生产形状下的每请求开销：读进响应自己那块按连接复用的缓冲（Platform 的
    // readFileContentsInto），两档大小各测「复用缓冲」与「每次新建缓冲」两条——后者才是
    // 把正文缓冲改成随响应复用之前真正的样子，两档相减就是那次改动省掉的分配量。
    // 上面那对 static-small-file-* 用的是 std::ifstream，不代表现在的服务端走法
    const std::filesystem::path bodyProbePath = std::filesystem::temp_directory_path() / "asyngyanis-microbench-body.bin";
    constexpr std::size_t kSmallBodyBytes = 4 * 1024;
    constexpr std::size_t kLargeBodyBytes = 256 * 1024;
    {
        std::ofstream bodyFile(bodyProbePath, std::ios::binary | std::ios::trunc);
        bodyFile << std::string(kLargeBodyBytes, 'b');
    }
    std::string reusedBodyBuffer;
    measureCase(
            "static-body-read-4k-reused",
            [&bodyProbePath, &reusedBodyBuffer]
            {
                const std::expected<std::size_t, std::error_code> readResult =
                        Platform::readFileContentsInto(bodyProbePath, 0, kSmallBodyBytes, reusedBodyBuffer);
                return readResult.has_value() ? *readResult : std::size_t{0};
            },
            results, checksum, failureCount);
    measureCase(
            "static-body-read-4k-fresh-buffer",
            [&bodyProbePath]
            {
                std::string freshBuffer;
                const std::expected<std::size_t, std::error_code> readResult =
                        Platform::readFileContentsInto(bodyProbePath, 0, kSmallBodyBytes, freshBuffer);
                return readResult.has_value() ? *readResult : std::size_t{0};
            },
            results, checksum, failureCount);
    measureCase(
            "static-body-read-256k-reused",
            [&bodyProbePath, &reusedBodyBuffer]
            {
                const std::expected<std::size_t, std::error_code> readResult =
                        Platform::readFileContentsInto(bodyProbePath, 0, kLargeBodyBytes, reusedBodyBuffer);
                return readResult.has_value() ? *readResult : std::size_t{0};
            },
            results, checksum, failureCount);
    measureCase(
            "static-body-read-256k-fresh-buffer",
            [&bodyProbePath]
            {
                std::string freshBuffer;
                const std::expected<std::size_t, std::error_code> readResult =
                        Platform::readFileContentsInto(bodyProbePath, 0, kLargeBodyBytes, freshBuffer);
                return readResult.has_value() ? *readResult : std::size_t{0};
            },
            results, checksum, failureCount);

    // 静态路由每请求还要在路径上做的两件小事，与上面的正文读取放在一起才知道值不值得动：
    // 扩展名查 MIME（含把整条路径转窄字符的那次拷贝，正是 HttpServer 现在的调用形状），
    // 以及只查 MIME、路径已经是窄字符串的情形。两行之差就是「给 contentTypeForFile 加一个
    // string_view 入口」理论上能省下的全部
    const std::filesystem::path mimeProbePath = bodyProbePath.parent_path() / "asyngyanis-microbench-index.html";
    const std::string mimeProbeText = mimeProbePath.string();
    measureCase(
            "mime-lookup-from-path",
            [&mimeProbePath]
            {
                // 现取 .string() 再喂进去，与 HttpServer 里那行调用逐字同形：量的包含整条路径的窄化拷贝
                const char *mimeType = Net::FileSender::contentTypeForFile(mimeProbePath.string());
                return std::string_view(mimeType).size();
            },
            results, checksum, failureCount);
    measureCase(
            "mime-lookup-from-string",
            [&mimeProbeText]
            {
                const char *mimeType = Net::FileSender::contentTypeForFile(mimeProbeText);
                return std::string_view(mimeType).size();
            },
            results, checksum, failureCount);
    std::filesystem::remove(bodyProbePath);

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

    // request-id 落定到请求上的两条出口（同一枚二进制里的消融对照）：旧写法每条请求现造一个串再
    // 交给请求（连着容量接管过来、把手上那块还掉），新写法先生成到调用线程的复用缓冲、再原地写进
    // 请求字段。计时体内做完一整趟——被量的正是「每请求一次堆块取还」这件事本身
    Net::HttpRequest requestIdTarget;
    measureCase(
            "request-id-land-owning",
            [&requestIdGenerator, &requestIdTarget]
            {
                requestIdTarget.setRequestId(requestIdGenerator.resolve(requestIdTarget));
                return requestIdTarget.requestId().size();
            },
            results, checksum, failureCount);
    measureCase(
            "request-id-land-into",
            [&requestIdGenerator, &requestIdTarget]
            {
                requestIdGenerator.resolveInto(requestIdTarget);
                return requestIdTarget.requestId().size();
            },
            results, checksum, failureCount);

    // 连接池的两条通路（本文件第一次量 Database）：一条是保活复用的稳态「取出 + 归还」，另一条是
    // maximumLifetimeSeconds 为 0 时「每次归还都丢弃、每次取出都新建」的抖动形态。前者量的是那几把
    // 锁与映射表查询的固定开销，后者额外把建连、映射表插删与断开一并计入——两种形态在真实服务里都
    // 存在（连接池被配成短存活期，或对端定期掐线），只量其一都会做错取舍。
    // 抖动形态还包含丢弃出口那一次「叫醒等待者」的取锁：这条出口每次归还都走，与有没有人等在决定
    // 读数的这一项无关，别把它当成稳态通路的退化
    Database::PoolConfig steadyPoolConfiguration;
    steadyPoolConfiguration.maximumPoolSize            = 4;
    steadyPoolConfiguration.idleTimeoutSeconds         = 3600;
    steadyPoolConfiguration.maximumLifetimeSeconds     = 3600;
    steadyPoolConfiguration.healthCheckIntervalSeconds = 3600; // 后台驱逐不参与这两例的时序
    const auto makeStubConnection = []() -> std::unique_ptr<Database::DatabaseConnection>
    {
        return std::make_unique<StubConnection>();
    };

    Database::ConnectionPool steadyPool(makeStubConnection, steadyPoolConfiguration);
    measureCase(
            "pool-acquire-return",
            [&steadyPool]
            {
                // 局部对象析构即归还：一次操作 = 一整趟取出与归还
                const Database::PooledConnection connection = steadyPool.acquire();
                return static_cast<std::uint64_t>(connection ? 1U : 0U);
            },
            results, checksum, failureCount);

    Database::PoolConfig churnPoolConfiguration = steadyPoolConfiguration;
    churnPoolConfiguration.maximumLifetimeSeconds = 0;
    Database::ConnectionPool churnPool(makeStubConnection, churnPoolConfiguration);
    measureCase(
            "pool-churn-acquire-return",
            [&churnPool]
            {
                const Database::PooledConnection connection = churnPool.acquire();
                return static_cast<std::uint64_t>(connection ? 1U : 0U);
            },
            results, checksum, failureCount);

    // SQLite 驱动的一条查询与一条写入（本文件第一次量驱动侧）。这两例里「编译 SQL」与「推进游标」
    // 目前是混在一起的：每次调用都重新 sqlite3_prepare_v2 再 finalize，所以读数里含一整趟词法分析、
    // 语法分析与字节码生成。参数表放在计时体外、只改主键，为的是把驱动本身的开销与调用方造参数的
    // 开销分开——被量的应该是驱动那一侧
    const std::unique_ptr<Database::SqliteConnection> benchDatabase = makePopulatedMemoryDatabase();
    if (benchDatabase == nullptr)
    {
        std::printf("  跳过 SQLite 两例：内存库没建起来\n");
    }
    else
    {
        std::array<Database::DatabaseValue, 1> keyParameters{std::int64_t{0}};
        // 占位符顺序就是绑定顺序：这条 SQL 里第一个 ? 是 label、第二个才是 id，摆反了会一行都不命中
        std::array<Database::DatabaseValue, 2> updateParameters{std::string("bench-label-0000"), std::int64_t{0}};
        int                                    benchCursor = 0;

        measureCase(
                "sqlite-select-by-primary-key",
                [&benchDatabase, &keyParameters, &benchCursor]() -> std::uint64_t
                {
                    keyParameters[0] = static_cast<std::int64_t>(benchCursor = (benchCursor + 1) % kBenchRowCount);
                    const std::unique_ptr<Database::DatabaseResult> result = benchDatabase->execute(
                            "SELECT label FROM bench WHERE id = ?", std::span<const Database::DatabaseValue>(keyParameters));
                    if (result == nullptr || !result->next())
                    {
                        return 0U;
                    }
                    // 真把那一列取出来：只判 next() 的话游标推进与列值物化都不在计时里
                    return std::get<std::string>(result->getValue(std::string_view("label"))).size();
                },
                results, checksum, failureCount);

        measureCase(
                "sqlite-update-by-primary-key",
                [&benchDatabase, &updateParameters, &benchCursor]() -> std::uint64_t
                {
                    updateParameters[1] = static_cast<std::int64_t>(benchCursor = (benchCursor + 1) % kBenchRowCount);
                    const std::unique_ptr<Database::DatabaseResult> result = benchDatabase->execute(
                            "UPDATE bench SET label = ? WHERE id = ?", std::span<const Database::DatabaseValue>(updateParameters));
                    if (result == nullptr)
                    {
                        return 0U;
                    }
                    // 影响行数当自检证据：为 0 说明这条 UPDATE 根本没命中行，量到的就是空跑
                    return static_cast<std::uint64_t>(result->affectedRowCount());
                },
                results, checksum, failureCount);
    }

    // ORM 侧的三段合成本：查询树副本 + 方言翻译 + 结果映射。上一行的 sqlite-select-* 只量到驱动那一侧，
    // 这三例把差距定位到「ORM 自己加了多少钱」上，之后动查询树或映射路径就有读数可依，不必再靠推理取舍。
    const std::unique_ptr<Database::ConnectionPool> ormPool = makeOrmBenchPool();
    if (ormPool == nullptr)
    {
        std::printf("  跳过 ORM 三例：内存库没建起来\n");
    }
    else
    {
        int ormCursor = 0;
        measureCase(
                "orm-first-by-primary-key",
                [&ormPool, &ormCursor]() -> std::uint64_t
                {
                    try
                    {
                        Database::Queryable::Queryable<BenchAccountRow> query(*ormPool);
                        query.where(Database::Queryable::Column(&BenchAccountRow::id, "id")
                                    == static_cast<std::int64_t>(ormCursor = (ormCursor + 1) % kOrmBenchRowCount));
                        const std::optional<BenchAccountRow> row = query.first();
                        // 真把文本列取出来：只判有没有命中的话，逐列取值与类型转换都不在计时里
                        return row.has_value() ? row->label.size() + row->payload.size() : 0U;
                    }
                    catch (const std::exception &)
                    {
                        return 0U;
                    }
                },
                results, checksum, failureCount);

        // 20 行与 50 行成对：两档的差值就是「每多一行」的边际成本，也是判断映射路径是否仍在按行分配的依据
        const auto measureOrmList = [&ormPool](const std::string_view caseName, const std::size_t rowLimit,
                                               std::vector<CaseResult> &results, std::uint64_t &checksum, int &failureCount)
        {
            measureCase(
                    std::string(caseName),
                    [&ormPool, rowLimit]() -> std::uint64_t
                    {
                        try
                        {
                            Database::Queryable::Queryable<BenchAccountRow> query(*ormPool);
                            query.orderBy(Database::Queryable::asc("id"));
                            query.limit(rowLimit);
                            std::vector<BenchAccountRow> rows = query.toList();
                            std::uint64_t payloadBytes = 0U;
                            for (const BenchAccountRow &row: rows)
                            {
                                payloadBytes += row.label.size() + row.note.size() + row.payload.size();
                            }
                            return payloadBytes;
                        }
                        catch (const std::exception &)
                        {
                            return 0U;
                        }
                    },
                    results, checksum, failureCount);
        };
        measureOrmList("orm-tolist-20-rows", 20U, results, checksum, failureCount);
        measureOrmList("orm-tolist-50-rows", 50U, results, checksum, failureCount);
    }

    // 方言纯文本合成本：一次多行 INSERT 的组帧。这一例不碰驱动也不碰结果集，量到的全是「每占位符」的成本，
    // 因此占位符渲染方式（每格一个临时串还是一次 push_back）在这里能直接读出来
    const std::shared_ptr<Database::SqlDialect> batchDialect =
            Database::DialectRegistry::dialectFor(Database::DatabaseType::Sqlite);
    Database::Queryable::QueryNode batchNode;
    batchNode.tableName     = "bench_accounts";
    batchNode.selectColumns = {"id", "label", "note", "payload", "quantity", "price"};
    std::vector<std::vector<Database::DatabaseValue> > batchRows;
    batchRows.reserve(static_cast<std::size_t>(kDialectBatchRowCount));
    for (int rowIndex = 0; rowIndex < kDialectBatchRowCount; ++rowIndex)
    {
        batchRows.emplace_back(std::vector<Database::DatabaseValue>{static_cast<std::int64_t>(rowIndex),
                                                                   std::string("bench-account-label-value"),
                                                                   std::string("bench-account-note-value"),
                                                                   std::string("bench-account-payload-value"),
                                                                   static_cast<std::int64_t>(rowIndex),
                                                                   1.5});
    }
    measureCase(
            "dialect-insert-batch-150-rows",
            [&batchDialect, &batchNode, &batchRows]() -> std::uint64_t
            {
                const Database::SqlStatement statement =
                        batchDialect->translateInsertBatch(batchNode, std::span<const std::vector<Database::DatabaseValue> >(batchRows));
                // 参数个数当自检证据：少一个就说明渲染与收集不同源，量到的只是半条语句
                return statement.parameters.size() == batchRows.size() * 6U ? static_cast<std::uint64_t>(statement.sql.size()) : 0U;
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
                // 存储交出的是「字节缓冲 + 偏移」，不再交出 owning 容器；这里只要求常数级的自检
                //（非空即说明装进去过），逐条点数会把一趟遍历算进本底读数里
                return singleLookupStore.empty() ? 0U : 1U;
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
    // 承接返回值的 optional 必须在计时之外：留在体内就变成每轮现取现还一块堆内存，这一例量的
    // 就成了分配器那个 size class 的运气而不是查询本身。实测同一份被测代码会因此随无关文件的
    // 代码布局在 ~320 ns 与 ~540 ns 之间摆（相邻的 header-refill-only 始终在 1% 内），
    // 挪出来之后同一二进制跨进程的重测离散回到几个百分点
    std::optional<std::string> clientRequestId;
    measureCase(
            "header-first-value",
            [&firstValueStore, &refillHeaderStore, &clientRequestId]
            {
                refillHeaderStore(firstValueStore);
                // 改后形态与 HttpRequestIdGenerator::resolve() 一致：只取首条，不构造 vector
                clientRequestId = firstValueStore.firstValue("x-request-id");
                return clientRequestId.has_value() ? clientRequestId->size() : std::size_t{0};
            },
            results, checksum, failureCount);

    // 同一条查询的「不拷贝」出口，与 header-first-value 成对：差额就是把取值拷进一份新串
    // 的代价（这个 fixture 的 x-request-id 超过短串内联长度，owning 版每轮要向堆要一次）。
    // 走这条出口的是 request-id 的校验阶段与所有「只看存在性/读完就丢」的判定
    std::optional<std::string_view> clientRequestIdView;
    measureCase(
            "header-first-value-view",
            [&firstValueStore, &refillHeaderStore, &clientRequestIdView]
            {
                refillHeaderStore(firstValueStore);
                clientRequestIdView = firstValueStore.firstValueView("x-request-id");
                return clientRequestIdView.has_value() ? clientRequestIdView->size() : std::size_t{0};
            },
            results, checksum, failureCount);

    measureCase(
            "header-contains",
            [&firstValueStore, &refillHeaderStore]
            {
                refillHeaderStore(firstValueStore);
                // 存在性判定：过去调用方写 getHeader(x).has_value()，为一次布尔拷出整个取值
                return firstValueStore.contains("x-request-id") ? std::size_t{1} : std::size_t{0};
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

    // 与上一例同源，唯一差别是头部序列化进一块按连接复用的缓冲（保活会话走的路径）：
    // 相减即是「每响应省掉的一次堆分配 + 释放」的代价，response-head-serialize 是本底
    std::string reusedHeadScratch;
    measureCase(
            "response-head-serialize-reused",
            [&responseFixture, &reusedHeadScratch]
            {
                responseFixture.serializeHeadInto(reusedHeadScratch);
                const std::size_t lineCount = static_cast<std::size_t>(std::ranges::count(reusedHeadScratch, '\n'));
                return lineCount >= 8 ? reusedHeadScratch.size() : std::size_t{0};
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
                return longNameStore.empty() ? 0U : 1U;
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

    // 路由流式探测：h1/h2/h3 会话每条请求头部收齐后都要调 hasStreamingRoute 决定派发时机，
    // 微基准此前不覆盖 Router。路由表含一条 any("*") 兜底（HttpServer 开静态目录时正是这么挂的）
    // 与若干 ":id" 路由，贴近真实。两例钉住本轮两处优化各自留下的稳态成本：
    //  · get：GET 撞方法闸直接返回，不再跑整表扫描、也不再让通配路由把整段路径拷成 std::string；
    //  · post-param：POST 命中 ":id" 流式路由仍走匹配，但参数收集被关掉（免去键值各一次字符串构造）。
    Net::Router streamingRouter;
    const auto noOpHandler = []([[maybe_unused]] Net::HttpRequest &request,
                                [[maybe_unused]] Net::HttpResponse &response) -> Core::Task<void>
    { co_return; };
    streamingRouter.postStreaming("/upload/:id", noOpHandler);
    streamingRouter.get("/api/users/:id", noOpHandler);
    streamingRouter.post("/api/orders/:id", noOpHandler);
    streamingRouter.any("*", noOpHandler);
    measureCase(
            "router-streaming-get",
            [&streamingRouter]
            {
                // 累加进返回值：既保证这次跨库调用不被优化掉，又让自检把它认作成功路径（恒非 0）
                return std::size_t{1} + static_cast<std::size_t>(
                        streamingRouter.hasStreamingRoute(Net::HttpMethod::GET, "/static/css/main.css"));
            },
            results, checksum, failureCount);
    measureCase(
            "router-streaming-post-param",
            [&streamingRouter]
            {
                // POST 命中 /upload/:id 的流式路由 → true（非 0）；量的是「匹配但不收集参数」这条
                return streamingRouter.hasStreamingRoute(Net::HttpMethod::POST, "/upload/42") ? std::size_t{1}
                                                                                              : std::size_t{0};
            },
            results, checksum, failureCount);

    // 派发一整趟（Router::route）：会话把请求交给处理函数走一遍管道的成本。两例分别钉
    // 「命中精确路由」与「扫过模式候选并收下 :id」——参数收集表按候选新建还是跨候选复用，
    // 显形的正是后一例：候选每多扫一条，旧写法就多一张空哈希表（MSVC 上每张两次堆块取还）
    Net::Router dispatchRouter;
    const auto createdHandler = []([[maybe_unused]] Net::HttpRequest &request,
                                   Net::HttpResponse &response) -> Core::Task<void>
    {
        response.setStatus(201);
        co_return;
    };
    dispatchRouter.get("/health", createdHandler);
    dispatchRouter.get("/i/:id", createdHandler);
    dispatchRouter.get("/o/:id/status", createdHandler);
    Net::HttpRequest dispatchRequest;
    Net::HttpResponse dispatchResponse;
    const auto routeOnce = [&dispatchRouter, &dispatchRequest, &dispatchResponse](const std::string_view uri)
    {
        // 对象按连接复用：每轮 reset 后按新报文填回，URI 短到进小串内联，不额外造堆块
        dispatchRequest.reset();
        dispatchRequest.setMethod(Net::HttpMethod::GET);
        dispatchRequest.setUri(std::string(uri));
        dispatchRequest.setHttpVersion("HTTP/1.1");
        dispatchResponse.reset();
        Core::Task<> routeTask = dispatchRouter.route(dispatchRequest, dispatchResponse);
        routeTask.handle().resume();
        // 状态码由处理函数写下：没跑完或没命中都会留成别的值，自检据此判失败
        return routeTask.isReady() && dispatchResponse.status() == 201 ? std::size_t{1} : std::size_t{0};
    };
    measureCase(
            "router-route-exact",
            [&routeOnce]
            {
                return routeOnce("/health");
            },
            results, checksum, failureCount);
    measureCase(
            "router-route-pattern",
            [&routeOnce]
            {
                return routeOnce("/o/42/status");
            },
            results, checksum, failureCount);

    // WebSocket permessage-deflate 单条消息压缩：会话每发一条压缩消息都要走一次。改前每条都
    // deflateInit2/End（重建约 240KB 内部状态，短消息上比压缩本身还贵），改后复用 thread_local 流、
    // 每条 deflateReset。同一线程反复调用正落在复用路径上，量的就是稳态
    const std::string wsDeflatePayload(1024, 'a');
    measureCase(
            "ws-deflate-message",
            [&wsDeflatePayload]
            {
                const std::optional<std::string> compressed = Net::deflateWebSocketMessage(wsDeflatePayload);
                return compressed.has_value() ? compressed->size() : std::size_t{0};
            },
            results, checksum, failureCount);

    // 入站帧解掩码：服务端每收到一条客户端帧都要按 4 字节循环异或解除掩码（RFC 6455 §5.3），
    // 逐字节 + 表取值挡住向量化。预拼一条 4KiB 掩码帧，计时里只跑 decoder.parse（头部仅 8 字节，
    // 解掩码占满），reset 复用同一解码器落在稳态。这是本会话量「字级解掩码值不值」的对照例
    const std::string wsUnmaskPlain(4096, 'q');
    const std::array<std::uint8_t, 4> wsUnmaskKey{0xDE, 0xAD, 0xBE, 0xEF};
    std::string wsMaskedFrame;
    wsMaskedFrame.push_back(static_cast<char>(0x81)); // FIN + text
    wsMaskedFrame.push_back(static_cast<char>(0xFE)); // MASK=1 + 126（16 位扩展长度）
    wsMaskedFrame.push_back(static_cast<char>((4096 >> 8) & 0xFF));
    wsMaskedFrame.push_back(static_cast<char>(4096 & 0xFF));
    for (const std::uint8_t keyByte: wsUnmaskKey)
    {
        wsMaskedFrame.push_back(static_cast<char>(keyByte));
    }
    for (std::size_t index = 0; index < wsUnmaskPlain.size(); ++index)
    {
        wsMaskedFrame.push_back(
                static_cast<char>(static_cast<std::uint8_t>(wsUnmaskPlain[index]) ^ wsUnmaskKey[index % 4]));
    }
    Net::WebSocketFrameDecoder unmaskDecoder;
    measureCase(
            "ws-unmask-payload",
            [&wsMaskedFrame, &unmaskDecoder]
            {
                unmaskDecoder.reset();
                if (unmaskDecoder.parse(wsMaskedFrame.data(), wsMaskedFrame.size()) != Net::WebSocketDecodeStatus::Frame)
                {
                    return std::size_t{0};
                }
                const Net::WebSocketFrame frame = unmaskDecoder.takeFrame();
                // 逐字节求和：既强制真把解出来的负载读一遍（否则整趟异或可被优化掉），又恒非 0
                std::size_t checksumBytes = 0;
                for (const char payloadByte: frame.payload)
                {
                    checksumBytes += static_cast<std::uint8_t>(payloadByte);
                }
                return frame.payload.size() == 4096 ? checksumBytes : std::size_t{0};
            },
            results, checksum, failureCount);

    // 响应压缩中间件的 gzip / zstd 两条一次性压缩：改前每条响应都重建压缩器内部状态
    // （gzip deflateInit2/End、zstd 的 ZSTD_compress 内部建/销 CCtx），改后复用 thread_local 上下文。
    // 同一线程反复调用正落在复用路径上，量的是稳态。正文取 ~4KiB 接近真实响应的一份生成文本
    const std::string responseCompressBody = makeResponseLikeBody(4096);
    measureCase(
            "gzip-response-compress",
            [&responseCompressBody]
            {
                const std::optional<std::string> compressed = Net::gzipCompress(responseCompressBody);
                return compressed.has_value() ? compressed->size() : std::size_t{0};
            },
            results, checksum, failureCount);
    measureCase(
            "zstd-response-compress",
            [&responseCompressBody]
            {
                const std::optional<std::string> compressed = Net::zstdCompress(responseCompressBody);
                return compressed.has_value() ? compressed->size() : std::size_t{0};
            },
            results, checksum, failureCount);

    // 「把压缩交给工作线程」那一跳的回程成本：一次 外部线程 → 事件循环 的跨线程投递与唤醒。
    // 执行器那头还另有一次同量级的队列唤醒，因此一整跳约为本例的两倍。拿它与上面两例的每响应
    // 压缩耗时对照，才知道小到什么程度的正文不值得外派。计时体里 promise 的共享状态要取一次堆块，
    // 真实外派路径同样付这笔，故不剥到计时体外（本例单次约 10 µs，这点分配落在噪声里）
    Core::ThreadPool wakeupThreadPool(1);
    wakeupThreadPool.start();
    Core::EventLoop &wakeupLoop = wakeupThreadPool.eventLoop(0);
    measureCase(
            "eventloop-remote-post-roundtrip",
            [&wakeupLoop]
            {
                // 有界等待：这一跳没落地就返回 0 让自检报红，而不是让整轮基准挂在下一次 wait 上。
                // 回调按 shared_ptr 持有 promise——超时后本线程先走，迟到的回调写的仍是活着的对象
                auto arrived = std::make_shared<std::promise<void>>();
                std::future<void> arrivedFuture = arrived->get_future();
                wakeupLoop.scheduler().postRemote([arrived]
                                                  { arrived->set_value(); });
                constexpr auto kWakeupHopDeadline = std::chrono::seconds{2};
                return arrivedFuture.wait_for(kWakeupHopDeadline) == std::future_status::ready ? std::size_t{1}
                                                                                               : std::size_t{0};
            },
            results, checksum, failureCount);
    wakeupThreadPool.stop();

    // 大正文的一次性压缩：中间件在调用协程所在线程上同步压完整块正文，而 HTTP 三条路径的协程都跑在
    // 事件循环线程上——一条响应压多久，同一条循环上别的连接就等多久。按真实响应大小量出「每字节要占
    // 多少纳秒」才知道这个同步成本该不该被搬到工作线程上
    const std::string largeResponseCompressBody = makeResponseLikeBody(256 * 1024);
    const std::size_t largeResponseBodySize = largeResponseCompressBody.size();
    // 返回 0 即自检失败：压缩不可用、或压完不降反升（级别映射接错、把 0 当成「不压」都会走这条）
    const auto compressedOrZero = [largeResponseBodySize](std::optional<std::string> compressed)
    {
        if (!compressed.has_value() || compressed->size() >= largeResponseBodySize)
        {
            return std::size_t{0};
        }
        return compressed->size();
    };
    measureCase(
            "gzip-response-compress-256k",
            [&largeResponseCompressBody, &compressedOrZero]
            {
                return compressedOrZero(Net::gzipCompress(largeResponseCompressBody));
            },
            results, checksum, failureCount);
    measureCase(
            "gzip-response-compress-256k-level1",
            [&largeResponseCompressBody, &compressedOrZero]
            {
                return compressedOrZero(Net::gzipCompress(largeResponseCompressBody, 1));
            },
            results, checksum, failureCount);
    measureCase(
            "zstd-response-compress-256k",
            [&largeResponseCompressBody, &compressedOrZero]
            {
                return compressedOrZero(Net::zstdCompress(largeResponseCompressBody));
            },
            results, checksum, failureCount);
    measureCase(
            "brotli-response-compress-256k",
            [&largeResponseCompressBody, &compressedOrZero]
            {
                return compressedOrZero(Net::brotliCompress(largeResponseCompressBody));
            },
            results, checksum, failureCount);

    // 时间与压缩比要一起看才做得出「要不要为 gzip-only 的对端降档」这个判断，所以在这里各调一次
    // 把长度打出来（不计入任何用例的计时，也不影响校验和）
    const std::optional<std::string> gzipRatioReference = Net::gzipCompress(largeResponseCompressBody, 6);
    const std::optional<std::string> gzipRatioLevel1    = Net::gzipCompress(largeResponseCompressBody, 1);
    const std::optional<std::string> zstdRatio          = Net::zstdCompress(largeResponseCompressBody);
    const std::optional<std::string> brotliRatio        = Net::brotliCompress(largeResponseCompressBody);
    if (gzipRatioReference && gzipRatioLevel1 && zstdRatio && brotliRatio)
    {
        std::printf("  256 KiB 正文压缩后长度：gzip6=%zu gzip1=%zu zstd3=%zu brotli6=%zu\n",
                    gzipRatioReference->size(), gzipRatioLevel1->size(), zstdRatio->size(), brotliRatio->size());
    }

    // ------------------------------------------------------------------
    // 日志热路径：一条日志从「调用 log()」到「渲成一行人读文本」的成本。
    // 事件循环线程上每请求至少一条，这条路的钱要和 h1 解析同量级才不打扰业务。
    // ------------------------------------------------------------------
    constexpr std::string_view kLogMessage = "request served in 12 ms, path=/api/orders, status=200";
    const Base::LogEvent logEvent{Base::LogLevel::Info, std::chrono::system_clock::now(), "tid-bench",
                                  Base::SourceLocation("microbench.cpp", 4711, "benchLogFunction"),
                                  "bench.logger", std::string{kLogMessage}};

    Base::DefaultFormatter defaultFormatter;
    // 格式化器的两个出口：copy 那条造一个结果串（也是任何自定义格式化器走的路），
    // into 那条直接续写调用方留了容量的缓冲。两者产出逐字相同，只比取堆的次数
    std::string formatterLineBuffer;
    formatterLineBuffer.reserve(256U);
    measureCase(
            "log-format-default-copy",
            [&defaultFormatter, &logEvent]
            {
                return defaultFormatter.format(logEvent).size();
            },
            results, checksum, failureCount);
    measureCase(
            "log-format-default-into",
            [&defaultFormatter, &logEvent, &formatterLineBuffer]
            {
                formatterLineBuffer.clear();
                defaultFormatter.formatInto(formatterLineBuffer, logEvent);
                return formatterLineBuffer.size();
            },
            results, checksum, failureCount);
    // JSON 版式仍走 nlohmann DOM（建对象 + 逐字段插入 + dump），留在这里当下一步的对照基数
    Base::JsonFormatter jsonFormatter;
    measureCase(
            "log-format-json",
            [&jsonFormatter, &logEvent]
            {
                return jsonFormatter.format(logEvent).size();
            },
            results, checksum, failureCount);

    /**
     * @brief 收下事件就丢的下游：让异步队列的读数只含「投递 + 消费循环」，不含真实 IO
     */
    struct DiscardingSink final : Base::LogSink
    {
        /**
         * @brief 收下事件但不落地
         * @details 重写 LogSink::write()：刻意什么都不做，把消费端成本压到零，
         *          这样读数剩下的就是队列与唤醒本身。
         * @param event 日志事件
         */
        void write(const Base::LogEvent &event) override
        {
            static_cast<void>(event);
        }

        /**
         * @brief 无缓冲可刷，空实现
         */
        void flush() override
        {
        }
    };

    // 这一条量的是生产者的单跳成本：事件构造 + 搬运 + 抢一次队列锁 + 唤醒消费者。
    // 消费线程同时在跑，所以它是「生产消费耦合下的单条成本」，不是纯入队耗时。
    // 刻意不进基线：机器上另有负载时这条会摆到 2 倍（实测空载 549 ns、并发构建下中位数 935 ns），
    // 拿它当门禁只会产出假红
    Base::Logger benchLogger("bench.async");
    benchLogger.addSink(std::make_unique<Base::AsyncSink>(std::make_unique<DiscardingSink>(), 4096U,
                                                         Base::AsyncSink::OverflowPolicy::Block));
    measureCase(
            "log-async-enqueue",
            [&benchLogger]
            {
                benchLogger.log(Base::LogLevel::Info, kLogMessage);
                return 1U;
            },
            results, checksum, failureCount);

    // 被等级挡下的那一条：生产进程按 INFO 跑，代码里成片的 TRACE/DEBUG 调用走的就是这条路径，
    // 它付的钱应当只有「读一次等级」。这条盯住一个具体的退化：入口若先按 string_view 拷出消息体
    // 再去问等级，一条注定丢弃的记录也要为正文取一块堆；同一形状的分配判据由
    // tests/Base/Log/TestLogHotPathAllocations.cpp 的 FilteredOutLineIsAllocationFree 钉住
    struct CountingSink final : Base::LogSink
    {
        /**
         * @brief 只数条数，不格式化也不写盘
         * @param event 日志事件
         */
        void write(const Base::LogEvent &event) override
        {
            static_cast<void>(event);
            ++receivedCount;
        }

        /**
         * @brief 无缓冲可刷
         */
        void flush() override
        {
        }

        std::uint64_t receivedCount{0U}; ///< 收到的事件条数，用于自检「这条确实被挡下了」
    };

    Base::Logger filteredLogger("bench.filtered");
    auto         countingSink           = std::make_unique<CountingSink>();
    CountingSink &observedCountingSink  = *countingSink;
    filteredLogger.addSink(std::move(countingSink));
    filteredLogger.setLevel(Base::LogLevel::Error);
    measureCase(
            "log-filtered-out",
            [&filteredLogger, &observedCountingSink]
            {
                filteredLogger.log(Base::LogLevel::Trace, kLogMessage);
                // 自检判据：正文没落到 Sink 才算走对了形状；落进去了说明量的不是「被丢弃」这条路径
                return observedCountingSink.receivedCount == 0U ? 1U : 0U;
            },
            results, checksum, failureCount);

    // 这条盯的是「编一帧要不要为历史的流买单」：连接先跑完 5000 条完整请求，再量每条请求收口后
    // 剩下的那次「写一段响应 + 编一帧」。实测（容器 GCC 13 Release、绑核）本端 255~261 ns/op；
    // 把 collectFrames 开头那一次 retireSettledStreams() 去掉后同一条跳到 88.7 µs/op（340 倍），
    // 也就是说这条能 unmistakably 检出「流记录不再被摘掉」这个回归。
    // 单看空载编帧的扫描成本随连接年龄的曲线（同法量，只做 collectFrames）：
    //   开着回收：0 / 100 / 1000 / 5000 / 20000 条请求分别是 7.3 / 6.3 / 6.3 / 6.3 / 6.7 ns；
    //   去掉回收：同样年龄是 9.5 / 917 / 12556 / 71290 / 823700 ns（记录数 0 / 200 / 2000 / 10000 / 40000）。
    // 超线性是因为两张 std::map 的节点随连接时长铺开后再也放不进缓存。
    // 刻意不进基线：这条要在 Windows 上重算整套基线才有意义，先把读数记在这里
    constexpr std::uint64_t kAgedConnectionRequestCount = 5000;
    auto agedLayer = makeAgedQuicStreamLayer(kAgedConnectionRequestCount);
    const std::uint64_t liveStreamId = kAgedConnectionRequestCount * 4U;
    static const std::vector<std::uint8_t> kTinyResponseBody = {'x'};
    measureCase(
            "quic-frame-scan-aged",
            [&agedLayer, liveStreamId]
            {
                std::string frames;
                std::vector<Net::QuicStreamRange> sentRanges;
                std::vector<Net::QuicStreamAnnouncement> announcements;
                static_cast<void>(
                        agedLayer->writeStreamData(liveStreamId, std::span<const std::uint8_t>(kTinyResponseBody), false));
                return agedLayer->collectFrames(frames, 1200U, sentRanges, announcements) ? 1U : 0U;
            },
            results, checksum, failureCount);

    // ---- Config 层：读一次配置与改一次配置 ----
    // 读这条在每次请求都可能付（开关、限额从配置里取），写这条是给 setValue 定价：一次写入要复制
    // 整份快照，键越多越贵。容器（GCC 13 Release、-O3）连跑三次的读数：
    //   config-get-int      30.4 / 31.2 / 32.1 ns（256 键的快照里取一个整数）
    //   config-set-value  9 840 / 9 891 / 9 969 ns（同一份快照上覆盖一个已存在的键）
    // 差在哪：把「定形扫描」单独摘掉再量一次是 9 908 / 9 926 ns —— 与带着它时同量级，也就是说
    // 这 10 µs 全是「复制整份快照」的钱，为正确性加的那次全表扫描不到 1%。反过来这也定死了
    // setValue 的用法：它按启动期/偶发覆盖定价，不该进逐请求路径（要按请求读的键应在启动时读进
    // 自己的字段）。刻意不进基线：基线文件里其余读数出自 Windows/MSVC，混进来会让门禁量错东西
    constexpr std::uint64_t kConfigFixtureKeyCount = 256;
    auto &configManager = Base::ConfigManager::instance();
    configManager.clear();
    for (std::uint64_t keyIndex = 0; keyIndex < kConfigFixtureKeyCount; ++keyIndex)
    {
        static_cast<void>(configManager.setValue("fixture.key" + std::to_string(keyIndex), Base::ConfigValue(static_cast<std::int64_t>(keyIndex))));
    }
    measureCase(
            "config-get-int",
            [&configManager]
            {
                // 判据取「取回的就是当初写进去的那个值」：直接把值返回的话，键丢了会落到默认值 -1，
                // 转成无符号仍是非零数，自检检不出来
                return configManager.getInt("fixture.key128", -1) == 128 ? 1U : 0U;
            },
            results, checksum, failureCount);
    measureCase(
            "config-set-value",
            [&configManager]
            {
                return configManager.setValue("hot.override", Base::ConfigValue(std::int64_t{7})) ? 1U : 0U;
            },
            results, checksum, failureCount);

    // ---- Platform 层：每条请求都会摸到的几个底座 ----
    // 这几条的存在是为了让「Platform 侧的性能结论」有地方落脚：日历分解在每条响应的 Date 头与每行
    // 日志的时间戳上，查文件信息在每次静态文件命中判定上，唤醒一次跨线程通知在每次「从别的线程
    // 把活投进事件循环」的路径上。它们都不在协议解析的量级里（h1 解析 ~424 ns），但都是每次付的。
    // 本机（Release/MSVC，20 逻辑核）空载连跑三次的读数：
    //   platform-time-utc            9.3-9.4 ns
    //   platform-time-local          1.7-1.9 ns（同一秒命中缓存；未命中见下）
    //   platform-time-local-varying  17.6-18.2 ns（每次换输入，走完整换算）
    //   platform-wakeup-roundtrip    4.79-5.21 µs
    //   platform-stat-basic-info     8.26-10.75 µs
    // 够格进基线的只有前两条：`-varying` 的输入每调用换一次，机器负载直接印在读数上；后两条各自量的
    // 都不是「每请求付一次」的钱——唤醒这条是 notify+drain 紧挨着的**最坏形状**（循环真的睡下去时才
    // 付一次真写，忙的时候 notify 只是一次原子交换、几纳秒就合并掉），stat 那条由文件系统过滤器支配、
    // 单次摆动就有 20% 以上。拿这两条定阈值只会产出假红，也别拿它们的读数去推断「每请求 5-10 微秒」。
    // utc 这条的算法是「一次除法配一对余数」：改回「整除后乘回去再减」看着更省，但同一台机器三次读数
    // 是 8.6 / 8.9 / 11.9 ns，与这里的 9.3–9.4 分不出胜负，而那一步乘法在 time_t 取最小值时会溢出
    constexpr std::time_t kMeasuredInstant = 1767225600; // 2026-01-01T00:00:00Z，固定值：稳态下相邻请求落在同一秒
    measureCase(
            "platform-time-utc",
            []
            {
                const Platform::UtcTimeFields fields = Platform::PlatformTime::utcTime(kMeasuredInstant);
                return static_cast<std::uint64_t>(fields.year);
            },
            results, checksum, failureCount);

    measureCase(
            "platform-time-local",
            []
            {
                const std::tm fields = Platform::PlatformTime::localTime(kMeasuredInstant);
                return static_cast<std::uint64_t>(fields.tm_year + 1);
            },
            results, checksum, failureCount);

    // 与上一条成对：这条每次换一个输入，专量「缓存永远命不中」的最坏形状。两条一起看才说得清单格
    // 缓存省下的到底是什么，也才看得出未命中时多付的那次比较有没有把成本顶上去
    std::time_t varyingLocalSecond = kMeasuredInstant;
    measureCase(
            "platform-time-local-varying",
            [&varyingLocalSecond]
            {
                const std::tm fields = Platform::PlatformTime::localTime(varyingLocalSecond++);
                return static_cast<std::uint64_t>(fields.tm_year + 1);
            },
            results, checksum, failureCount);

    // 一次完整的跨线程唤醒：写一枚标记 + 读空它。eventfd 与 socketpair 的代价差在这条上会显形
    Platform::EventNotifier wakeupNotifier;
    measureCase(
            "platform-wakeup-roundtrip",
            [&wakeupNotifier]
            {
                wakeupNotifier.notify();
                wakeupNotifier.drain();
                return 1U;
            },
            results, checksum, failureCount);

    // 静态文件的命中判据：一次系统调用读回大小、修改秒与身份标记（三次 std::filesystem 调用的替代）
    const std::filesystem::path statProbePath = std::filesystem::temp_directory_path() / "asyn-microbench-stat.bin";
    {
        std::ofstream statProbeFile(statProbePath, std::ios::out | std::ios::binary | std::ios::trunc);
        statProbeFile << std::string(64, 'x');
    }
    measureCase(
            "platform-stat-basic-info",
            [statProbePath]
            {
                const std::optional<Platform::FileBasicInfo> info = Platform::queryFileBasicInfo(statProbePath);
                return info.has_value() ? static_cast<std::uint64_t>(info->sizeBytes) : 0U;
            },
            results, checksum, failureCount);
    std::error_code statProbeRemoveError;
    std::filesystem::remove(statProbePath, statProbeRemoveError);

    printTable(results, failureCount, checksum);
    if (!jsonOutputPath.empty())
    {
        writeJson(jsonOutputPath, results, failureCount);
    }
    // 自检失败要能从退出码看出来：门禁脚本也会按 JSON 里的 failures 判失败
    return failureCount == 0 ? 0 : 1;
}

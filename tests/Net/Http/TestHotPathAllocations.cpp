// 热路径的「每次操作付出多少次堆分配」画像。计时归 benchmarks/microbench，这里只数分配——
// 一对 malloc/free 在几十纳秒量级，每请求的分配次数是「还能不能再省」最直接的读数。
//
// 量法：把形状连跑一千次，前后各取一次全局分配计数，差值摊到每次操作。三条纪律保证读数不是假的：
//   · 被测体里不放断言（gtest 造判词本身要分配，会把被测形状淹掉），改成返回一个与「做成了多少」
//     成正比的标记，跑完再核对总和——顺带也防住「编译器把空转调用删掉」这种假读数；
//   · 先量一次「什么都不做」的本底，本底不为零说明量的不是被测形状；
//   · 计数件自己要有自检：哪天 operator new 的替换件被顶掉，所有读数都会是 0，而 0 看着像
//     「零分配的优秀实现」。
//
// 量出来的读数（Release，摊平到每次操作）。凡声称「稳态零分配」的形状，判据一律取一千次的原值：
// 摊平是整除，「每次 0 次」藏得住一千次里的 999 次分配。
//   · 解析一条 h1 请求：一千次共 0 次。头部收进一条字节缓冲，URI、头部与正文交给请求时按整块交换，
//     四份容器的容量跨报文留着。要先前置暖身若干轮才读到这个稳态——倍扩容本身是暖期成本；
//   · 装 10 条头部：0 次 / 0 字节。clear 只清内容、留着容量，整块头部的字节都写进同一条缓冲；
//   · 派发一条请求（命中精确路由）：一千次共 0 次。参数收集表要进模式路由那一层才建；
//   · 派发一条请求（扫过模式候选并收下 :id）：每次 4 次 / 368 字节，与扫过几条候选无关
//     （候选表跨候选复用，逐候选新建会随模式路由条数线性放大）；
//   · 落定一条请求的 request-id：一千次共 0 次。id 先生成到调用线程自己的复用缓冲，再原地写进
//     请求字段（两条都不再新取堆块；改前每请求 1 次 / 32 字节）；
//   · 响应头序列化：每次新建串 1 次，复用同一块缓冲 0 次；
//   · 解一帧 200 字节头块的 HEADERS：1 次 / 208 字节，就是取走的那份负载；
//   · 组一帧 256 字节分块帧：每次新建串 1 次 / 272 字节，复用帧缓冲 0 次。
// 同一条形状在 Debug（带迭代器调试代理）下的读数只作打印参考，确切值按 Release 钉。

#include "Net/Http/HttpChunkFrame.h"
#include "Net/Http/HttpHeaderFieldStore.h"
#include "Net/Http/HttpParser.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpRequestId.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"
#include "Net/Http2/Http2Frame.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    std::atomic<std::uint64_t> allocationCount{0};
    std::atomic<std::uint64_t> allocationBytes{0};

    /**
     * @brief 记一次分配：relaxed 足够，这两个数只当读数用，不靠它们同步任何状态
     */
    void recordAllocation(const std::size_t size) noexcept
    {
        allocationCount.fetch_add(1U, std::memory_order_relaxed);
        allocationBytes.fetch_add(static_cast<std::uint64_t>(size), std::memory_order_relaxed);
    }
} // namespace

// 全局替换：本可执行体里所有走 operator new 的分配都过这里（静态链接进来的第三方也一样）。
// 转发给 malloc/free，语义与默认实现一致，只是多记两笔数
[[nodiscard]] void *operator new(const std::size_t size)
{
    recordAllocation(size);
    void *const pointer = std::malloc(size == 0U ? 1U : size);
    if (pointer == nullptr)
    {
        throw std::bad_alloc();
    }
    return pointer;
}

[[nodiscard]] void *operator new[](const std::size_t size)
{
    recordAllocation(size);
    void *const pointer = std::malloc(size == 0U ? 1U : size);
    if (pointer == nullptr)
    {
        throw std::bad_alloc();
    }
    return pointer;
}

[[nodiscard]] void *operator new(const std::size_t size, const std::nothrow_t &) noexcept
{
    recordAllocation(size);
    return std::malloc(size == 0U ? 1U : size);
}

[[nodiscard]] void *operator new[](const std::size_t size, const std::nothrow_t &) noexcept
{
    recordAllocation(size);
    return std::malloc(size == 0U ? 1U : size);
}

void operator delete(void *pointer) noexcept
{
    std::free(pointer);
}

void operator delete(void *pointer, const std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete[](void *pointer) noexcept
{
    std::free(pointer);
}

void operator delete[](void *pointer, const std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete(void *pointer, const std::nothrow_t &) noexcept
{
    std::free(pointer);
}

void operator delete[](void *pointer, const std::nothrow_t &) noexcept
{
    std::free(pointer);
}

namespace AsynGyanis::Net
{
    namespace
    {
        /// 每个形状连跑这么多次再摊平：单次读数会被「临时串先分配后释放」这类顺序细节影响
        constexpr std::uint64_t kMeasurementIterations = 1000U;

        /// 解析形状暖身用的轮数：四份容器（解析器与请求各自的字节缓冲和记录表）按倍扩容要几轮才长到位
        constexpr std::uint64_t kParseWarmUpIterationCount = 64U;

#ifdef NDEBUG
        // 下面这些数是 Release（发布形态真正跑的那套配置）下的实测分配数。只在 Release 上钉死：
        // Debug 的 STL 迭代器调试代理会给每个容器对象多挂一块代理，读数被实现细节放大一个量级，
        // 钉它等于钉噪声。Debug 侧仍跑同样的形状，把读数打出来供对照，并保留两条与配置无关的结构判据。
        // 名字里带 Total 的钉的是「一千次一共多少次」（原值），其余是摊平到每次操作的读数
        constexpr std::uint64_t kHttp1ParseTotalAllocationsPerThousand = 0U; ///< URI、头部、正文都整块交接，暂存容量跨报文留着
        constexpr std::uint64_t kHeaderRefillTotalAllocationsPerThousand = 0U; ///< clear 只清内容、留着容量，整块头部写进同一条字节缓冲
        constexpr std::uint64_t kDispatchExactTotalAllocationsPerThousand = 0U; ///< 命中精确路由不建参数表，派发本身不再碰堆
        constexpr std::uint64_t kDispatchPatternAllocationsPerRequest = 4U; ///< 复用的候选表 2 + 候选里那一条 ":id" 1 + 提交给请求 1
        constexpr std::uint64_t kRequestIdTotalAllocationsPerThousand = 0U; ///< id 就地写进请求自己的缓冲，两侧容量都留着
        constexpr std::uint64_t kHeadSerializeAllocationsFresh = 1U;
        constexpr std::uint64_t kHeadSerializeTotalAllocationsReused = 0U;
        constexpr std::uint64_t kFrameDecodeAllocationsPerFrame = 1U;
        constexpr std::uint64_t kChunkFrameAllocationsFresh = 1U;      ///< 每次新建一个帧串：一次分配
        constexpr std::uint64_t kChunkFrameTotalAllocationsReused = 0U; ///< 复用帧缓冲：容量长够之后一次都不碰堆
#endif

        /**
         * @brief 累计计数的一次快照
         */
        struct Snapshot
        {
            std::uint64_t count{0}; ///< 累计分配次数
            std::uint64_t bytes{0}; ///< 累计申请字节数
        };

        /**
         * @brief 取当前累计计数
         * @note 两个计数分别读，不是同一时刻的原子对——被测形状都在单线程里跑，差值仍然准确
         */
        [[nodiscard]] Snapshot sample() noexcept
        {
            return {allocationCount.load(std::memory_order_relaxed), allocationBytes.load(std::memory_order_relaxed)};
        }

        /**
         * @brief 一次测量的结果：窗口内的分配原值，外加摊平到每次操作的读数
         * @details 摊平是整除，因此「每次 0 次」这个读数掩盖得住一千次里的零星几次分配。
         *          凡是要钉「稳态一次都不碰堆」的形状，判据一律用 totalAllocations 原值。
         */
        struct AllocationProfile
        {
            std::uint64_t totalAllocations{0};        ///< 整个测量窗口的分配总次数
            std::uint64_t totalBytes{0};              ///< 整个测量窗口申请的字节总数
            std::uint64_t allocationsPerOperation{0}; ///< 每次操作的分配次数
            std::uint64_t bytesPerOperation{0};       ///< 每次操作申请的字节数
            std::uint64_t resultSum{0};               ///< 被测体标记之和，用于证明它真的跑了
        };

        /**
         * @brief 把 body 连跑 kMeasurementIterations 次，摊平给出每次操作的分配数
         * @param body 被测形状：只做事、不断言，返回一个与「做成了多少」成正比的标记
         * @return AllocationProfile 每次操作的分配次数与字节数，外加所有标记之和
         */
        template<typename Body>
        [[nodiscard]] AllocationProfile measurePerOperation(const Body &body)
        {
            const Snapshot began = sample();
            std::uint64_t resultSum = 0;
            for (std::uint64_t iteration = 0; iteration < kMeasurementIterations; ++iteration)
            {
                resultSum += static_cast<std::uint64_t>(body());
            }
            const Snapshot ended = sample();

            AllocationProfile profile;
            profile.totalAllocations    = ended.count - began.count;
            profile.totalBytes          = ended.bytes - began.bytes;
            profile.allocationsPerOperation = profile.totalAllocations / kMeasurementIterations;
            profile.bytesPerOperation       = profile.totalBytes / kMeasurementIterations;
            profile.resultSum = resultSum;
            return profile;
        }

        /// 一条贴近真实的 h1 请求：10 个头部 + 64 字节正文（与微基准的 http1-parse-request 同形）
        std::string makeRequestText()
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

        /// 一帧带 200 字节头块的 HEADERS（END_HEADERS 置位），与微基准的 http2-frame-decode 同形
        std::string makeHeadersFrameText()
        {
            std::string headerBlock;
            headerBlock.reserve(200);
            for (std::size_t index = 0; index < 200U; ++index)
            {
                headerBlock.push_back(static_cast<char>(static_cast<unsigned char>(index % 251U)));
            }

            Http2HeadersPayload payload;
            payload.endHeaders = true;
            payload.headerBlockFragment = std::move(headerBlock);
            return encodeHttp2HeadersFrame(payload, 1);
        }

        /// 装 10 条头部用的字段表（与微基准的 header-refill-only 同形）
        std::vector<std::pair<std::string, std::string>> makeHeaderFixtures()
        {
            return {
                    {"host", "api.example.com"},
                    {"user-agent", "curl/8.7.1"},
                    {"accept", "*/*"},
                    {"content-type", "application/json"},
                    {"content-length", "64"},
                    {"accept-encoding", "gzip, deflate, br"},
                    {"authorization", "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"},
                    {"x-request-id", "0001-0000000000000abc"},
                    {"connection", "keep-alive"},
                    {"cache-control", "no-cache"},
            };
        }

        /// 一份典型的 200 响应：4 条自设头部 + 自动 content-length 与 Date
        HttpResponse makeResponseFixture()
        {
            HttpResponse response;
            response.setStatus(200);
            response.setHeader("content-type", "application/json");
            response.setHeader("server", "AsynGyanis/1.1");
            response.setHeader("cache-control", "no-store");
            response.setHeader("x-request-id", "0001-0000000000000abc");
            response.setBody(std::string(64, 'x'));
            return response;
        }

        /// 按「新报文到达」的样子把请求填回初态：URI 与版本都短到进小串内联，不额外造堆块，
        /// 这样窗里量到的就是派发本身的分配而不是把字符串搬进容器
        void fillDispatchRequest(HttpRequest &request, const std::string_view uri)
        {
            request.setMethod(HttpMethod::GET);
            request.setUri(std::string(uri));
            request.setHttpVersion("HTTP/1.1");
        }

        /// 即时写下 200 并计一次调用的处理函数：正文留空，免得把「小串内联」之外的成本算进读数。
        /// 计数放在体外核对，被测窗里就不必为「取参数」造一个临时串
        Router::Handler makeCountingOkHandler(int &callCounter)
        {
            return [&callCounter]([[maybe_unused]] HttpRequest &request, HttpResponse &response) -> Core::Task<void>
            {
                ++callCounter;
                response.setStatus(200);
                co_return;
            };
        }
    } // namespace

    /**
     * @brief 计数件要证明自己看得见分配
     * @details 少了这条，哪天替换件被链接顺序顶掉（或本目标改用自定义分配器），所有读数都会是 0，
     *          而 0 看着像「零分配的优秀实现」——画像会一路骗下去
     */
    TEST(HotPathAllocations, CountingHookSeesAPlainHeapAllocation)
    {
        const AllocationProfile profile = measurePerOperation(
                []
                {
                    // 64 字节超过短串内联缓冲，每次都要向堆要一次
                    const std::string allocated(64, 'x');
                    return allocated.size();
                });
        EXPECT_GE(profile.allocationsPerOperation, 1U) << "operator new 的替换件没生效，本文件所有读数都不可信";
        EXPECT_GE(profile.bytesPerOperation, 64U);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * 64U);
    }

    /**
     * @brief 测量窗自身的本底：什么都不做的循环必须量出 0 次分配
     * @details 本底不为零就说明量到的是循环、gtest 或 CRT 的开销，后面那些形状读数一概不能信
     */
    TEST(HotPathAllocations, MeasurementWindowHasNoBackgroundAllocations)
    {
        std::uint64_t sink = 0;
        const AllocationProfile profile = measurePerOperation(
                [&sink]
                {
                    // 留一条对 sink 的写，编译器就不能把整段循环判成空转删掉
                    sink += 1U;
                    return std::size_t{0};
                });
        EXPECT_EQ(profile.totalAllocations, 0U) << "测量窗里有背景分配，形状读数不可信";
        EXPECT_EQ(profile.resultSum, 0U);
        EXPECT_EQ(sink, kMeasurementIterations);
    }

    /**
     * @brief 解析一条完整 h1 请求付出多少次分配
     * @details 解析器把请求行、10 条头部与正文边界装进自己的存储，这是每条入站请求的固定成本
     */
    TEST(HotPathAllocations, Http1RequestParseAllocations)
    {
        const std::string requestText = makeRequestText();
        // 解析器按声明的 Content-Length 收正文（这份语料的正文比声明的长一字节，多出的那字节
        // 属于下一条报文），所以判据取「头部之后 64 字节」而不是整段文本长度
        const std::size_t expectedConsumed = requestText.find("\r\n\r\n") + 4U + 64U;
        HttpParser parser;
        const auto parseOnce = [&parser, &requestText]
        {
            // 返回「本次吃掉的字节数」：没解析完就是 0，最后核对总和能看出来
            const ParseStatus status = parser.parse(requestText.data(), requestText.size());
            const std::size_t consumed = status == ParseStatus::Done ? parser.consumedByteCount() : 0U;
            parser.reset();
            return consumed;
        };
        EXPECT_EQ(parseOnce(), expectedConsumed) << "这条形状的解析结果本身就不对";
        // 跑够轮数让各容器长到位再开始测量：字节缓冲与记录表各有两份在解析器与请求之间交换，
        // 按倍扩容要几轮才收敛，只暖一轮会把这点暖期成本读成「稳态仍在分配」
        for (std::uint64_t warmUpIndex = 0; warmUpIndex < kParseWarmUpIterationCount; ++warmUpIndex)
        {
            parseOnce();
        }

        const AllocationProfile profile = measurePerOperation(parseOnce);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * expectedConsumed) << "有几次解析没走到 Done";
        std::printf("http1-parse-request 每次分配 %llu 次 / %llu 字节（一千次共 %llu 次 / %llu 字节）\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation),
                    static_cast<unsigned long long>(profile.totalAllocations),
                    static_cast<unsigned long long>(profile.totalBytes));
#ifdef NDEBUG
        // 判据取原值而不是摊平读数：摊平是整除，「每次 0 次」允许一千次里藏住 999 次分配
        EXPECT_EQ(profile.totalAllocations, kHttp1ParseTotalAllocationsPerThousand)
                << "每条入站请求的分配数变了：要么多了一次每请求堆块，要么这份读数需要按实测重录";
#endif
    }

    /**
     * @brief 把 10 条头部装进字段存储付出多少次分配
     * @details 请求侧与响应侧每条报文都要走这一步，clear 之后重装的稳态成本就是它的读数
     */
    TEST(HotPathAllocations, HeaderStoreRefillAllocations)
    {
        const std::vector<std::pair<std::string, std::string>> fixtures = makeHeaderFixtures();
        HttpHeaderFieldStore store;
        const auto refill = [&store, &fixtures]
        {
            store.clear();
            for (const auto &[name, value]: fixtures)
            {
                store.append(name, value);
            }
            // 数权威记录只能走遍历出口：存储交出的是字节缓冲加偏移，不再交出 owning 容器。
            // 这里的 lambda 不捕堆、也不让存储建单值视图，因此不污染读数
            std::size_t fieldCount = 0;
            store.forEachField([&fieldCount](const std::string_view, const std::string_view)
                               {
                                   ++fieldCount;
                               });
            return fieldCount;
        };
        refill();

        const AllocationProfile profile = measurePerOperation(refill);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * fixtures.size());
        std::printf("header-store-refill 每次分配 %llu 次 / %llu 字节（一千次共 %llu 次）\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation),
                    static_cast<unsigned long long>(profile.totalAllocations));
#ifdef NDEBUG
        EXPECT_EQ(profile.totalAllocations, kHeaderRefillTotalAllocationsPerThousand)
                << "装 10 条头部的分配数变了：稳态下这张表只该按需扩容，不该每条头各要一块";
#endif
    }

    /**
     * @brief 响应头序列化的两种出口各付出多少次分配
     * @details 两条出口产出的头部文本必须一模一样，所以这里同时核对结果等长；两者的读数差就是
     *          每次新建串那一笔分配
     */
    TEST(HotPathAllocations, ResponseHeadSerializeAllocations)
    {
        const HttpResponse response = makeResponseFixture();
        const auto serializeFresh = [&response]
        {
            const std::string serialized = response.serializeHead();
            return serialized.size();
        };
        std::string reusedHead;
        const auto serializeReused = [&response, &reusedHead]
        {
            response.serializeHeadInto(reusedHead);
            return reusedHead.size();
        };
        serializeFresh();
        serializeReused();

        const AllocationProfile fresh = measurePerOperation(serializeFresh);
        const AllocationProfile reused = measurePerOperation(serializeReused);
        EXPECT_GT(fresh.resultSum, 0U) << "序列化没产出任何字节，读的是空转";
        EXPECT_EQ(fresh.resultSum, reused.resultSum) << "两条出口产出的头部文本长度不一致";
        // 与构建配置无关的结构判据：复用缓冲那条不该比新建串那条花更多分配，破了就说明有人
        // 把序列化的产物又放回了每次新建的串里
        EXPECT_LE(reused.allocationsPerOperation, fresh.allocationsPerOperation)
                << "复用缓冲的出口反而比每次新建串更费分配";
        std::printf("response-head-serialize 每次分配 %llu 次 / %llu 字节；复用缓冲 %llu 次 / %llu 字节（一千次共 %llu 次）\n",
                    static_cast<unsigned long long>(fresh.allocationsPerOperation),
                    static_cast<unsigned long long>(fresh.bytesPerOperation),
                    static_cast<unsigned long long>(reused.allocationsPerOperation),
                    static_cast<unsigned long long>(reused.bytesPerOperation),
                    static_cast<unsigned long long>(reused.totalAllocations));
#ifdef NDEBUG
        EXPECT_EQ(fresh.allocationsPerOperation, kHeadSerializeAllocationsFresh) << "每响应一份新头部串的读数变了";
        EXPECT_EQ(reused.totalAllocations, kHeadSerializeTotalAllocationsReused)
                << "复用缓冲这条路应当一次堆块都不碰";
#endif
    }

    /**
     * @brief 解一帧 HEADERS 并把负载取走付出多少次分配
     * @details 取帧时负载按值交出，这条读数是 h2 每帧成本里的分配部分
     */
    TEST(HotPathAllocations, Http2FrameDecodeAllocations)
    {
        const std::string frameText = makeHeadersFrameText();
        Http2FrameDecoder decoder;
        const auto decodeOnce = [&decoder, &frameText]
        {
            std::size_t payloadLength = 0;
            const Http2FrameDecodeStatus status = decoder.parse(frameText.data(), frameText.size());
            if (status == Http2FrameDecodeStatus::Frame)
            {
                payloadLength = decoder.takeFrame().payload.size();
            }
            decoder.reset();
            return payloadLength;
        };
        decodeOnce();

        const AllocationProfile profile = measurePerOperation(decodeOnce);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * 200U) << "有几次没解出帧或负载长度不对";
        std::printf("http2-frame-decode 每次分配 %llu 次 / %llu 字节\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation));
#ifdef NDEBUG
        EXPECT_EQ(profile.allocationsPerOperation, kFrameDecodeAllocationsPerFrame)
                << "解一帧的分配数变了：稳态下只有取走的负载那份串要堆块";
#endif
    }

    /**
     * @brief 组一帧分块帧付出多少次分配：每次新建串 vs 复用缓冲
     * @details `HttpResponse::writeChunk()` 走的就是这个出口。SSE 这类「小段、高频」的流式响应里，
     *          每段一次分配会盖过组帧本身的工作量，所以这条形状值得单独钉住
     */
    TEST(HotPathAllocations, ChunkFrameAppendAllocations)
    {
        const std::string payload(256, 'a');

        const auto buildFresh = [&payload]
        {
            std::string frame;
            appendChunkFrame(frame, payload);
            return frame.size();
        };
        std::string reusedFrame;
        const auto buildReused = [&payload, &reusedFrame]
        {
            appendChunkFrame(reusedFrame, payload);
            return reusedFrame.size();
        };
        // 各先跑一次：复用缓冲的第一轮要把容量长出来，那笔分配不属于稳态成本
        buildFresh();
        buildReused();

        // 省分配不能省成错帧：写出的这一帧必须能被同文件的解析侧原样认回负载
        std::string probeFrame;
        appendChunkFrame(probeFrame, payload);
        EXPECT_EQ(chunkFramePayload(std::string_view{probeFrame}), payload) << "写出的帧解析侧认不回来";

        const AllocationProfile fresh = measurePerOperation(buildFresh);
        const AllocationProfile reused = measurePerOperation(buildReused);
        EXPECT_GT(fresh.resultSum, 0U) << "组帧没写出任何字节，读的是空转";
        EXPECT_EQ(fresh.resultSum, reused.resultSum) << "两条出口产出的帧长度不一致";
        EXPECT_LE(reused.allocationsPerOperation, fresh.allocationsPerOperation)
                << "复用缓冲的出口反而比每次新建串更费分配";
        std::printf("chunk-frame 每次分配 新建串 %llu 次 / %llu 字节；复用缓冲 %llu 次 / %llu 字节（一千次共 %llu 次）\n",
                    static_cast<unsigned long long>(fresh.allocationsPerOperation),
                    static_cast<unsigned long long>(fresh.bytesPerOperation),
                    static_cast<unsigned long long>(reused.allocationsPerOperation),
                    static_cast<unsigned long long>(reused.bytesPerOperation),
                    static_cast<unsigned long long>(reused.totalAllocations));
#ifdef NDEBUG
        EXPECT_EQ(fresh.allocationsPerOperation, kChunkFrameAllocationsFresh) << "每次新建帧串的分配数变了";
        EXPECT_EQ(reused.totalAllocations, kChunkFrameTotalAllocationsReused)
                << "复用帧缓冲这条路应当一次堆块都不碰";
#endif
    }

    /**
     * @brief 路由把一条请求交给处理函数跑一遍付出多少次分配（精确路径命中）
     * @details 解析侧的稳态分配已经归零，这条量的是另一半：派发。读数含协程帧本身
     */
    TEST(HotPathAllocations, RouterDispatchExactPathAllocations)
    {
        int handlerCallCount = 0;
        Router router;
        router.get("/health", makeCountingOkHandler(handlerCallCount));

        HttpRequest request;
        HttpResponse response;
        const auto dispatchOnce = [&router, &request, &response]
        {
            // 每轮先 reset 再按新报文填回：对象按连接复用，容量留在原地
            request.reset();
            fillDispatchRequest(request, "/health");
            response.reset();
            Core::Task<> routeTask = router.route(request, response);
            routeTask.handle().resume();
            return routeTask.isReady() ? std::size_t{1} : std::size_t{0};
        };
        ASSERT_EQ(dispatchOnce(), 1U) << "这条形状没在同步路径上跑完，或者根本没命中处理函数";

        const AllocationProfile profile = measurePerOperation(dispatchOnce);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations) << "有几次派发没跑完（handler 里偷偷挂起了）";
        EXPECT_EQ(handlerCallCount, static_cast<int>(kMeasurementIterations) + 1) << "有几次派发没走到处理函数";
        std::printf("router-dispatch-exact 每次分配 %llu 次 / %llu 字节（一千次共 %llu 次）\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation),
                    static_cast<unsigned long long>(profile.totalAllocations));
#ifdef NDEBUG
        EXPECT_EQ(profile.totalAllocations, kDispatchExactTotalAllocationsPerThousand)
                << "命中精确路由的派发多碰了堆：参数收集表又被无条件建出来了？";
#endif
    }

    /**
     * @brief 扫过一张模式路由表并命中带 :id 的那条，付出多少次分配
     * @details 表里放四条模式路由、命中的是第二条，这样每一轮的候选扫描都会走一遍模式层——
     *          模式层的收集容器是不是逐候选新建一张表，读数会直接显形
     */
    TEST(HotPathAllocations, RouterDispatchPatternScanAllocations)
    {
        int handlerCallCount = 0;
        Router router;
        router.get("/i/:id", makeCountingOkHandler(handlerCallCount));
        router.get("/o/:id/status", makeCountingOkHandler(handlerCallCount));
        router.get("/f/*", makeCountingOkHandler(handlerCallCount));
        router.get("/r/:y/:m", makeCountingOkHandler(handlerCallCount));

        HttpRequest request;
        HttpResponse response;
        const auto dispatchOnce = [&router, &request, &response]
        {
            request.reset();
            fillDispatchRequest(request, "/o/42/status");
            response.reset();
            Core::Task<> routeTask = router.route(request, response);
            routeTask.handle().resume();
            return routeTask.isReady() ? std::size_t{1} : std::size_t{0};
        };
        ASSERT_EQ(dispatchOnce(), 1U) << "这条形状没在同步路径上跑完";
        // 参数在窗外核对：取参数要造临时串，放进被测窗会把读数弄脏
        EXPECT_EQ(request.param("id").value_or("<缺失>"), "42") << "扫表没把 :id 收下来，量的就不是那条形状";

        const AllocationProfile profile = measurePerOperation(dispatchOnce);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations) << "有几次派发没跑完（handler 里偷偷挂起了）";
        EXPECT_EQ(handlerCallCount, static_cast<int>(kMeasurementIterations) + 1) << "有几次派发没命中那条模式路由";
        std::printf("router-dispatch-pattern 每次分配 %llu 次 / %llu 字节（一千次共 %llu 次）\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation),
                    static_cast<unsigned long long>(profile.totalAllocations));
#ifdef NDEBUG
        // 这条读数与「扫过几条候选」无关：候选表跨候选复用，多扫一条不该多要堆块
        EXPECT_EQ(profile.allocationsPerOperation, kDispatchPatternAllocationsPerRequest)
                << "模式层派发的分配数变了：候选表回到逐候选新建，或参数提交多了一次拷贝";
#endif
    }

    /**
     * @brief 给一条请求落定 request-id 付出多少次分配
     * @details 会话在派发之前要为每条请求落定一个 `<前缀>-<16 位序号>` 形态的标识（21 字节，
     *          长过小串内联），这条量的是「生成 + 交给请求」这一对的稳态成本
     */
    TEST(HotPathAllocations, RequestIdResolveAllocations)
    {
        const HttpRequestIdGenerator generator;
        HttpRequest request;
        const auto resolveOnce = [&generator, &request]
        {
            request.reset();
            generator.resolveInto(request);
            return request.requestId().size();
        };
        ASSERT_EQ(resolveOnce(), 21U) << "生成的 id 形态变了，这条读数对应的形状也就不对";

        const AllocationProfile profile = measurePerOperation(resolveOnce);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * 21U) << "有几次没落定 id";
        std::printf("request-id-resolve 每次分配 %llu 次 / %llu 字节（一千次共 %llu 次 / %llu 字节）\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation),
                    static_cast<unsigned long long>(profile.totalAllocations),
                    static_cast<unsigned long long>(profile.totalBytes));
#ifdef NDEBUG
        EXPECT_EQ(profile.totalAllocations, kRequestIdTotalAllocationsPerThousand)
                << "落定 request-id 又开始碰堆了：生成或写入那条路上有人现造串？";
#endif
    }
} // namespace AsynGyanis::Net

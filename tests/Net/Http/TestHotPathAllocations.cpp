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
// 本轮量出来的读数（Release，摊平到每次操作）：解析一条 h1 请求 6 次 / 256 字节；装 10 条头部
// 4 次 / 144 字节——正好是四个超过短串内联缓冲的取值各一次；响应头序列化每次新建串 1 次，
// 复用同一块缓冲 0 次；解一帧 200 字节头块的 HEADERS 1 次 / 208 字节，就是取走的那份负载。
// 同一条形状在 Debug（带迭代器调试代理）下是 117 / 64 / 3 与 1 / 4。

#include "Net/Http/HttpHeaderFieldStore.h"
#include "Net/Http/HttpParser.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http2/Http2Frame.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
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

#ifdef NDEBUG
        // 下面这五个数是 Release（发布形态真正跑的那套配置）下实测摊平到每次操作的分配数。
        // 只在 Release 上钉死：Debug 的 STL 迭代器调试代理会给每个容器对象多挂一块代理，
        // 读数被实现细节放大一个量级（同一条 h1 解析实测 6 对 117），钉它等于钉噪声。
        // Debug 侧仍跑同样的形状，把读数打出来供对照，并保留两条与配置无关的结构判据
        constexpr std::uint64_t kHttp1ParseAllocationsPerRequest = 6U;
        constexpr std::uint64_t kHeaderRefillAllocationsPerTenFields = 4U;
        constexpr std::uint64_t kHeadSerializeAllocationsFresh = 1U;
        constexpr std::uint64_t kHeadSerializeAllocationsReused = 0U;
        constexpr std::uint64_t kFrameDecodeAllocationsPerFrame = 1U;
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
         * @brief 一次测量的结果：摊到每次操作的分配数，加上被测体返回标记的总和
         */
        struct AllocationProfile
        {
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
            profile.allocationsPerOperation = (ended.count - began.count) / kMeasurementIterations;
            profile.bytesPerOperation = (ended.bytes - began.bytes) / kMeasurementIterations;
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
        EXPECT_EQ(profile.allocationsPerOperation, 0U) << "测量窗里有背景分配，形状读数不可信";
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

        const AllocationProfile profile = measurePerOperation(parseOnce);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * expectedConsumed) << "有几次解析没走到 Done";
        std::printf("http1-parse-request 每次分配 %llu 次 / %llu 字节\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation));
#ifdef NDEBUG
        EXPECT_EQ(profile.allocationsPerOperation, kHttp1ParseAllocationsPerRequest)
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
            return store.fields().size();
        };
        refill();

        const AllocationProfile profile = measurePerOperation(refill);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * fixtures.size());
        std::printf("header-store-refill 每次分配 %llu 次 / %llu 字节\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation));
#ifdef NDEBUG
        EXPECT_EQ(profile.allocationsPerOperation, kHeaderRefillAllocationsPerTenFields)
                << "装 10 条头部的分配数变了：稳态下这张表只该按需扩容，不该每条头各要一块";
#endif
    }

    /**
     * @brief 响应头序列化的两种出口各付出多少次分配
     * @details 复用缓冲那条正是上一轮「头部缓冲按连接复用」冲着的读数：两条出口产出的文本一模一样，
     *          省下的就是那次新建串的分配，因此这里同时核对两者结果等长
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
        std::printf("response-head-serialize 每次分配 %llu 次 / %llu 字节；复用缓冲 %llu 次 / %llu 字节\n",
                    static_cast<unsigned long long>(fresh.allocationsPerOperation),
                    static_cast<unsigned long long>(fresh.bytesPerOperation),
                    static_cast<unsigned long long>(reused.allocationsPerOperation),
                    static_cast<unsigned long long>(reused.bytesPerOperation));
#ifdef NDEBUG
        EXPECT_EQ(fresh.allocationsPerOperation, kHeadSerializeAllocationsFresh) << "每响应一份新头部串的读数变了";
        EXPECT_EQ(reused.allocationsPerOperation, kHeadSerializeAllocationsReused)
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
} // namespace AsynGyanis::Net

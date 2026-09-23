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
//   · 收一条 h2 请求（四条伪头加三条普通头部的 GET，与 h1 那条同一批语料）：14 次 / 1325 字节。
//     同一条请求在 h1 侧是 0 次——差下来的是「每条请求各要一套头部存储」这条结构性成本：h1 的请求
//     对象按连接复用、容量留着，h2 每条流一份；这条形状刚钉下时是 18 次 / 2824 字节；
//   · 一条请求新建一个 HttpRequest 逐条装 10 条头部：不预留 24 次 / 2588 字节，先留 4 条 128 字节
//     是 12 次 / 2080 字节。HTTP/3 收请求头走的正是这条形状（请求对象随流新建，头部一条一条写进去）；
//   · 答一条 h2 响应：会话侧摊字段行 3 次 / 576 字节，连接侧组帧发出摊平 2 次 / 243 字节（一千次共
//     2014 次，多出那 14 次是 HPACK 动态表的插入与逐出）。这条量下来不是靶子——字段行那份向量本来
//     就 reserve(8) 过，响应头部的取值多数字符短到进小串内联；
//   · 组一帧 256 字节分块帧：每次新建串 1 次 / 272 字节，复用帧缓冲 0 次；
//   · 一条 h2 连接握手到关掉：每连接的固定成本（空闲连接也要付，故只作打印对照）。
// 同一条形状在 Debug（带迭代器调试代理）下的读数只作打印参考，确切值按 Release 钉。

#include "Net/Http/HttpChunkFrame.h"
#include "Net/Http/HttpHeaderFieldStore.h"
#include "Net/Http/HttpParser.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/HttpRequestId.h"
#include "Net/Http/HttpResponse.h"
#include "Net/Http/Router.h"
#include "Net/Http2/Hpack.h"
#include "Net/Http2/Http2Connection.h"
#include "Net/Http2/Http2Frame.h"

#include "AllocationProbe.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        // 轮数常量、AllocationProfile 与 measurePerOperation 都来自共用探针（AllocationProbe.h）
        using AsynGyanis::TestSupport::AllocationProfile;
        using AsynGyanis::TestSupport::kMeasurementIterations;
        using AsynGyanis::TestSupport::measurePerOperation;

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
        // 收一条 h2 请求（7 条头部）：解出来的字段串、整块头部的两份缓冲、流记录。交出请求的那份向量
        // 按会话的节奏还回来复用容量，因此不再计一次容器重建。
        // 同一条语料在 h1 那条形状上是 0 次——差值里剩的是「每条请求各要一套存储」这一条结构性成本
        constexpr std::uint64_t kRequestIngestTotalAllocationsPerThousand = 14000U;
        constexpr std::uint64_t kRequestIngestTotalBytesPerThousand = 1325000U;
        // 一条请求新建一个 HttpRequest 装 10 条头部：记录表与字节缓冲都从 0 按倍长上去的代价。
        // 预留那一档只留 4 条 / 128 字节（HTTP/3 收头实际用的猜测值），超出部分照常扩容
        constexpr std::uint64_t kAssemblyWithoutReserveTotalAllocationsPerThousand = 24000U;
        constexpr std::uint64_t kAssemblyWithSmallReserveTotalAllocationsPerThousand = 12000U;
        // 答一条 h2 响应分两段：会话侧摊字段行 3 次 / 576 字节，连接侧组帧发出摊平 2 次 / 243 字节。
        // 连接侧按一千次的原值钉：HPACK 动态表的插入与逐出不是每轮一次，摊平会把这点抖动抹平
        constexpr std::uint64_t kResponseCollectTotalAllocationsPerThousand = 3000U;
        constexpr std::uint64_t kResponseSendTotalAllocationsPerThousand = 2014U;
        // 每条额外头部应当不额外向堆要一次：HPACK 解出的字段直接落进请求的头部存储，
        // 名字与值都短到进小串内联，所以这里钉 0——哪天逐字段暂存容器回来了，这条先红
        constexpr std::uint64_t kMarginalAllocationsPerHeaderField = 0U;
        // 字节这条只设上界不设等号：一条 5 字节的头部实际占下的是「名字加值加偏移表里的一位」，
        // 而两块缓冲都按倍长留位，摊到一位头部上是几十量级——钉成等号等于钉住实现细节，
        // 换分配器或换偏移记录的宽度就会假红。上界的用途是抓住「头部开始为一位留胖位置」
        constexpr std::uint64_t kMarginalBytesPerHeaderFieldUpperPerThousand = 128000U;
#endif
        /// 宽形状比基准形状多出的头部条数：两种形状的差按它摊平才是「每条头部」的边际成本
        constexpr std::uint64_t kMarginalHeaderFieldCount = 10U;

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

        /**
         * @brief 编一个「不索引的字面量字段」，名取自静态表（RFC 7541 §6.2.2）
         * @details 整块头部一律用不索引的表示：动态表全程不动，读数里就只剩本端每条请求付的成本。
         *          带增量索引的写法会把「客户端要求的插表与逐出」也算进来，那量的是对端的选型。
         * @param staticNameIndex 静态表名下标（1..61；:authority 取 1，那是只带名字的一项）
         * @param value 头值，按原文写出（不走 Huffman）
         * @return std::string 编码后字节
         */
        std::string hpackUnindexedNamedField(const std::size_t staticNameIndex, const std::string_view value)
        {
            std::string bytes = encodeHpackInteger(staticNameIndex, 4, 0x10);
            appendHpackString(bytes, value);
            return bytes;
        }

        /**
         * @brief 编一个「名与值都是字面量」的不索引字段（§6.2.2 的名字索引 0）
         * @param name 头名，小写（本端会按 §8.1.2 拒掉大写名）
         * @param value 头值
         * @return std::string 编码后字节
         */
        std::string hpackUnindexedNamedField(const std::string_view name, const std::string_view value)
        {
            std::string bytes = encodeHpackInteger(0, 4, 0x10);
            appendHpackString(bytes, name);
            appendHpackString(bytes, value);
            return bytes;
        }

        /// 一条请求交出后能代表「内容真的落地了」的标记：伪头与每条普通头的名值长度之和
        std::size_t decodedRequestMark(const std::vector<Http2Request> &requests)
        {
            std::size_t mark = 0;
            for (const Http2Request &request: requests)
            {
                mark += request.method.size() + request.path.size() + request.authority.size();
                request.headerFields.forEachField([&mark](const std::string_view name, const std::string_view value)
                                                   {
                                                       mark += name.size() + value.size();
                                                   });
            }
            return mark;
        }

        /**
         * @brief 把一个头块包成一条 HEADERS 帧（END_STREAM | END_HEADERS，流号由调用方给）
         * @param headerBlock 已编好的 HPACK 头块
         * @param streamId 这条请求的流号，必须比上一条大（连接按递增校验）
         * @return std::string 完整帧字节
         */
        std::string makeHeadersFrameWithBlock(const std::string_view headerBlock, const std::uint32_t streamId)
        {
            std::string frame;
            frame.push_back(static_cast<char>((headerBlock.size() >> 16) & 0xFFU));
            frame.push_back(static_cast<char>((headerBlock.size() >> 8) & 0xFFU));
            frame.push_back(static_cast<char>(headerBlock.size() & 0xFFU));
            frame.push_back(0x01);                                          // HEADERS
            frame.push_back(static_cast<char>(0x01U | 0x04U));              // END_STREAM | END_HEADERS
            frame.push_back(static_cast<char>((streamId >> 24) & 0xFFU));   // 流号最高位是保留位，必须为 0
            frame.push_back(static_cast<char>((streamId >> 16) & 0xFFU));
            frame.push_back(static_cast<char>((streamId >> 8) & 0xFFU));
            frame.push_back(static_cast<char>(streamId & 0xFFU));
            frame += headerBlock;
            return frame;
        }

        /**
         * @brief 量「一条连接收下一条请求」的分配画像：喂 HEADERS → 解头块 → 交出 Http2Request
         * @details 两种头部数量不同的形状共用这一条通路，差值才是「每条头部付多少」；
         *          读数只覆盖收的方向，响应组帧与发出由 Http2ResponseSendAllocations 单独量。
         * @param headerBlock 这条请求要带的 HPACK 头块（用不索引的表示，动态表不参与）
         * @param firstMark 输出：暖身那一轮解出的判据值，调用方据此确认「真的解出了东西」
         * @return AllocationProfile 每请求摊平与原值两份读数
         */
        AllocationProfile measureH2RequestIngest(const std::string &headerBlock, std::size_t &firstMark)
        {
            // 一帧带 200 字节头块的 HEADERS 的兄弟形状：一条连接上的流号必须严格递增，因此每轮一帧，
            // 预先造好，测量窗口里不再组帧（否则量到的是用例自己的分配）。多造一帧给暖身那一轮吃掉，
            // 否则测量到最后会绕回第一条流号——重复使用已终止的流被判错，读数就少一轮
            std::vector<std::string> requestFrames;
            requestFrames.reserve(kMeasurementIterations + 1U);
            for (std::uint64_t iteration = 0; iteration <= kMeasurementIterations; ++iteration)
            {
                const std::uint32_t streamId = static_cast<std::uint32_t>(2U * iteration + 1U);
                requestFrames.push_back(makeHeadersFrameWithBlock(headerBlock, streamId));
            }

            std::string settingsFrame;
            settingsFrame.append(3, '\0');      // 帧长度 0：一个空 SETTINGS，只为把连接推进到能用
            settingsFrame.push_back(0x04);
            settingsFrame.push_back(0x00);
            settingsFrame.append(4, '\0');
            // 本端并发达 100 条：不回响应就不算「双向 END_STREAM 终止」，流会一直占着并发额度，
            // 第 101 条起被 REFUSED_STREAM 拒掉，读数就断在半路。放到覆盖整个测量窗口
            Http2ConnectionConfiguration configuration;
            configuration.maximumConcurrentStreams = static_cast<std::uint32_t>(kMeasurementIterations + 1U);
            Http2Connection connection{configuration};
            static_cast<void>(connection.feedBytes(kHttp2ConnectionPreface.data(), kHttp2ConnectionPreface.size()));
            static_cast<void>(connection.feedBytes(settingsFrame.data(), settingsFrame.size()));
            static_cast<void>(connection.takeOutgoingBytes());

            std::size_t frameCursor = 0;
            const auto ingestOnce = [&connection, &requestFrames, &frameCursor]() -> std::size_t
            {
                const std::string &frame = requestFrames[frameCursor % requestFrames.size()];
                ++frameCursor;
                static_cast<void>(connection.feedBytes(frame.data(), frame.size()));
                // 判据只取长度之和：这里既不能构造临时串也不能建容器，否则量进来的是用例自己的分配
                std::vector<Http2Request> requests = connection.takeRequests();
                const std::size_t mark = decodedRequestMark(requests);
                // 会话侧是「遍历完把向量还回去复用容量」的（absorbPendingRequests），这里跟同一条节奏：
                // 不还的话每轮都要为这份向量另要一块堆，量的就不是稳态成本
                connection.recycleRequests(std::move(requests));
                static_cast<void>(connection.takeOutgoingBytes());
                return mark;
            };

            firstMark = ingestOnce();
            EXPECT_FALSE(connection.hasFailed()) << connection.errorMessage();
            return measurePerOperation(ingestOnce);
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
     * @brief 一条请求新建一个 HttpRequest 逐条装头部付多少次分配（不预留 vs 预留四分之一个量）
     * @details HTTP/3 收请求头走的就是这条形状：请求对象随流新建，头部一条条 addHeader 进去，装完
     *          随请求交给业务。与上面「复用同一份存储装 10 条头部＝0 次」对照，差的全在
     *          「每条请求各要一套新缓冲」——记录表与字节缓冲各自从 0 按倍长上去。
     */
    TEST(HotPathAllocations, HttpRequestHeaderAssemblyAllocations)
    {
        const std::vector<std::pair<std::string, std::string>> fixtures = makeHeaderFixtures();
        const auto markOf = [](const HttpRequest &request)
        {
            // 只走视图出口：取值返回 optional<string> 的那条每次都要造临时串，会把读数弄脏
            return request.firstHeaderValueView("content-length").value_or(std::string_view{}).size()
                 + request.firstHeaderValueView("authorization").value_or(std::string_view{}).size();
        };
        const auto assembleWithoutReserve = [&fixtures, &markOf]
        {
            HttpRequest request;
            for (const auto &[name, value]: fixtures)
            {
                request.addHeader(name, value);
            }
            return markOf(request);
        };
        // 4 条 / 128 字节是 HTTP/3 那边实际要用的猜测值：常见请求头比这少，超出照样扩容
        const auto assembleWithSmallReserve = [&fixtures, &markOf]
        {
            HttpRequest request;
            request.reserveHeaders(4U, 128U);
            for (const auto &[name, value]: fixtures)
            {
                request.addHeader(name, value);
            }
            return markOf(request);
        };
        // content-length 的 "64" 两条字符，加 authorization 那条 43 字符的取值
        const std::size_t expectedMark = 2U + 43U;
        ASSERT_EQ(assembleWithoutReserve(), expectedMark) << "这条形状没把头部装上，读数没意义";
        ASSERT_EQ(assembleWithSmallReserve(), expectedMark) << "预留过的那份与不预留的那份读到的值不一致";

        const AllocationProfile fresh = measurePerOperation(assembleWithoutReserve);
        const AllocationProfile reserved = measurePerOperation(assembleWithSmallReserve);
        EXPECT_LT(reserved.totalAllocations, fresh.totalAllocations)
                << "一次留够反而不比逐条扩容省：reserveHeaders 没接到存储侧，或两侧容器已不再按倍长";
        std::printf("request-header-assembly 每次分配 不预留 %llu 次 / %llu 字节；预留 4 条 128 字节 %llu 次 / %llu 字节\n",
                    static_cast<unsigned long long>(fresh.allocationsPerOperation),
                    static_cast<unsigned long long>(fresh.bytesPerOperation),
                    static_cast<unsigned long long>(reserved.allocationsPerOperation),
                    static_cast<unsigned long long>(reserved.bytesPerOperation));
#ifdef NDEBUG
        EXPECT_EQ(fresh.totalAllocations, kAssemblyWithoutReserveTotalAllocationsPerThousand)
                << "逐条装 10 条头部的分配数变了：记录表或字节缓冲的扩容节奏改了";
        EXPECT_EQ(reserved.totalAllocations, kAssemblyWithSmallReserveTotalAllocationsPerThousand)
                << "预留过的那条读数变了：留的量或扩容节奏改了，HTTP/3 收头那条路径要按这条重估";
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
     * @brief 光建起一条 h2 连接再析构付出多少次分配（一个字节都没读写）
     * @details 与下一条用例合起来把「每连接固定成本」拆成两段：这一段量的是容器与成员的默认构造，
     *          里面每一笔都是「还没用就先占一块」，也就都是可以去掉的
     */
    TEST(HotPathAllocations, Http2ConnectionConstructionAllocations)
    {
        const auto constructOnce = []() -> std::size_t
        {
            Http2Connection connection;
            return static_cast<std::size_t>(connection.state());
        };
        static_cast<void>(constructOnce());

        const AllocationProfile profile = measurePerOperation(constructOnce);
        std::printf("h2 每条连接的构造成本 %llu 次分配 / %llu 字节\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation));
    }

    /**
     * @brief 一条 h2 连接从建起到握手完成再关掉付出多少次分配（每连接的固定成本）
     * @details 喂完 preface 与一个空 SETTINGS 就析构，一个头部没解、一条流没开——空闲连接付的就是
     *          这一笔，因此它是「每连接内存」的下界：容器与缓冲里那些压根没用过却先占一块的，全在
     *          这个读数里。这里不钉硬阈值：这条形状的存在理由就是把改动前后的对照数字留在那里
     */
    TEST(HotPathAllocations, Http2ConnectionHandshakeAllocations)
    {
        std::string settingsFrame;
        settingsFrame.append(3, '\0');      // 帧长度 0
        settingsFrame.push_back(0x04);      // SETTINGS
        settingsFrame.push_back(0x00);      // 标志位：非 ACK
        settingsFrame.append(4, '\0');      // 流标识 0
        const std::string handshakeBytes = std::string(kHttp2ConnectionPreface) + settingsFrame;

        const auto handshakeOnce = [&handshakeBytes]() -> std::size_t
        {
            Http2Connection connection;
            static_cast<void>(connection.feedBytes(handshakeBytes.data(), handshakeBytes.size()));
            return connection.takeOutgoingBytes().size();
        };
        const std::size_t firstOutgoingByteCount = handshakeOnce();
        EXPECT_GT(firstOutgoingByteCount, 0U) << "握手一个字节都没发，这条用例没测到东西";

        const AllocationProfile profile = measurePerOperation(handshakeOnce);
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * firstOutgoingByteCount)
                << "有几次握手的产物长度不一致，读数不可信";
        std::printf("h2 每条连接的握手成本 %llu 次分配 / %llu 字节\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation));
    }

    /**
     * @brief 一条 h2 请求从字节走到交给上层的 Http2Request，本端付出多少次分配
     * @details 窗里含「解头块 → 伪头分档 → 普通头逐条落地 → 建流记录 → 交出请求」整段，不含响应方向。
     *          头块的字段与 h1 那条形状同一批语料（同一条 GET /api/v1/orders?trace=1 加三条常用头部），
     *          两条读数因此可直接对照：h1 侧的请求对象按连接复用、容量跨报文留着，所以是 0 次。
     */
    TEST(HotPathAllocations, Http2RequestIngestAllocations)
    {
        std::string headerBlock;
        headerBlock += hpackUnindexedNamedField(2U, "GET");                     // :method
        headerBlock += hpackUnindexedNamedField(6U, "http");                    // :scheme
        headerBlock += hpackUnindexedNamedField(4U, "/api/v1/orders?trace=1");  // :path
        headerBlock += hpackUnindexedNamedField(1U, "api.example.com");         // :authority
        headerBlock += hpackUnindexedNamedField("user-agent", "curl/8.7.1");
        headerBlock += hpackUnindexedNamedField("accept", "*/*");
        headerBlock += hpackUnindexedNamedField("accept-encoding", "gzip, deflate, br");

        std::size_t firstMark = 0;
        const AllocationProfile profile = measureH2RequestIngest(headerBlock, firstMark);
        EXPECT_GT(firstMark, 60U) << "请求没解出来或伪头没落地，这条用例没测到东西";
        std::printf("h2 每收一条请求（HPACK 解头块到交出 Http2Request）%llu 次分配 / %llu 字节（一千次共 %llu 次 / %llu 字节）\n",
                    static_cast<unsigned long long>(profile.allocationsPerOperation),
                    static_cast<unsigned long long>(profile.bytesPerOperation),
                    static_cast<unsigned long long>(profile.totalAllocations),
                    static_cast<unsigned long long>(profile.totalBytes));
#ifdef NDEBUG
        EXPECT_EQ(profile.resultSum, kMeasurementIterations * firstMark) << "有几次请求解得不一样，读数不可信";
        EXPECT_EQ(profile.totalAllocations, kRequestIngestTotalAllocationsPerThousand)
                << "收一条 h2 请求的分配数变了：头部逐字段落串、流记录与交出向量这三处都会计进来";
        EXPECT_EQ(profile.totalBytes, kRequestIngestTotalBytesPerThousand)
                << "读数按一千次原值钉：条数不变但每块更大，同样是实现变了";
#endif
    }

    /**
     * @brief 钉住「每条请求头部多一条，本端多付多少分配」这条边际成本
     * @details 绝对读数看不出「逐字段落串」和「逐请求落一次」的区别：七条头部时量到 14 次，
     *          可能全是每字段两次，也可能是固定的每请求开销。取两种形状的差才指得准该修哪一处，
     *          也因此这里刻意让多出来的十条名字与值都短到进小串内联——差值里就不含「字符串本身要堆」，
     *          剩下的纯是容器与逐字段暂存。
     */
    TEST(HotPathAllocations, Http2RequestIngestMarginalCostPerHeaderField)
    {
        std::string baseBlock;
        baseBlock += hpackUnindexedNamedField(2U, "GET");
        baseBlock += hpackUnindexedNamedField(6U, "http");
        baseBlock += hpackUnindexedNamedField(4U, "/api/v1/orders?trace=1");
        baseBlock += hpackUnindexedNamedField(1U, "api.example.com");
        baseBlock += hpackUnindexedNamedField("user-agent", "curl/8.7.1");
        baseBlock += hpackUnindexedNamedField("accept", "*/*");
        baseBlock += hpackUnindexedNamedField("accept-encoding", "gzip, deflate, br");

        std::string wideBlock = baseBlock;
        for (char letter = 'a'; letter <= 'j'; ++letter)
        {
            const std::string fieldName = std::string("x-t") + letter;
            wideBlock += hpackUnindexedNamedField(fieldName, "ab");
        }

        std::size_t baseMark = 0;
        std::size_t wideMark = 0;
        const AllocationProfile base = measureH2RequestIngest(baseBlock, baseMark);
        const AllocationProfile wide = measureH2RequestIngest(wideBlock, wideMark);
        EXPECT_GT(wideMark, baseMark) << "宽形状没多解出头部，差值不是每条头部的成本";

        const std::uint64_t marginalAllocations = base.totalAllocations > wide.totalAllocations
                                                       ? 0U
                                                       : (wide.totalAllocations - base.totalAllocations) / kMarginalHeaderFieldCount;
        const std::uint64_t marginalBytes = (wide.totalBytes - base.totalBytes) / kMarginalHeaderFieldCount;
        std::printf("h2 每条额外头部的边际成本：%llu 次分配 / %llu 字节（一千次里，七条头部共 %llu 次，十七条共 %llu 次）\n",
                    static_cast<unsigned long long>(marginalAllocations),
                    static_cast<unsigned long long>(marginalBytes),
                    static_cast<unsigned long long>(base.totalAllocations),
                    static_cast<unsigned long long>(wide.totalAllocations));
#ifdef NDEBUG
        EXPECT_EQ(base.resultSum, kMeasurementIterations * baseMark) << "基准形状有几次请求解得不一样";
        EXPECT_EQ(wide.resultSum, kMeasurementIterations * wideMark) << "宽形状有几次请求解得不一样";
        EXPECT_EQ(marginalAllocations, kMarginalAllocationsPerHeaderField)
                << "每条头部多付了一次分配：HPACK 解出的字段又落进暂存容器，而不是直接进请求的头部存储";
        // 字节只卡上界：一条头部占下的应当只是「名字加值加一位记录」这一档，两块缓冲都按倍长
        // 留位，摊到一位头部是几十量级；钉成等号就是把实现细节当判据，换分配器即假红
        EXPECT_LE(marginalBytes, kMarginalBytesPerHeaderFieldUpperPerThousand)
                << "每位头部的字节成本越过上界（读数是「一千次里每条头部占下的字节」）：头部存储开始为一位留胖位置";

        // 对照形状：同样多十条头部，但每条的值长到 64 字节，装不进小串内联。这条必须量出
        // 「每条头部至少一次分配」——否则上面那个 0 只能说明探针看不见逐字段分配，而不是没有
        std::string longValueBlock = baseBlock;
        for (char letter = 'a'; letter <= 'j'; ++letter)
        {
            const std::string fieldName = std::string("x-t") + letter;
            longValueBlock += hpackUnindexedNamedField(fieldName, std::string(64U, 'v'));
        }
        std::size_t longValueMark = 0;
        const AllocationProfile longValue = measureH2RequestIngest(longValueBlock, longValueMark);
        EXPECT_GT(longValueMark, wideMark) << "对照形状没比宽形状多解出内容，这条对照无效";
        const std::uint64_t longValueMarginalAllocations =
                (longValue.totalAllocations - base.totalAllocations) / kMarginalHeaderFieldCount;
        EXPECT_GE(longValueMarginalAllocations, 1000U)
                << "探针看不见逐字段的堆分配：十条 64 字节值的头部一次都没多要，那个 0 读数就不能作数";
#endif
    }

    /**
     * @brief 答一条 h2 请求付多少次分配：会话侧摊字段行、连接侧组帧发出
     * @details 两段分开量——采集器那条与 `Http2Session::collectResponseHeaderFields()` 同形（每响应
     *          一份 `vector<HpackHeaderField>`，逐条两份 owning 串），组帧那条走连接层的公开出口并按
     *          会话的节奏把待发缓冲还回去。两条相加才是一条响应的每响应成本。
     */
    TEST(HotPathAllocations, Http2ResponseSendAllocations)
    {
        const HttpResponse response = makeResponseFixture();
        const auto collectOnce = [&response]
        {
            std::vector<HpackHeaderField> fieldLines;
            fieldLines.reserve(8U);
            std::size_t mark = 0;
            response.forEachHeaderField([&fieldLines, &mark](const std::string_view name, const std::string_view value)
                                        {
                                            mark += name.size() + value.size();
                                            fieldLines.push_back(HpackHeaderField{std::string(name), std::string(value)});
                                        });
            return mark + fieldLines.size();
        };
        ASSERT_GT(collectOnce(), 40U) << "这条形状没把响应头部摊出来，读数没意义";

        const AllocationProfile collected = measurePerOperation(collectOnce);
        std::printf("h2 每条响应：会话侧摊字段行 %llu 次分配 / %llu 字节\n",
                    static_cast<unsigned long long>(collected.allocationsPerOperation),
                    static_cast<unsigned long long>(collected.bytesPerOperation));

        // 组帧要有活着的流：先把整窗口的请求喂在窗外，测量窗口里只剩「发响应」这一段
        Http2ConnectionConfiguration configuration;
        configuration.maximumConcurrentStreams = static_cast<std::uint32_t>(kMeasurementIterations + 2U);
        Http2Connection connection{configuration};
        std::string settingsFrame;
        settingsFrame.append(3, '\0');
        settingsFrame.push_back(0x04);
        settingsFrame.push_back(0x00);
        settingsFrame.append(4, '\0');
        static_cast<void>(connection.feedBytes(kHttp2ConnectionPreface.data(), kHttp2ConnectionPreface.size()));
        static_cast<void>(connection.feedBytes(settingsFrame.data(), settingsFrame.size()));
        static_cast<void>(connection.takeOutgoingBytes());

        std::string requestBlock;
        requestBlock += hpackUnindexedNamedField(2U, "GET");
        requestBlock += hpackUnindexedNamedField(6U, "http");
        requestBlock += hpackUnindexedNamedField(4U, "/bench");
        // 多喂一条给窗外那次「先看产物长度」的调用吃掉，测量窗口里才不会用到没开过的流
        for (std::uint64_t iteration = 0; iteration <= kMeasurementIterations; ++iteration)
        {
            Http2HeadersPayload headPayload;
            headPayload.endStream = true;
            headPayload.endHeaders = true; // 不落 END_HEADERS 就变成「等 CONTINUATION」，下一帧的 HEADERS 会被 §6.10 判成连接错误
            headPayload.headerBlockFragment = requestBlock;
            const std::string frame = encodeHttp2HeadersFrame(headPayload, static_cast<std::uint32_t>(2U * iteration + 1U));
            static_cast<void>(connection.feedBytes(frame.data(), frame.size()));
            static_cast<void>(connection.takeRequests());
            static_cast<void>(connection.takeOutgoingBytes());
        }

        const std::vector<HpackHeaderField> fieldLines = [&response]
        {
            std::vector<HpackHeaderField> lines;
            response.forEachHeaderField([&lines](const std::string_view name, const std::string_view value)
                                        {
                                            lines.push_back(HpackHeaderField{std::string(name), std::string(value)});
                                        });
            return lines;
        }();
        const std::string_view body = response.body();

        std::uint64_t sendCursor = 0;
        std::size_t lastResponseBytes = 0;
        std::string lastErrorText;
        const auto sendOnce = [&connection, &fieldLines, &body, &sendCursor, &lastResponseBytes, &lastErrorText]() -> std::size_t
        {
            const std::uint32_t streamId = static_cast<std::uint32_t>(2U * sendCursor + 1U);
            ++sendCursor;
            std::string errorText;
            const Http2ResponseSendStatus headerStatus =
                    connection.sendResponseHeaders(streamId, 200U, fieldLines, false, &errorText);
            const Http2ResponseSendStatus bodyStatus = connection.sendResponseData(streamId, body, true, &errorText);
            std::string outgoingBytes = connection.takeOutgoingBytes();
            lastResponseBytes = outgoingBytes.size();
            lastErrorText = errorText;
            // 会话发完就把这块还回去复用容量，这里跟着同一个节奏走，免得把「每响应一整块待发串」算成实现的成本
            connection.recycleOutgoingBytes(std::move(outgoingBytes));
            // 判据取「这一轮确实把响应发出去了」而不是产物长度：HPACK 动态表会把同一条响应越编越短，
            // 长度逐轮变小是设计行为，拿它当一致性判据会把正确的实现读成「有几次没发全」
            return headerStatus == Http2ResponseSendStatus::Sent && bodyStatus == Http2ResponseSendStatus::Sent
                           ? std::size_t{1}
                           : std::size_t{0};
        };
        // 先把「响应真的发出去了」钉住再谈读数：Rejected 与 StreamNotWritable 也会产出几十字节的 RST，
        // 只按字节数判绿会把一条根本没发的响应读成「每响应只付 2 次分配」
        ASSERT_EQ(sendOnce(), 1U) << lastErrorText;
        EXPECT_GT(lastResponseBytes, 60U) << "响应一帧都没发出去，这条用例没测到东西";

        const AllocationProfile sent = measurePerOperation(sendOnce);
        EXPECT_EQ(sent.resultSum, kMeasurementIterations) << "有几次响应没发出去，这条读数测的不是「发一条响应」";
        std::printf("h2 每条响应：连接组帧发出 %llu 次分配 / %llu 字节\n",
                    static_cast<unsigned long long>(sent.allocationsPerOperation),
                    static_cast<unsigned long long>(sent.bytesPerOperation));
#ifdef NDEBUG
        EXPECT_EQ(collected.totalAllocations, kResponseCollectTotalAllocationsPerThousand)
                << "会话侧摊一次响应头部的分配数变了：字段行的形状或预留方式改了";
        EXPECT_EQ(sent.totalAllocations, kResponseSendTotalAllocationsPerThousand)
                << "连接侧发一条响应的分配数变了：HPACK 组帧、DATA 帧或待发缓冲的复用改了";
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

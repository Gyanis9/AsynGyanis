// TestHttpObservability.cpp —— HTTP 侧可观测性的系统级覆盖（真实回环 + 真实 HttpServer）：
//   一. request-id 生成：请求未带 x-request-id 时由服务器生成定长标识并写进响应头，
//       同一服务器连发两条请求拿到的标识互不相同；
//   二. request-id 采信面：客户端自带合法取值时原样回显；
//   三. request-id 拒绝面：超长、含 TAB、含高位字节的取值一律按「客户端没给」处理，
//       响应里绝不出现这些不可信内容；
//   四. 统计快照：连发 5 条 200 与 1 条 404 后，请求条数、状态码类计数与延迟直方图样本数对得上，
//       客户端主动断开不计入超时计数，活跃连接数随连接关闭回落到 0；
//   五. 超时计数：被空闲清扫协程按空闲超时收口的连接计入 timeoutClosedCount，且不计入请求条数。
// 夹具（RunningHttpServerFixture / LoopbackClient / 报文组装）在 HttpTestSupport.h 中，与限额用例共用一份。

#include "Net/Http/HttpServer.h"
#include "Net/Http/HttpServerStats.h"

#include "HttpTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using namespace HttpTestSupport;

        /// 生成的 request-id 形态：4 位十六进制前缀 + '-' + 16 位十六进制序号（见 HttpRequestId.h）
        constexpr std::size_t kGeneratedRequestIdLength = 4 + 1 + 16;

        /// 生成形态里分隔前缀与序号的位置
        constexpr std::size_t kGeneratedRequestIdSeparatorIndex = 4;

        /// request-id 响应头的行内前缀（响应头名前带 CRLF，避免误命中正文里的同名文本）
        constexpr std::string_view kRequestIdHeaderLinePrefix = "\r\nx-request-id: ";

        /**
         * @brief 观测性用例用的限额：超时三项都设得很长
         * @details 这些用例只关心计数与标识，不该被空闲清扫在中途收口连接（超时计数用例另行覆盖该项）。
         * @return HttpServerLimits 关掉超时保护的配置
         */
        HttpServerLimits makeLongTimeoutLimits()
        {
            HttpServerLimits limits;
            limits.idleTimeout  = std::chrono::seconds{10};
            limits.readTimeout  = std::chrono::seconds{10};
            limits.writeTimeout = std::chrono::seconds{10};
            return limits;
        }

        /**
         * @brief 判断一段文本是否是服务器生成的 request-id
         * @param requestId 待判定的标识
         * @return true 长度符合约定，且分隔符位置与其余字符都是小写十六进制
         */
        bool looksLikeGeneratedRequestId(const std::string_view requestId)
        {
            if (requestId.size() != kGeneratedRequestIdLength || requestId[kGeneratedRequestIdSeparatorIndex] != '-')
            {
                return false;
            }

            for (std::size_t index = 0; index < requestId.size(); ++index)
            {
                if (index == kGeneratedRequestIdSeparatorIndex)
                {
                    continue;
                }
                const char character = requestId[index];
                const bool isLowerHexDigit = (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
                if (!isLowerHexDigit)
                {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief 统计一段文本里子串出现的次数
         * @param text 待搜索文本
         * @param needle 目标子串
         * @return std::size_t 出现次数
         */
        std::size_t countTextOccurrences(const std::string &text, const std::string_view needle)
        {
            std::size_t occurrenceCount = 0;
            for (std::size_t foundPosition = text.find(needle); foundPosition != std::string::npos;
                 foundPosition = text.find(needle, foundPosition + needle.size()))
            {
                ++occurrenceCount;
            }
            return occurrenceCount;
        }

        /**
         * @brief 轮询读，直到累计出现的标记文本达到指定次数
         * @details 断言「第 n 条响应里的某个头部」必须用本方法：读到正文标记说明该响应的头部块
         *          已经完整落在累计缓冲里（TCP 保序，头部字节一定先到），按状态行提前返回则可能
         *          只读到半截头部。
         * @param client 回环客户端
         * @param accumulated 输入输出：累计读到的字节
         * @param expectedText 作为「响应已完整到达」判据的标记文本
         * @param expectedCount 期望出现的次数
         * @param timeout 等待上限
         * @return true 在时限内凑齐
         */
        bool waitForTextOccurrences(const LoopbackClient &client, std::string &accumulated, const std::string_view expectedText,
                                    const std::size_t expectedCount, const std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (countTextOccurrences(accumulated, expectedText) < expectedCount)
            {
                const ReadOutcome outcome = client.readOnce(accumulated);
                if (outcome == ReadOutcome::PeerClosed || outcome == ReadOutcome::Broken)
                {
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return countTextOccurrences(accumulated, expectedText) >= expectedCount;
        }

        /**
         * @brief 取响应文本里第 occurrence 个 x-request-id 头的值
         * @param responseText 已收到的全部字节
         * @param occurrence 第几个（从 0 开始）
         * @return std::string 头部值；不存在时返回空串
         */
        std::string requestIdHeaderAt(const std::string &responseText, const std::size_t occurrence)
        {
            std::size_t foundPosition = responseText.find(kRequestIdHeaderLinePrefix);
            for (std::size_t skippedCount = 0; foundPosition != std::string::npos; ++skippedCount)
            {
                if (skippedCount == occurrence)
                {
                    const std::size_t valueStart = foundPosition + kRequestIdHeaderLinePrefix.size();
                    const std::size_t lineEnd    = responseText.find("\r\n", valueStart);
                    if (lineEnd == std::string::npos)
                    {
                        return {};
                    }
                    return responseText.substr(valueStart, lineEnd - valueStart);
                }
                foundPosition = responseText.find(kRequestIdHeaderLinePrefix, foundPosition + kRequestIdHeaderLinePrefix.size());
            }
            return {};
        }

        /**
         * @brief 不可信取值用例的参数
         */
        struct UnacceptableRequestIdCase
        {
            std::string description; ///< 用例内的说明，用于断言失败时定位是哪一种取值
            std::string value;       ///< 放进 x-request-id 的原文
        };

        /**
         * @brief 三种不可信取值：超长（70 字节）、含 TAB（0x09）、含高位字节（0x80）
         * @details 三者都能通过报文解析（解析器允许 TAB 与 >= 0x80 的字节），因此这里拒绝它们的
         *          只可能是 request-id 自身的采信判定——这一点正是本组用例要钉住的。
         * @return std::vector<UnacceptableRequestIdCase> 取值列表，每个都带 "untrusted" 标记
         */
        std::vector<UnacceptableRequestIdCase> makeUnacceptableRequestIdCases()
        {
            std::vector<UnacceptableRequestIdCase> cases;

            // 70 字节，超过 64 字节上限
            cases.push_back({"超长取值", "untrusted-" + std::string(60, 'a')});

            // 值中间的水平制表符：解析器只在两端裁 OWS，中间这个 TAB 会原样留在值里
            std::string tabbedValue = "untrusted";
            tabbedValue.push_back('\t');
            tabbedValue.append("tab");
            cases.push_back({"含 TAB 的取值", std::move(tabbedValue)});

            // 高位字节：可见 ASCII 之外，回显进响应头与日志都会产生看不见的错位
            std::string highByteValue = "untrusted";
            highByteValue.push_back(static_cast<char>(0x80));
            cases.push_back({"含高位字节的取值", std::move(highByteValue)});

            return cases;
        }
    } // namespace

    /**
     * @brief 钉住：请求未带 x-request-id 时，服务器生成一个定长标识并写进响应头，且两条请求不重复
     */
    TEST(HttpObservability, GeneratesRequestIdWhenClientDidNotProvideOne)
    {
        RunningHttpServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";

        // 同一条 keep-alive 连接上连发两条：既验证逐条生成，也验证生成的标识不重复
        std::string receivedText;
        ASSERT_TRUE(client.sendText(helloRequestText(), kWaitTimeout)) << "第 1 条请求未能写入";
        ASSERT_TRUE(waitForTextOccurrences(client, receivedText, "served-hello", 1, kWaitTimeout)) << "第 1 条请求未得到完整响应";
        ASSERT_TRUE(client.sendText(helloRequestText(), kWaitTimeout)) << "第 2 条请求未能写入";
        ASSERT_TRUE(waitForTextOccurrences(client, receivedText, "served-hello", 2, kWaitTimeout)) << "第 2 条请求未得到完整响应";

        const std::string firstRequestId  = requestIdHeaderAt(receivedText, 0);
        const std::string secondRequestId = requestIdHeaderAt(receivedText, 1);

        EXPECT_TRUE(looksLikeGeneratedRequestId(firstRequestId))
                << "第 1 条响应的 request-id 形态不符合约定（应为 4 位十六进制前缀 + '-' + 16 位十六进制序号）：「" << firstRequestId << "」";
        EXPECT_TRUE(looksLikeGeneratedRequestId(secondRequestId))
                << "第 2 条响应的 request-id 形态不符合约定：「" << secondRequestId << "」";
        EXPECT_NE(firstRequestId, secondRequestId) << "同一服务器上的两条请求拿到了同一个 request-id";
    }

    /**
     * @brief 钉住：客户端自带合法 x-request-id 时原样回显（替换掉就断了上游链路关联）
     */
    TEST(HttpObservability, EchoesAcceptableClientRequestIdVerbatim)
    {
        RunningHttpServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";

        // 取 64 字节上限内的合法值：可见 ASCII，且带常见的前缀与连字符形态
        const std::string clientRequestId = "trace-id-from-client-42";
        std::string       receivedText;
        ASSERT_TRUE(client.sendText(makeRequestText("GET /hello HTTP/1.1", {"x-request-id: " + clientRequestId}), kWaitTimeout))
                << "带 x-request-id 的请求未能写入";
        ASSERT_TRUE(waitForTextOccurrences(client, receivedText, "served-hello", 1, kWaitTimeout)) << "请求未得到完整响应";

        EXPECT_EQ(requestIdHeaderAt(receivedText, 0), clientRequestId) << "客户端自带的合法 request-id 没有被原样回显";
    }

    /**
     * @brief 钉住拒绝面：超长 / 含 TAB / 含高位字节的 x-request-id 一律按「客户端没给」处理
     * @details 这些取值在协议层都是合法头部值，因此必须由 request-id 的采信判定拦下；
     *          断言同时检查「响应里没出现不可信内容」与「换成了生成形态」两件事。
     */
    TEST(HttpObservability, RefusesToEchoUnacceptableClientRequestId)
    {
        RunningHttpServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        for (const UnacceptableRequestIdCase &unacceptableCase: makeUnacceptableRequestIdCases())
        {
            // 每种取值各用一条独立连接：断言因此能按「本次响应里唯一的那个头」取值，不必做下标推算
            LoopbackClient client(listeningPort);
            ASSERT_TRUE(client.isValid()) << "回环连接失败：" << unacceptableCase.description;

            std::string receivedText;
            ASSERT_TRUE(client.sendText(makeRequestText("GET /hello HTTP/1.1", {"x-request-id: " + unacceptableCase.value}), kWaitTimeout))
                    << "请求未能写入：" << unacceptableCase.description;
            ASSERT_TRUE(waitForTextOccurrences(client, receivedText, "served-hello", 1, kWaitTimeout))
                    << "请求未得到完整响应：" << unacceptableCase.description;

            const std::string echoedRequestId = requestIdHeaderAt(receivedText, 0);
            EXPECT_EQ(echoedRequestId.find("untrusted"), std::string::npos)
                    << "不可信的 request-id 被回显进了响应头：" << unacceptableCase.description;
            EXPECT_NE(echoedRequestId, unacceptableCase.value) << "不可信的 request-id 被原样采信：" << unacceptableCase.description;
            EXPECT_TRUE(looksLikeGeneratedRequestId(echoedRequestId))
                    << "不可信取值没有被换成生成形态（而应「按客户端没给」处理）：「" << echoedRequestId << "」";
        }
    }

    /**
     * @brief 钉住：请求条数、状态码类计数、延迟直方图样本数与活跃连接数在真实流量下对得上
     */
    TEST(HttpObservability, CountsRequestsStatusClassesAndLatency)
    {
        RunningHttpServerFixture fixture(makeLongTimeoutLimits(), std::chrono::milliseconds{100});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        constexpr std::size_t kSuccessfulRequestCount = 5;
        constexpr std::size_t kTotalRequestCount      = kSuccessfulRequestCount + 1;

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";

        // 5 条 200（已注册的 /hello）与 1 条 404（未注册路径），全部走同一条 keep-alive 连接
        std::string receivedText;
        for (std::size_t requestIndex = 0; requestIndex < kSuccessfulRequestCount; ++requestIndex)
        {
            ASSERT_TRUE(client.sendText(helloRequestText(), kWaitTimeout)) << "第 " << requestIndex + 1 << " 条请求未能写入";
            ASSERT_TRUE(waitForTextOccurrences(client, receivedText, "served-hello", requestIndex + 1, kWaitTimeout))
                    << "第 " << requestIndex + 1 << " 条请求未得到完整响应";
        }
        ASSERT_TRUE(client.sendText(makeRequestText("GET /missing HTTP/1.1"), kWaitTimeout)) << "404 请求未能写入";
        ASSERT_TRUE(client.waitForStatusLines(receivedText, kTotalRequestCount, kWaitTimeout)) << "404 响应没有到达";

        // 响应发出与计数落账之间隔着一次协程恢复，因此按条件轮询而不是立刻断言
        ASSERT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().stats().totalRequestCount >= kTotalRequestCount;
                },
                kWaitTimeout)) << "统计未在时限内记满 " << kTotalRequestCount << " 条请求";

        const HttpServerStats stats = fixture.server().stats();
        EXPECT_EQ(stats.totalRequestCount, kTotalRequestCount) << "累计请求条数不符（不含解析失败）";
        EXPECT_EQ(stats.status2xxCount, kSuccessfulRequestCount) << "2xx 计数不符";
        EXPECT_EQ(stats.status4xxCount, 1u) << "404 没有计入 4xx";
        EXPECT_EQ(stats.status1xxCount, 0u) << "1xx 计数应为 0";
        EXPECT_EQ(stats.status3xxCount, 0u) << "3xx 计数应为 0";
        EXPECT_EQ(stats.status5xxCount, 0u) << "5xx 计数应为 0";
        EXPECT_EQ(stats.latencySampleCount(), stats.totalRequestCount)
                << "延迟直方图的样本数与请求条数不一致：有一条请求漏进了直方图";
        EXPECT_EQ(stats.badRequestCount, 0u) << "正常报文被记成了协议错误";
        EXPECT_GE(stats.activeConnectionCount, 1u) << "连接仍在服务，活跃连接数却是 0";

        // 客户端主动断开：会话自然收口，因此只该减少活跃连接数，不该计入超时收口
        client.closeNow();
        EXPECT_TRUE(fixture.awaitConnectionsDrained(kWaitTimeout)) << "会话收口后未从连接管理器摘除";

        const HttpServerStats drainedStats = fixture.server().stats();
        EXPECT_EQ(drainedStats.activeConnectionCount, 0u) << "连接已关闭，活跃连接数没有回落";
        EXPECT_EQ(drainedStats.timeoutClosedCount, 0u) << "客户端主动断开被记成了空闲超时收口";
    }

    /**
     * @brief 钉住：被空闲清扫协程按空闲超时关闭的连接计入 timeoutClosedCount，且不计入请求条数
     */
    TEST(HttpObservability, CountsConnectionsClosedByIdleTimeout)
    {
        // 清扫节拍 30ms、空闲容忍度 300ms：与限额用例同一套「短超时 + 短节拍」的构造
        HttpServerLimits limits = makeLongTimeoutLimits();
        limits.idleTimeout      = std::chrono::milliseconds{300};

        RunningHttpServerFixture fixture(limits, std::chrono::milliseconds{30});
        ASSERT_TRUE(fixture.awaitRunning(kWaitTimeout)) << "服务器未在时限内进入接受循环：上界 kWaitTimeout";

        const std::uint16_t listeningPort = fixture.listeningPort();
        ASSERT_NE(listeningPort, 0);

        LoopbackClient client(listeningPort);
        ASSERT_TRUE(client.isValid()) << "回环连接失败";

        // 先确认这条连接已被挂上连接管理器，再等它被清扫：否则下面的断言可能只是「连上就被关」
        ASSERT_TRUE(fixture.awaitConnectionAccepted(kWaitTimeout)) << "新连接未被挂上连接管理器";

        std::string receivedText;
        EXPECT_TRUE(client.waitForClosure(receivedText, kWaitTimeout)) << "空闲连接未被清扫协程收口";
        EXPECT_TRUE(receivedText.empty()) << "服务端在空闲连接上发了不该发的字节";

        EXPECT_TRUE(waitForCondition(
                [&fixture]
                {
                    return fixture.server().stats().timeoutClosedCount >= 1;
                },
                kWaitTimeout)) << "被超时收口的连接没有计入 timeoutClosedCount";

        const HttpServerStats stats = fixture.server().stats();
        // 断言下界而不是恰好相等：会话退出与「从连接管理器摘除」之间隔着一轮调度，
        // 理论上允许下一个清扫节拍再报一次同一条连接，写死 1 会给 CI 埋下偶发失败
        EXPECT_GE(stats.timeoutClosedCount, 1u) << "超时收口计数未累加";
        EXPECT_EQ(stats.totalRequestCount, 0u) << "一个字节都没发的连接不该计入已处理请求";
        EXPECT_EQ(stats.badRequestCount, 0u) << "空闲收口不该算成协议错误";
    }
} // namespace AsynGyanis::Net

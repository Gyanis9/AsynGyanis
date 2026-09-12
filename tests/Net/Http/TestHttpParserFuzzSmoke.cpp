// TestHttpParserFuzzSmoke.cpp —— 手写 HTTP 解析器的确定性模糊冒烟：
//   一. 截断矩阵：合法报文的**每一个前缀**都只能是 NeedMore 或 Error，绝不可能是 Done；
//   二. 随机字节流：随机长度、随机内容、随机切分方式喂入，解析器只能给出三种状态之一，
//       且永远不消费超过喂入的字节数；
//   三. 单字节变异：合法报文逐字节翻转后，要么解析成功且字段自洽，要么判错；
//   四. 粘滞错误：一旦 Error，再喂完整合法报文仍是 Error 且不再消费字节；
//   五. reset() 之后错误状态必须干净，能重新接受一条完整报文；
//   六. 资源上限：超长 URI / 超长头部块必须判错，而不是无限吃内存。
// 随机源是自带种子的 LCG（不用 std::random_device），因此失败可复现：日志里给出轮次与字节。
// 真正的 libFuzzer 目标需要 clang + Linux，本机工具链没有，故用这套属性化用例作为常驻防线。

#include "Net/Http/HttpParser.h"

#include "Net/Http/HttpMethod.h"
#include "Net/Http/HttpRequest.h"
#include "Net/Http/ParseStatus.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 合法请求样本：完整、带头部与正文，正文长度与 content-length 一致
        constexpr std::string_view kValidRequest =
                "POST /submit?x=1 HTTP/1.1\r\nHost: example.com\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";

        /// 随机流轮次：够覆盖各种切分与畸形组合，又不至于拖慢全量用例
        constexpr int kRandomRoundCount = 4000;

        /// 单轮随机流的最大长度
        constexpr std::size_t kMaximumRandomLength = 512;

        /// 自带种子的线性同余发生器：固定种子保证失败可复现
        class DeterministicRandom
        {
        public:
            explicit DeterministicRandom(const std::uint64_t seed) noexcept :
                m_state(seed)
            {
            }

            /// @brief 取下一个 32 位随机数
            [[nodiscard]] std::uint32_t next() noexcept
            {
                m_state = m_state * 6364136223846793005ULL + 1442695040888963407ULL;
                return static_cast<std::uint32_t>(m_state >> 32U);
            }

            /// @brief 取 [0, bound) 内的随机数；bound 为 0 时返回 0
            [[nodiscard]] std::size_t nextBelow(const std::size_t bound) noexcept
            {
                return bound == 0 ? 0U : static_cast<std::size_t>(next()) % bound;
            }

        private:
            std::uint64_t m_state; ///< 当前状态
        };

        /// @brief 把字节串渲染成带引号的可读文本，供失败信息用
        std::string toEscapedText(const std::string_view text)
        {
            std::string escaped;
            for (const char character: text)
            {
                if (character >= 0x20 && character < 0x7F)
                {
                    escaped.push_back(character);
                } else
                {
                    constexpr char kHexDigits[] = "0123456789abcdef";
                    escaped.append("\\x");
                    escaped.push_back(kHexDigits[(static_cast<unsigned char>(character) >> 4U) & 0x0FU]);
                    escaped.push_back(kHexDigits[static_cast<unsigned char>(character) & 0x0FU]);
                }
            }
            return escaped;
        }

        /// @brief 判断状态是否是三个合法取值之一（防止将来加枚举值时漏改）
        bool isKnownStatus(const ParseStatus status)
        {
            return status == ParseStatus::Error || status == ParseStatus::NeedMore || status == ParseStatus::Done;
        }
    } // namespace

    TEST(HttpParserFuzzSmoke, EveryTruncatedPrefixFailsToComplete)
    {
        // 截断只能让解析器「等更多」，永远不能宣布完成：宣布完成就意味着上层会把半条报文
        // 当成一条完整请求处理，这是请求走私与越界读的温床
        for (std::size_t prefixLength = 0; prefixLength < kValidRequest.size(); ++prefixLength)
        {
            HttpParser parser;
            const ParseStatus status = parser.parse(kValidRequest.data(), prefixLength);

            EXPECT_TRUE(isKnownStatus(status)) << "前缀长度 " << prefixLength << " 返回了未知状态";
            EXPECT_NE(status, ParseStatus::Done) << "前缀长度 " << prefixLength << " 被当成了完整报文";
            EXPECT_LE(parser.consumedByteCount(), prefixLength) << "前缀长度 " << prefixLength << " 消费了超出喂入的字节";
            ASSERT_FALSE(parser.isLimitExceeded()) << "前缀长度 " << prefixLength << " 误判为超限";
        }

        // 完整报文必须恰好一次判定为 Done，且消费全部字节
        HttpParser parser;
        ASSERT_EQ(parser.parse(kValidRequest.data(), kValidRequest.size()), ParseStatus::Done);
        EXPECT_EQ(parser.consumedByteCount(), kValidRequest.size());
        EXPECT_EQ(parser.request().method(), HttpMethod::POST);
        EXPECT_EQ(parser.request().path(), "/submit");
    }

    TEST(HttpParserFuzzSmoke, RandomByteStreamsKeepParserInvariants)
    {
        DeterministicRandom random{0x5EED1234U};

        for (int round = 0; round < kRandomRoundCount; ++round)
        {
            const std::size_t totalLength = random.nextBelow(kMaximumRandomLength + 1);

            std::string input;
            input.reserve(totalLength);
            for (std::size_t index = 0; index < totalLength; ++index)
            {
                // 偏向可打印字符：纯随机字节大多是畸形报文，命中不了「几乎合法」的边界路径
                input.push_back(static_cast<char>(random.nextBelow(4) == 0 ? random.nextBelow(256) : 0x20 + random.nextBelow(0x5F)));
            }

            HttpParser parser;
            std::size_t fedLength = 0;
            std::size_t consumedTotal = 0;

            // 随机切分喂入：模拟 TCP 把一条报文拆成任意片段
            while (fedLength < input.size())
            {
                const std::size_t chunkLength = 1 + random.nextBelow(input.size() - fedLength);
                const ParseStatus status = parser.parse(input.data() + fedLength, chunkLength);

                ASSERT_TRUE(isKnownStatus(status)) << "第 " << round << " 轮返回未知状态，输入：" << toEscapedText(input);
                EXPECT_LE(parser.consumedByteCount(), chunkLength)
                        << "第 " << round << " 轮消费超界，输入：" << toEscapedText(input);

                consumedTotal += parser.consumedByteCount();
                fedLength += chunkLength;

                if (status == ParseStatus::Error)
                {
                    // 已失败之后不再消费字节，也一直维持错误态
                    EXPECT_EQ(parser.parse(input.data() + fedLength - chunkLength, chunkLength), ParseStatus::Error);
                    EXPECT_EQ(parser.consumedByteCount(), 0U) << "错误态仍消费字节，输入：" << toEscapedText(input);
                    break;
                }
            }

            EXPECT_LE(consumedTotal, input.size()) << "第 " << round << " 轮消费总量超过喂入总量";
        }
    }

    TEST(HttpParserFuzzSmoke, SingleByteMutationsNeverYieldContradictoryRequest)
    {
        DeterministicRandom random{0xC0FFEEU};

        for (std::size_t position = 0; position < kValidRequest.size(); ++position)
        {
            // 每个位置试一个不同的字节，覆盖「破坏分隔符」「破坏头部名」「破坏长度」三类
            std::string mutated(kValidRequest);
            mutated[position] = static_cast<char>(random.nextBelow(256));

            HttpParser parser;
            const ParseStatus status = parser.parse(mutated.data(), mutated.size());

            ASSERT_TRUE(isKnownStatus(status)) << "位置 " << position << " 返回未知状态";

            if (status == ParseStatus::Done)
            {
                // 判成功就必须给出自洽的请求：路径非空且不含 CR/LF（含了就说明头部被吞进路径）
                const std::string_view path = parser.request().path();
                EXPECT_FALSE(path.empty()) << "位置 " << position << " 解析出的路径为空";
                EXPECT_EQ(path.find('\r'), std::string_view::npos) << "位置 " << position << " 路径里含 CR";
                EXPECT_EQ(path.find('\n'), std::string_view::npos) << "位置 " << position << " 路径里含 LF";
            } else if (status == ParseStatus::Error)
            {
                // 判失败必须给出明确类别；停在 NeedMore 不算失败，无需类别
                EXPECT_NE(parser.errorKind(), HttpParseErrorKind::None) << "位置 " << position << " 判错却没给类别";
            }
        }
    }

    TEST(HttpParserFuzzSmoke, ErrorStateIsStickyUntilReset)
    {
        HttpParser parser;
        ASSERT_EQ(parser.parse("GET / HTTP/1.1\r\nBad Header\r\n\r\n", 30), ParseStatus::Error);

        // 粘滞：再喂一条完整合法报文也不能「自愈」
        EXPECT_EQ(parser.parse(kValidRequest.data(), kValidRequest.size()), ParseStatus::Error);
        EXPECT_EQ(parser.consumedByteCount(), 0U);

        // reset() 之后恢复可解析
        parser.reset();
        EXPECT_EQ(parser.errorKind(), HttpParseErrorKind::None);
        EXPECT_EQ(parser.parse(kValidRequest.data(), kValidRequest.size()), ParseStatus::Done);
    }

    TEST(HttpParserFuzzSmoke, OversizedInputIsRejectedInsteadOfConsumed)
    {
        // 上限是 DoS 防线：超长 URI 与超长头部块都必须判错，而不是无限吃内存
        const std::string oversizedUri = "GET /" + std::string(64 * 1024, 'a') + " HTTP/1.1\r\n\r\n";
        HttpParser        parser;
        const ParseStatus uriStatus = parser.parse(oversizedUri.data(), oversizedUri.size());
        EXPECT_EQ(uriStatus, ParseStatus::Error);
        EXPECT_TRUE(parser.isLimitExceeded());

        // 头部：一条语法上完全合法、但值长度远超上限的头部。用「合法的超长」而不是「畸形的超长」，
        // 是为了让失败原因只可能来自长度上限本身（畸形输入会先被语法判定拦下）
        parser.reset();
        const std::string oversizedHeaders = "GET / HTTP/1.1\r\nX-Big: " + std::string(256 * 1024, 'a') + "\r\n\r\n";
        EXPECT_EQ(parser.parse(oversizedHeaders.data(), oversizedHeaders.size()), ParseStatus::Error);
        EXPECT_TRUE(parser.isLimitExceeded());
        EXPECT_EQ(parser.errorKind(), HttpParseErrorKind::HeaderTooLarge);
    }
} // namespace AsynGyanis::Net

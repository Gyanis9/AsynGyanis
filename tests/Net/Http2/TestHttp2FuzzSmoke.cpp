// TestHttp2FuzzSmoke.cpp —— HTTP/2 协议栈（帧解码器 / HPACK / 连接层状态机）的确定性模糊冒烟：
//   一. 截断矩阵：每一条合法帧的**每一个前缀**都只能是 NeedMore 或 Error；若给出 Frame，则该前缀必须
//       正好自成一帧（消费字节数等于前缀长度），绝不允许「收了半个负载也算一帧」；
//   二. 随机字节流：随机长度、随机内容、随机切分喂入帧解码器，状态只能三选一；给出 Frame 时必须消费了
//       至少 1 字节且不超过本次喂入的字节数；一旦 Error 就粘滞（再喂不消费字节），reset() 后才能重来；
//   三. 连接层不变式：把合法帧做随机单字节变异后喂进已完成握手的连接，**它吐出的每一个字节都必须仍能
//       被一份全新解码器解成完整帧**——本端绝不产出畸形帧或半条帧，这一条是接收侧任何校验都替代不了的；
//   四. HPACK：随机头部列表用同一个编码器/解码器连续往返，必须逐字段相等（覆盖索引与字面量两条路径）；
//       随机字节块解码只能成功或判错，且判错后 hasError() 必须为真。
// 随机源是自带种子的 LCG（不用 std::random_device），因此失败可复现：断言消息里带上轮次，必要时可复算。
// 真正的 libFuzzer 目标需要 clang + Linux，本机工具链没有，故用这套属性化用例作为常驻防线。

#include "Net/Http2/Http2Connection.h"
#include "Net/Http2/Http2Frame.h"
#include "Net/Http2/Hpack.h"

#include <gtest/gtest.h>

#include <algorithm>
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
        /// 随机流轮次：够覆盖各种切分与畸形组合，又不至于拖慢全量用例
        constexpr int kRandomStreamRoundCount = 1500;

        /// 单轮随机流的最大长度
        constexpr std::size_t kMaximumRandomLength = 512;

        /// 连接层变异轮次（每轮喂一帧，轮数按「帧数 × 变异」覆盖）
        constexpr int kConnectionMutationRoundCount = 600;

        /// HPACK 往返轮次与随机块轮次
        constexpr int kHpackRoundTripRoundCount = 400;
        constexpr int kHpackRandomBlockRoundCount = 400;

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

            /// @brief 取一个随机可见字节（不追求所有取值都出现，只求内容随机）
            [[nodiscard]] char nextByte() noexcept
            {
                return static_cast<char>(next() & 0xFFU);
            }

        private:
            std::uint64_t m_state; ///< 发生器状态
        };

        /**
         * @brief 每种帧一条合法样本，用于截断矩阵与连接层变异
         * @return std::vector<std::string> 完整帧字节；负载都取非空值
         */
        std::vector<std::string> makeValidFrameCorpus()
        {
            std::vector<std::string> corpus;
            corpus.push_back(encodeHttp2SettingsFrame(Http2SettingsPayload{
                    .parameters = {{static_cast<std::uint16_t>(Http2SettingIdentifier::MaxFrameSize), 16384U}}}));
            corpus.push_back(encodeHttp2PingFrame(Http2PingPayload{}));
            corpus.push_back(encodeHttp2RstStreamFrame(Http2RstStreamPayload{.errorCode = Http2ErrorCode::Cancel}, 1U));
            corpus.push_back(encodeHttp2WindowUpdateFrame(Http2WindowUpdatePayload{.windowSizeIncrement = 1024U}, 1U));
            corpus.push_back(encodeHttp2GoAwayFrame(Http2GoAwayPayload{.lastStreamId = 1U, .debugData = "bye"}));
            corpus.push_back(encodeHttp2HeadersFrame(
                    Http2HeadersPayload{.endStream = true, .endHeaders = true, .headerBlockFragment = "\x82\x86\x84"}, 1U));
            corpus.push_back(encodeHttp2DataFrame(Http2DataPayload{.endStream = false, .data = "payload"}, 1U));
            corpus.push_back(encodeHttp2ContinuationFrame(
                    Http2ContinuationPayload{.endHeaders = true, .headerBlockFragment = "\x82\x86\x84"}, 1U));
            return corpus;
        }

        /**
         * @brief 断言连接层吐出的字节能恰好由整数条合法帧构成
         * @param connection 被测连接
         * @param roundIndex 轮次，失败时打进消息便于复算
         * @details 这是「本端编码器绝不产出畸形帧」的守门断言：接收侧的一切校验都替代不了它——
         *          对端若因为我们的半条帧而挂死，问题在本端。
         */
        void expectOutgoingBytesAreWellFormed(Http2Connection &connection, const int roundIndex)
        {
            const std::string outgoingBytes = connection.takeOutgoingBytes();
            if (outgoingBytes.empty())
            {
                return;
            }

            Http2FrameDecoder decoder;
            std::size_t consumedByteCount = 0;
            while (consumedByteCount < outgoingBytes.size())
            {
                const Http2FrameDecodeStatus status =
                        decoder.parse(outgoingBytes.data() + consumedByteCount, outgoingBytes.size() - consumedByteCount);
                ASSERT_NE(status, Http2FrameDecodeStatus::Error)
                        << "第 " << roundIndex << " 轮：本端产出的字节里有非法帧——" << decoder.errorMessage();
                if (status == Http2FrameDecodeStatus::NeedMore)
                {
                    break;
                }
                const std::size_t frameByteCount = decoder.consumedByteCount();
                ASSERT_GT(frameByteCount, 0U) << "第 " << roundIndex << " 轮：产出帧却不消费字节";
                consumedByteCount += frameByteCount;
                static_cast<void>(decoder.takeFrame());
            }
            EXPECT_EQ(consumedByteCount, outgoingBytes.size())
                    << "第 " << roundIndex << " 轮：本端产出的字节不是整数条帧（有半条帧留在里面）";
        }
    } // namespace

    /**
     * @brief 钉住：合法帧的每一个前缀都不会被当成完整帧接受（除非该前缀本身就自成一帧）
     */
    TEST(Http2FuzzSmoke, TruncatedFramesAreNeverAcceptedAsComplete)
    {
        for (const std::string &frameBytes: makeValidFrameCorpus())
        {
            ASSERT_GT(frameBytes.size(), 9U) << "语料里的帧都必须带非空负载，截断矩阵才有意义";
            for (std::size_t prefixLength = 0; prefixLength < frameBytes.size(); ++prefixLength)
            {
                Http2FrameDecoder decoder;
                const Http2FrameDecodeStatus status = decoder.parse(frameBytes.data(), prefixLength);
                if (status == Http2FrameDecodeStatus::Frame)
                {
                    // 前缀自成一帧是允许的（例如变长负载帧的短负载），但必须正好吃掉整个前缀
                    EXPECT_EQ(decoder.consumedByteCount(), prefixLength)
                            << "前缀长度 " << prefixLength << " 被当成帧，却只消费了 " << decoder.consumedByteCount() << " 字节";
                    continue;
                }
                EXPECT_NE(status, Http2FrameDecodeStatus::Frame);
            }
        }
    }

    /**
     * @brief 钉住：随机字节流喂入帧解码器时状态与消费量始终自洽，且错误是粘滞的
     */
    TEST(Http2FuzzSmoke, RandomByteStreamsKeepFrameDecoderInvariants)
    {
        DeterministicRandom random(0x5eed2026U);

        for (int roundIndex = 0; roundIndex < kRandomStreamRoundCount; ++roundIndex)
        {
            std::string streamBytes;
            const std::size_t streamLength = random.nextBelow(kMaximumRandomLength);
            streamBytes.reserve(streamLength);
            for (std::size_t index = 0; index < streamLength; ++index)
            {
                streamBytes.push_back(random.nextByte());
            }

            // 随机切分：每次喂 1..17 字节，模拟 TCP 分段
            Http2FrameDecoder decoder;
            std::size_t offset = 0;
            bool hasFailed = false;
            while (offset < streamBytes.size())
            {
                const std::size_t chunkLength = std::min<std::size_t>(1U + random.nextBelow(17U), streamBytes.size() - offset);
                const Http2FrameDecodeStatus status = decoder.parse(streamBytes.data() + offset, chunkLength);
                if (status == Http2FrameDecodeStatus::Frame)
                {
                    const std::size_t frameByteCount = decoder.consumedByteCount();
                    ASSERT_GT(frameByteCount, 0U) << "第 " << roundIndex << " 轮：产出帧却不消费字节";
                    ASSERT_LE(frameByteCount, chunkLength) << "第 " << roundIndex << " 轮：消费字节数超过本次喂入长度";
                    static_cast<void>(decoder.takeFrame());
                    offset += frameByteCount;
                    continue;
                }
                if (status == Http2FrameDecodeStatus::Error)
                {
                    // 粘滞：再喂一段随机字节仍然报错，且一个字节都不消费
                    EXPECT_EQ(decoder.parse(streamBytes.data() + offset, chunkLength), Http2FrameDecodeStatus::Error);
                    EXPECT_EQ(decoder.consumedByteCount(), 0U) << "错误态不得消费字节";
                    decoder.reset();
                    EXPECT_FALSE(decoder.hasError()) << "reset() 之后错误状态必须干净";
                    hasFailed = true;
                    break;
                }
                offset += chunkLength;
            }
            static_cast<void>(hasFailed);
        }
    }

    /**
     * @brief 钉住：喂入变异帧后，连接层吐出的字节永远是整数条合法帧
     */
    TEST(Http2FuzzSmoke, ConnectionEmitsOnlyWellFormedFrames)
    {
        const std::vector<std::string> corpus = makeValidFrameCorpus();
        DeterministicRandom random(0xc0ffeeU);

        Http2Connection connection;
        ASSERT_EQ(connection.feedBytes(kHttp2ConnectionPreface.data(), kHttp2ConnectionPreface.size()), Http2ConnectionFeedStatus::NeedMore);
        const std::string clientSettings = encodeHttp2SettingsFrame(Http2SettingsPayload{});
        ASSERT_EQ(connection.feedBytes(clientSettings.data(), clientSettings.size()), Http2ConnectionFeedStatus::NeedMore);
        expectOutgoingBytesAreWellFormed(connection, -1);

        for (int roundIndex = 0; roundIndex < kConnectionMutationRoundCount; ++roundIndex)
        {
            std::string mutatedFrame = corpus[random.nextBelow(corpus.size())];
            const std::size_t mutationCount = 1U + random.nextBelow(4U);
            for (std::size_t mutation = 0; mutation < mutationCount; ++mutation)
            {
                const std::size_t position = random.nextBelow(mutatedFrame.size());
                mutatedFrame[position] = static_cast<char>(mutatedFrame[position] ^ (1 << random.nextBelow(8U)));
            }

            // 随机切分成 1..3 段喂入：连接层必须只回 NeedMore 或 Failed
            std::size_t offset = 0;
            while (offset < mutatedFrame.size())
            {
                const std::size_t chunkLength = std::min<std::size_t>(1U + random.nextBelow(mutatedFrame.size()), mutatedFrame.size() - offset);
                const Http2ConnectionFeedStatus feedStatus = connection.feedBytes(mutatedFrame.data() + offset, chunkLength);
                offset += chunkLength;
                expectOutgoingBytesAreWellFormed(connection, roundIndex);
                if (feedStatus == Http2ConnectionFeedStatus::Failed)
                {
                    break;
                }
            }

            if (connection.hasFailed())
            {
                // 失败态是粘滞的：此后不再解释任何字节，也不会再产出
                EXPECT_EQ(connection.feedBytes(mutatedFrame.data(), mutatedFrame.size()), Http2ConnectionFeedStatus::Failed);
                expectOutgoingBytesAreWellFormed(connection, roundIndex);
                break;
            }
        }
    }

    /**
     * @brief 钉住：随机头部列表编码后再解码必须逐字段相等（同一对编/解码器连续往返，覆盖动态表索引）
     */
    TEST(Http2FuzzSmoke, HpackRoundTripsRandomHeaderLists)
    {
        DeterministicRandom random(0xabcdefU);
        HpackEncoder encoder;
        HpackDecoder decoder;

        for (int roundIndex = 0; roundIndex < kHpackRoundTripRoundCount; ++roundIndex)
        {
            std::vector<HpackHeaderField> headerFields;
            const std::size_t fieldCount = 1U + random.nextBelow(8U);
            for (std::size_t index = 0; index < fieldCount; ++index)
            {
                // 名字在固定池里取（覆盖静态表命中与全新名字），值取随机字母数字串
                static constexpr std::string_view kNamePool[] = {"content-type", "date", "x-round", "user-agent", "x-value"};
                HpackHeaderField field;
                field.name = std::string(kNamePool[random.nextBelow(std::size(kNamePool))]);
                const std::size_t valueLength = random.nextBelow(24U);
                field.value.reserve(valueLength);
                for (std::size_t valueIndex = 0; valueIndex < valueLength; ++valueIndex)
                {
                    static constexpr std::string_view kAlphabet = "abcdefghijklmnopqrstuvwxyz0123456789-_";
                    field.value.push_back(kAlphabet[random.nextBelow(kAlphabet.size())]);
                }
                headerFields.push_back(std::move(field));
            }

            const std::string headerBlock = encoder.encode(headerFields);
            std::vector<HpackHeaderField> decodedFields;
            std::string errorText;
            ASSERT_TRUE(decoder.decode(headerBlock, decodedFields, &errorText))
                    << "第 " << roundIndex << " 轮：本端编码出的头块解不开——" << errorText;
            ASSERT_EQ(decodedFields.size(), headerFields.size()) << "第 " << roundIndex << " 轮：往返后字段数不一致";
            for (std::size_t index = 0; index < headerFields.size(); ++index)
            {
                EXPECT_EQ(decodedFields[index].name, headerFields[index].name) << "第 " << roundIndex << " 轮：第 " << index << " 个字段名不一致";
                EXPECT_EQ(decodedFields[index].value, headerFields[index].value) << "第 " << roundIndex << " 轮：第 " << index << " 个字段值不一致";
            }
        }
    }

    /**
     * @brief 钉住：随机字节块喂入 HPACK 解码器只能成功或判错，判错后 hasError() 状态自洽
     */
    TEST(Http2FuzzSmoke, HpackDecoderSurvivesRandomBlocks)
    {
        DeterministicRandom random(0x13579bdfU);

        for (int roundIndex = 0; roundIndex < kHpackRandomBlockRoundCount; ++roundIndex)
        {
            std::string headerBlock;
            const std::size_t blockLength = random.nextBelow(128U);
            headerBlock.reserve(blockLength);
            for (std::size_t index = 0; index < blockLength; ++index)
            {
                headerBlock.push_back(random.nextByte());
            }

            // 每轮用全新解码器：失败是粘滞的，复用会把上一轮的失败带进来
            HpackDecoder decoder;
            std::vector<HpackHeaderField> decodedFields;
            std::string errorText;
            const bool isDecoded = decoder.decode(headerBlock, decodedFields, &errorText);
            if (!isDecoded)
            {
                EXPECT_TRUE(decoder.hasError()) << "第 " << roundIndex << " 轮：解码失败却没有落错误标记";
                EXPECT_FALSE(errorText.empty()) << "第 " << roundIndex << " 轮：失败必须给出可排查的中文原因";
                EXPECT_TRUE(decodedFields.empty()) << "第 " << roundIndex << " 轮：失败时不得交出任何字段";
            }
        }
    }
} // namespace AsynGyanis::Net

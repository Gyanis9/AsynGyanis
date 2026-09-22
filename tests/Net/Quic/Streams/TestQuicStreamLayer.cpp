// TestQuicStreamLayer.cpp —— QUIC 流层（RFC 9000 §2–§4.6、§19.4–§19.11）用例
//
// 流层是纯计算件，所以不起网络也不依赖外部服务：入站帧手工造，出站帧编完直接解码回来比对。
// 本端一律按服务端视角（流号低位 0x01/0x03 是本端发起的）。覆盖：
//   1) 三类流的窗口按「谁发起」取档，两端各自的视角不能写反（§18.2）；
//   2) 流级与连接级额度越界各判 FLOW_CONTROL_ERROR，重复偏移不重复记账（§4.1、§4.5）；
//   3) 收尾长度不一致、越过收尾长度各判 FINAL_SIZE_ERROR（§4.5）；
//   4) 乱序缓存、重叠裁剪、纯 FIN 也要通知（§7.5）；
//   5) 流数超限判 STREAM_LIMIT_ERROR，用掉一半自动续上限（§4.6）；
//   6) 单向方向性错误的三处 STREAM_STATE_ERROR 与 PROTOCOL_VIOLATION（§2.1、§4.5、§4.6）；
//   7) 发送排队：按额度与包预算分片、判丢按原偏移重排、确认销账、STOP_SENDING 作废待发（§4.5）；
//   8) 窗口续期按「消费掉一半」批量抬，且任何预算下都不超编（§2.2）；
//   9) 本端收口一条流：RESET_STREAM / STOP_SENDING 各编得出、在途不重发、判丢补发同一份、确认即落定（§3.5、§13.3）。

#include "Net/Quic/Streams/QuicStreamLayer.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 造一份传输参数，只填流层关心的那六项
         * @param maximumData 连接级额度
         * @param bidiLocal 本端发起的双向流的流级额度
         * @param bidiRemote 对端发起的双向流的流级额度
         * @param uni 对端发起的单向流的流级额度
         * @param maximumBidirectionalStreams 允许对端发起的双向流数
         * @param maximumUnidirectionalStreams 允许对端发起的单向流数
         * @return QuicTransportParameters 填好的参数
         */
        QuicTransportParameters makeParameters(const std::uint64_t maximumData, const std::uint64_t bidiLocal,
                                               const std::uint64_t bidiRemote, const std::uint64_t uni,
                                               const std::uint64_t maximumBidirectionalStreams,
                                               const std::uint64_t maximumUnidirectionalStreams)
        {
            QuicTransportParameters parameters;
            parameters.initialMaximumData = maximumData;
            parameters.initialMaximumStreamDataBidirectionalLocal = bidiLocal;
            parameters.initialMaximumStreamDataBidirectionalRemote = bidiRemote;
            parameters.initialMaximumStreamDataUnidirectional = uni;
            parameters.initialMaximumBidirectionalStreams = maximumBidirectionalStreams;
            parameters.initialMaximumUnidirectionalStreams = maximumUnidirectionalStreams;
            return parameters;
        }

        /// 本端（服务端）参数：连接级 4096，各类流级 1024，双向 4 条、单向 2 条
        QuicTransportParameters makeLocalParameters()
        {
            return makeParameters(4096, 1024, 1024, 1024, 4, 2);
        }

        /// 对端（客户端）参数：连接级 8192，流级 2048，双向 4 条、单向 2 条
        QuicTransportParameters makePeerParameters()
        {
            return makeParameters(8192, 2048, 2048, 2048, 4, 2);
        }

        /// 建好并把对端参数用上：多数发送侧用例省掉一遍 adopt
        QuicStreamLayer makeEstablishedLayer()
        {
            QuicStreamLayer layer(makeLocalParameters());
            layer.adoptPeerParameters(makePeerParameters());
            return layer;
        }

        /// @return 文本对应的字节 vector，造帧与比数据用
        std::vector<std::uint8_t> bytesOf(const std::string_view text)
        {
            return std::vector<std::uint8_t>(text.begin(), text.end());
        }

        /**
         * @brief 造一条入站 STREAM 帧
         * @param streamId 流号
         * @param offset 偏移
         * @param data 数据本体，调用期间必须活着（帧里是视图）
         * @param isFinal 是否带 FIN
         * @return QuicStreamFrame 交进流层
         */
        QuicStreamFrame makeStreamFrame(const std::uint64_t streamId, const std::uint64_t offset,
                                        const std::vector<std::uint8_t> &data, const bool isFinal = false)
        {
            QuicStreamFrame frame;
            frame.streamId = streamId;
            frame.offset = offset;
            frame.data = std::span<const std::uint8_t>(data);
            frame.isFinal = isFinal;
            return frame;
        }

        /// 把编出来的帧字节解回帧序列；解不开即失败，返回空序列
        std::vector<QuicFrame> decodeFrames(const std::string &encoded)
        {
            const std::span<const std::uint8_t> payload(reinterpret_cast<const std::uint8_t *>(encoded.data()), encoded.size());
            const std::expected<std::vector<QuicFrame>, QuicDecodeError> decoded = decodeQuicFrames(payload);
            EXPECT_TRUE(decoded.has_value()) << "编出来的帧解不回去";
            return decoded.has_value() ? *decoded : std::vector<QuicFrame>{};
        }

        /**
         * @brief 从编好的帧里挑出指定类型的若干条
         * @tparam FrameType 目标帧类型
         * @param encoded collectFrames 的产物
         * @return std::vector<FrameType> 按出现顺序
         */
        template <typename FrameType>
        std::vector<FrameType> framesOfType(const std::string &encoded)
        {
            std::vector<FrameType> picked;
            for (const QuicFrame &frame : decodeFrames(encoded))
            {
                if (std::get_if<FrameType>(&frame) != nullptr)
                {
                    picked.push_back(std::get<FrameType>(frame));
                }
            }
            return picked;
        }

        /// @return 一条 STREAM 帧的载荷文本
        std::string payloadTextOf(const QuicStreamFrame &frame)
        {
            return std::string(frame.data.begin(), frame.data.end());
        }

        /**
         * @brief 取空交付队列，只把这条流的字节拼起来返回
         * @param layer 流层
         * @param streamId 要收的流号
         * @return std::string 拼接结果
         */
        std::string drainDeliveries(QuicStreamLayer &layer, const std::uint64_t streamId)
        {
            std::string text;
            while (std::optional<QuicStreamDelivery> delivery = layer.takeDelivery())
            {
                if (delivery->streamId == streamId)
                {
                    text.append(delivery->bytes.begin(), delivery->bytes.end());
                }
            }
            return text;
        }

        /// 一次窗口更新的收集结果，省掉每个用例两行样板
        struct Collected
        {
            std::string frames{};                                    ///< 编出来的帧字节
            std::vector<QuicStreamRange> ranges{};                   ///< 排进数据帧的字节区间
            std::vector<QuicStreamAnnouncement> announcements{};      ///< 排进本包的收口宣告
            bool hasFrames{false};                                   ///< collectFrames 的返回值
        };

        /// 按给定预算收一轮帧
        Collected collect(QuicStreamLayer &layer, const std::size_t byteBudget)
        {
            Collected collected;
            collected.hasFrames = layer.collectFrames(collected.frames, byteBudget, collected.ranges,
                                                      collected.announcements);
            return collected;
        }

        /**
         * @brief 违规用例专用：取传输错误码
         * @details 先确认这一帧真被判成违规再取值——直接 `.error()` 在退化成了「接受」时会踩空，
         *          表现为崩溃而不是断言失败
         * @param result 入站帧的处理结果
         * @return std::uint64_t §11.1 的错误码；本该失败却成功时返回 0
         */
        std::uint64_t violationCodeOf(const std::expected<void, QuicStreamViolation> &result)
        {
            EXPECT_FALSE(result.has_value()) << "本应判违规的一帧被当成了合法";
            return result.has_value() ? 0U : result.error().errorCode;
        }

        /// @return 编好的字节里第一条指定类型帧；一条都没有即失败
        template <typename FrameType>
        FrameType firstFrameOf(const std::string &encoded)
        {
            const std::vector<FrameType> picked = framesOfType<FrameType>(encoded);
            EXPECT_FALSE(picked.empty());
            return picked.empty() ? FrameType{} : picked.front();
        }
    } // namespace

    /**
     * @brief 三类流的接收窗口各按对端参数的档位取（§18.2）
     * @details 100/200/300 三个不同的值就是为了能被区分开：写反任何一档，断言立刻偏
     */
    TEST(QuicStreamLayer, AppliesAdvertisedWindowByStreamClass)
    {
        QuicStreamLayer layer(makeParameters(100000, 100, 200, 300, 4, 4));
        // 对端参数只用来让本端能先开出 0x01 那条流；接收侧的三档额度全在本地参数里
        layer.adoptPeerParameters(makeParameters(100000, 1000, 1000, 1000, 4, 4));
        const auto tail = bytesOf("x");

        // 0x00 与 0x04 都是对端发起的双向流：本端的 0x06 = 200
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 199, tail)).has_value());
        EXPECT_EQ(violationCodeOf(layer.onStreamFrame(makeStreamFrame(0x00, 200, tail))), 0x03U);
        // 0x02 是对端发起的单向流：本端的 0x07 = 300
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x02, 299, tail)).has_value());
        EXPECT_EQ(violationCodeOf(layer.onStreamFrame(makeStreamFrame(0x02, 300, tail))), 0x03U);
        // 0x01 是本端发起的双向流，对端回数据：本端的 0x05 = 100
        EXPECT_EQ(layer.writeStreamData(0x01, bytesOf("r"), false), 1U);
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x01, 99, tail)).has_value());
        EXPECT_EQ(violationCodeOf(layer.onStreamFrame(makeStreamFrame(0x01, 100, tail))), 0x03U);
    }

    /**
     * @brief 本端发起的单向流（0x03）上对端没有发言权：PROTOCOL_VIOLATION
     */
    TEST(QuicStreamLayer, RejectsDataOnLocallyInitiatedUnidirectionalStream)
    {
        QuicStreamLayer layer(makeLocalParameters());
        const std::expected<void, QuicStreamViolation> accepted = layer.onStreamFrame(makeStreamFrame(0x03, 0, bytesOf("hi")));
        ASSERT_FALSE(accepted.has_value());
        EXPECT_EQ(accepted.error().errorCode, 0x0aU);
    }

    /**
     * @brief 本端没发起过的双向流不能凭空回数据：PROTOCOL_VIOLATION
     */
    TEST(QuicStreamLayer, RejectsDataOnUnknownLocallyInitiatedBidirectionalStream)
    {
        QuicStreamLayer layer(makeLocalParameters());
        const std::expected<void, QuicStreamViolation> accepted = layer.onStreamFrame(makeStreamFrame(0x01, 0, bytesOf("hi")));
        ASSERT_FALSE(accepted.has_value());
        EXPECT_EQ(accepted.error().errorCode, 0x0aU);
    }

    /**
     * @brief 本端不打算收任何流时，第一条就是 STREAM_LIMIT_ERROR（§4.6）
     */
    TEST(QuicStreamLayer, RejectsStreamsBeyondAdvertisedCount)
    {
        QuicStreamLayer layer(makeParameters(4096, 1024, 1024, 1024, 0, 0));
        EXPECT_EQ(violationCodeOf(layer.onStreamFrame(makeStreamFrame(0x00, 0, bytesOf("hi")))), 0x04U);
        EXPECT_EQ(violationCodeOf(layer.onStreamFrame(makeStreamFrame(0x02, 0, bytesOf("hi")))), 0x04U);
    }

    /**
     * @brief 按序交付，FIN 落在最后一段上，且不走「被打断」那条通道
     */
    TEST(QuicStreamLayer, DeliversInOrderBytesAndMarksFinal)
    {
        QuicStreamLayer layer(makeLocalParameters());
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 0, bytesOf("abc"))).has_value());
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 3, bytesOf("de"), true)).has_value());

        ASSERT_TRUE(layer.hasDeliveries());
        const std::optional<QuicStreamDelivery> first = layer.takeDelivery();
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(std::string(first->bytes.begin(), first->bytes.end()), "abc");
        EXPECT_FALSE(first->isFinal);

        const std::optional<QuicStreamDelivery> second = layer.takeDelivery();
        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(std::string(second->bytes.begin(), second->bytes.end()), "de");
        EXPECT_TRUE(second->isFinal);
        EXPECT_FALSE(layer.hasDeliveries());
        EXPECT_FALSE(layer.hasAbortedStreams());
    }

    /**
     * @brief 零长收尾也要通知一次，且同一个 FIN 重发不重复通知
     */
    TEST(QuicStreamLayer, DeliversEmptyFinalSegmentForZeroLengthStream)
    {
        QuicStreamLayer layer(makeLocalParameters());
        const std::vector<std::uint8_t> nothing;
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 0, nothing, true)).has_value());
        ASSERT_TRUE(layer.hasDeliveries());
        const std::optional<QuicStreamDelivery> delivery = layer.takeDelivery();
        ASSERT_TRUE(delivery.has_value());
        EXPECT_TRUE(delivery->bytes.empty());
        EXPECT_TRUE(delivery->isFinal);

        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 0, nothing, true)).has_value());
        EXPECT_FALSE(layer.hasDeliveries());
    }

    /**
     * @brief 早到的乱序段先缓存，缺口补上之后连着一起交付
     */
    TEST(QuicStreamLayer, ReassemblesOutOfOrderFragments)
    {
        QuicStreamLayer layer(makeLocalParameters());
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 5, bytesOf("56789"), true)).has_value());
        EXPECT_FALSE(layer.hasDeliveries());
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 2, bytesOf("34"))).has_value());
        EXPECT_FALSE(layer.hasDeliveries());

        // 0..2 接上了，但 [4,5) 还空着，只能先交出一段
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 0, bytesOf("ab"))).has_value());
        const std::optional<QuicStreamDelivery> partial = layer.takeDelivery();
        ASSERT_TRUE(partial.has_value());
        EXPECT_EQ(std::string(partial->bytes.begin(), partial->bytes.end()), "ab34");
        EXPECT_FALSE(partial->isFinal);

        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 4, bytesOf("4"))).has_value());
        const std::optional<QuicStreamDelivery> rest = layer.takeDelivery();
        ASSERT_TRUE(rest.has_value());
        EXPECT_EQ(std::string(rest->bytes.begin(), rest->bytes.end()), "456789");
        EXPECT_TRUE(rest->isFinal);
    }

    /**
     * @brief 重发换了分片大小时，起点落在已缓存段中间的那一段也得并进来（§7.5）
     * @details 只按起始偏移建索引的话，排干 [0,20) 之后再去找键 20 就找不到第二段（它的键是 15），
     *          尾巴那五个字节会永远留在缓存里
     */
    TEST(QuicStreamLayer, DeliversBytesWhenRetransmissionStraddlesCachedRange)
    {
        QuicStreamLayer layer(makeLocalParameters());
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 10, bytesOf("abcdefghij"))).has_value());
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 15, bytesOf("KLMNOPQRST"))).has_value());
        EXPECT_FALSE(layer.hasDeliveries());

        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 0, bytesOf("0123456789"))).has_value());
        EXPECT_EQ(drainDeliveries(layer, 0x00), "0123456789abcdeKLMNOPQRST");

        const std::vector<std::uint8_t> nothing;
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 25, nothing, true)).has_value());
        const std::optional<QuicStreamDelivery> finalDelivery = layer.takeDelivery();
        ASSERT_TRUE(finalDelivery.has_value());
        EXPECT_TRUE(finalDelivery->isFinal);
    }

    /**
     * @brief 与已交付部分重叠的重传：整段旧的丢弃，只补没见过的尾巴（§7.5）
     */
    TEST(QuicStreamLayer, TrimsOverlapWithDeliveredBytes)
    {
        QuicStreamLayer layer(makeLocalParameters());
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 0, bytesOf("01234"))).has_value());
        EXPECT_EQ(drainDeliveries(layer, 0x00), "01234");

        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 1, bytesOf("123"))).has_value());
        EXPECT_FALSE(layer.hasDeliveries());

        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 3, bytesOf("34567"))).has_value());
        EXPECT_EQ(drainDeliveries(layer, 0x00), "567");
    }

    /**
     * @brief 越过流级上限是 FLOW_CONTROL_ERROR（§4.5）
     */
    TEST(QuicStreamLayer, RejectsStreamOffsetBeyondStreamLimit)
    {
        QuicStreamLayer layer(makeLocalParameters());
        const std::expected<void, QuicStreamViolation> accepted = layer.onStreamFrame(makeStreamFrame(0x00, 1020, bytesOf("0123456789")));
        ASSERT_FALSE(accepted.has_value());
        EXPECT_EQ(accepted.error().errorCode, 0x03U);
    }

    /**
     * @brief 各流偏移之和越过连接级上限同样是 FLOW_CONTROL_ERROR（§4.1）
     */
    TEST(QuicStreamLayer, RejectsConnectionLevelLimitOverflow)
    {
        QuicStreamLayer layer(makeParameters(100, 1000, 1000, 1000, 4, 4));
        // 单条流的额度够，连接级只有 100：第二条流一进来就越界
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 0, std::vector<std::uint8_t>(80, 'a'))).has_value());
        const std::expected<void, QuicStreamViolation> accepted =
                layer.onStreamFrame(makeStreamFrame(0x04, 0, std::vector<std::uint8_t>(30, 'b')));
        ASSERT_FALSE(accepted.has_value());
        EXPECT_EQ(accepted.error().errorCode, 0x03U);
    }

    /**
     * @brief 重叠的重传只按新增的那一段占用连接级额度：每条流只算最大结束偏移（§4.1）
     * @details 连接级只剩 150：正确的记账是 60 + 30 + 40 = 130，按帧长重复计一次则是 160，
     *          第三条流就会被误判成越界
     */
    TEST(QuicStreamLayer, CountsEachStreamOnceAtItsHighWaterOffset)
    {
        QuicStreamLayer layer(makeParameters(150, 1000, 1000, 1000, 4, 4));
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 0, std::vector<std::uint8_t>(60, 'a'))).has_value());
        // 与上一段重叠 30 字节的补发：越过 60 的那 30 字节才算新账
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 30, std::vector<std::uint8_t>(60, 'a'))).has_value());
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x04, 0, std::vector<std::uint8_t>(40, 'b'))).has_value());
    }

    /**
     * @brief 收尾之后再越过收尾长度的数据是 FINAL_SIZE_ERROR（§4.5）
     */
    TEST(QuicStreamLayer, RejectsDataBeyondFinalSize)
    {
        QuicStreamLayer layer(makeLocalParameters());
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 0, bytesOf("abc"), true)).has_value());
        EXPECT_EQ(violationCodeOf(layer.onStreamFrame(makeStreamFrame(0x00, 1, bytesOf("xxxx")))), 0x06U);
    }

    /**
     * @brief 两次 FIN 报的长度不一致是 FINAL_SIZE_ERROR
     */
    TEST(QuicStreamLayer, RejectsInconsistentFinalSize)
    {
        QuicStreamLayer layer(makeLocalParameters());
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 0, bytesOf("abc"), true)).has_value());
        EXPECT_EQ(violationCodeOf(layer.onStreamFrame(makeStreamFrame(0x00, 0, bytesOf("abcdefg"), true))), 0x06U);
    }

    /**
     * @brief MAX_DATA 与 MAX_STREAM_DATA 只抬不回落，更小的值没有效果也不算违规（§4.1）
     */
    TEST(QuicStreamLayer, IgnoresSmallerFlowControlUpdates)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        EXPECT_EQ(layer.writeStreamData(0x03, bytesOf("0123456789"), false), 10U);

        QuicMaxDataFrame shrinkData;
        shrinkData.maximumData = 4;
        EXPECT_TRUE(layer.onMaxDataFrame(shrinkData).has_value());
        QuicMaxStreamDataFrame shrinkStream;
        shrinkStream.streamId = 0x03;
        shrinkStream.maximumStreamData = 4;
        EXPECT_TRUE(layer.onMaxStreamDataFrame(shrinkStream).has_value());

        const Collected collected = collect(layer, 1200);
        EXPECT_TRUE(collected.hasFrames);
        EXPECT_EQ(payloadTextOf(firstFrameOf<QuicStreamFrame>(collected.frames)), "0123456789");
    }

    /**
     * @brief 发送额度按「谁发起这条流」取对端参数的档位（§18.2 的 local/remote 是对端视角）
     * @details 这是最容易写反的一处：本端发起的 0x01 在对端那边算 remote，因此取对端的 0x06；
     *          对端发起的 0x00 取对端的 0x05。三档给三个不同的值，反了就会当场对不上。
     */
    TEST(QuicStreamLayer, TakesSendWindowFromPeerViewOfInitiator)
    {
        QuicStreamLayer layer(makeLocalParameters());
        layer.adoptPeerParameters(makeParameters(8192, 4, 22, 33, 4, 2));
        const auto payload = bytesOf("0123456789");
        EXPECT_EQ(layer.writeStreamData(0x00, payload, false), 10U);
        EXPECT_EQ(layer.writeStreamData(0x01, payload, false), 10U);
        EXPECT_EQ(layer.writeStreamData(0x03, payload, false), 10U);

        const Collected collected = collect(layer, 1200);
        const std::vector<QuicStreamFrame> sent = framesOfType<QuicStreamFrame>(collected.frames);
        ASSERT_EQ(sent.size(), 3U);
        EXPECT_EQ(sent[0].streamId, 0x00U);
        EXPECT_EQ(payloadTextOf(sent[0]).size(), 4U) << "对端发起的流取对端的 0x05";
        EXPECT_EQ(sent[1].streamId, 0x01U);
        EXPECT_EQ(payloadTextOf(sent[1]).size(), 10U) << "本端发起的双向流取对端的 0x06";
        EXPECT_EQ(sent[2].streamId, 0x03U);
        EXPECT_EQ(payloadTextOf(sent[2]).size(), 10U) << "本端发起的单向流取对端的 0x07";
    }

    /**
     * @brief 对端参数没到手之前一字节也发不出去（§4.1）
     */
    TEST(QuicStreamLayer, HoldsBackSendUntilPeerParametersArrive)
    {
        QuicStreamLayer layer(makeLocalParameters());
        EXPECT_EQ(layer.writeStreamData(0x00, bytesOf("abc"), false), 3U);
        EXPECT_TRUE(layer.hasOutgoingFrames());

        const Collected blocked = collect(layer, 1200);
        EXPECT_FALSE(blocked.hasFrames);
        EXPECT_TRUE(blocked.frames.empty());
        EXPECT_TRUE(blocked.ranges.empty());

        layer.adoptPeerParameters(makePeerParameters());
        const Collected sent = collect(layer, 1200);
        EXPECT_TRUE(sent.hasFrames);
        EXPECT_EQ(payloadTextOf(firstFrameOf<QuicStreamFrame>(sent.frames)), "abc");
    }

    /**
     * @brief 超出对端给的流数上限的流开不出来，单向流号也不越界发放
     */
    TEST(QuicStreamLayer, RefusesToOpenStreamsBeyondPeerLimit)
    {
        QuicStreamLayer layer(makeLocalParameters());
        layer.adoptPeerParameters(makeParameters(8192, 2048, 2048, 2048, 0, 1));
        EXPECT_EQ(layer.writeStreamData(0x01, bytesOf("abc"), false), 0U) << "对端没给本端开双向流的额度";

        const std::optional<std::uint64_t> first = layer.openUnidirectionalStream();
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(*first, 3U);
        EXPECT_FALSE(layer.openUnidirectionalStream().has_value()) << "唯一一条单向流已经用掉，第二次开流不该给出流号";
        EXPECT_EQ(layer.writeStreamData(*first, bytesOf("abc"), false), 3U);
    }

    /**
     * @brief 一个包里装不下的数据分片续发，偏移首尾相接
     */
    TEST(QuicStreamLayer, SplitsPendingDataAcrossByteBudget)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        EXPECT_EQ(layer.writeStreamData(0x03, bytesOf("0123456789"), true), 10U);

        std::string text;
        std::vector<QuicStreamRange> allRanges;
        for (std::size_t round = 0; round < 6; ++round)
        {
            const Collected collected = collect(layer, 12);
            if (!collected.hasFrames)
            {
                break;
            }
            for (const QuicStreamFrame &frame : framesOfType<QuicStreamFrame>(collected.frames))
            {
                EXPECT_EQ(frame.offset, allRanges.empty() ? 0U : allRanges.back().endOffset) << "分片必须首尾相接";
                text += payloadTextOf(frame);
            }
            allRanges.insert(allRanges.end(), collected.ranges.begin(), collected.ranges.end());
        }
        EXPECT_EQ(text, "0123456789");
        ASSERT_FALSE(allRanges.empty());
        EXPECT_TRUE(allRanges.back().isFinal) << "最后一片要带着 FIN";
        EXPECT_FALSE(layer.hasOutgoingFrames());
    }

    /**
     * @brief 流级额度用完就停发，等 MAX_STREAM_DATA 抬起来再继续
     */
    TEST(QuicStreamLayer, StopsAtStreamLimitUntilCreditArrives)
    {
        QuicStreamLayer layer(makeLocalParameters());
        layer.adoptPeerParameters(makeParameters(8192, 6, 6, 6, 4, 2));
        EXPECT_EQ(layer.writeStreamData(0x03, bytesOf("0123456789"), false), 10U);

        const Collected first = collect(layer, 1200);
        ASSERT_EQ(payloadTextOf(firstFrameOf<QuicStreamFrame>(first.frames)), "012345") << "只有 6 字节额度";
        EXPECT_FALSE(collect(layer, 1200).hasFrames) << "额度用完了，不该再编数据帧";

        QuicMaxStreamDataFrame maxStreamData;
        maxStreamData.streamId = 0x03;
        maxStreamData.maximumStreamData = 10;
        EXPECT_TRUE(layer.onMaxStreamDataFrame(maxStreamData).has_value());
        const Collected second = collect(layer, 1200);
        EXPECT_EQ(payloadTextOf(firstFrameOf<QuicStreamFrame>(second.frames)), "6789");
        EXPECT_EQ(second.ranges.front().beginOffset, 6U);
    }

    /**
     * @brief 连接级额度是各流最大偏移之和，第二条流不能越过总账（§4.1）
     */
    TEST(QuicStreamLayer, SharesConnectionSendLimitAcrossStreams)
    {
        QuicStreamLayer layer(makeLocalParameters());
        layer.adoptPeerParameters(makeParameters(12, 1000, 1000, 1000, 4, 4));
        EXPECT_EQ(layer.writeStreamData(0x03, bytesOf("0123456789"), false), 10U);
        EXPECT_EQ(layer.writeStreamData(0x07, bytesOf("0123456789"), false), 10U);

        const Collected collected = collect(layer, 1200);
        const std::vector<QuicStreamFrame> sent = framesOfType<QuicStreamFrame>(collected.frames);
        ASSERT_EQ(sent.size(), 2U);
        std::size_t sentByteCount = 0;
        for (const QuicStreamFrame &frame : sent)
        {
            sentByteCount += frame.data.size();
        }
        EXPECT_EQ(sentByteCount, 12U) << "两条流加起来正好花光连接级额度";
        EXPECT_FALSE(collect(layer, 1200).hasFrames);
    }

    /**
     * @brief 对端长期不给窗口时，本端的待发队列停在上界，而不是替对端无限占内存
     * @details 排空只发生在编帧那一刻，而编帧要等对端给窗口，因此「只收不授窗口」正是上界要拦住的那类对端
     */
    TEST(QuicStreamLayer, CapsPendingSendQueueWhenPeerGrantsNoWindow)
    {
        QuicStreamLayer layer(makeLocalParameters());
        layer.adoptPeerParameters(makeParameters(0, 0, 0, 0, 4, 4));

        const std::vector<std::uint8_t> segment(64U * 1024U, std::uint8_t{'x'});
        std::size_t acceptedTotalByteCount = 0;
        for (int attempt = 0; attempt < 32; ++attempt)
        {
            acceptedTotalByteCount += layer.writeStreamData(0x03, segment, false);
        }
        EXPECT_EQ(acceptedTotalByteCount, QuicStreamLayer::kMaximumPendingSendByteCount)
                << "收下的总量必须恰好停在上界：多一个字节就是让对端替本端决定占多少内存";
        EXPECT_EQ(layer.pendingSendByteCount(0x03), acceptedTotalByteCount);
        EXPECT_EQ(layer.writeStreamData(0x03, segment, false), 0U) << "到界之后一字节也不收";
        EXPECT_FALSE(collect(layer, 1200).hasFrames) << "没有窗口就不该编出数据帧";
    }

    /**
     * @brief 队列到界之后收尾写照收，编帧排空后回报腾出的地方并恢复接收
     */
    TEST(QuicStreamLayer, KeepsAcceptingFinalMarkerAndResumesAfterTheQueueDrains)
    {
        QuicStreamLayer layer(makeLocalParameters());
        layer.adoptPeerParameters(makeParameters(8U * 1024U * 1024U, 8U * 1024U * 1024U, 8U * 1024U * 1024U,
                                                 8U * 1024U * 1024U, 4, 4));
        const std::size_t capByteCount = QuicStreamLayer::kMaximumPendingSendByteCount;
        const std::vector<std::uint8_t> fullSegment(capByteCount, std::uint8_t{'y'});
        ASSERT_EQ(layer.writeStreamData(0x03, fullSegment, false), capByteCount) << "恰好等于上界的那一段应当全收下";
        EXPECT_EQ(layer.writeStreamData(0x03, fullSegment, false), 0U) << "队列已满，第二段一个字节也不该收";

        // 零长的收尾写不吃队列空间：到界也要收下它，否则这条流永远收不了口、正文停在半路
        layer.writeStreamData(0x03, std::span<const std::uint8_t>{}, true);

        const Collected collected = collect(layer, 16U * 1024U * 1024U);
        const std::vector<QuicStreamFrame> sent = framesOfType<QuicStreamFrame>(collected.frames);
        std::size_t framedByteCount = 0;
        for (const QuicStreamFrame &frame : sent)
        {
            framedByteCount += frame.data.size();
        }
        ASSERT_FALSE(sent.empty());
        EXPECT_EQ(framedByteCount, capByteCount) << "交出去一个字节就该在线上看见一个字节，不多不少";
        EXPECT_TRUE(sent.back().isFinal) << "队列到界时交来的收尾必须排在最后一段上";
        EXPECT_EQ(layer.takeDrainedSendByteCount(), capByteCount);
        EXPECT_EQ(layer.pendingSendByteCount(0x03), 0U);

        // 排空之后重新接收：上界拦的是「压着多少字节没走」，不是「这条流一共发过多少」
        ASSERT_EQ(layer.writeStreamData(0x07, fullSegment, false), capByteCount);
        EXPECT_EQ(layer.writeStreamData(0x07, fullSegment, false), 0U);
        EXPECT_EQ(layer.takeDrainedSendByteCount(), 0U) << "没编帧就不该报排空";
        EXPECT_TRUE(collect(layer, 16U * 1024U * 1024U).hasFrames);
        EXPECT_EQ(layer.takeDrainedSendByteCount(), capByteCount);
        EXPECT_EQ(layer.writeStreamData(0x07, bytesOf("after drain"), false), 11U);
    }

    /**
     * @brief 逐流各卡一点也要被连接级总量拦住
     * @details 单流 1 MiB 乘以流数是一条与流数同增的账：几十条停滞的请求流就能各压 1 MiB。
     *          本用例把窗口给足（宽到不可能因为流控而拒收），于是唯一的拒收理由就是总量
     */
    TEST(QuicStreamLayer, CapsConnectionPendingQueueAcrossStreams)
    {
        QuicStreamLayer layer(makeLocalParameters());
        layer.adoptPeerParameters(makeParameters(1024U * 1024U * 1024U, 1024U * 1024U * 1024U, 1024U * 1024U * 1024U,
                                                 1024U * 1024U * 1024U, 4, 16));

        const std::size_t streamCount = QuicStreamLayer::kMaximumConnectionPendingSendByteCount
                                        / QuicStreamLayer::kMaximumPendingSendByteCount;
        const std::vector<std::uint8_t> segment(QuicStreamLayer::kMaximumPendingSendByteCount, std::uint8_t{'z'});
        for (std::size_t streamIndex = 0; streamIndex < streamCount; ++streamIndex)
        {
            ASSERT_EQ(layer.writeStreamData(3 + streamIndex * 4, segment, false), segment.size())
                    << "第 " << streamIndex << " 条流要先各自填满单流额度";
        }
        EXPECT_EQ(layer.totalPendingSendByteCount(), QuicStreamLayer::kMaximumConnectionPendingSendByteCount);

        const std::uint64_t freshStreamId = 3 + streamCount * 4;
        EXPECT_EQ(layer.writeStreamData(freshStreamId, segment, false), 0U) << "触顶的是连接级总量，不是这条流自己的额度";
        EXPECT_EQ(layer.pendingSendByteCount(freshStreamId), 0U);

        // 排空多少就能再收多少：拦路的是总量这本账，任何一条流走掉一段都算数
        ASSERT_TRUE(collect(layer, 200U * 1024U).hasFrames);
        const std::size_t drainedByteCount = layer.takeDrainedSendByteCount();
        ASSERT_GT(drainedByteCount, 0U);
        const std::vector<std::uint8_t> drainedSegment(drainedByteCount, std::uint8_t{'w'});
        EXPECT_EQ(layer.writeStreamData(freshStreamId, drainedSegment, false), drainedByteCount)
                << "只按刚腾出的量收，多一个字节都是把总量闸放开";
    }

    /**
     * @brief 判丢的段按原偏移重新排队，重发不重复占用额度
     */
    TEST(QuicStreamLayer, RequeuesLostRangesAtSameOffset)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        EXPECT_EQ(layer.writeStreamData(0x03, bytesOf("abcdef"), false), 6U);
        const Collected first = collect(layer, 1200);
        ASSERT_EQ(first.ranges.size(), 1U);
        EXPECT_EQ(first.ranges.front().beginOffset, 0U);
        EXPECT_EQ(first.ranges.front().endOffset, 6U);

        layer.onSendRangesLost(first.ranges);
        const Collected resent = collect(layer, 1200);
        ASSERT_EQ(resent.ranges.size(), 1U);
        EXPECT_EQ(resent.ranges.front(), first.ranges.front()) << "重发的偏移必须和原来那条一模一样";
        EXPECT_EQ(payloadTextOf(firstFrameOf<QuicStreamFrame>(resent.frames)), "abcdef");
    }

    /**
     * @brief 确认只销在途账：之后的数据照常往前发
     */
    TEST(QuicStreamLayer, ReleasesInFlightRangesOnAcknowledgement)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        EXPECT_EQ(layer.writeStreamData(0x03, bytesOf("abcdef"), false), 6U);
        layer.onSendRangesAcknowledged(collect(layer, 1200).ranges);

        EXPECT_EQ(layer.writeStreamData(0x03, bytesOf("gh"), true), 2U);
        const Collected next = collect(layer, 1200);
        const std::vector<QuicStreamFrame> sent = framesOfType<QuicStreamFrame>(next.frames);
        ASSERT_EQ(sent.size(), 1U);
        EXPECT_EQ(payloadTextOf(sent.front()), "gh");
        EXPECT_EQ(sent.front().offset, 6U);
        EXPECT_TRUE(sent.front().isFinal);
    }

    /**
     * @brief 部分确认+部分判丢混在一轮里时，只有丢的那段回来
     */
    TEST(QuicStreamLayer, AcknowledgesOneRangeAndRetransmitsAnother)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        EXPECT_EQ(layer.writeStreamData(0x03, bytesOf("abcdefghij"), true), 10U);
        const Collected first = collect(layer, 8);
        const Collected second = collect(layer, 40);
        ASSERT_EQ(first.ranges.size(), 1U);
        ASSERT_EQ(second.ranges.size(), 1U);
        const std::string firstText = payloadTextOf(firstFrameOf<QuicStreamFrame>(first.frames));
        const std::string secondText = payloadTextOf(firstFrameOf<QuicStreamFrame>(second.frames));
        EXPECT_EQ(firstText + secondText, "abcdefghij");
        EXPECT_EQ(second.ranges.front().beginOffset, first.ranges.front().endOffset);

        layer.onSendRangesAcknowledged(first.ranges);
        layer.onSendRangesLost(second.ranges);
        const Collected resent = collect(layer, 40);
        ASSERT_EQ(resent.ranges.size(), 1U);
        EXPECT_EQ(resent.ranges.front(), second.ranges.front()) << "只有判丢的那段回来，且偏移不变";
        EXPECT_EQ(payloadTextOf(firstFrameOf<QuicStreamFrame>(resent.frames)), secondText);
        EXPECT_TRUE(resent.ranges.front().isFinal);
    }

    /**
     * @brief 已经确认过的区间再报一次判丢：不该把送出去的字节重新排队
     */
    TEST(QuicStreamLayer, IgnoresLossReportsForAlreadyAcknowledgedRanges)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        EXPECT_EQ(layer.writeStreamData(0x03, bytesOf("abcdef"), true), 6U);
        const Collected first = collect(layer, 1200);
        ASSERT_EQ(first.ranges.size(), 1U);

        layer.onSendRangesAcknowledged(first.ranges);
        layer.onSendRangesLost(first.ranges);
        EXPECT_FALSE(collect(layer, 1200).hasFrames) << "确认过的段被判丢复活，对端会看到重复字节";
    }

    /**
     * @brief 消费掉一半窗口才抬一次额度，避免每次读都补一帧（§2.2）
     */
    TEST(QuicStreamLayer, RaisesReceiveWindowAtHalfConsumed)
    {
        // 流级 64、连接级 4096，好让流级的 MAX_STREAM_DATA 先于连接级发生
        QuicStreamLayer layer(makeParameters(4096, 64, 64, 64, 4, 4));
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 0, std::vector<std::uint8_t>(32, 'a'))).has_value());
        EXPECT_EQ(drainDeliveries(layer, 0x00), std::string(32, 'a'));

        layer.releaseReceiveWindow(0x00, 16);
        EXPECT_FALSE(collect(layer, 1200).hasFrames) << "还剩一半，不该抬";

        layer.releaseReceiveWindow(0x00, 16);
        const Collected collected = collect(layer, 1200);
        const std::vector<QuicMaxStreamDataFrame> updates = framesOfType<QuicMaxStreamDataFrame>(collected.frames);
        ASSERT_EQ(updates.size(), 1U);
        EXPECT_EQ(updates.front().streamId, 0x00U);
        EXPECT_EQ(updates.front().maximumStreamData, 96U) << "抬到消费点 + 初始窗口";
    }

    /**
     * @brief 连接级与流级各自记账，一次消费可以两条都欠着
     */
    TEST(QuicStreamLayer, RaisesBothWindowLevelsTogether)
    {
        QuicStreamLayer layer(makeParameters(64, 64, 64, 64, 4, 4));
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 0, std::vector<std::uint8_t>(64, 'a'))).has_value());
        drainDeliveries(layer, 0x00);
        layer.releaseReceiveWindow(0x00, 32);

        const Collected collected = collect(layer, 1200);
        EXPECT_EQ(framesOfType<QuicMaxDataFrame>(collected.frames).size(), 1U);
        EXPECT_EQ(framesOfType<QuicMaxStreamDataFrame>(collected.frames).size(), 1U);
        EXPECT_FALSE(layer.hasOutgoingFrames());
    }

    /**
     * @brief 对端用掉一半流数就补一条 MAX_STREAMS，否则它会卡在 0x04 上（§4.6）
     */
    TEST(QuicStreamLayer, RefreshesAdvertisedStreamCount)
    {
        QuicStreamLayer layer(makeParameters(4096, 1024, 1024, 1024, 2, 0));
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 0, bytesOf("a"))).has_value());
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x04, 0, bytesOf("b"))).has_value());

        const Collected collected = collect(layer, 1200);
        const std::vector<QuicMaxStreamsFrame> updates = framesOfType<QuicMaxStreamsFrame>(collected.frames);
        ASSERT_EQ(updates.size(), 1U);
        EXPECT_FALSE(updates.front().isUnidirectional) << "单向流一条没给，也就没有单向的上限可续";
        EXPECT_EQ(updates.front().maximumStreams, 4U) << "2 条用满，按初始值再给一轮";
    }

    /**
     * @brief 复位打断入站流：缓存作废、迟到数据丢弃，连接级额度不退
     */
    TEST(QuicStreamLayer, AbortsIncomingStreamOnReset)
    {
        QuicStreamLayer layer(makeLocalParameters());
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 0, bytesOf("abc"))).has_value());
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 8, bytesOf("ijkl"))).has_value());
        EXPECT_EQ(drainDeliveries(layer, 0x00), "abc");

        QuicResetStreamFrame reset;
        reset.streamId = 0x00;
        reset.applicationErrorCode = 0x100;
        reset.finalSize = 12;
        EXPECT_TRUE(layer.onResetStreamFrame(reset).has_value());
        ASSERT_TRUE(layer.hasAbortedStreams());
        EXPECT_EQ(layer.takeAbortedStream().value_or(999), 0x00U);

        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 4, bytesOf("defg"))).has_value());
        EXPECT_FALSE(layer.hasDeliveries());
    }

    /**
     * @brief 复位长度与已知收尾、或与已交付部分不符，都是 FINAL_SIZE_ERROR（§4.5）
     */
    TEST(QuicStreamLayer, RejectsInconsistentResetFinalSize)
    {
        QuicStreamLayer layer(makeLocalParameters());
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 0, bytesOf("abc"), true)).has_value());
        QuicResetStreamFrame reset;
        reset.streamId = 0x00;
        reset.finalSize = 2;
        EXPECT_EQ(violationCodeOf(layer.onResetStreamFrame(reset)), 0x06U);
    }

    /**
     * @brief 单向流的方向搞错就是 STREAM_STATE_ERROR（§4.5、§4.6）
     */
    TEST(QuicStreamLayer, RejectsStateErrorsOnOneWayStreams)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        QuicResetStreamFrame reset;
        reset.streamId = 0x03;
        EXPECT_EQ(violationCodeOf(layer.onResetStreamFrame(reset)), 0x05U) << "本端发起的单向流没有可对端复位的东西";

        QuicMaxStreamDataFrame maxStreamData;
        maxStreamData.streamId = 0x02;
        maxStreamData.maximumStreamData = 100;
        EXPECT_EQ(violationCodeOf(layer.onMaxStreamDataFrame(maxStreamData)), 0x05U) << "对端发起的单向流本端不能发";

        QuicStopSendingFrame stopSending;
        stopSending.streamId = 0x02;
        EXPECT_EQ(violationCodeOf(layer.onStopSendingFrame(stopSending)), 0x05U) << "本端在这条流上本来就不发数据";
    }

    /**
     * @brief 还没建流的入站流被复位：没有账要作废，忽略即可
     */
    TEST(QuicStreamLayer, IgnoresResetForUnknownStream)
    {
        QuicStreamLayer layer(makeLocalParameters());
        QuicResetStreamFrame reset;
        reset.streamId = 0x08;
        reset.finalSize = 100;
        EXPECT_TRUE(layer.onResetStreamFrame(reset).has_value());
        EXPECT_FALSE(layer.hasAbortedStreams());
    }

    /**
     * @brief STOP_SENDING 叫停本端的发送：待发队列作废，之后的写入也不收
     * @details 旧断言「叫停之后本端没有任何东西要发」按 §3.5 是错的：被叫停的一方必须回一条
     *          RESET_STREAM 交代收尾，否则对端永远等不到那条流的终局信号
     */
    TEST(QuicStreamLayer, DropsPendingDataOnStopSending)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        EXPECT_EQ(layer.writeStreamData(0x03, bytesOf("abcdef"), false), 6U);
        EXPECT_TRUE(layer.hasOutgoingFrames());

        QuicStopSendingFrame stopSending;
        stopSending.streamId = 0x03;
        stopSending.applicationErrorCode = 0x200;
        EXPECT_TRUE(layer.onStopSendingFrame(stopSending).has_value());
        EXPECT_EQ(layer.takeAbortedStream().value_or(999), 0x03U);
        const Collected collected = collect(layer, 1200);
        EXPECT_TRUE(framesOfType<QuicStreamFrame>(collected.frames).empty()) << "叫停之后不再排数据帧";
        EXPECT_EQ(framesOfType<QuicResetStreamFrame>(collected.frames).size(), 1U);
        EXPECT_EQ(layer.writeStreamData(0x03, bytesOf("ghi"), false), 0U) << "叫停之后不再收写";
    }

    /**
     * @brief 收尾之后不能重复写，零长非 FIN 的写也不进队列
     */
    TEST(QuicStreamLayer, RefusesWritesAfterFinalAndEmptyWrites)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        EXPECT_EQ(layer.writeStreamData(0x03, bytesOf("abc"), true), 3U);
        EXPECT_EQ(layer.writeStreamData(0x03, bytesOf("def"), false), 0U);
        EXPECT_TRUE(layer.hasOutgoingFrames());

        QuicStreamLayer fresh(makeEstablishedLayer());
        EXPECT_EQ(fresh.writeStreamData(0x03, std::vector<std::uint8_t>{}, false), 0U);
        EXPECT_FALSE(fresh.hasOutgoingFrames()) << "空写不该留下一条永远发不出去队列项";
    }

    /**
     * @brief 任何预算下编出来的字节数都不超出预算，含窗口更新与数据帧混排的那一包
     */
    TEST(QuicStreamLayer, NeverExceedsByteBudget)
    {
        // 窗口只有 64：消费掉一半就会同时欠下一条 MAX_DATA 与一条 MAX_STREAM_DATA
        QuicStreamLayer layer(makeParameters(64, 64, 64, 64, 4, 4));
        layer.adoptPeerParameters(makeParameters(65536, 65536, 65536, 65536, 4, 4));
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 0, std::vector<std::uint8_t>(32, 'a'))).has_value());
        drainDeliveries(layer, 0x00);
        layer.releaseReceiveWindow(0x00, 32);
        EXPECT_TRUE(layer.hasOutgoingFrames());
        // 待发量要大于整轮预算之和：否则扫到大预算时队列早空了，超一档的长度域就看不出来
        EXPECT_EQ(layer.writeStreamData(0x03, std::vector<std::uint8_t>(80000, 'b'), false), 80000U);

        for (std::size_t budget = 1; budget <= 400; ++budget)
        {
            const Collected collected = collect(layer, budget);
            EXPECT_LE(collected.frames.size(), budget) << "预算 " << budget << " 字节，实际编出 " << collected.frames.size();
            EXPECT_TRUE(layer.hasOutgoingFrames()) << "预算扫到 " << budget << " 时待发已被清空，后面的轮次是空转";
        }
    }

    /**
     * @brief 本端放弃一条流的发送侧：作废待发与在途，编一条 RESET_STREAM 出去（§4.5、§13.3）
     * @details 收尾长度按「已上线的字节」算，不是上层写过的总数——报一个对端没见过的偏移，
     *          它按越界处理就会把整条连接判死
     */
    TEST(QuicStreamLayer, EmitsResetStreamWhenLocalSendIsAbandoned)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        EXPECT_EQ(layer.writeStreamData(0x01, bytesOf("abcde"), false), 5U);
        const Collected sent = collect(layer, 1200);
        ASSERT_EQ(framesOfType<QuicStreamFrame>(sent.frames).size(), 1U) << "用例前提：5 字节先上线";

        layer.resetStreamSending(0x01, 0x010b);
        EXPECT_TRUE(layer.hasOutgoingFrames());
        const Collected aborted = collect(layer, 1200);
        const std::vector<QuicResetStreamFrame> resets = framesOfType<QuicResetStreamFrame>(aborted.frames);
        ASSERT_EQ(resets.size(), 1U);
        EXPECT_EQ(resets.front().streamId, 0x01U);
        EXPECT_EQ(resets.front().applicationErrorCode, 0x010bU);
        EXPECT_EQ(resets.front().finalSize, 5U);
        EXPECT_TRUE(framesOfType<QuicStreamFrame>(aborted.frames).empty()) << "作废之后不该再排数据帧";
        ASSERT_EQ(aborted.announcements.size(), 1U);
        EXPECT_TRUE(aborted.announcements.front().isResetStream);
        EXPECT_EQ(layer.writeStreamData(0x01, bytesOf("fgh"), false), 0U) << "复位之后不再收写";
    }

    /**
     * @brief 判丢的在途数据不重发：那一侧的收尾交给 RESET_STREAM 交代（§3.5）
     */
    TEST(QuicStreamLayer, DoesNotRetransmitDataAfterLocalReset)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        EXPECT_EQ(layer.writeStreamData(0x01, bytesOf("hello"), false), 5U);
        const Collected sent = collect(layer, 1200);
        ASSERT_EQ(sent.ranges.size(), 1U);

        layer.resetStreamSending(0x01, 0x010b);
        layer.onSendRangesLost(sent.ranges);
        const Collected afterLoss = collect(layer, 1200);
        EXPECT_TRUE(framesOfType<QuicStreamFrame>(afterLoss.frames).empty()) << "判丢的段不该排回待发";
        EXPECT_EQ(framesOfType<QuicResetStreamFrame>(afterLoss.frames).size(), 1U);
    }

    /**
     * @brief 宣告在途不重发、判丢补发同一份、确认之后落定（§13.3「发到被确认为止」且内容不许变）
     */
    TEST(QuicStreamLayer, RetransmitsResetStreamOnlyWhileUnacknowledged)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        layer.resetStreamSending(0x01, 0x010b);

        const Collected first = collect(layer, 1200);
        ASSERT_EQ(first.announcements.size(), 1U);
        EXPECT_FALSE(collect(layer, 1200).hasFrames) << "在途期间不重复发同一份宣告";
        EXPECT_FALSE(layer.hasOutgoingFrames()) << "在途的宣告不该把出包循环吊住";

        layer.onStreamAnnouncementsLost(first.announcements);
        const std::vector<QuicResetStreamFrame> resent = framesOfType<QuicResetStreamFrame>(collect(layer, 1200).frames);
        ASSERT_EQ(resent.size(), 1U);
        EXPECT_EQ(resent.front().applicationErrorCode, 0x010bU);
        EXPECT_EQ(resent.front().finalSize, 0U) << "补发的内容必须与第一份一致";

        const Collected second = collect(layer, 1200);
        layer.onStreamAnnouncementsAcknowledged(second.announcements);
        EXPECT_FALSE(layer.hasOutgoingFrames()) << "已确认的宣告这辈子不再发第二遍";
        EXPECT_FALSE(collect(layer, 1200).hasFrames);
    }

    /**
     * @brief FIN 已经上线之后不再复位：那种情况对端迟早收齐，补 RESET 是自相矛盾
     */
    TEST(QuicStreamLayer, SkipsResetOnceFinalMarkerIsSent)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        EXPECT_EQ(layer.writeStreamData(0x01, bytesOf("abc"), true), 3U);
        static_cast<void>(collect(layer, 1200));

        layer.resetStreamSending(0x01, 0x010b);
        EXPECT_TRUE(framesOfType<QuicResetStreamFrame>(collect(layer, 1200).frames).empty());
    }

    /**
     * @brief FIN 还排在队列里（上层已交出收尾、尚未上线）时也不复位：抢一份 RESET 会把收尾字节作废
     * @details h3 侧「先把 413 完整交出去、再请对端停发」正依赖这一点：那时 FIN 通常还没上线
     */
    TEST(QuicStreamLayer, SkipsResetWhenFinalIsQueuedButNotSent)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        EXPECT_EQ(layer.writeStreamData(0x01, bytesOf("abc"), true), 3U);

        layer.resetStreamSending(0x01, 0x010b);
        const Collected collected = collect(layer, 1200);
        EXPECT_TRUE(framesOfType<QuicResetStreamFrame>(collected.frames).empty()) << "不该抢出一份 RESET";
        EXPECT_EQ(framesOfType<QuicStreamFrame>(collected.frames).size(), 1U) << "排队中的收尾字节要照发";
    }

    /**
     * @brief 收到 STOP_SENDING 就要回一条 RESET_STREAM，错误码照抄（§3.5 的 MUST）
     * @details 只作废队列不作废发送侧的话，对端会一直等那条流的收尾信号；本端被叫停之后
     *          不可能再发 FIN，所以收尾只能由 RESET_STREAM 给出
     */
    TEST(QuicStreamLayer, AnswersStopSendingWithResetStream)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        EXPECT_EQ(layer.writeStreamData(0x01, bytesOf("hello"), false), 5U);
        static_cast<void>(collect(layer, 1200));

        QuicStopSendingFrame stopSending;
        stopSending.streamId = 0x01;
        stopSending.applicationErrorCode = 0x010b;
        EXPECT_TRUE(layer.onStopSendingFrame(stopSending).has_value());

        const std::vector<QuicResetStreamFrame> resets = framesOfType<QuicResetStreamFrame>(collect(layer, 1200).frames);
        ASSERT_EQ(resets.size(), 1U);
        EXPECT_EQ(resets.front().applicationErrorCode, 0x010bU) << "错误码照抄对端的停发请求";
        EXPECT_EQ(resets.front().finalSize, 5U);
    }

    /**
     * @brief 请对端停发：编一条 STOP_SENDING，对端复位之后就不必再发（§3.5）
     */
    TEST(QuicStreamLayer, EmitsStopSendingForIncomingStreamUntilItIsTerminal)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        EXPECT_TRUE(layer.onStreamFrame(makeStreamFrame(0x00, 0, bytesOf("abc"))).has_value());
        layer.stopStreamReceiving(0x00, 0x010b);

        const Collected collected = collect(layer, 1200);
        const std::vector<QuicStopSendingFrame> stops = framesOfType<QuicStopSendingFrame>(collected.frames);
        ASSERT_EQ(stops.size(), 1U);
        EXPECT_EQ(stops.front().streamId, 0x00U);
        EXPECT_EQ(stops.front().applicationErrorCode, 0x010bU);
        ASSERT_EQ(collected.announcements.size(), 1U);
        EXPECT_FALSE(collected.announcements.front().isResetStream);

        QuicResetStreamFrame reset;
        reset.streamId = 0x00;
        reset.finalSize = 3;
        EXPECT_TRUE(layer.onResetStreamFrame(reset).has_value());
        // 让那条宣告重新变成待发：若它没被撤下，这一轮就会把一帧多余的 STOP_SENDING 发出去
        layer.onStreamAnnouncementsLost(collected.announcements);
        EXPECT_TRUE(framesOfType<QuicStopSendingFrame>(collect(layer, 1200).frames).empty()) << "对端已复位，停发请求不再必要";
    }

    /**
     * @brief 本端没有入站记账的流不必请它停发：那种流上对端本来就不发字节
     */
    TEST(QuicStreamLayer, IgnoresStopRequestForStreamWithoutIncomingState)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        layer.stopStreamReceiving(0x03, 0x010b);
        layer.stopStreamReceiving(0x08, 0x010b);
        EXPECT_FALSE(layer.hasOutgoingFrames());
        EXPECT_FALSE(collect(layer, 1200).hasFrames);
    }

    /**
     * @brief 预算装不下一帧宣告时，它得留在待发里等下一包（§13.3 不能丢信号）
     */
    TEST(QuicStreamLayer, KeepsAbortAnnouncementPendingWhenBudgetIsTooSmall)
    {
        QuicStreamLayer layer(makeEstablishedLayer());
        layer.resetStreamSending(0x01, 0x010b);
        // 一条 RESET_STREAM 至少五字节：三字节预算必须整帧退回
        EXPECT_FALSE(collect(layer, 3).hasFrames);
        EXPECT_TRUE(layer.hasOutgoingFrames()) << "被挤掉的宣告不能当成已经发过";
        EXPECT_EQ(framesOfType<QuicResetStreamFrame>(collect(layer, 1200).frames).size(), 1U);
    }
} // namespace AsynGyanis::Net

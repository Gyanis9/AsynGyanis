// TestWebSocketFrame.cpp —— WebSocket 帧编解码（RFC 6455 §5）的单元测试
//
// 覆盖三块：RFC 6455 §5.7 示例帧的黄金字节（编码器产出、解码器解开）、编码器与解码器的正常
// 往返与三档长度边界、以及拒绝面（未掩码、RSV 非 0、未定义操作码、控制帧越界与分片、孤立继续帧、
// 单帧与消息超限、非最短长度编码）。另有用例钉住「逐字节切分等价」「错误态粘滞且不消费字节」
// 两条契约，以及「文本负载的 UTF-8 校验不在帧层做」这一职责边界（非法字节原样交给会话层）。
// 用例都是纯计算，不起网络、不依赖任何外部服务。

#include "Net/WebSocket/WebSocketFrame.h"
#include "Net/WebSocket/WebSocketUtf8.h"

#include "NetTestSupport.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// RFC 6455 §5.7 示例 2 里的掩码键，用例沿用它与示例的负载字节对上规范原文
        constexpr std::array<std::uint8_t, 4> kRfcExampleMaskKey{0x37, 0xfa, 0x21, 0x3d};

        /**
         * @brief 判断文本里是否出现指定子串（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::containsText;

        /**
         * @brief 由字节序列拼出二进制文本（定义见 NetTestSupport.h）
         */
        using AsynGyanis::Net::TestSupport::makeBytes;

        /**
         * @brief 写出带掩码的帧头：首字节 + 长度域 + 4 字节掩码键（不含负载）
         * @details 用例既用它拼完整帧，也用它构造「声明了长度但负载没到」的输入来撞上限。
         * @param opCode 操作码
         * @param payloadLength 声明的负载长度，单位字节
         * @param isFinal 是否末帧
         * @param maskKey 4 字节掩码键
         * @return std::string 帧头字节
         */
        std::string makeMaskedFrameHead(const WebSocketOpCode opCode, const std::size_t payloadLength, const bool isFinal, const std::array<std::uint8_t, 4> &maskKey)
        {
            std::string frame;
            frame.push_back(static_cast<char>(static_cast<std::uint8_t>(opCode) | (isFinal ? 0x80U : 0x00U)));

            if (payloadLength < 126)
            {
                frame.push_back(static_cast<char>(0x80U | payloadLength));
            } else if (payloadLength < 65536)
            {
                frame.push_back(static_cast<char>(0x80U | 126U));
                frame.push_back(static_cast<char>((payloadLength >> 8) & 0xFFU));
                frame.push_back(static_cast<char>(payloadLength & 0xFFU));
            } else
            {
                frame.push_back(static_cast<char>(0x80U | 127U));
                for (int shiftBitCount = 56; shiftBitCount >= 0; shiftBitCount -= 8)
                {
                    frame.push_back(static_cast<char>((static_cast<std::uint64_t>(payloadLength) >> shiftBitCount) & 0xFFU));
                }
            }

            for (const std::uint8_t keyByte: maskKey)
            {
                frame.push_back(static_cast<char>(keyByte));
            }
            return frame;
        }

        /**
         * @brief 拼出一条完整的客户端帧：带掩码的帧头 + 逐字节循环异或后的负载
         * @param opCode 操作码
         * @param payload 未掩码负载
         * @param isFinal 是否末帧
         * @param maskKey 4 字节掩码键
         * @return std::string 客户端帧字节
         */
        std::string makeMaskedClientFrame(const WebSocketOpCode opCode, const std::string_view payload, const bool isFinal = true,
                                          const std::array<std::uint8_t, 4> &maskKey = kRfcExampleMaskKey)
        {
            std::string frame = makeMaskedFrameHead(opCode, payload.size(), isFinal, maskKey);
            for (std::size_t index = 0; index < payload.size(); ++index)
            {
                frame.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[index]) ^ maskKey[index % 4]));
            }
            return frame;
        }

        /**
         * @brief 把编码器产出的服务端帧改写成客户端帧（置 MASK 位并对负载异或）
         * @details 服务端帧不带掩码、客户端帧必须带掩码，往返只有经过这一步才谈得上；
         *          长度域与负载位置都按原样保留，因此改写本身的成功也钉住了编码器的头部布局。
         * @param serverFrame 编码器产出的服务端帧，必须是一整帧
         * @param maskKey 4 字节掩码键
         * @return std::string 带掩码的客户端帧
         */
        std::string maskUnmaskedFrame(const std::string_view serverFrame, const std::array<std::uint8_t, 4> &maskKey = kRfcExampleMaskKey)
        {
            std::size_t headerLength     = 2;
            const auto  lengthFieldValue = static_cast<unsigned char>(serverFrame[1]);
            if (lengthFieldValue == 126)
            {
                headerLength += 2;
            } else if (lengthFieldValue == 127)
            {
                headerLength += 8;
            }

            std::string clientFrame(serverFrame.substr(0, headerLength));
            // 服务端帧的第二个字节最高位恒为 0，改写成客户端方向要求的 1 即可
            clientFrame[1] = static_cast<char>(static_cast<unsigned char>(clientFrame[1]) | 0x80U);
            for (const std::uint8_t keyByte: maskKey)
            {
                clientFrame.push_back(static_cast<char>(keyByte));
            }

            for (std::size_t index = headerLength; index < serverFrame.size(); ++index)
            {
                clientFrame.push_back(static_cast<char>(static_cast<unsigned char>(serverFrame[index]) ^ maskKey[(index - headerLength) % 4]));
            }
            return clientFrame;
        }

        /**
         * @brief 读回编码器产出的帧里长度域声明的负载长度
         * @param frame 编码器产出的帧
         * @return std::uint64_t 长度域声明的负载长度
         */
        std::uint64_t readEncodedPayloadLength(const std::string &frame)
        {
            const auto lengthFieldValue = static_cast<unsigned char>(frame[1]);
            if (lengthFieldValue < 126)
            {
                return lengthFieldValue;
            }

            const std::size_t extensionByteCount = lengthFieldValue == 126 ? 2 : 8;
            std::uint64_t     payloadLength      = 0;
            for (std::size_t index = 0; index < extensionByteCount; ++index)
            {
                payloadLength = (payloadLength << 8) | static_cast<unsigned char>(frame[2 + index]);
            }
            return payloadLength;
        }

        /**
         * @brief 把一段字节喂给解码器
         * @param decoder 解码器
         * @param bytes 本次喂入的字节
         * @return WebSocketDecodeStatus 本次结论
         */
        WebSocketDecodeStatus feed(WebSocketFrameDecoder &decoder, const std::string_view bytes)
        {
            return decoder.parse(bytes.data(), bytes.size());
        }

        /**
         * @brief 断言收到一帧并取出来
         * @param decoder 解码器
         * @param bytes 本次喂入的字节
         * @return WebSocketFrame 取出的帧；未产出帧时返回默认帧，且用例已被记为失败
         */
        WebSocketFrame feedAndTakeFrame(WebSocketFrameDecoder &decoder, const std::string_view bytes)
        {
            const WebSocketDecodeStatus status = feed(decoder, bytes);
            EXPECT_EQ(status, WebSocketDecodeStatus::Frame) << "本应产出一帧，错误：" << decoder.errorMessage();
            if (status != WebSocketDecodeStatus::Frame)
            {
                return {};
            }
            return decoder.takeFrame();
        }

        /**
         * @brief 断言这段字节被判错，并交出中文原因
         * @param decoder 解码器
         * @param bytes 本次喂入的字节
         * @return std::string 错误文案，已断言非空
         */
        std::string feedAndExpectError(WebSocketFrameDecoder &decoder, const std::string_view bytes)
        {
            EXPECT_EQ(feed(decoder, bytes), WebSocketDecodeStatus::Error) << "这段输入本应被判错";
            EXPECT_TRUE(decoder.hasError());
            EXPECT_FALSE(decoder.errorMessage().empty()) << "失败必须给出可排查的原因";
            return decoder.errorMessage();
        }

        /**
         * @brief 断言帧的操作码、末帧标记与负载都符合预期
         * @param frame 待校验的帧
         * @param expectedOpCode 期望的操作码
         * @param expectedPayload 期望的负载
         */
        void expectFrameEquals(const WebSocketFrame &frame, const WebSocketOpCode expectedOpCode, const std::string_view expectedPayload)
        {
            EXPECT_EQ(frame.opCode, expectedOpCode);
            EXPECT_TRUE(frame.isFinal) << "解码器交付的帧一定是末帧：分片已在解码层重组";
            EXPECT_EQ(frame.payload, std::string(expectedPayload));
        }

        /**
         * @brief 一次把整串字节喂完并取出全部帧
         * @details 按 consumedByteCount() 推进偏移，因此这同时也在钉「返回 Frame 时只消费本帧字节」
         *          这条契约。
         * @param decoder 解码器
         * @param stream 整串字节
         * @return std::vector<WebSocketFrame> 按顺序取出的帧
         */
        std::vector<WebSocketFrame> decodeAllInOneFeed(WebSocketFrameDecoder &decoder, const std::string_view stream)
        {
            std::vector<WebSocketFrame> frames;
            std::size_t                 offset = 0;
            while (offset < stream.size())
            {
                const WebSocketDecodeStatus status = decoder.parse(stream.data() + offset, stream.size() - offset);
                if (status != WebSocketDecodeStatus::Frame)
                {
                    // 记一次失败即停：继续跑只会重复同一个结论
                    ADD_FAILURE() << "本应产出一帧，错误：" << decoder.errorMessage();
                    return frames;
                }

                EXPECT_GT(decoder.consumedByteCount(), 0U);
                offset += decoder.consumedByteCount();
                frames.push_back(decoder.takeFrame());
            }
            return frames;
        }

        /**
         * @brief 把同一串字节按每 1 字节喂入并取出全部帧
         * @param decoder 解码器
         * @param stream 整串字节
         * @return std::vector<WebSocketFrame> 按顺序取出的帧
         */
        std::vector<WebSocketFrame> decodeByteByByte(WebSocketFrameDecoder &decoder, const std::string_view stream)
        {
            std::vector<WebSocketFrame> frames;
            for (const char byteValue: stream)
            {
                const WebSocketDecodeStatus status = decoder.parse(&byteValue, 1);
                if (status == WebSocketDecodeStatus::Frame)
                {
                    frames.push_back(decoder.takeFrame());
                    continue;
                }
                if (status == WebSocketDecodeStatus::Error)
                {
                    ADD_FAILURE() << "逐字节喂入不应判错，原因：" << decoder.errorMessage();
                    return frames;
                }
            }
            return frames;
        }
    } // namespace

    // ============================================================================
    // RFC 6455 §5.7 的示例帧：黄金字节
    // ============================================================================

    /**
     * @brief 钉住 RFC 6455 §5.7 示例 1：未掩码单帧文本 "Hello" 的逐字节形态
     */
    TEST(WebSocketFrame, EncoderMatchesRfc6455UnmaskedHelloTextFrame)
    {
        EXPECT_EQ(encodeWebSocketFrame(WebSocketOpCode::Text, "Hello"), std::string("\x81\x05Hello"));
    }

    /**
     * @brief 钉住 RFC 6455 §5.7 示例 2：带掩码的单帧文本 "Hello" 必须被解出 "Hello"
     * @details 掩码键与负载都取自规范原文，因此这条用例是解掩码正确性的第一判据。
     */
    TEST(WebSocketFrame, DecoderUnmasksRfc6455MaskedHelloTextFrame)
    {
        WebSocketFrameDecoder decoder;
        const std::string     rfcMaskedFrame = "\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58";

        const WebSocketFrame frame = feedAndTakeFrame(decoder, rfcMaskedFrame);

        EXPECT_EQ(decoder.consumedByteCount(), rfcMaskedFrame.size());
        expectFrameEquals(frame, WebSocketOpCode::Text, "Hello");
    }

    /**
     * @brief 钉住 RFC 6455 §5.7 示例 3：分片文本消息「Hel」+「lo」两帧的逐字节形态
     */
    TEST(WebSocketFrame, EncoderMatchesRfc6455UnmaskedFragmentedTextFrames)
    {
        EXPECT_EQ(encodeWebSocketFrame(WebSocketOpCode::Text, "Hel", false), std::string("\x01\x03Hel"));
        EXPECT_EQ(encodeWebSocketFrame(WebSocketOpCode::Continuation, "lo", true), std::string("\x80\x02lo"));
    }

    /**
     * @brief 钉住 RFC 6455 §5.7 示例 4：未掩码 Ping 的形态，以及它的带掩码形态能被解出
     */
    TEST(WebSocketFrame, EncoderMatchesRfc6455UnmaskedPingAndDecoderUnmasksItsMaskedForm)
    {
        EXPECT_EQ(encodeWebSocketFrame(WebSocketOpCode::Ping, "Hello"), std::string("\x89\x05Hello"));

        // 掩码键与负载取自 §5.7 示例 4 的带掩码 Pong 帧，只把操作码换成客户端方向的 Ping
        WebSocketFrameDecoder decoder;
        const WebSocketFrame  frame = feedAndTakeFrame(decoder, "\x89\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58");

        expectFrameEquals(frame, WebSocketOpCode::Ping, "Hello");
    }

    /**
     * @brief 钉住 RFC 6455 §5.7 示例 5/6：256 字节与 64 KiB 负载的长度域形态
     */
    TEST(WebSocketFrame, EncoderMatchesRfc6455SixteenAndSixtyFourBitLengthHeaders)
    {
        const std::string twoHundredFiftySixBytePayload(256, 'x');
        const std::string sixteenBitFrame = encodeWebSocketFrame(WebSocketOpCode::Binary, twoHundredFiftySixBytePayload);
        EXPECT_EQ(sixteenBitFrame.substr(0, 4), makeBytes({0x82, 0x7e, 0x01, 0x00})) << "256 字节要用 16 位长度域（示例 5）";
        EXPECT_EQ(sixteenBitFrame.size(), 4U + twoHundredFiftySixBytePayload.size());

        const std::string sixtyFourKiBBytePayload(65536, 'y');
        const std::string sixtyFourBitFrame = encodeWebSocketFrame(WebSocketOpCode::Binary, sixtyFourKiBBytePayload);
        EXPECT_EQ(sixtyFourBitFrame.substr(0, 10), makeBytes({0x82, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00})) << "64 KiB 要用 64 位长度域（示例 6）";
        // 64 位长度的最高位必须为 0（RFC 6455 §5.2）
        EXPECT_EQ(static_cast<unsigned char>(sixtyFourBitFrame[2]) & 0x80U, 0U);
        EXPECT_EQ(sixtyFourBitFrame.size(), 10U + sixtyFourKiBBytePayload.size());
    }

    // ============================================================================
    // 长度三档：125 / 126 / 65535 / 65536 四个边界
    // ============================================================================

    /**
     * @brief 编码器在四个边界上各走对档位，服务端帧一律不置掩码位
     */
    TEST(WebSocketFrame, EncodesPayloadLengthInTheRightWidthAtEveryBoundary)
    {
        struct LengthProbe
        {
            std::size_t payloadLength; ///< 负载长度
            std::size_t headLength;    ///< 期望的帧头长度（首字节 + 长度域）
        };
        const std::vector<LengthProbe> probes{{0, 2}, {125, 2}, {126, 4}, {65535, 4}, {65536, 10}};

        for (const LengthProbe &probe: probes)
        {
            const std::string payload(probe.payloadLength, 'a');
            const std::string frame = encodeWebSocketFrame(WebSocketOpCode::Binary, payload);

            EXPECT_EQ(readEncodedPayloadLength(frame), static_cast<std::uint64_t>(probe.payloadLength)) << "负载长度 " << probe.payloadLength;
            EXPECT_EQ(frame.size(), probe.headLength + probe.payloadLength) << "帧头长度不符：负载长度 " << probe.payloadLength;
            // 服务端发出的帧一律不加掩码（RFC 6455 §5.1）
            EXPECT_EQ(static_cast<unsigned char>(frame[1]) & 0x80U, 0U) << "第二个字节的 MASK 位必须为 0";
        }
    }

    /**
     * @brief 解码器在同一组边界上都能收下端到端的完整负载（含 16 位与 64 位长度域）
     */
    TEST(WebSocketFrame, DecodesPayloadLengthAcrossTheSameBoundaries)
    {
        for (const std::size_t payloadLength: {0U, 125U, 126U, 65535U, 65536U})
        {
            const std::string payload(payloadLength, 'b');
            const std::string clientFrame = makeMaskedClientFrame(WebSocketOpCode::Binary, payload);

            WebSocketFrameDecoder decoder;
            const WebSocketFrame  frame = feedAndTakeFrame(decoder, clientFrame);

            EXPECT_EQ(decoder.consumedByteCount(), clientFrame.size()) << "负载长度 " << payloadLength;
            expectFrameEquals(frame, WebSocketOpCode::Binary, payload);
        }
    }

    // ============================================================================
    // 掩码解除与往返
    // ============================================================================

    /**
     * @brief 掩码按 4 字节循环解除：10 字节负载会让键走完两轮再回到开头
     */
    TEST(WebSocketFrame, UnmaskingCyclesTheFourByteKey)
    {
        const std::array<std::uint8_t, 4> maskKey{0x01, 0x02, 0x03, 0x04};
        const std::string                 payload = "0123456789";

        WebSocketFrameDecoder decoder;
        const std::string     clientFrame = makeMaskedClientFrame(WebSocketOpCode::Binary, payload, true, maskKey);
        const WebSocketFrame  frame       = feedAndTakeFrame(decoder, clientFrame);

        // 线路上的字节确实被异或过：键非 0，密文与明文不可能相等
        EXPECT_NE(clientFrame.substr(clientFrame.size() - payload.size()), payload);
        EXPECT_EQ(frame.payload, payload) << "解掩码必须还原出原始负载（含键的第 4 字节之后回头重用）";
    }

    /**
     * @brief 编码 → 加掩码 → 解码的往返：数据帧与控制帧的负载都逐字节一致
     */
    TEST(WebSocketFrame, RoundTripsEveryOpCodeThroughTheMaskingStep)
    {
        for (const WebSocketOpCode opCode: {WebSocketOpCode::Text, WebSocketOpCode::Binary, WebSocketOpCode::Ping, WebSocketOpCode::Pong, WebSocketOpCode::Close})
        {
            // 负载刻意含 NUL 与高位字节：文本帧与二进制帧都必须按「指针 + 长度」处理
            const std::string payload = opCode == WebSocketOpCode::Text ? std::string("hello 世界") : makeBytes({0x00, 0x7f, 0x80, 0xff});

            const std::string     serverFrame = encodeWebSocketFrame(opCode, payload);
            WebSocketFrameDecoder decoder;
            const WebSocketFrame  frame = feedAndTakeFrame(decoder, maskUnmaskedFrame(serverFrame));

            expectFrameEquals(frame, opCode, payload);
        }
    }

    // ============================================================================
    // 分片重组
    // ============================================================================

    /**
     * @brief 分片消息在解码层重组：中间片段不产出帧，末帧交出整条消息与首帧的操作码
     */
    TEST(WebSocketFrame, ReassemblesFragmentedMessageIntoOneFrame)
    {
        WebSocketFrameDecoder decoder;
        const std::string     firstFragment = makeMaskedClientFrame(WebSocketOpCode::Text, "Hel", false);
        const std::string     lastFragment  = makeMaskedClientFrame(WebSocketOpCode::Continuation, "lo", true);

        EXPECT_EQ(feed(decoder, firstFragment), WebSocketDecodeStatus::NeedMore) << "中间片段不构成完整消息";

        EXPECT_EQ(feed(decoder, lastFragment), WebSocketDecodeStatus::Frame);
        expectFrameEquals(decoder.takeFrame(), WebSocketOpCode::Text, "Hello");
    }

    /**
     * @brief 用 RFC 6455 §5.7 示例 3 的负载字节与示例 2 的掩码键重组出 "Hello"
     * @details 示例里那两帧是未掩码的服务端帧，这里按客户端方向的要求补上掩码，负载字节不变。
     */
    TEST(WebSocketFrame, ReassemblesRfc6455FragmentedHelloFromMaskedFrames)
    {
        WebSocketFrameDecoder decoder;

        EXPECT_EQ(feed(decoder, "\x01\x83\x37\xfa\x21\x3d\x7f\x9f\x4d"), WebSocketDecodeStatus::NeedMore);
        EXPECT_EQ(feed(decoder, "\x80\x82\x37\xfa\x21\x3d\x5b\x95"), WebSocketDecodeStatus::Frame);

        expectFrameEquals(decoder.takeFrame(), WebSocketOpCode::Text, "Hello");
    }

    /**
     * @brief 三段分片也重组成一条消息，操作码始终取第一段的（继续帧的操作码只表示「我是后续片段」）
     */
    TEST(WebSocketFrame, KeepsTheFirstFragmentOpCodeAcrossThreeFragments)
    {
        WebSocketFrameDecoder decoder;

        EXPECT_EQ(feed(decoder, makeMaskedClientFrame(WebSocketOpCode::Binary, "aa", false)), WebSocketDecodeStatus::NeedMore);
        EXPECT_EQ(feed(decoder, makeMaskedClientFrame(WebSocketOpCode::Continuation, "bb", false)), WebSocketDecodeStatus::NeedMore);
        EXPECT_EQ(feed(decoder, makeMaskedClientFrame(WebSocketOpCode::Continuation, "cc", true)), WebSocketDecodeStatus::Frame);

        expectFrameEquals(decoder.takeFrame(), WebSocketOpCode::Binary, "aabbcc");
    }

    /**
     * @brief 控制帧各自单独成帧，125 字节的 Pong 是允许的上界
     */
    TEST(WebSocketFrame, DeliversControlFramesIndividually)
    {
        const std::string     maximumControlPayload(125, 'p');
        WebSocketFrameDecoder decoder;

        expectFrameEquals(feedAndTakeFrame(decoder, makeMaskedClientFrame(WebSocketOpCode::Pong, maximumControlPayload)), WebSocketOpCode::Pong, maximumControlPayload);
        // Close 帧的负载在这里只当字节处理：状态码与原因文本的语义校验属于上层
        expectFrameEquals(feedAndTakeFrame(decoder, makeMaskedClientFrame(WebSocketOpCode::Close, makeBytes({0x03, 0xe8}))), WebSocketOpCode::Close, makeBytes({0x03, 0xe8}));
    }

    /**
     * @brief 钉住帧层的职责边界：文本负载的 UTF-8 合法性不在帧层判，非法字节原样交给会话层
     * @details RFC 6455 §5.6 要求文本消息的负载是合法 UTF-8，但帧层的契约只是「按帧格式原样交付
     *          已重组的消息」；校验发生在会话层交付业务之前，不合法即回 1007（见
     *          WebSocketPeer::feedBytes() 与 TestWebSocketSession 的 1007 用例）。这里钉住两点：
     *          帧层不得静默替换或截断非法字节，且交出去的这段字节确实会被上层的校验判为非法。
     */
    TEST(WebSocketFrame, HandsInvalidUtf8TextPayloadToSessionLayerUnchanged)
    {
        // 0xFF 0xFE 不是合法的 UTF-8 序列：帧层只保证帧格式，文本语义由上层负责
        const std::string     invalidUtf8Payload = makeBytes({0xff, 0xfe});
        WebSocketFrameDecoder decoder;

        const WebSocketFrame frame = feedAndTakeFrame(decoder, makeMaskedClientFrame(WebSocketOpCode::Text, invalidUtf8Payload));

        EXPECT_EQ(frame.payload, invalidUtf8Payload) << "帧层必须原样交付，不得静默替换或截断";
        EXPECT_FALSE(isValidWebSocketUtf8(frame.payload)) << "这段字节必须被上层的校验判为非法，否则 1007 收口不会被触发";
        EXPECT_EQ(findInvalidWebSocketUtf8ByteOffset(frame.payload), 0U) << "违规位置由帧层交出的原样字节算得，起点是第 0 字节";
    }

    // ============================================================================
    // 拒绝面
    // ============================================================================

    /**
     * @brief 未掩码的客户端帧必须被判错：这正是 RFC 6455 §5.7 示例 1 那串服务端字节
     */
    TEST(WebSocketFrame, RejectsUnmaskedFrameUsingRfcGoldenBytes)
    {
        WebSocketFrameDecoder decoder;

        const std::string reason = feedAndExpectError(decoder, "\x81\x05Hello");

        EXPECT_TRUE(containsText(reason, "掩码")) << "原因里要写清客户端帧必须带掩码";
    }

    /**
     * @brief RSV1/RSV2/RSV3 任一为 1 都判错：本实现不协商任何扩展
     */
    TEST(WebSocketFrame, RejectsNonZeroReservedBits)
    {
        // 用 array<unsigned char> 而不是 {0xc1U, ...} 这种 unsigned int 列表：
        // 后者在 /W4 下会按「窄化转换」报 C4244
        constexpr std::array<unsigned char, 3> kReservedBitVariants{0xc1U, 0xa1U, 0x91U};
        for (const unsigned char firstByte: kReservedBitVariants)
        {
            WebSocketFrameDecoder decoder;
            std::string           frame = makeMaskedClientFrame(WebSocketOpCode::Text, "x");
            frame[0]                    = static_cast<char>(firstByte);

            EXPECT_TRUE(containsText(feedAndExpectError(decoder, frame), "RSV")) << "首字节 " << static_cast<int>(firstByte);
        }
    }

    /**
     * @brief 未定义的操作码（保留区间）判错
     */
    TEST(WebSocketFrame, RejectsUndefinedOpCode)
    {
        // 同上：显式给出 unsigned char 形态的取值，避免窄化告警
        constexpr std::array<unsigned char, 4> kReservedOpCodeValues{3U, 7U, 0xbU, 0xfU};
        for (const unsigned char opCodeValue: kReservedOpCodeValues)
        {
            WebSocketFrameDecoder decoder;
            std::string           frame = makeMaskedClientFrame(WebSocketOpCode::Text, "x");
            // 保留操作码必须带 FIN 才走到「操作码未定义」这一条判定上
            frame[0] = static_cast<char>(0x80U | opCodeValue);

            EXPECT_TRUE(containsText(feedAndExpectError(decoder, frame), "操作码")) << "操作码 " << static_cast<int>(opCodeValue);
        }
    }

    /**
     * @brief 控制帧负载超过 125 字节判错，原因里要写出上限
     */
    TEST(WebSocketFrame, RejectsControlFrameWithPayloadOverTheLimit)
    {
        // 声明 126 字节的 Ping：长度域一出现 126 这个转义取值就已经越界（RFC 6455 §5.5）
        const std::string declaredTooLong = makeMaskedFrameHead(WebSocketOpCode::Ping, 126, true, kRfcExampleMaskKey);

        WebSocketFrameDecoder decoder;
        const std::string     reason = feedAndExpectError(decoder, declaredTooLong);

        EXPECT_TRUE(containsText(reason, "125")) << "原因里要给上限数值";
        EXPECT_FALSE(decoder.isLimitExceeded()) << "这是协议错误，不是资源超限";
    }

    /**
     * @brief 控制帧要求分片（FIN=0）判错
     */
    TEST(WebSocketFrame, RejectsFragmentedControlFrame)
    {
        for (const WebSocketOpCode opCode: {WebSocketOpCode::Close, WebSocketOpCode::Ping, WebSocketOpCode::Pong})
        {
            WebSocketFrameDecoder decoder;

            EXPECT_TRUE(containsText(feedAndExpectError(decoder, makeMaskedClientFrame(opCode, "x", false)), "控制帧"));
        }
    }

    /**
     * @brief 没有前置分片消息的孤立继续帧判错
     */
    TEST(WebSocketFrame, RejectsOrphanContinuationFrame)
    {
        WebSocketFrameDecoder decoder;

        EXPECT_TRUE(containsText(feedAndExpectError(decoder, makeMaskedClientFrame(WebSocketOpCode::Continuation, "x")), "继续帧"));
    }

    /**
     * @brief 分片消息没收尾就发新的数据帧判错
     */
    TEST(WebSocketFrame, RejectsNewDataFrameWhileFragmentIsOpen)
    {
        WebSocketFrameDecoder decoder;
        EXPECT_EQ(feed(decoder, makeMaskedClientFrame(WebSocketOpCode::Text, "Hel", false)), WebSocketDecodeStatus::NeedMore);

        EXPECT_TRUE(containsText(feedAndExpectError(decoder, makeMaskedClientFrame(WebSocketOpCode::Text, "lo")), "分片消息"));
    }

    /**
     * @brief 分片消息中间的控制帧照常交付，且不影响分片重组（RFC 6455 §5.4：控制帧可以插在分片消息中间）
     */
    TEST(WebSocketFrame, AcceptsControlFrameBetweenFragments)
    {
        WebSocketFrameDecoder decoder;
        EXPECT_EQ(feed(decoder, makeMaskedClientFrame(WebSocketOpCode::Text, "Hel", false)), WebSocketDecodeStatus::NeedMore);

        // 中间插一条 Ping：它独立成帧交付，分片消息仍在进行中
        ASSERT_EQ(feed(decoder, makeMaskedClientFrame(WebSocketOpCode::Ping, "hi")), WebSocketDecodeStatus::Frame);
        EXPECT_EQ(decoder.takeFrame().opCode, WebSocketOpCode::Ping);

        // 末片照旧接上：重组出的仍是完整的 "Hello"
        ASSERT_EQ(feed(decoder, makeMaskedClientFrame(WebSocketOpCode::Continuation, "lo", true)), WebSocketDecodeStatus::Frame);
        const WebSocketFrame reassembled = decoder.takeFrame();
        EXPECT_EQ(reassembled.opCode, WebSocketOpCode::Text);
        EXPECT_EQ(reassembled.payload, "Hello");
    }

    /**
     * @brief 单帧负载超过上限判错，且「超限」与协议错误可区分
     * @details 上限只在「声明」阶段生效：对端报一个天文数字的长度就必须当场撞线，否则本端会一直等着
     *          那些永远不来的字节。单帧上限取的是消息总上限那一档（浏览器类客户端一条消息发一帧，
     *          另设更小的帧上限只会把合法的大消息判死），故这里的判据与消息总量同源。
     */
    TEST(WebSocketFrame, RejectsSingleFrameOverTheFrameLimit)
    {
        // 只发帧头就撞线：上限必须在「声明」阶段生效，否则对端报一个天文数字就能让本端一直等着
        const std::size_t declaredLength = WebSocketFrameDecoder::kMaximumFramePayloadLength + 1;

        WebSocketFrameDecoder decoder;
        const std::string     reason = feedAndExpectError(decoder, makeMaskedFrameHead(WebSocketOpCode::Binary, declaredLength, true, kRfcExampleMaskKey));

        EXPECT_TRUE(containsText(reason, "上限")) << "原因里要给上限数值";
        EXPECT_TRUE(decoder.isLimitExceeded()) << "超限必须能与协议错误区分开";
        EXPECT_TRUE(containsText(reason, "消息")) << "原因里要说清「分片也不更宽松」这条替代做法";
    }

    /**
     * @brief 分片消息重组后超过 8 MiB 总上限判错
     */
    TEST(WebSocketFrame, RejectsFragmentedMessageOverTheTotalLimit)
    {
        WebSocketFrameDecoder decoder;

        // 每片用半个单帧上限（4 MiB），两片正好触到消息总上限（8 MiB），第三片哪怕只声明 1 字节也撞线。
        // 刻意用「小于单帧上限」的分片：这条判据考的是重组总量，不该被帧上限那一道先挡住
        const std::string fragment(WebSocketFrameDecoder::kMaximumMessagePayloadLength / 2, 'z');
        EXPECT_EQ(feed(decoder, makeMaskedClientFrame(WebSocketOpCode::Binary, fragment, false)), WebSocketDecodeStatus::NeedMore);
        EXPECT_EQ(feed(decoder, makeMaskedClientFrame(WebSocketOpCode::Continuation, fragment, false)), WebSocketDecodeStatus::NeedMore);

        // 此刻已重组 8 MiB：再来一片哪怕只声明 1 字节，总量也会越界
        const std::string reason = feedAndExpectError(decoder, makeMaskedFrameHead(WebSocketOpCode::Continuation, 1, true, kRfcExampleMaskKey));

        EXPECT_TRUE(containsText(reason, "上限"));
        EXPECT_TRUE(decoder.isLimitExceeded());
    }

    /**
     * @brief 长度域必须用最少的字节数表示：16 位档不得小于 126（RFC 6455 §5.2）
     */
    TEST(WebSocketFrame, RejectsNonMinimalSixteenBitLength)
    {
        WebSocketFrameDecoder decoder;

        // 0x81 0xFE 0x00 0x05：用了 16 位档却只写 5
        const std::string reason = feedAndExpectError(decoder, makeBytes({0x81, 0xfe, 0x00, 0x05, 0x37, 0xfa, 0x21, 0x3d}));

        EXPECT_TRUE(containsText(reason, "最短"));
    }

    /**
     * @brief 64 位负载长度的最高位必须为 0，且不得小于 65536
     */
    TEST(WebSocketFrame, RejectsIllegalSixtyFourBitLength)
    {
        {
            // 0x82 0xFF 后跟 8 字节长度，最高位（0x80）被置位
            WebSocketFrameDecoder decoder;
            const std::string     reason = feedAndExpectError(decoder, makeBytes({0x82, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00}));

            EXPECT_TRUE(containsText(reason, "最高位"));
        }
        {
            // 用了 64 位档却只写 65535：比 16 位档还短
            WebSocketFrameDecoder decoder;
            const std::string     reason = feedAndExpectError(decoder, makeBytes({0x82, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff}));

            EXPECT_TRUE(containsText(reason, "最短"));
        }
    }

    // ============================================================================
    // 切分、粘滞与复位
    // ============================================================================

    /**
     * @brief 任意字节边界都能切开续上：每 1 字节喂入与一次喂入的结果完全相同
     * @details 帧串里混了三档长度、两类操作码与一组分片，切点因此会落在掩码键、扩展长度与负载中间。
     */
    TEST(WebSocketFrame, ByteByByteFeedingYieldsTheSameFramesAsOneFeed)
    {
        std::string stream;
        stream += makeMaskedClientFrame(WebSocketOpCode::Text, "Hello");
        stream += makeMaskedClientFrame(WebSocketOpCode::Ping, "ping");
        stream += makeMaskedClientFrame(WebSocketOpCode::Binary, std::string(126, 'x'));
        stream += makeMaskedClientFrame(WebSocketOpCode::Binary, std::string(65536, 'y'));
        stream += makeMaskedClientFrame(WebSocketOpCode::Text, "Hel", false);
        stream += makeMaskedClientFrame(WebSocketOpCode::Continuation, "lo", true);

        WebSocketFrameDecoder             oneFeedDecoder;
        const std::vector<WebSocketFrame> oneFeedFrames = decodeAllInOneFeed(oneFeedDecoder, stream);

        WebSocketFrameDecoder             byteByByteDecoder;
        const std::vector<WebSocketFrame> byteByByteFrames = decodeByteByByte(byteByByteDecoder, stream);

        ASSERT_EQ(oneFeedFrames.size(), 5U) << "六帧输入应产出五条消息（分片两帧合成一条）";
        ASSERT_EQ(byteByByteFrames.size(), oneFeedFrames.size());
        for (std::size_t index = 0; index < oneFeedFrames.size(); ++index)
        {
            EXPECT_EQ(byteByByteFrames[index].opCode, oneFeedFrames[index].opCode) << "第 " << index << " 条消息";
            EXPECT_EQ(byteByByteFrames[index].payload, oneFeedFrames[index].payload) << "第 " << index << " 条消息";
        }
    }

    /**
     * @brief 需要更多数据时不产出帧，且本次喂入的字节全部被消费
     */
    TEST(WebSocketFrame, NeedMoreConsumesEverythingAndProducesNoFrame)
    {
        const std::string frame = makeMaskedClientFrame(WebSocketOpCode::Text, "Hello");
        // 只喂帧头前 3 字节：切在掩码键中间
        const std::string_view truncatedFrame(frame.data(), 3);

        WebSocketFrameDecoder decoder;
        EXPECT_EQ(feed(decoder, truncatedFrame), WebSocketDecodeStatus::NeedMore);
        EXPECT_EQ(decoder.consumedByteCount(), truncatedFrame.size());
        EXPECT_FALSE(decoder.hasError());

        EXPECT_EQ(decoder.parse(frame.data() + truncatedFrame.size(), frame.size() - truncatedFrame.size()), WebSocketDecodeStatus::Frame);
        expectFrameEquals(decoder.takeFrame(), WebSocketOpCode::Text, "Hello");
    }

    /**
     * @brief 切在 16 位扩展长度中间也能续上
     */
    TEST(WebSocketFrame, ResumesWhenSplitInsideTheExtendedLength)
    {
        const std::string payload(300, 'k');
        const std::string frame = makeMaskedClientFrame(WebSocketOpCode::Binary, payload);

        WebSocketFrameDecoder decoder;
        // 前 3 字节 = 首字节 + 0xFE + 16 位长度的第一个字节
        EXPECT_EQ(feed(decoder, std::string_view(frame.data(), 3)), WebSocketDecodeStatus::NeedMore);
        EXPECT_EQ(decoder.parse(frame.data() + 3, frame.size() - 3), WebSocketDecodeStatus::Frame);

        expectFrameEquals(decoder.takeFrame(), WebSocketOpCode::Binary, payload);
    }

    /**
     * @brief 错误态粘滞：判错后不再产出帧、不再消费字节，reset() 才能恢复
     */
    TEST(WebSocketFrame, ErrorStateIsStickyAndStopsConsumingBytes)
    {
        const std::string     validFrame = makeMaskedClientFrame(WebSocketOpCode::Text, "Hello");
        WebSocketFrameDecoder decoder;

        EXPECT_EQ(feed(decoder, "\x81\x05Hello"), WebSocketDecodeStatus::Error);

        EXPECT_EQ(feed(decoder, validFrame), WebSocketDecodeStatus::Error);
        EXPECT_EQ(decoder.consumedByteCount(), 0U) << "错误态下必须一字节不吃";

        // 复位把粘滞错误清掉，之后按全新的字节流重新解码
        decoder.reset();
        EXPECT_FALSE(decoder.hasError());
        EXPECT_TRUE(decoder.errorMessage().empty());
        EXPECT_EQ(feed(decoder, validFrame), WebSocketDecodeStatus::Frame);
        expectFrameEquals(decoder.takeFrame(), WebSocketOpCode::Text, "Hello");
    }

    /**
     * @brief 半成品帧状态在 reset() 后被清干净，不会影响下一条字节流
     */
    TEST(WebSocketFrame, ResetClearsPartialFrameState)
    {
        const std::string     frame = makeMaskedClientFrame(WebSocketOpCode::Text, "Hello");
        WebSocketFrameDecoder decoder;

        // 先让状态停在「负载收到一半」
        EXPECT_EQ(feed(decoder, std::string_view(frame.data(), 7)), WebSocketDecodeStatus::NeedMore);
        decoder.reset();

        // 若半成品状态没清干净，这里会从错误的阶段继续解析而拿不到帧
        EXPECT_EQ(feed(decoder, frame), WebSocketDecodeStatus::Frame);
        expectFrameEquals(decoder.takeFrame(), WebSocketOpCode::Text, "Hello");
    }

    /**
     * @brief 已产出的帧不会被后续喂入的字节覆盖，且未取走时一字节不吃
     */
    TEST(WebSocketFrame, PendingFrameIsNotOverwrittenAndConsumesNoByte)
    {
        const std::string     firstFrame  = makeMaskedClientFrame(WebSocketOpCode::Text, "one");
        const std::string     secondFrame = makeMaskedClientFrame(WebSocketOpCode::Text, "two");
        WebSocketFrameDecoder decoder;

        EXPECT_EQ(feed(decoder, firstFrame), WebSocketDecodeStatus::Frame);

        // 产出还没取走：再喂什么都只回 Frame，且不消费字节——那些字节属于下一帧
        EXPECT_EQ(feed(decoder, firstFrame + secondFrame), WebSocketDecodeStatus::Frame);
        EXPECT_EQ(decoder.consumedByteCount(), 0U);
        expectFrameEquals(decoder.takeFrame(), WebSocketOpCode::Text, "one");

        // 取走之后才继续消费：返回 Frame 时消费的正是第一帧的字节数
        EXPECT_EQ(feed(decoder, firstFrame + secondFrame), WebSocketDecodeStatus::Frame);
        EXPECT_EQ(decoder.consumedByteCount(), firstFrame.size());
        expectFrameEquals(decoder.takeFrame(), WebSocketOpCode::Text, "one");
    }

    /**
     * @brief 还没产出帧就取帧属于用法错误，必须抛异常而不是静默给一个空帧
     */
    TEST(WebSocketFrame, TakingFrameBeforeItIsReadyThrows)
    {
        WebSocketFrameDecoder decoder;

        EXPECT_THROW(static_cast<void>(decoder.takeFrame()), Base::LogicException);
        // 用法错误归 std::logic_error 分支，不并入运行期故障的捕获面
        EXPECT_THROW(static_cast<void>(decoder.takeFrame()), std::logic_error);
    }

    // ============================================================================
    // 编码器的用法错误（抛异常而不是产出非法帧）
    // ============================================================================

    /**
     * @brief 编码器拒绝控制帧超长、控制帧分片与未定义操作码，且 125 字节的上界放行
     */
    TEST(WebSocketFrame, EncoderRejectsControlFrameMisuse)
    {
        EXPECT_THROW(static_cast<void>(encodeWebSocketFrame(WebSocketOpCode::Ping, std::string(126, 'x'))), Base::InvalidArgumentException);
        EXPECT_THROW(static_cast<void>(encodeWebSocketFrame(WebSocketOpCode::Close, "x", false)), Base::InvalidArgumentException);
        EXPECT_THROW(static_cast<void>(encodeWebSocketFrame(static_cast<WebSocketOpCode>(0x3), "x")), Base::InvalidArgumentException);

        // 用途错误也留在 std::logic_error 这条分支上，便于调用方一次网住所有用法错误
        EXPECT_THROW(static_cast<void>(encodeWebSocketFrame(WebSocketOpCode::Ping, std::string(126, 'x'))), std::invalid_argument);

        // 125 字节是控制帧的合法上界：帧头 2 字节 + 负载 125 字节
        EXPECT_EQ(encodeWebSocketFrame(WebSocketOpCode::Ping, std::string(125, 'x')).size(), 127U);
    }

    /**
     * @brief 编码器的报错文案要写清上限与替代做法
     */
    TEST(WebSocketFrame, EncoderErrorMessageIsActionable)
    {
        try
        {
            static_cast<void>(encodeWebSocketFrame(WebSocketOpCode::Ping, std::string(126, 'x')));
            ADD_FAILURE() << "控制帧超长必须被拒";
        } catch (const Base::InvalidArgumentException &exception)
        {
            const std::string message = exception.what();

            EXPECT_TRUE(containsText(message, "125")) << "报错要带上限数值";
            EXPECT_TRUE(containsText(message, "Text/Binary")) << "报错要写清替代做法";
        }
    }
    /// RSV1 位：permessage-deflate 用它声明本条消息被压缩过（RFC 7692 §6）
    constexpr std::uint8_t kRsv1Bit = 0x40U;

    /// RSV2 位：本实现永远拒绝——RFC 7692 只让出了 RSV1
    constexpr std::uint8_t kRsv2Bit = 0x20U;

    /// 由 Python zlib 独立算出的 "hello" 压缩负载（裸 deflate + Z_SYNC_FLUSH，去掉尾部 00 00 FF FF）
    constexpr std::string_view kCompressedHelloPayload = "\xCA\x48\xCD\xC9\xC9\x07\x00";

    /**
     * @brief 拼一条置了 RSV1 的客户端数据帧
     * @details 先按普通帧拼好再置位首字节的 RSV1：不复用被测编码器，编码器出错时断言不会跟着错
     * @param opCode 操作码
     * @param payload 负载
     * @param isFinal 是否末帧
     * @return std::string 客户端帧字节
     */
    std::string makeMaskedCompressedFrame(const WebSocketOpCode opCode, const std::string_view payload, const bool isFinal = true)
    {
        std::string frame = makeMaskedClientFrame(opCode, payload, isFinal);
        frame[0]          = static_cast<char>(static_cast<std::uint8_t>(frame[0]) | kRsv1Bit);
        return frame;
    }

    // ============================================================================
    // permessage-deflate（RFC 7692）：RSV1 的编码与解码口径
    // ============================================================================

    /**
     * @brief 钉住编码器只在被要求压缩时置 RSV1，且只改首字节那一位
     */
    TEST(WebSocketFrame, EncoderSetsRsv1OnlyWhenCompressing)
    {
        const std::string plainFrame = encodeWebSocketFrame(WebSocketOpCode::Text, "hello");
        ASSERT_FALSE(plainFrame.empty());
        EXPECT_EQ(static_cast<std::uint8_t>(plainFrame[0]) & kRsv1Bit, 0U) << "未要求压缩的帧不得置 RSV1";

        const std::string compressedFrame = encodeWebSocketFrame(WebSocketOpCode::Text, "hello", true, true);
        EXPECT_EQ(static_cast<std::uint8_t>(compressedFrame[0]) & kRsv1Bit, kRsv1Bit) << "被要求压缩就必须置 RSV1";
        EXPECT_EQ(static_cast<std::uint8_t>(compressedFrame[0]) & 0x0FU, 0x01U) << "置 RSV1 不该动操作码";
        // 首字节之外逐字节相同：压缩只改那一位，负载由调用方给什么就发什么
        EXPECT_EQ(compressedFrame.substr(1), plainFrame.substr(1));
    }

    /**
     * @brief 钉住编码器拒绝把压缩标记写到控制帧与继续帧上（RFC 7692 §6.1）
     */
    TEST(WebSocketFrame, EncoderRejectsCompressionOnControlFrameAndContinuation)
    {
        try
        {
            static_cast<void>(encodeWebSocketFrame(WebSocketOpCode::Ping, "p", true, true));
            ADD_FAILURE() << "控制帧不得压缩，编码器应当场拒绝";
        } catch (const Base::InvalidArgumentException &exception)
        {
            EXPECT_TRUE(containsText(exception.what(), "RSV1")) << "原因要指出违规的是 RSV1，实际：" << exception.what();
        }

        try
        {
            static_cast<void>(encodeWebSocketFrame(WebSocketOpCode::Continuation, "c", true, true));
            ADD_FAILURE() << "继续帧不是消息首帧，不得置 RSV1";
        } catch (const Base::InvalidArgumentException &exception)
        {
            EXPECT_TRUE(containsText(exception.what(), "RSV1")) << "原因要指出违规的是 RSV1，实际：" << exception.what();
        }
    }

    /**
     * @brief 钉住没协商就使用扩展必须判错（RFC 6455 §5.2）
     */
    TEST(WebSocketFrame, DecoderRejectsRsv1WithoutNegotiation)
    {
        WebSocketFrameDecoder decoder;
        const std::string     reason = feedAndExpectError(decoder, makeMaskedCompressedFrame(WebSocketOpCode::Text, "hello"));
        EXPECT_TRUE(containsText(reason, "permessage-deflate")) << "原因要指出缺的是这项扩展协商，实际：" << reason;
    }

    /**
     * @brief 钉住协商之后 RSV1 被接受，且压缩标记随帧交给上层
     */
    TEST(WebSocketFrame, DecoderAcceptsRsv1AfterNegotiation)
    {
        WebSocketFrameDecoder decoder;
        decoder.setPerMessageDeflateEnabled(true);

        // 协商之后收到的未压缩帧照常交付，压缩标记必须是 false —— 不能一律当成压缩消息
        const WebSocketFrame plainFrame = feedAndTakeFrame(decoder, makeMaskedClientFrame(WebSocketOpCode::Text, "plain"));
        expectFrameEquals(plainFrame, WebSocketOpCode::Text, "plain");
        EXPECT_FALSE(plainFrame.isCompressed);

        const WebSocketFrame compressedFrame = feedAndTakeFrame(decoder, makeMaskedCompressedFrame(WebSocketOpCode::Text, kCompressedHelloPayload));
        expectFrameEquals(compressedFrame, WebSocketOpCode::Text, kCompressedHelloPayload);
        EXPECT_TRUE(compressedFrame.isCompressed) << "置了 RSV1 的消息必须带压缩标记交给上层去解压";
    }

    /**
     * @brief 钉住 RSV1 只属于数据消息首帧：协商之后控制帧与继续帧置 RSV1 依然判错
     */
    TEST(WebSocketFrame, DecoderRejectsRsv1OnControlFrameAndContinuation)
    {
        WebSocketFrameDecoder controlDecoder;
        controlDecoder.setPerMessageDeflateEnabled(true);
        const std::string controlReason = feedAndExpectError(controlDecoder, makeMaskedCompressedFrame(WebSocketOpCode::Close, ""));
        EXPECT_TRUE(containsText(controlReason, "控制帧")) << controlReason;

        WebSocketFrameDecoder continuationDecoder;
        continuationDecoder.setPerMessageDeflateEnabled(true);
        ASSERT_EQ(feed(continuationDecoder, makeMaskedClientFrame(WebSocketOpCode::Text, "he", false)), WebSocketDecodeStatus::NeedMore);
        const std::string continuationReason = feedAndExpectError(continuationDecoder, makeMaskedCompressedFrame(WebSocketOpCode::Continuation, "llo"));
        EXPECT_TRUE(containsText(continuationReason, "继续帧")) << continuationReason;
    }

    /**
     * @brief 钉住分片消息的压缩标记取自首帧，后续分片不带 RSV1 也改变不了结论
     */
    TEST(WebSocketFrame, DecoderKeepsCompressionFlagAcrossFragments)
    {
        WebSocketFrameDecoder decoder;
        decoder.setPerMessageDeflateEnabled(true);

        ASSERT_EQ(feed(decoder, makeMaskedCompressedFrame(WebSocketOpCode::Text, "AAA", false)), WebSocketDecodeStatus::NeedMore);
        const WebSocketFrame frame = feedAndTakeFrame(decoder, makeMaskedClientFrame(WebSocketOpCode::Continuation, "BBB"));
        expectFrameEquals(frame, WebSocketOpCode::Text, "AAABBB");
        EXPECT_TRUE(frame.isCompressed) << "压缩标记写在首帧上，重组出来的整条消息仍然是压缩消息";
    }

    /**
     * @brief 钉住 RSV2 与协商无关：即使协商过 permessage-deflate 也一律判错（RFC 7692 只让出了 RSV1）
     */
    TEST(WebSocketFrame, DecoderRejectsRsv2EvenAfterNegotiation)
    {
        WebSocketFrameDecoder decoder;
        decoder.setPerMessageDeflateEnabled(true);

        std::string frame        = makeMaskedClientFrame(WebSocketOpCode::Text, "x");
        frame[0]                 = static_cast<char>(static_cast<std::uint8_t>(frame[0]) | kRsv2Bit);
        const std::string reason = feedAndExpectError(decoder, frame);
        EXPECT_TRUE(containsText(reason, "RSV2")) << reason;
    }

    /**
     * @brief 钉住字级解掩码在任意分片相位下都与「朴素逐字节」逐字一致
     * @details 解掩码改为「先对齐到键相位 0、再按 4 字节一字异或、后收尾」以吃满向量化。分片输入下
     *          每段首字节的键下标可为 0..3，一旦对齐/尾处理有偏差就会让某个步长解错。用一条 33 字节
     *          （8 整字 + 1 尾）且字节值互不相同的负载，按 1..9、17、整段一次共多种步长喂入，逐一比对
     *          重组结果 == 原文；任何一种步长解错都当场红，是这条改动的证伪点。
     */
    TEST(WebSocketFrame, DecoderUnmasksIdenticallyAcrossEveryChunkStride)
    {
        std::string plaintext;
        for (std::size_t index = 0; index < 33; ++index)
        {
            plaintext.push_back(static_cast<char>(static_cast<std::uint8_t>(index * 7 + 1)));
        }
        const std::array<std::uint8_t, 4> maskKey{0x11, 0x22, 0x33, 0x44};
        const std::string                 clientFrame = makeMaskedClientFrame(WebSocketOpCode::Binary, plaintext, true, maskKey);

        const std::vector<std::size_t> strides{1, 2, 3, 4, 5, 6, 7, 9, 17, clientFrame.size()};
        for (const std::size_t stride: strides)
        {
            WebSocketFrameDecoder decoder;
            std::string           reassembled;
            std::size_t           offset = 0;
            while (offset < clientFrame.size())
            {
                const std::size_t           remaining   = clientFrame.size() - offset;
                const std::size_t           chunkLength = stride < remaining ? stride : remaining;
                const WebSocketDecodeStatus status      = decoder.parse(clientFrame.data() + offset, chunkLength);
                ASSERT_TRUE(status == WebSocketDecodeStatus::Frame || status == WebSocketDecodeStatus::NeedMore)
                        << "步长 " << stride << " 喂到 offset " << offset << " 判错：" << decoder.errorMessage();
                // 契约：NeedMore 吃满本段、Frame 只吃本帧字节；两者都按 consumedByteCount() 推进
                offset += decoder.consumedByteCount();
                if (status == WebSocketDecodeStatus::Frame)
                {
                    reassembled.append(decoder.takeFrame().payload);
                }
            }
            EXPECT_EQ(reassembled, plaintext) << "步长 " << stride << " 下解掩码结果与原文不一致";
        }
    }

} // namespace AsynGyanis::Net

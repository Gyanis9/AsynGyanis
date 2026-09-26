/**
 * @file ProtocolFuzzKernel.cpp
 * @brief 协议解码器属性化模糊的实现：按目标造输入、整体与逐字节两路驱动、六条不变量的判定
 * @author Gyanis
 * @date 2026-09-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Fuzz/ProtocolFuzzKernel.h"

#include "Net/Http2/Hpack.h"
#include "Net/Http2/Http2Frame.h"
#include "Net/Http3/Http3Frame.h"
#include "Net/WebSocket/WebSocketFrame.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace AsynGyanis::Net::Fuzz
{
    namespace
    {
        /// h2 / h3 解码器的本端上限：故意比样本输入大，让「畸形」而不是「超限」成为主要拒绝原因
        constexpr std::size_t kDecoderFrameLimitBytes = 64U * 1024U;

        /// 单轮驱动允许的最大迭代数。解码器只要不推进就会永远转下去，而模糊内核**必须**比被
        /// 测物更可靠：挂住或无限堆内存的话，报告里看到的就只有超时，没有「哪条不变量被破坏」。
        /// 上限取输入长度的 4 倍再加一段余量，正常路径远碰不到
        constexpr std::size_t maximumDriveSteps(const std::size_t inputLength) noexcept
        {
            return inputLength * 4U + 16U;
        }

        /**
         * @brief 一次驱动得到的可观测量：产出的帧序列 + 终态 + 总消费量
         */
        struct RunTrace
        {
            std::vector<std::string> frameKeys;              ///< 依次产出的帧标识（可比对的纯值文本）
            bool                     isEndedInError{false};  ///< 终态是否为错误
            bool                     isEndedNeedMore{false}; ///< 终态是否为「还要数据」
            std::size_t              consumedByteCount{0};   ///< 累计消费字节数
        };

        /**
         * @brief 用 parse/takeFrame 形状的解码器把 input 跑一遍
         * @tparam Decoder 解码器类型（WebSocketFrameDecoder / Http2FrameDecoder）
         * @tparam Status 三态枚举类型
         * @param decoder 已构造好的解码器（调用方负责每轮新建，保证两路驱动互不污染）
         * @param input 输入字节
         * @param chunkSize 每次喂入的字节数；1 就是「一次一字节」
         * @param describe 把产出帧折成可比对文本的回调
         * @return RunTrace 本轮轨迹；同时把不变量违例写进 errorText（空表示没违例）
         */
        template<typename Decoder, typename DescribeFrame>
        RunTrace driveParseStyle(Decoder &decoder, const std::string &input, const std::size_t chunkSize, DescribeFrame describe, std::string &errorText)
        {
            RunTrace trace;

            std::size_t offset = 0;
            for (std::size_t step = 0; offset < input.size(); ++step)
            {
                if (step > maximumDriveSteps(input.size()))
                {
                    errorText = "解码器不推进，驱动被判卡死（I2）";
                    return trace;
                }

                const std::size_t availableLength = std::min(chunkSize, input.size() - offset);
                const auto        status          = decoder.parse(input.data() + offset, availableLength);
                using Status                      = decltype(status);

                // 契约是「本次 parse 实际消费了多少」，因此每次都要按它推进 offset：
                // 产出一帧不等于没消费字节，不推进就是把同一段字节反复重喂
                const std::size_t consumed = decoder.consumedByteCount();
                if (consumed > availableLength)
                {
                    errorText = "单次消费超过本次喂入（I2）";
                    return trace;
                }
                offset += consumed;
                trace.consumedByteCount += consumed;

                if (status == Status::Frame)
                {
                    trace.frameKeys.push_back(describe(decoder.takeFrame()));
                    continue;
                }

                if (status == Status::Error)
                {
                    trace.isEndedInError = true;
                    return trace;
                }

                if (status != Status::NeedMore)
                {
                    errorText = "结论落在三类之外（I1）";
                    return trace;
                }

                if (consumed == 0)
                {
                    errorText = "NeedMore 却一个字节都不消费（I2）";
                    return trace;
                }
            }

            trace.isEndedNeedMore = !decoder.hasError();
            return trace;
        }

        /// WebSocket 帧的可比对标识：操作码、压缩位与负载字节
        std::string describeWebSocketFrame(const WebSocketFrame &frame)
        {
            return std::to_string(static_cast<int>(frame.opCode)) + '/' + (frame.isFinal ? "F" : "-") + (frame.isCompressed ? "C" : "-") + '/' + frame.payload;
        }

        /// HTTP/2 帧的可比对标识：帧类型、标志、流号、负载长度与负载字节
        std::string describeHttp2Frame(const Http2Frame &frame)
        {
            return std::to_string(static_cast<int>(frame.header.type)) + '/' + std::to_string(frame.header.flags) + '/' + std::to_string(frame.header.streamId) + '/' +
                   std::to_string(frame.header.payloadLength) + '/' + frame.payload;
        }

        /// 拼一个「掩码 + 定长」的合法客户端文本帧（服务端解码器要求客户端帧必须带掩码，RFC 6455 §5.3）
        std::string makeMaskedTextFrame(const std::string &payload)
        {
            std::string frame;
            frame.push_back(static_cast<char>(0x81U)); // FIN + TEXT
            frame.push_back(static_cast<char>(0x80U | static_cast<unsigned char>(payload.size())));
            static constexpr char kMaskKey[4] = {'k', 'e', 'y', '!'};
            frame.append(kMaskKey, 4U);
            for (std::size_t index = 0; index < payload.size(); ++index)
            {
                frame.push_back(static_cast<char>(static_cast<unsigned char>(payload[index]) ^ static_cast<unsigned char>(kMaskKey[index % 4U])));
            }
            return frame;
        }

        /// 为 WebSocket 目标造输入：合法帧头骨架 + 随机长度/标志位/负载
        std::string makeWebSocketInput(DeterministicRandom &random)
        {
            std::string       input         = makeMaskedTextFrame("hi");
            const std::size_t mutationCount = 1U + random.nextBelow(4U);
            for (std::size_t mutation = 0; mutation < mutationCount && !input.empty(); ++mutation)
            {
                const std::size_t position = random.nextBelow(input.size());
                input[position]            = static_cast<char>(static_cast<unsigned char>(random.next()));
            }
            const std::size_t tailLength = random.nextBelow(24U);
            for (std::size_t index = 0; index < tailLength; ++index)
            {
                input.push_back(static_cast<char>(static_cast<unsigned char>(random.next())));
            }
            return input;
        }

        /// 为 HTTP/2 目标造输入：9 字节帧头（长度/类型/标志/流号都随机，R 位偶尔置上）+ 随机负载
        std::string makeHttp2Input(DeterministicRandom &random)
        {
            const std::size_t   payloadLength = random.nextBelow(40U);
            const std::uint32_t declaredLength =
                    random.nextBelow(2) == 0U ? static_cast<std::uint32_t>(payloadLength) : static_cast<std::uint32_t>(random.nextBelow(1U << 18U)); // 偶尔与实际长度不符
            std::string header;
            header.push_back(static_cast<char>((declaredLength >> 16U) & 0xFFU));
            header.push_back(static_cast<char>((declaredLength >> 8U) & 0xFFU));
            header.push_back(static_cast<char>(declaredLength & 0xFFU));
            header.push_back(static_cast<char>(static_cast<unsigned char>(random.nextBelow(12U)))); // 帧类型：多数落在已定义区间
            header.push_back(static_cast<char>(static_cast<unsigned char>(random.nextBelow(64U)))); // 标志位
            const std::uint32_t streamId = random.nextBelow(2) == 0U ? 0U : static_cast<std::uint32_t>(random.nextBelow(512U));
            header.push_back(static_cast<char>(streamId & 0xFFU));
            header.push_back(static_cast<char>((streamId >> 8U) & 0xFFU));
            header.push_back(static_cast<char>((streamId >> 16U) & 0xFFU));
            header.push_back(static_cast<char>((streamId >> 24U) & 0x7FU)); // 最高位按 §4.1 必须为 0，偶尔置上才有变异价值
            if (random.nextBelow(4U) == 0U)
            {
                header[8] = static_cast<char>(static_cast<unsigned char>(header[8]) | 0x80U);
            }

            std::string input = header;
            for (std::size_t index = 0; index < payloadLength; ++index)
            {
                input.push_back(static_cast<char>(static_cast<unsigned char>(random.next())));
            }
            return input;
        }

        /// 为 HTTP/3 目标造输入：varint 帧类型 + varint 长度（1/2/4 字节编码都试）+ 随机负载
        std::string makeHttp3Input(DeterministicRandom &random)
        {
            std::string         input;
            const std::uint64_t frameType = random.nextBelow(2) == 0U ? 0x00U : random.nextBelow(0x100U); // DATA / HEADERS / 少量未知类型
            input.push_back(static_cast<char>(static_cast<unsigned char>(frameType & 0x3FU)));

            const std::size_t   payloadLength  = random.nextBelow(32U);
            const std::uint64_t declaredLength = random.nextBelow(2) == 0U ? static_cast<std::uint64_t>(payloadLength) : random.nextBelow(1U << 20U);
            if (declaredLength < 64U)
            {
                input.push_back(static_cast<char>(static_cast<unsigned char>(declaredLength & 0x3FU)));
            } else
            {
                input.push_back(static_cast<char>(static_cast<unsigned char>(0x40U | ((declaredLength >> 8U) & 0x3FU))));
                input.push_back(static_cast<char>(declaredLength & 0xFFU));
            }

            for (std::size_t index = 0; index < payloadLength; ++index)
            {
                input.push_back(static_cast<char>(static_cast<unsigned char>(random.next())));
            }
            return input;
        }

        /// 为 HPACK 目标造输入：多数轮拼一个**长度自洽**的字面量头块，少量轮直接用编码器产出的整块
        /// @details 只喂随机字节的话成功路径基本走不到（HPACK 状态机的大部分代码在解码成功之路上），
        ///          因此这里刻意让相当比例的输入是可解的——用例末尾的「既产出过帧、也判过错」正是
        ///          在钉这件事：哪边长期为 0，就是生成器退化了
        std::string makeHpackInput(DeterministicRandom &random)
        {
            static constexpr char kAlphabet[] = "abcxyz019:/- ";

            std::string input;
            input.push_back(static_cast<char>(0x20U));                // 不索引的字面量头字段，名字紧随其后
            const std::size_t nameLength = 1U + random.nextBelow(6U); // 名长（7 位前缀，单字节内）
            input.push_back(static_cast<char>(static_cast<unsigned char>(nameLength)));
            for (std::size_t index = 0; index < nameLength; ++index)
            {
                input.push_back(kAlphabet[random.nextBelow(sizeof(kAlphabet) - 1U)]);
            }
            const std::size_t valueLength = random.nextBelow(10U);
            input.push_back(static_cast<char>(static_cast<unsigned char>(valueLength)));
            for (std::size_t index = 0; index < valueLength; ++index)
            {
                input.push_back(kAlphabet[random.nextBelow(sizeof(kAlphabet) - 1U)]);
            }

            switch (random.nextBelow(4U))
            {
                case 0U:
                    // 整块换成编码器产出的合法头块：钉住「成功路径确实可解」这条参照
                    input = validInput(Target::HpackBlock);
                    break;
                case 1U:
                    // 长度域与内容故意不符：打拒绝分支
                    if (!input.empty())
                    {
                        input[input.size() - 1U] = static_cast<char>(static_cast<unsigned char>(random.next()));
                    }
                    break;
                case 2U:
                    // 尾部多一段：打「解完后剩余字节」的形态
                    input.push_back(static_cast<char>(static_cast<unsigned char>(random.next())));
                    break;
                default:
                    break;
            }
            return input;
        }

        /**
         * @brief 用 feed + nextFrame 形状的 h3 解码器把 input 跑一遍
         * @details h3 侧没有「本次消费多少」的出口，因此推进判据是喂入偏移、卡死判据是步数上限：
         *          外层每轮喂一块并就地取空帧，喂完再取一次即可收尾。对端字节的对错由解码器判，
         *          本函数只在「错误态没粘住」和「不推进」两处记违例
         * @param reader 目标解码器（调用方持有：I5 要在**同一个**对象上 reset() 后复跑，新建会绕过「粘滞没清干净」）
         * @param input 输入字节
         * @param chunkSize 每次 feed 的字节数；1 就是「一次一字节」
         * @param errorText 输出：不变量违例文案（空表示没违例）
         * @return RunTrace 本轮轨迹
         */
        RunTrace driveHttp3(Http3FrameReader &reader, const std::string &input, const std::size_t chunkSize, std::string &errorText)
        {
            RunTrace          trace;
            const std::size_t maximumSteps = maximumDriveSteps(input.size());

            std::size_t offset = 0;
            for (std::size_t step = 0; step <= maximumSteps; ++step)
            {
                const bool isDraining = offset >= input.size();
                if (!isDraining)
                {
                    const std::size_t availableLength = std::min(chunkSize, input.size() - offset);
                    const auto       *newBytes        = reinterpret_cast<const std::uint8_t *>(input.data() + offset);
                    offset += availableLength;
                    trace.consumedByteCount += availableLength;
                    if (const auto fed = reader.feed({newBytes, availableLength}); !fed.has_value())
                    {
                        // feed 只可能因「本块或缓冲超限」「已在错误态」失败，都是解码器的正当结论：
                        // 记进轨迹而不是违例，粘滞性由下面的复查钉
                        trace.isEndedInError = true;
                        return trace;
                    }
                }

                for (std::size_t frameStep = 0; frameStep <= maximumSteps; ++frameStep)
                {
                    const auto next = reader.nextFrame();
                    if (!next.has_value())
                    {
                        trace.isEndedInError = true;
                        // I4 粘滞：判错之后每一次调用都必须重复同一个错——复查能正常返回（哪怕只回
                        // 「还差字节」）就说明错误态被洗掉了，剩下的字节会被当成新数据重新解释
                        if (reader.nextFrame().has_value())
                        {
                            errorText = "h3 错误态没粘住，复查 nextFrame 不再报错（I4）";
                        }
                        return trace;
                    }
                    if (!next->has_value())
                    {
                        break; // 帧头或载荷还差字节：正常等待，不是错误
                    }
                    trace.frameKeys.push_back(std::to_string(http3FrameTypeValue(next->value())));
                    continue;
                }
                if (isDraining)
                {
                    trace.isEndedNeedMore = reader.pendingByteCount() > 0U;
                    return trace;
                }
            }

            errorText = "解码器不推进，驱动被判卡死（I2）";
            return trace;
        }

        /**
         * @brief 用一次性解码器解一个 HPACK 头块
         * @details 头块接口没有分片语义，因此这里的「两路驱动」是两次互相独立的整块解码——比的是**确定性**
         *          而不是增量等价（见 checkInvariants 的 HpackBlock 分支）
         * @param decoder 目标解码器（调用方持有：I5 要在**同一个**对象上 reset() 后复跑）
         * @param input 头块字节
         * @param errorText 输出：不变量违例文案（本目标只在「失败却留下字段」时写）
         * @return RunTrace 本轮轨迹；帧标识是 `名=值` 文本
         */
        RunTrace driveHpack(HpackDecoder &decoder, const std::string &input, std::string &errorText)
        {
            RunTrace                      trace;
            std::vector<HpackHeaderField> fields;
            std::string                   failureText;
            if (decoder.decode(input, fields, &failureText))
            {
                for (const auto &field: fields)
                {
                    trace.frameKeys.push_back(field.name + '=' + field.value);
                }
                trace.consumedByteCount = input.size();
                return trace;
            }

            trace.isEndedInError = true;
            // 契约写在 decode 的注释里：失败时字段表被清空。留下半截头块等于把伪造的头放出去
            if (!fields.empty())
            {
                errorText = "decode 失败却留下了字段（I6）";
            }
            return trace;
        }
    } // namespace

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
                static constexpr char kHexDigits[] = "0123456789abcdef";
                escaped.append("\\x");
                escaped.push_back(kHexDigits[(static_cast<unsigned char>(character) >> 4U) & 0x0FU]);
                escaped.push_back(kHexDigits[static_cast<unsigned char>(character) & 0x0FU]);
            }
        }
        return escaped;
    }

    std::string_view targetName(const Target target) noexcept
    {
        switch (target)
        {
            case Target::WebSocketFrame:
                return "WebSocketFrame";
            case Target::Http2Frame:
                return "Http2Frame";
            case Target::Http3Frame:
                return "Http3Frame";
            case Target::HpackBlock:
                return "HpackBlock";
            case Target::Count:
                break;
        }
        return "Unknown";
    }

    std::string makeInput(const Target target, DeterministicRandom &random, const std::size_t maximumLength)
    {
        std::string input;
        switch (target)
        {
            case Target::WebSocketFrame:
                input = makeWebSocketInput(random);
                break;
            case Target::Http2Frame:
                input = makeHttp2Input(random);
                break;
            case Target::Http3Frame:
                input = makeHttp3Input(random);
                break;
            case Target::HpackBlock:
                input = makeHpackInput(random);
                break;
            case Target::Count:
                break;
        }
        if (input.size() > maximumLength && maximumLength > 0)
        {
            input.resize(maximumLength);
        }
        return input;
    }

    std::string validInput(const Target target)
    {
        switch (target)
        {
            case Target::WebSocketFrame:
                return makeMaskedTextFrame("reset-probe");
            case Target::Http2Frame:
                return encodeHttp2SettingsFrame(Http2SettingsPayload{});
            case Target::Http3Frame:
            {
                std::string frame;
                frame.push_back(static_cast<char>(0x00)); // DATA
                frame.push_back(static_cast<char>(0x01)); // 长度 1
                frame.push_back('x');
                return frame;
            }
            case Target::HpackBlock:
            {
                // 用生产编码器生成：编解码互逆本就是它的契约，手写头块一旦与编码器口径有差，
                // I5 判的就是「我自己的假设」而不是解码器
                HpackEncoder                        encoder;
                const std::vector<HpackHeaderField> fields{{HpackHeaderField{.name = ":status", .value = "200"}, HpackHeaderField{.name = "content-type", .value = "text/plain"}}};
                return encoder.encode(fields);
            }
            case Target::Count:
                break;
        }
        return {};
    }

    std::string checkInvariants(const Target target, const std::string &input, RunStats *stats)
    {
        std::string errorText;

        // 把「本轮产出了什么」回填给调用方，供其累计成防空转的断言
        const auto collect = [&stats](const RunTrace &trace)
        {
            if (stats == nullptr)
            {
                return;
            }
            stats->producedFrameCount += trace.frameKeys.size();
            stats->isErrorEnd = trace.isEndedInError;
        };

        switch (target)
        {
            case Target::WebSocketFrame:
            {
                WebSocketFrameDecoder wholeDecoder;
                const RunTrace        whole = driveParseStyle(wholeDecoder, input, input.size(), describeWebSocketFrame, errorText);
                collect(whole);
                if (!errorText.empty())
                {
                    return errorText;
                }

                WebSocketFrameDecoder byteWiseDecoder;
                const RunTrace        byteWise = driveParseStyle(byteWiseDecoder, input, 1U, describeWebSocketFrame, errorText);
                if (!errorText.empty())
                {
                    return errorText;
                }
                if (whole.frameKeys != byteWise.frameKeys || whole.isEndedInError != byteWise.isEndedInError)
                {
                    return "整体喂与逐字节喂结论不同（I3）";
                }

                // I4 粘滞 + I6 越权产出：错误态下再喂任何字节，仍判错且一个字节都不消费
                if (whole.isEndedInError)
                {
                    if (wholeDecoder.parse("tail-more-bytes", 15U) != WebSocketDecodeStatus::Error || wholeDecoder.consumedByteCount() != 0U)
                    {
                        return "错误态没粘住，或还在消费字节（I4/I6）";
                    }
                }

                // I5 复位可用：在**同一个**解码器上 reset() 后必须还能解出合法输入。
                // 换成新建一个对象，这条就退化成「解码器能用」——粘滞没清干净恰好被绕过
                wholeDecoder.reset();
                const std::string probe      = validInput(target);
                const RunTrace    resetTrace = driveParseStyle(wholeDecoder, probe, probe.size(), describeWebSocketFrame, errorText);
                if (resetTrace.isEndedInError || resetTrace.frameKeys.empty())
                {
                    return "reset() 后合法输入解不出帧（I5）";
                }
                return {};
            }

            case Target::Http2Frame:
            {
                Http2FrameDecoder wholeDecoder(Http2FrameLimits{.maximumFrameSizeByteCount = kDecoderFrameLimitBytes});
                const RunTrace    whole = driveParseStyle(wholeDecoder, input, input.size(), describeHttp2Frame, errorText);
                collect(whole);
                if (!errorText.empty())
                {
                    return errorText;
                }

                Http2FrameDecoder byteWiseDecoder(Http2FrameLimits{.maximumFrameSizeByteCount = kDecoderFrameLimitBytes});
                const RunTrace    byteWise = driveParseStyle(byteWiseDecoder, input, 1U, describeHttp2Frame, errorText);
                if (!errorText.empty())
                {
                    return errorText;
                }
                if (whole.frameKeys != byteWise.frameKeys || whole.isEndedInError != byteWise.isEndedInError)
                {
                    return "整体喂与逐字节喂结论不同（I3）";
                }

                // I4 粘滞 + I6 越权产出：错误态下再喂任何字节，仍判错且一个字节都不消费
                if (whole.isEndedInError)
                {
                    const auto again = wholeDecoder.parse("tail-more-bytes", 15U);
                    if (again != decltype(again)::Error || wholeDecoder.consumedByteCount() != 0U)
                    {
                        return "错误态没粘住，或还在消费字节（I4/I6）";
                    }
                }

                // I5 复位可用：同一个对象 reset() 后必须还能解出合法帧（见 WebSocket 分支里同样的说明）
                wholeDecoder.reset();
                const std::string probe      = validInput(target);
                const RunTrace    resetTrace = driveParseStyle(wholeDecoder, probe, probe.size(), describeHttp2Frame, errorText);
                if (resetTrace.isEndedInError || resetTrace.frameKeys.empty())
                {
                    return "reset() 后合法输入解不出帧（I5）";
                }
                return {};
            }

            case Target::Http3Frame:
            {
                Http3FrameReader wholeReader(kDecoderFrameLimitBytes);
                const RunTrace   whole = driveHttp3(wholeReader, input, input.size(), errorText);
                collect(whole);
                if (!errorText.empty())
                {
                    return errorText;
                }
                Http3FrameReader byteWiseReader(kDecoderFrameLimitBytes);
                const RunTrace   byteWise = driveHttp3(byteWiseReader, input, 1U, errorText);
                if (!errorText.empty())
                {
                    return errorText;
                }
                if (whole.frameKeys != byteWise.frameKeys || whole.isEndedInError != byteWise.isEndedInError)
                {
                    return "整体喂与逐字节喂结论不同（I3）";
                }

                // I5 复位可用：同一个 reader reset() 后必须还能解出合法帧
                wholeReader.reset();
                const std::string probe = validInput(target);
                std::string       probeError;
                const RunTrace    resetTrace = driveHttp3(wholeReader, probe, probe.size(), probeError);
                if (!probeError.empty())
                {
                    return probeError;
                }
                if (resetTrace.isEndedInError || resetTrace.frameKeys.empty())
                {
                    return "reset() 后合法输入解不出帧（I5）";
                }
                return {};
            }

            case Target::HpackBlock:
            {
                HpackDecoder   firstDecoder;
                std::string    firstError;
                const RunTrace first = driveHpack(firstDecoder, input, firstError);
                collect(first);
                if (!firstError.empty())
                {
                    return firstError;
                }
                HpackDecoder   secondDecoder;
                std::string    secondError;
                const RunTrace second = driveHpack(secondDecoder, input, secondError);
                if (!secondError.empty())
                {
                    return secondError;
                }
                // 一次性解码器没有分片问题，但必须**确定**：同一份输入两次结果不同即状态泄漏
                if (first.frameKeys != second.frameKeys || first.isEndedInError != second.isEndedInError)
                {
                    return "同一输入两次解码结果不同（确定性）";
                }
                // I5 复位可用：同一个 decoder reset() 之后必须还能整块解出合法头块。
                // 不挂在 isEndedInError 分支下：从「干净态 reset()」也该可用，且失败路径往往正是那种
                firstDecoder.reset();
                std::string    resetError;
                const RunTrace resetTrace = driveHpack(firstDecoder, validInput(target), resetError);
                if (!resetError.empty())
                {
                    return resetError;
                }
                if (resetTrace.isEndedInError || resetTrace.frameKeys.empty())
                {
                    return "reset() 后合法头块解不出字段（I5）";
                }
                return {};
            }

            case Target::Count:
                break;
        }
        return "未知目标";
    }
} // namespace AsynGyanis::Net::Fuzz

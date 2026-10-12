/**
 * @file ProtocolFuzzKernel.cpp
 * @brief 协议解码器属性化模糊的实现：按目标造输入、整体与逐字节两路驱动、六条不变量的判定
 * @author Gyanis
 * @date 2026-09-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Fuzz/ProtocolFuzzKernel.h"

#include "Net/Http/Client/HttpResponseParser.h"
#include "Net/Http/HttpParser.h"
#include "Net/Http2/Hpack.h"
#include "Net/Http2/Http2Frame.h"
#include "Net/Http3/Http3Frame.h"
#include "Net/Http3/Qpack.h"
#include "Net/Quic/Codec/QuicFrame.h"
#include "Net/Quic/Codec/QuicPacketHeader.h"
#include "Net/Quic/Codec/QuicTransportParameters.h"
#include "Net/WebSocket/PerMessageDeflate.h"
#include "Net/WebSocket/WebSocketFrame.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
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

        /// 按 RFC 9000 §16 的最短编码写一个变长整数（只给生成器用，取值压在三字节档内）
        void appendFuzzVarint(std::string &bytes, const std::uint64_t value)
        {
            if (value < 64U)
            {
                bytes.push_back(static_cast<char>(static_cast<unsigned char>(value)));
            } else if (value < 16384U)
            {
                bytes.push_back(static_cast<char>(static_cast<unsigned char>(0x40U | (value >> 8U))));
                bytes.push_back(static_cast<char>(static_cast<unsigned char>(value & 0xFFU)));
            } else
            {
                bytes.push_back(static_cast<char>(static_cast<unsigned char>(0x80U | (value >> 24U))));
                bytes.push_back(static_cast<char>(static_cast<unsigned char>((value >> 16U) & 0xFFU)));
                bytes.push_back(static_cast<char>(static_cast<unsigned char>((value >> 8U) & 0xFFU)));
                bytes.push_back(static_cast<char>(static_cast<unsigned char>(value & 0xFFU)));
            }
        }

        /// 往串尾拍 n 个随机字节
        void appendFuzzRandomBytes(std::string &bytes, DeterministicRandom &random, const std::size_t count)
        {
            for (std::size_t index = 0; index < count; ++index)
            {
                bytes.push_back(static_cast<char>(static_cast<unsigned char>(random.next())));
            }
        }

        /**
         * @brief 为 HTTP/1 请求解析器造一份输入：请求行 + 头部块 + 空行 + 正文四段各自变异
         * @details 这段是 TCP 上最先被外部打到的解码器，而它的拒绝面还是分层的（414/431/400/413 各归一类），
         *          纯随机字节只会停在「首行凑不齐两个空格」那一层。所以先拼自洽骨架再变异：请求目标在
         *          普通路径、带查询、绝对形式、`*`、含空白与控制字符、超长这几档里轮转；头部条数 0..5，
         *          名与值偶尔掺控制字符或拍一段随机字节；正文形态轮转「无 / Content-Length 相符 /
         *          声称比实给多 / 零长 / 分块合法 / 分块大小是垃圾 / 分块永不收尾 / 分块后带 trailer /
         *          urlencoded / JSON / multipart」——前八档是历史缺陷的形状，后三档专门配出触发头与
         *          可解正文，让 Cookie / 表单 / multipart / JSON 四个已链解码器每轮都有机会真跑。
         */
        std::string makeHttp1RequestInput(DeterministicRandom &random)
        {
            static constexpr std::array<std::string_view, 8> kMethods{"GET", "POST", "HEAD", "PUT", "DELETE", "CONNECT", "OPTIONS", "TRACE"};
            static constexpr std::array<std::string_view, 7> kHeaderNames{"Host", "host", "Content-Length", "content-length", "Transfer-Encoding", "X-Test", "Accept"};

            std::string input;
            if (const std::size_t methodIndex = random.nextBelow(9U); methodIndex < kMethods.size())
            {
                input += kMethods[methodIndex];
            } else
            {
                appendFuzzRandomBytes(input, random, 1U + random.nextBelow(5U)); // 垃圾动词：token 位上的非法字符
            }
            input += ' ';

            switch (random.nextBelow(6U))
            {
                case 0U:
                    input += "/index.html";
                    break;
                case 1U:
                    input += "/a?x=1&y=%20";
                    break;
                case 2U:
                    input += "http://example.com/p";
                    break;
                case 3U:
                    input += "*";
                    break;
                case 4U:
                    input += "/a b\x01"
                             "c";
                    break;
                default:
                    input += '/';
                    appendFuzzRandomBytes(input, random, 64U + random.nextBelow(200U)); // 超长请求目标：打 uri 那道闸
                    break;
            }

            // 版本位绝大多数是 1.1，偶尔换一个两位数的怪版本（本层按形态判而不按语义判，值得撞一次）
            input += random.nextBelow(8U) == 0U ? " HTTP/9.9\r\n" : " HTTP/1.1\r\n";

            const std::size_t headerCount = random.nextBelow(6U);
            bool              hasHost     = false;
            for (std::size_t index = 0; index < headerCount; ++index)
            {
                const std::string_view name = kHeaderNames[index % kHeaderNames.size()];
                if (name == "Host" || name == "host")
                {
                    hasHost = true;
                }
                input += name;
                input += ": ";
                switch (random.nextBelow(4U))
                {
                    case 0U:
                        input += "sample-value";
                        break;
                    case 1U:
                        break; // 空值：合法形态之一
                    case 2U:
                        input += "a\x01"
                                 "b";
                        break;
                    default:
                        appendFuzzRandomBytes(input, random, 1U + random.nextBelow(40U));
                        break;
                }
                input += "\r\n";
            }
            if (!hasHost && random.nextBelow(4U) != 0U)
            {
                // HTTP/1.1 缺 Host 是有拒绝面的：四条里给三条补上，别让每轮都停在同一道闸前
                input += "Host: example.com\r\n";
            }

            // 触发头：上面那张名字表只有 7 个、头部条数最多 5 条，模不出来 Cookie；而没有它，
            // describeHttp1Request 里的 cookies() 调用恒走早退（「调用了」不等于「驱动了」）
            if (random.nextBelow(2U) == 0U)
            {
                input += "Cookie: session=abc; theme=dark-mode\r\n";
            }

            std::string body;
            // 先定分帧形态：8/9/10 是三档解码器触发（urlencoded / JSON / multipart），
            // 它们要走一条自洽骨架而不是穿过上面的随机头段
            const std::size_t framing                 = random.nextBelow(11U);
            const bool        isDecoderTriggerFraming = framing >= 8U;
            if (isDecoderTriggerFraming)
            {
                // 触发档把随机头段的产出整体换掉：那张表按设计就放着重名与垃圾值（Host/host 与
                // Content-Length/content-length 成对出现、值有 1/4 概率带 \x01），重名头与坏值头
                // 都会被解析器拒；触发轮再自带 Content-Type/Content-Length，于是没有一轮到得了
                // Done——四个解码器的命中计数结构性恒零，「补了调用」等于空转（实测 21/21 轮全
                // 死在重复头/坏值上）。已消耗的抽样照旧消耗（随机流不动），只是这段产出换掉，
                // 让触发轮判的是解码器而不是头表的拒绝面
                input = "POST /upload HTTP/1.1\r\nHost: example.com\r\n";
            }
            switch (framing)
            {
                case 1U:
                    input += "Content-Length: 5\r\n";
                    body = "hello";
                    break;
                case 2U:
                    input += "Content-Length: 50\r\n"; // 声称的比实给的多：该停在「还要数据」
                    body = "hello";
                    break;
                case 3U:
                    input += "Content-Length: 0\r\n";
                    break;
                case 4U:
                    input += "Transfer-Encoding: chunked\r\n";
                    body = "5\r\nhello\r\n0\r\n\r\n";
                    break;
                case 5U:
                    input += "Transfer-Encoding: chunked\r\n";
                    body = "zz\r\nhello\r\n"; // 块大小行是垃圾
                    break;
                case 6U:
                    input += "Transfer-Encoding: chunked\r\n";
                    body = "5\r\nhello\r\n"; // 永不收尾的分块
                    break;
                case 7U:
                    input += "Transfer-Encoding: chunked\r\n";
                    body = "5\r\nhello\r\n0\r\nX-Trail: 1\r\n\r\n"; // 收完还带 trailer 段
                    break;
                case 8U:
                    // 表单形态：urlencoded 的触发头与正文配对，formFields 的解析器才真跑
                    input += "Content-Type: application/x-www-form-urlencoded\r\n";
                    body = "a=1&b=%20two&empty=";
                    input += "Content-Length: " + std::to_string(body.size()) + "\r\n";
                    break;
                case 9U:
                    // JSON 形态：jsonBody 走完整的配置值解析（对象、数组、null 三支都进）
                    input += "Content-Type: application/json\r\n";
                    body = "{\"a\":[1,2],\"b\":{\"c\":\"d\"},\"e\":null}";
                    input += "Content-Length: " + std::to_string(body.size()) + "\r\n";
                    break;
                case 10U:
                    // multipart 形态：分段器跑到收尾边界才算解出，边界名与正文里成对
                    input += "Content-Type: multipart/form-data; boundary=fuzzb\r\n";
                    body = "--fuzzb\r\nContent-Disposition: form-data; name=\"f\"; filename=\"a.txt\"\r\nContent-Type: text/plain\r\n\r\nhello\r\n--fuzzb--";
                    input += "Content-Length: " + std::to_string(body.size()) + "\r\n";
                    break;
                default:
                    break;
            }

            input += "\r\n"; // 头部块收尾
            // 触发档不插流水线：插在头与正文之间时，那条「下一条请求」会被当成本条请求的正文吃掉，
            // 解码器解到的是乱串（字段恒空），触发轮的命中又会归零
            if (!isDecoderTriggerFraming && random.nextBelow(3U) == 0U)
            {
                input += "GET /next HTTP/1.1\r\nHost: example.com\r\n\r\n"; // 流水线里的下一条
            }
            input += body;
            if (random.nextBelow(4U) == 0U)
            {
                appendFuzzRandomBytes(input, random, 1U + random.nextBelow(8U));
            }
            return input;
        }

        /// 一条解出的 HTTP/1 请求的可比对标识：方法、版本、路径、逐条头部、正文，以及四个已链进
        /// 闭包却从没被调用过的解码入口（Cookie / 表单 / multipart / JSON）的解码结果
        /// @param request 解出的请求
        /// @param stats 分项命中账（可空）：四个解码器各自解出过东西才计数，供防空转断言判「真驱动了」
        std::string describeHttp1Request(const HttpRequest &request, RunStats *stats)
        {
            std::string text =
                    std::to_string(static_cast<int>(request.method())) + '/' + request.httpVersion() + '/' + std::string(request.path()) + '/' + std::string(request.body());
            request.forEachHeaderField([&text](const std::string_view name, const std::string_view value) { text += " {" + std::string(name) + ':' + std::string(value) + '}'; });

            // HttpCookie.cpp 与 MultipartForm.cpp 本来就在模糊闭包里（fuzz-net.sh 列它们是为了链接期不报
            // 未定义符号），可这两个解码器一次都没被模糊器推到过——链上不等于覆盖。这里真调一遍，把解出的
            // 形状并进同一条可比对标识：解错或解崩都会当场体现在 trace 上，差分判据也就覆盖到这一段
            const std::vector<HttpCookie> cookies = request.cookies();
            if (stats != nullptr && !cookies.empty())
            {
                ++stats->cookieDecodeHitCount;
            }
            for (const auto &cookie: cookies)
            {
                text += " [cookie:" + cookie.name() + '=' + cookie.value() + ']';
            }

            // unordered_map 的两份实例不保证同序，直接拼会让「按值出口 vs 出参出口」那类比对出现假差异
            const std::unordered_map<std::string, std::string> formFields = request.formFields();
            if (stats != nullptr && !formFields.empty())
            {
                ++stats->formDecodeHitCount;
            }
            std::vector<std::string> formPairs;
            formPairs.reserve(formFields.size());
            for (const auto &[name, value]: formFields)
            {
                formPairs.push_back(name + '=' + value);
            }
            std::ranges::sort(formPairs);
            for (const auto &pair: formPairs)
            {
                text += " [form:" + pair + ']';
            }

            // 段数与每段名足以逼 multipart 分段器跑完整条状态机；非法边界由它自己回空，不在这里判
            if (const auto multipart = request.multipartForm(); multipart.has_value())
            {
                if (stats != nullptr)
                {
                    ++stats->multipartDecodeHitCount;
                }
                const auto &parts = multipart->parts();
                text += " [multipart:" + std::to_string(parts.size()) + ']';
                for (const auto &part: parts)
                {
                    // 三个文本成员都拼进去：filename 的目录剥离与 Content-Type 的空值分支也在被推的那条路上
                    text += " [part:" + part.name + '/' + part.fileName + '/' + part.contentType + '/' + std::to_string(part.content.size()) + ']';
                }
            }

            // JSON 值模型太大且在本层之外验过，这里只要「解得开」这件事进 trace：语法错误回空，不进这一支
            if (request.jsonBody().has_value())
            {
                if (stats != nullptr)
                {
                    ++stats->jsonDecodeHitCount;
                }
                text += " [json]";
            }
            return text;
        }

        /**
         * @brief 用 HTTP/1 请求解析器把 input 跑一遍（parse 三态 + consumedByteCount + request()）
         * @details Done 是「解出一条报文」的边界，与 h2 的 Frame 同位；解完立刻 reset() 再喂剩余字节，
         *          这与服务端的流水线口径一致（HttpSession 就是在 Done 之后复位）。不复位时本层是
         *          「一字节都不吃地停在 Complete」，那是契约而不是违例，因此判据必须按流水线的走法来。
         * @tparam Parser HttpParser
         */
        template<typename Parser>
        RunTrace driveHttp1(Parser &parser, const std::string &input, const std::size_t chunkSize, std::string &errorText, RunStats *stats)
        {
            RunTrace    trace;
            std::size_t offset = 0;

            for (std::size_t step = 0; offset < input.size(); ++step)
            {
                if (step > maximumDriveSteps(input.size()))
                {
                    errorText = "解析器不推进，驱动被判卡死（I2）";
                    return trace;
                }

                const std::size_t availableLength = std::min(chunkSize, input.size() - offset);
                const ParseStatus status          = parser.parse(input.data() + offset, availableLength);
                const std::size_t consumed        = parser.consumedByteCount();
                if (consumed > availableLength)
                {
                    errorText = "单次消费超过本次喂入（I2）";
                    return trace;
                }
                offset += consumed;
                trace.consumedByteCount += consumed;

                if (status == ParseStatus::Done)
                {
                    trace.frameKeys.push_back(describeHttp1Request(parser.request(), stats));
                    parser.reset(); // 流水线：一条报文解完即复位，剩余字节属于下一条
                    continue;
                }
                if (status == ParseStatus::Error)
                {
                    trace.isEndedInError = true;
                    return trace;
                }
                if (status != ParseStatus::NeedMore)
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

            trace.isEndedNeedMore = !parser.hasError();
            return trace;
        }

        /**
         * @brief 为 QUIC 报文头目标造输入：长头与短头两形轮转，Length 域多数轮按真实剩余自洽
         * @details 这段解码器是 UDP 上最先被外部打到的，且**不需要任何密钥**就能走进去：首字节、版本、
         *          两条连接标识的长度、Token 长度、Length 域、包号长度位全是发包方可控的字段。
         *          纯随机字节多半停在「首字节最高位不是 1」那一层，因此先拼自洽骨架再变异：多数轮 Length
         *          与剩余一致（走通成功路径），少数轮故意偏大 / 清零 / 差一，版本偶尔换成 0 或 2——
         *          这些正是 §17.2/§17.3 拒绝面的形状。
         */
        std::string makeQuicPacketInput(DeterministicRandom &random)
        {
            std::string       input;
            const std::size_t destinationLength = random.nextBelow(9U);
            const std::size_t sourceLength      = random.nextBelow(9U);
            const std::size_t tokenLength       = random.nextBelow(2) == 0U ? 0U : 1U + random.nextBelow(7U);
            const std::size_t payloadLength     = 1U + random.nextBelow(24U);
            const std::size_t packetNumberBytes = 1U + random.nextBelow(4U);

            if (random.nextBelow(3U) == 0U)
            {
                // 短头（1-RTT）：线上既没有长度字段也没有连接标识长度，后者只能由调用方按本端签发的那个传
                input.push_back(static_cast<char>(static_cast<unsigned char>(0x40U | random.nextBelow(16U))));
                appendFuzzRandomBytes(input, random, destinationLength);
                appendFuzzRandomBytes(input, random, packetNumberBytes + payloadLength);
                return input;
            }

            const std::size_t typeBits = random.nextBelow(4U);
            input.push_back(static_cast<char>(static_cast<unsigned char>(0xC0U | (typeBits << 4U) | (packetNumberBytes - 1U))));
            // 版本：大多种子是 1，少量换成 0（版本协商）或 2（本层按 Malformed 拒）
            const std::uint64_t version = random.nextBelow(8U) == 0U ? random.nextBelow(3U) : 1U;
            input.push_back(static_cast<char>(static_cast<unsigned char>((version >> 24U) & 0xFFU)));
            input.push_back(static_cast<char>(static_cast<unsigned char>((version >> 16U) & 0xFFU)));
            input.push_back(static_cast<char>(static_cast<unsigned char>((version >> 8U) & 0xFFU)));
            input.push_back(static_cast<char>(static_cast<unsigned char>(version & 0xFFU)));

            input.push_back(static_cast<char>(static_cast<unsigned char>(destinationLength)));
            appendFuzzRandomBytes(input, random, destinationLength);
            input.push_back(static_cast<char>(static_cast<unsigned char>(sourceLength)));
            appendFuzzRandomBytes(input, random, sourceLength);

            appendFuzzVarint(input, tokenLength);
            appendFuzzRandomBytes(input, random, tokenLength);

            const std::uint64_t declaredLength = packetNumberBytes + payloadLength;
            switch (random.nextBelow(6U))
            {
                case 0U:
                    appendFuzzVarint(input, random.nextBelow(1U << 17U)); // 偏大：字段越出数据报末尾
                    break;
                case 1U:
                    appendFuzzVarint(input, 0U); // 零长：§17.2 明文禁止
                    break;
                case 2U:
                    appendFuzzVarint(input, declaredLength - 1U); // 差一：正好切在包号或载荷中间
                    break;
                default:
                    appendFuzzVarint(input, declaredLength); // 自洽：成功路径的参照
                    break;
            }
            appendFuzzRandomBytes(input, random, packetNumberBytes + payloadLength);
            return input;
        }

        /**
         * @brief 为 QUIC 帧序列目标造输入：先用**生产编码器**拍 1～3 帧，再按轮次变异
         * @details 与 HPACK 那一档同理——编解码互逆本就是契约，手写帧头一旦与编码器口径有差，
         *          判的就是「我自己的假设」而不是解码器。变异集中在三处：截掉尾部（打 Truncated）、
         *          改首字节类型位（打未定义类型）、尾部补一段（打「解完仍有剩余」）。
         */
        std::string makeQuicFrameInput(DeterministicRandom &random)
        {
            std::string input;

            // 每帧现拍现编：解出的帧持的是**指向载荷的视图**，先把几帧攒进一个 vector 再统一编码，
            // 那些视图会随缓冲重新分配或复用而失效——生成器自己就成了野指针来源
            const std::size_t frameCount = 1U + random.nextBelow(3U);
            for (std::size_t index = 0; index < frameCount; ++index)
            {
                std::vector<std::uint8_t> payloadBytes;

                switch (random.nextBelow(5U))
                {
                    case 0U:
                        appendQuicFrame(input, QuicPingFrame{});
                        break;
                    case 1U:
                        appendQuicFrame(input, QuicPaddingFrame{});
                        break;
                    case 2U:
                    {
                        const std::size_t dataLength = random.nextBelow(12U);
                        for (std::size_t byte = 0; byte < dataLength; ++byte)
                        {
                            payloadBytes.push_back(static_cast<std::uint8_t>(random.next()));
                        }
                        QuicStreamFrame stream;
                        stream.streamId = random.nextBelow(64U);
                        stream.offset   = random.nextBelow(2U) == 0U ? 0U : random.nextBelow(1024U);
                        stream.isFinal  = random.nextBelow(2U) == 0U;
                        stream.data     = std::span<const std::uint8_t>(payloadBytes);
                        appendQuicFrame(input, stream);
                        break;
                    }
                    case 3U:
                    {
                        QuicCryptoFrame crypto;
                        crypto.offset = random.nextBelow(4U) == 0U ? 0U : random.nextBelow(256U);
                        appendQuicFrame(input, crypto);
                        break;
                    }
                    default:
                    {
                        QuicAcknowledgementFrame acknowledgement;
                        acknowledgement.largestAcknowledgedPacketNumber = random.nextBelow(1U << 20U);
                        acknowledgement.acknowledgementDelay            = random.nextBelow(1U << 14U);
                        // 编码器要求至少一个区间（线格式的 First ACK Range 恒描述含最大包号那一段），
                        // 给一个「只认最大包号」的最小区间：这是可编码形态，不是生成器偷懒
                        acknowledgement.ranges.push_back(
                                QuicAcknowledgementRange{acknowledgement.largestAcknowledgedPacketNumber, acknowledgement.largestAcknowledgedPacketNumber});
                        appendQuicFrame(input, acknowledgement);
                        break;
                    }
                }
            }

            if (input.empty())
            {
                appendFuzzRandomBytes(input, random, 1U + random.nextBelow(8U));
            }
            switch (random.nextBelow(4U))
            {
                case 0U:
                    input.resize(input.size() / 2U); // 切在半帧中间
                    break;
                case 1U:
                    input[0] = static_cast<char>(static_cast<unsigned char>(random.next())); // 类型域换成未知值
                    break;
                case 2U:
                    appendFuzzRandomBytes(input, random, 1U + random.nextBelow(5U)); // 解完仍有剩余
                    break;
                default:
                    break;
            }
            return input;
        }

        /**
         * @brief 为 QUIC 传输参数目标造输入：编码器产出的合法块 + 四档变异
         * @details §7.3 的完备性校验（`initial_source_connection_id` 必须在、不得重复、取值长度与范围要合、
         *          末尾不得有余）是一段一段边界逻辑，纯随机字节一条都碰不到，所以骨架必须真解得开。
         */
        std::string makeQuicParametersInput(DeterministicRandom &random)
        {
            QuicTransportParameters parameters;
            parameters.initialSourceConnectionId      = std::vector<std::uint8_t>(4U, static_cast<std::uint8_t>(random.next()));
            parameters.maximumIdleTimeoutMilliseconds = random.nextBelow(1U << 15U);
            parameters.initialMaximumData             = random.nextBelow(1U << 16U);
            parameters.maximumUdpPayloadSize          = 1200U + random.nextBelow(64U);

            std::string input;
            appendQuicTransportParameters(input, parameters);

            switch (random.nextBelow(5U))
            {
                case 0U:
                    input.resize(input.size() / 2U); // 切在参数值中间
                    break;
                case 1U:
                    input.push_back(static_cast<char>(static_cast<unsigned char>(random.next()))); // 末尾余一个字节
                    break;
                case 2U:
                    if (!input.empty())
                    {
                        input[0] = static_cast<char>(static_cast<unsigned char>(random.next())); // 首个标识符换成未知项或非法长度
                    }
                    break;
                case 3U:
                    input.clear(); // 空参数串：缺 initial_source_connection_id，必须被拒
                    break;
                default:
                    break;
            }
            return input;
        }

        /// QPACK 合法样本共用的字段集：validInput 与「编解码互逆」对照必须逐字段对得上，故只此一份
        std::vector<QpackHeaderField> sampleQpackFields()
        {
            std::vector<QpackHeaderField> fields;
            fields.push_back(QpackHeaderField{.name = ":method", .value = "GET"});
            fields.push_back(QpackHeaderField{.name = "x-qpack-probe", .value = "reset"});
            return fields;
        }

        /// QPACK 解码器的本端约束（模糊档）：容量与阻塞流数都取非零，让插入/Blocked 两条路都能被随机字节走到；
        /// 字段段上限 64 KiB 与 h3 侧默认同档，越限本就会判 FieldSectionTooLarge（拒绝面之一，不是违例）
        constexpr QpackDecoderSettings kQpackFuzzDecoderSettings{
                .maximumTableCapacityByteCount = 4096U, .maximumBlockedStreamCount = 4U, .maximumFieldSectionSizeByteCount = 64U * 1024U};

        /**
         * @brief 为 QPACK 目标造输入
         * @details 对端在 QPACK 上有两条可控字节流：编码器流指令与头块。生成器按这两种语境轮转产合法样本
         *          （各由生产编码器产出：容量 0 → 纯字面量头块；容量 4096 → 含容量指令与插入的指令流），
         *          其余轮用近失败与纯随机字节打拒绝与挂起分支
         */
        std::string makeQpackInput(DeterministicRandom &random)
        {
            static constexpr char kAlphabet[] = "abcxyz019:/- ";

            std::vector<QpackHeaderField> fields;
            const std::size_t             fieldCount = 1U + random.nextBelow(3U);
            for (std::size_t index = 0; index < fieldCount; ++index)
            {
                QpackHeaderField  field;
                const std::size_t nameLength = 1U + random.nextBelow(6U);
                for (std::size_t character = 0; character < nameLength; ++character)
                {
                    field.name.push_back(kAlphabet[random.nextBelow(sizeof(kAlphabet) - 1U)]);
                }
                const std::size_t valueLength = random.nextBelow(8U);
                for (std::size_t character = 0; character < valueLength; ++character)
                {
                    field.value.push_back(kAlphabet[random.nextBelow(sizeof(kAlphabet) - 1U)]);
                }
                fields.push_back(std::move(field));
            }

            std::string input;
            switch (random.nextBelow(5U))
            {
                case 0U:
                {
                    // 合法头块：对端容量为 0 的编码器产出的纯字面量表示，打解码成功路径
                    QpackEncoder encoder(0U, 0U, 0U);
                    std::string  headerBlock;
                    std::string  encoderStreamBytes;
                    if (encoder.encodeFieldSection(0U, fields, headerBlock, encoderStreamBytes).has_value())
                    {
                        input = headerBlock;
                    }
                    break;
                }
                case 1U:
                {
                    // 合法编码器流指令：容量变更 + 插入 + 引用，打 feedEncoderStream 的成功路径
                    QpackEncoder encoder(4096U, 4U, 4096U);
                    std::string  headerBlock;
                    std::string  encoderStreamBytes;
                    if (encoder.encodeFieldSection(0U, fields, headerBlock, encoderStreamBytes).has_value())
                    {
                        input = encoderStreamBytes;
                    }
                    break;
                }
                default:
                {
                    // 近失败与纯随机：首字节偶尔顶成索引型表示，打越界索引与 Required Insert Count 两道前缀闸
                    appendFuzzRandomBytes(input, random, 1U + random.nextBelow(24U));
                    if (random.nextBelow(2U) == 0U && !input.empty())
                    {
                        input[0] = static_cast<char>(static_cast<unsigned char>(0x80U | random.nextBelow(0x80U)));
                    }
                    break;
                }
            }
            if (input.empty())
            {
                appendFuzzRandomBytes(input, random, 8U);
            }
            return input;
        }

        /**
         * @brief 为 permessage-deflate 目标造输入
         * @details 两个面共用同一份字节：扩展要约头（对端可写任意值时不会被回显/接受）与压缩负载
         *          （对端发来的压缩消息）。生成器按两种成功语境轮转——合法要约头（带窗口参数），
         *          以及本端压缩器产出的真实 deflate 负载（解压成功路径的参照）——其余轮是近失败与随机
         */
        std::string makePerMessageDeflateInput(DeterministicRandom &random)
        {
            switch (random.nextBelow(5U))
            {
                case 0U:
                {
                    // 合法要约：窗口参数按 §7.1 的取值区间
                    std::string input = "permessage-deflate; client_max_window_bits=" + std::to_string(kWebSocketMinimumWindowBits + random.nextBelow(8));
                    if (random.nextBelow(2U) == 0U)
                    {
                        input += "; server_no_context_takeover";
                    }
                    return input;
                }
                case 1U:
                {
                    // 本端压缩器产出的真实负载：解压成功路径与往返互逆的参照。载荷刻意跨过 64 字节一档——
                    // 小载荷解出来太小，「上限是防爆闸」那一条永远触不到，挡板失效也没人看得出来
                    std::string payload;
                    appendFuzzRandomBytes(payload, random, 1U + random.nextBelow(256U));
                    const auto deflated = deflateWebSocketMessage(payload, kWebSocketDefaultWindowBits);
                    if (deflated.has_value())
                    {
                        return *deflated;
                    }
                    return payload;
                }
                case 2U:
                {
                    // 要约头的拒绝面形状：未知参数名、越界窗口位数、重复参数
                    static constexpr const char *kMalformedOffers[] = {"permessage-deflate; x-unknown=1", "permessage-deflate; server_max_window_bits=7",
                                                                       "permessage-deflate; client_max_window_bits=16", "permessage-deflate; server_no_context_takeover=1",
                                                                       "permessage-deflate; client_max_window_bits=8; client_max_window_bits=9"};
                    std::string                  input              = kMalformedOffers[random.nextBelow(sizeof(kMalformedOffers) / sizeof(kMalformedOffers[0]))];
                    if (random.nextBelow(2U) == 0U && !input.empty())
                    {
                        input[random.nextBelow(input.size())] = static_cast<char>(static_cast<unsigned char>(random.next()));
                    }
                    return input;
                }
                default:
                {
                    // 纯随机压缩负载：zlib 的拒绝面（头部、校验与回溯三类都有）
                    std::string input;
                    appendFuzzRandomBytes(input, random, 1U + random.nextBelow(48U));
                    return input;
                }
            }
        }

        /**
         * @brief 为出站 HTTP/1 响应解析器造输入
         * @details 三种定界的成功样本（定长 / chunked / 读到连接关闭）各占一份，其余轮打畸形拒绝面
         */
        std::string makeHttpResponseInput(DeterministicRandom &random)
        {
            switch (random.nextBelow(5U))
            {
                case 0U:
                {
                    const std::size_t bodyLength = random.nextBelow(16U);
                    std::string       input      = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(bodyLength) + "\r\n\r\n";
                    appendFuzzRandomBytes(input, random, bodyLength);
                    return input;
                }
                case 1U:
                {
                    // chunked：块大小行与块数据两条相位，末块后空 trailer 收尾
                    std::string       input      = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
                    const std::size_t chunkCount = 1U + random.nextBelow(3U);
                    for (std::size_t chunk = 0; chunk < chunkCount; ++chunk)
                    {
                        const std::size_t chunkLength = 1U + random.nextBelow(8U);
                        input += std::format("{:x}\r\n", chunkLength);
                        appendFuzzRandomBytes(input, random, chunkLength);
                        input += "\r\n";
                    }
                    input += "0\r\n\r\n";
                    return input;
                }
                case 2U:
                {
                    // 无定界头：读到连接关闭（endOfStream 收尾）；头部行本身随机，打头部解析
                    std::string       input       = "HTTP/1.1 200 OK\r\n";
                    const std::size_t headerCount = random.nextBelow(4U);
                    for (std::size_t index = 0; index < headerCount; ++index)
                    {
                        input += "x-fuzz-" + std::to_string(random.nextBelow(100U)) + ": ";
                        appendFuzzRandomBytes(input, random, random.nextBelow(8U));
                        input += "\r\n";
                    }
                    input += "\r\n";
                    appendFuzzRandomBytes(input, random, random.nextBelow(16U));
                    return input;
                }
                default:
                {
                    // 畸形：状态行/长度头/定界头冲突/分块大小行四个拒绝面各一条
                    static constexpr const char *kMalformedResponses[] = {
                            "HTTP/1.1 20",                                                                // 状态码不足三位
                            "HTTP/1.1 2000 OK\r\n\r\n",                                                   // 状态码超过三位
                            "HTTP/1.1 2x0 OK\r\n\r\n",                                                    // 状态码非数字
                            "HTTP/1.1 200 OK\r\nContent-Length: 12x\r\n\r\n",                             // 长度不是十进制
                            "HTTP/1.1 200 OK\r\nContent-Length: 8\r\nTransfer-Encoding: chunked\r\n\r\n", // 双定界头
                            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nz\r\n",                 // 非法块大小行
                    };
                    std::string input = kMalformedResponses[random.nextBelow(sizeof(kMalformedResponses) / sizeof(kMalformedResponses[0]))];
                    if (random.nextBelow(2U) == 0U && !input.empty())
                    {
                        input[random.nextBelow(input.size())] = static_cast<char>(static_cast<unsigned char>(random.next()));
                    }
                    return input;
                }
            }
        }

        /**
         * @brief 跑一遍 QPACK 编码器流会话：对端指令字节喂给一个全新解码器，把结果折成可比对的轨迹
         * @details 编码器流与头块是两条独立字节流、两条独立语境，因此各起一个全新会话（见下一条同族函数）。
         *          「同一个解码器把两种字节都吃一遍」会把两条语境搅在一起，确定性对照就说不清查的是谁
         */
        RunTrace driveQpackFeedSession(const std::string &input, std::string &errorText)
        {
            QpackDecoder                        decoder(kQpackFuzzDecoderSettings);
            const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t *>(input.data()), input.size());

            std::vector<std::uint64_t> unblockedStreamIds;
            std::string                decoderStreamBytes;
            const auto                 fed = decoder.feedEncoderStream(bytes, unblockedStreamIds, decoderStreamBytes);

            RunTrace trace;
            if (!fed.has_value())
            {
                trace.isEndedInError = true;
                trace.frameKeys.push_back(std::format("feed-error={}", static_cast<int>(fed.error().kind)));
                return trace;
            }
            if (*fed > input.size())
            {
                errorText = "单次消费超过本次喂入（I2）";
                return trace;
            }
            // 表记账折成摘要：两遍会话的插入数/表大小/项数必须逐个相同，否则就是状态在会话间泄漏
            trace.frameKeys.push_back(std::format("feed-ok={}/{}/{}/{}", *fed, decoder.insertCount(), decoder.dynamicTableSizeByteCount(), decoder.dynamicTableEntries().size()));
            return trace;
        }

        /**
         * @brief 跑一遍 QPACK 头块会话：对端头块字节喂给一个全新解码器
         * @details 契约：只有 Decoded 才交付字段行；Blocked（§2.2.1 的正常路径）与失败都不得留下半截产出
         */
        RunTrace driveQpackSectionSession(const std::string &input, std::string &errorText)
        {
            QpackDecoder                        decoder(kQpackFuzzDecoderSettings);
            const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t *>(input.data()), input.size());

            std::vector<QpackHeaderField> fields;
            std::string                   decoderStreamBytes;
            const auto                    decoded = decoder.decodeFieldSection(0U, bytes, fields, decoderStreamBytes);

            RunTrace trace;
            if (!decoded.has_value())
            {
                trace.isEndedInError = true;
                trace.frameKeys.push_back(std::format("decode-error={}", static_cast<int>(decoded.error().kind)));
                if (!fields.empty())
                {
                    errorText = "头块解码失败却留下了字段（I6）";
                }
                return trace;
            }
            if (*decoded == QpackFieldSectionDecodeStatus::Blocked)
            {
                trace.frameKeys.push_back("blocked");
                if (!fields.empty())
                {
                    errorText = "头块挂起（Blocked）却留下了字段（I6）";
                }
                return trace;
            }
            for (const auto &field: fields)
            {
                trace.frameKeys.push_back(field.name + '=' + field.value);
            }
            return trace;
        }

        /**
         * @brief 用 feed + 三态阶段机的出站响应解析器把 input 跑一遍
         * @details consumed 的契约是「本趟实际吃掉多少」，未吃掉的余量由调用方拼下次一起再喂——
         *          驱动就按这个走法维护 pending。喂完仍不完整时调 endOfStream()（读到连接关闭那条路的收尾）
         * @param parser 目标解析器（调用方持有：I5 要在**同一个**对象上 reset() 后复跑）
         * @param input 输入字节
         * @param chunkSize 每次取入的字节数；1 就是「一次一字节」
         * @param errorText 输出：不变量违例文案（空表示没违例）
         * @return RunTrace 本轮轨迹
         */
        RunTrace driveHttpResponse(HttpResponseParser &parser, const std::string &input, const std::size_t chunkSize, std::string &errorText)
        {
            RunTrace    trace;
            std::string pending;
            std::size_t offset = 0;
            std::size_t step   = 0;

            for (; step <= maximumDriveSteps(input.size()); ++step)
            {
                if (parser.isComplete() || parser.hasFailed())
                {
                    break;
                }
                if (pending.empty() && offset >= input.size())
                {
                    break;
                }
                if (offset < input.size())
                {
                    const std::size_t chunkLength = std::min(chunkSize, input.size() - offset);
                    pending.append(input, offset, chunkLength);
                    offset += chunkLength;
                }

                const std::size_t pendingBefore = pending.size();
                const std::size_t consumed      = parser.feed(pending);
                if (consumed > pendingBefore)
                {
                    errorText = "单次消费超过本次喂入（I2）";
                    return trace;
                }
                trace.consumedByteCount += consumed;
                pending.erase(0, consumed);

                // 余量非空且已无新字节可拼：解析器需要比手上更多才能推进，属正常等待，交给 endOfStream 收尾
                if (consumed == 0U && offset >= input.size())
                {
                    break;
                }
            }
            if (step > maximumDriveSteps(input.size()))
            {
                errorText = "解析器不推进，驱动被判卡死（I2）";
                return trace;
            }
            if (!parser.isComplete() && !parser.hasFailed())
            {
                parser.endOfStream();
            }

            trace.isEndedInError  = parser.hasFailed();
            trace.isEndedNeedMore = !parser.isComplete() && !parser.hasFailed();
            if (parser.isComplete())
            {
                const HttpResponseInfo &result = parser.result();
                trace.frameKeys.push_back(std::format("{}/{}/{}/{}", result.statusCode, result.headers.size(), result.body.size(), result.trailers.size()));
            }
            return trace;
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

    /**
     * @brief 把解出的帧里所有指向载荷的视图逐个核一遍「落在载荷之内」
     * @details 载荷视图是这一层的交付形状（CRYPTO/STREAM/NEW_TOKEN/NEW_CONNECTION_ID/CONNECTION_CLOSE
     *          各持有一段），帧结构本身不拥有字节。视图指到段外就是野指针，而 ASan 只在**真去读**时才报——
     *          模糊器解完就丢、未必读那段，所以这里当场按地址区间核，不等 sanitizer。
     * @param payload 载荷原文（视图必须整个落在它的范围内）
     * @param frames 解出来的帧序列
     * @return std::string 空串表示全部在界内；否则第一条越界说明
     */
    std::string quicFrameViewsInsidePayload(const std::span<const std::uint8_t> payload, const std::vector<QuicFrame> &frames)
    {
        const std::uintptr_t payloadBegin = reinterpret_cast<std::uintptr_t>(payload.data());
        const std::uintptr_t payloadEnd   = payloadBegin + payload.size();

        const auto checkSpan = [&](const std::string_view field, const std::span<const std::uint8_t> view) -> std::string
        {
            if (view.empty())
            {
                return {};
            }
            const std::uintptr_t viewBegin = reinterpret_cast<std::uintptr_t>(view.data());
            const std::uintptr_t viewEnd   = viewBegin + view.size();
            if (viewBegin < payloadBegin || viewEnd > payloadEnd)
            {
                return std::format("{} 的视图越出载荷：载荷 [{}, {})，视图 [{}, {})", field, payloadBegin, payloadEnd, viewBegin, viewEnd);
            }
            return {};
        };

        for (const QuicFrame &frame: frames)
        {
            std::string violation;
            if (const auto *stream = std::get_if<QuicStreamFrame>(&frame); stream != nullptr)
            {
                violation = checkSpan("STREAM", stream->data);
            } else if (const auto *crypto = std::get_if<QuicCryptoFrame>(&frame); crypto != nullptr)
            {
                violation = checkSpan("CRYPTO", crypto->data);
            } else if (const auto *token = std::get_if<QuicNewTokenFrame>(&frame); token != nullptr)
            {
                violation = checkSpan("NEW_TOKEN", token->token);
            } else if (const auto *identifier = std::get_if<QuicNewConnectionIdFrame>(&frame); identifier != nullptr)
            {
                violation = checkSpan("NEW_CONNECTION_ID 的连接标识", identifier->connectionId);
                if (violation.empty())
                {
                    violation = checkSpan("NEW_CONNECTION_ID 的无状态重置令牌", identifier->statelessResetToken);
                }
            } else if (const auto *closure = std::get_if<QuicConnectionCloseFrame>(&frame); closure != nullptr)
            {
                violation = checkSpan("CONNECTION_CLOSE 的原因短语", closure->reasonPhrase);
            }
            if (!violation.empty())
            {
                return violation;
            }
        }
        return {};
    }

    /**
     * @brief 把帧解码的两个出口（按值返回与写进调用方缓冲）跑一遍并核一致
     * @details 文档承诺两者「语义完全一致」，但它们是两条代码路径——一致就得当场核，不一致就得看得见。
     *          顺带在这里查出失败路径把半截产出留在调用方缓冲里（I6）与视图越界两类形状。
     * @param payload 载荷原文
     * @param errorText 输出：违例文案（空表示没违例）
     * @return std::size_t 成功时解出的帧数（失败返回 0，由 errorText 区分是拒绝还是违例）
     */
    std::size_t driveQuicFrames(const std::span<const std::uint8_t> payload, std::string &errorText)
    {
        const auto byValue = decodeQuicFrames(payload);

        std::vector<QuicFrame> reused;
        reused.reserve(4U); // 先占容量：出参那一份要证明「清空但不重建缓冲」这条路也不越界
        const auto byReference = decodeQuicFrames(payload, reused);

        if (byValue.has_value() != byReference.has_value())
        {
            errorText = "同一个载荷，按值出口与出参出口一个成功一个失败";
            return 0U;
        }
        if (!byValue.has_value())
        {
            if (!reused.empty())
            {
                errorText = "解码失败却把半截帧留在调用方的缓冲里（I6）";
            }
            return 0U;
        }
        if (byValue->size() != reused.size())
        {
            errorText = std::format("两路出口帧数不同：按值 {} 帧，出参 {} 帧", byValue->size(), reused.size());
            return 0U;
        }
        // 逐帧比类型值：不比字段里的字节视图，两路各自指向自己的缓冲来源
        for (std::size_t index = 0; index < byValue->size(); ++index)
        {
            if (quicFrameTypeValue((*byValue)[index]) != quicFrameTypeValue(reused[index]))
            {
                errorText = std::format("第 {} 帧的类型在两路出口间不同", index);
                return 0U;
            }
        }
        if (const std::string violation = quicFrameViewsInsidePayload(payload, reused); !violation.empty())
        {
            errorText = violation;
            return 0U;
        }
        return reused.size();
    }

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
            case Target::Http1Request:
                return "Http1Request";
            case Target::WebSocketFrame:
                return "WebSocketFrame";
            case Target::Http2Frame:
                return "Http2Frame";
            case Target::Http3Frame:
                return "Http3Frame";
            case Target::HpackBlock:
                return "HpackBlock";
            case Target::QuicPacket:
                return "QuicPacket";
            case Target::QuicFrameSequence:
                return "QuicFrameSequence";
            case Target::QuicParameters:
                return "QuicParameters";
            case Target::QpackBlock:
                return "QpackBlock";
            case Target::PerMessageDeflate:
                return "PerMessageDeflate";
            case Target::HttpResponse:
                return "HttpResponse";
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
            case Target::Http1Request:
                input = makeHttp1RequestInput(random);
                break;
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
            case Target::QuicPacket:
                input = makeQuicPacketInput(random);
                break;
            case Target::QuicFrameSequence:
                input = makeQuicFrameInput(random);
                break;
            case Target::QuicParameters:
                input = makeQuicParametersInput(random);
                break;
            case Target::QpackBlock:
                input = makeQpackInput(random);
                break;
            case Target::PerMessageDeflate:
                input = makePerMessageDeflateInput(random);
                break;
            case Target::HttpResponse:
                input = makeHttpResponseInput(random);
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
            case Target::Http1Request:
                // 一条自洽的完整请求：带查询的路径、定长正文，截断矩阵的每个前缀都落在「收行中」的某个相位上
                return "POST /upload?x=1 HTTP/1.1\r\nHost: example.com\r\nContent-Length: 5\r\n\r\nhello";
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
            case Target::QuicPacket:
            {
                // 一个自洽的最小 Initial：版本 1、8 字节目的标识、空源标识、无 Token、Length = 包号 4 + 载荷 1。
                // 直接拿 RFC 9001 A.2 那条 1200 字节的向量当样本也行，但截断矩阵要为此白跑 1200 轮，形态一样
                const std::string     destinationConnectionId = "\x83\x94\xc8\xf0\x3e\x51\x57\x08";
                static constexpr char kVersionBytes[]         = {'\x00', '\x00', '\x00', '\x01'}; // 版本 1：串里有空字节，只能按长度附加
                static constexpr char kPacketNumber[]         = {'\x00', '\x00', '\x00', '\x02'}; // 包号 2，同上
                std::string           packet;
                packet.push_back(static_cast<char>(0xC3)); // 长头 1|1 + Initial(00) + 保留位 0 + 包号长度 4
                packet.append(kVersionBytes, sizeof(kVersionBytes));
                packet.push_back(static_cast<char>(destinationConnectionId.size()));
                packet.append(destinationConnectionId);
                packet.push_back(static_cast<char>(0)); // 源标识长度 0
                packet.push_back(static_cast<char>(0)); // Token 长度 0
                packet.push_back(static_cast<char>(5)); // Length = 包号 4 + 载荷 1
                packet.append(kPacketNumber, sizeof(kPacketNumber));
                packet.push_back('x');
                return packet;
            }
            case Target::QuicFrameSequence:
            {
                std::string payload;
                appendQuicFrame(payload, QuicPingFrame{});
                return payload;
            }
            case Target::QuicParameters:
            {
                QuicTransportParameters parameters;
                parameters.initialSourceConnectionId = std::vector<std::uint8_t>(8U, 0x5aU); // 这一项缺失是硬错误，样本必须带上
                std::string bytes;
                appendQuicTransportParameters(bytes, parameters);
                return bytes;
            }
            case Target::QpackBlock:
            {
                // 用生产编码器生成（对端容量 0 → 纯字面量表示）：编解码互逆本就是契约，
                // 手写位模式一旦与实现的表示口径有差，I5 判的就是「我自己的假设」而不是解码器
                QpackEncoder encoder(0U, 0U, 0U);
                std::string  headerBlock;
                std::string  encoderStreamBytes;
                if (!encoder.encodeFieldSection(0U, sampleQpackFields(), headerBlock, encoderStreamBytes).has_value())
                {
                    return {};
                }
                return headerBlock;
            }
            case Target::PerMessageDeflate:
            {
                // 本端压缩器产出的真实负载：解压成功路径与往返互逆都靠它作参照
                const auto deflated = deflateWebSocketMessage("permessage-deflate reset probe", kWebSocketDefaultWindowBits);
                return deflated.has_value() ? *deflated : std::string{"permessage-deflate"};
            }
            case Target::HttpResponse:
                // 一条自洽的完整响应：定长正文收齐即 Complete，截断矩阵的每个前缀都落在某个相位上
                return "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";
            case Target::Count:
                break;
        }
        return {};
    }

    namespace
    {
        /// 各目标被真正解码过多少次（进程内累计）。模糊器多 worker 时每个 worker 一份，读的人按行聚合
        std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Target::Count)> g_targetCallCounts{};
    } // namespace

    std::array<std::uint64_t, static_cast<std::size_t>(Target::Count)> targetCallCounts()
    {
        std::array<std::uint64_t, static_cast<std::size_t>(Target::Count)> snapshot{};
        for (std::size_t index = 0; index < snapshot.size(); ++index)
        {
            snapshot[index] = g_targetCallCounts[index].load(std::memory_order_relaxed);
        }
        return snapshot;
    }

    std::string checkInvariants(const Target target, const std::string &input, RunStats *stats)
    {
        std::string errorText;

        // 记账排在一切之前：CI 判「每一档是不是都在被推」只认这一处计数。
        // Target 是公开枚举、调用方可以把它 cast 成越界值，所以这里按下标兜一层而不是假定它合法
        if (const auto index = static_cast<std::size_t>(target); index < g_targetCallCounts.size())
        {
            g_targetCallCounts[index].fetch_add(1, std::memory_order_relaxed);
        }

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
            case Target::Http1Request:
            {
                HttpParser     wholeParser;
                const RunTrace whole = driveHttp1(wholeParser, input, input.size(), errorText, stats);
                collect(whole);
                if (!errorText.empty())
                {
                    return errorText;
                }

                HttpParser     byteWiseParser;
                const RunTrace byteWise = driveHttp1(byteWiseParser, input, 1U, errorText, stats);
                if (!errorText.empty())
                {
                    return errorText;
                }
                if (whole.frameKeys != byteWise.frameKeys || whole.isEndedInError != byteWise.isEndedInError)
                {
                    return "整体喂与逐字节喂结论不同（I3）";
                }

                // I4 粘滞 + I6 越权产出：失败之后继续喂任何字节仍判错、一字节不吃，也不交半截报文
                if (whole.isEndedInError)
                {
                    const auto again = wholeParser.parse("tail-more-bytes", 15U);
                    if (again != ParseStatus::Error || wholeParser.consumedByteCount() != 0U)
                    {
                        return "错误态没粘住，或还在消费字节（I4/I6）";
                    }
                    if (whole.frameKeys.empty() && (!wholeParser.request().path().empty() || !wholeParser.request().body().empty()))
                    {
                        return "失败的一路把半截产出交给了调用方（I6）";
                    }
                }

                // I5 复位可用：同一个解析器 reset() 后必须还能解出一条完整请求（见 WebSocket 分支里同样的说明）
                wholeParser.reset();
                const std::string probe      = validInput(target);
                const RunTrace    resetTrace = driveHttp1(wholeParser, probe, probe.size(), errorText, stats);
                if (resetTrace.isEndedInError || resetTrace.frameKeys.empty())
                {
                    return "reset() 后合法请求解不出报文（I5）";
                }
                return {};
            }

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
            case Target::QuicPacket:
            {
                const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t *>(input.data()), input.size());

                // 短头线上既不带长度字段也不带连接标识长度，只能由调用方按本端签发的那个传（§17.3.1）——
                // 「传大了读进包号、传小了标识不完整」是这条参数独有的两类错，所以同一份字节按几个候选长度各解一遍
                const std::size_t candidates[] = {0U, 4U, 8U, 12U, 16U, 20U, input.size() % (kQuicMaximumConnectionIdLength + 1U)};
                RunTrace          trace;
                for (const std::size_t destinationLength: candidates)
                {
                    const auto first  = decodeQuicPacketHeader(bytes, destinationLength);
                    const auto second = decodeQuicPacketHeader(bytes, destinationLength);
                    if (first.has_value() != second.has_value() || (!first.has_value() && first.error().kind != second.error().kind))
                    {
                        return "同一份字节两遍解码结论不同（确定性）";
                    }
                    if (!first.has_value())
                    {
                        continue;
                    }
                    if (first->isLongHeader != ((bytes[0] & kQuicLongHeaderFlagBit) != 0U))
                    {
                        return "解出的长头标志与首字节高位不一致（§17.2）";
                    }
                    if (first->isLongHeader && first->version != kQuicVersion1)
                    {
                        return "长头却带非 v1 版本还解成功：本层只认 v1，其余一律按 Malformed 拒";
                    }
                    if (first->destinationConnectionId.size() > kQuicMaximumConnectionIdLength || first->sourceConnectionId.size() > kQuicMaximumConnectionIdLength)
                    {
                        return "连接标识超过 20 字节却解成功（§17.2 的 v1 上限）";
                    }
                    if (!first->isLongHeader && first->destinationConnectionId.size() != destinationLength)
                    {
                        return "短头解出的目的标识长度与调用方给的那个不一致：路由表从此命不中";
                    }
                    trace.frameKeys.push_back(first->isLongHeader ? "long" : "short");
                }
                trace.isEndedInError = trace.frameKeys.empty();
                collect(trace);
                return {};
            }

            case Target::QuicFrameSequence:
            {
                const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t *>(input.data()), input.size());

                std::string       violation;
                const std::size_t firstCount = driveQuicFrames(bytes, violation);
                if (!violation.empty())
                {
                    return violation;
                }
                std::string       secondViolation;
                const std::size_t secondCount = driveQuicFrames(bytes, secondViolation);
                if (!secondViolation.empty() || firstCount != secondCount)
                {
                    return "同一载荷两遍解码结论不同（确定性）";
                }

                const auto outcome = decodeQuicFrames(bytes);
                RunTrace   trace;
                trace.frameKeys.assign(firstCount, "frame");
                trace.isEndedInError = !outcome.has_value();
                collect(trace);
                return {};
            }

            case Target::QuicParameters:
            {
                const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t *>(input.data()), input.size());

                // 两个角色各过一遍：专属项的完备性判据按发送方分叉，只跑一边就有一半分支从没被喂过
                RunTrace trace;
                for (const QuicTransportParameterSenderRole role: {QuicTransportParameterSenderRole::Client, QuicTransportParameterSenderRole::Server})
                {
                    const auto first  = decodeQuicTransportParameters(bytes, role);
                    const auto second = decodeQuicTransportParameters(bytes, role);
                    if (first.has_value() != second.has_value() || (!first.has_value() && first.error().kind != second.error().kind))
                    {
                        return "同一参数串两遍解码结论不同（确定性）";
                    }
                    if (!first.has_value())
                    {
                        continue;
                    }
                    if (!first->initialSourceConnectionId.has_value())
                    {
                        return "解成功却缺 initial_source_connection_id（§7.3 的硬性要求）";
                    }
                    if (first->initialSourceConnectionId->size() > kQuicMaximumConnectionIdLength)
                    {
                        return "initial_source_connection_id 超过 20 字节却解成功（§7.3）";
                    }
                    trace.frameKeys.push_back(role == QuicTransportParameterSenderRole::Client ? "client" : "server");
                }
                trace.isEndedInError = trace.frameKeys.empty();
                collect(trace);
                return {};
            }

            case Target::QpackBlock:
            {
                // 编码器流与头块是两条独立字节流、两条独立语境：各起全新会话、各跑两遍，比的是确定性
                std::string    feedViolation;
                const RunTrace firstFeed = driveQpackFeedSession(input, feedViolation);
                if (!feedViolation.empty())
                {
                    return feedViolation;
                }
                std::string    secondFeedViolation;
                const RunTrace secondFeed = driveQpackFeedSession(input, secondFeedViolation);
                if (!secondFeedViolation.empty())
                {
                    return secondFeedViolation;
                }
                if (firstFeed.frameKeys != secondFeed.frameKeys || firstFeed.isEndedInError != secondFeed.isEndedInError)
                {
                    return "同一份指令字节两个独立会话结论不同（确定性）";
                }

                std::string    sectionViolation;
                const RunTrace firstSection = driveQpackSectionSession(input, sectionViolation);
                if (!sectionViolation.empty())
                {
                    return sectionViolation;
                }
                std::string    secondSectionViolation;
                const RunTrace secondSection = driveQpackSectionSession(input, secondSectionViolation);
                if (!secondSectionViolation.empty())
                {
                    return secondSectionViolation;
                }
                if (firstSection.frameKeys != secondSection.frameKeys || firstSection.isEndedInError != secondSection.isEndedInError)
                {
                    return "同一份头块字节两个独立会话结论不同（确定性）";
                }
                collect(firstSection);

                // I5 对照：生产编码器产出的头块必须能被全新解码器逐字段解回（编解码互逆是这一层的契约）
                const std::string probe = validInput(target);
                if (probe.empty())
                {
                    return "生产编码器产不出合法样本，I5 对照无从判定";
                }
                std::string    probeViolation;
                const RunTrace probeTrace = driveQpackSectionSession(probe, probeViolation);
                if (!probeViolation.empty())
                {
                    return probeViolation;
                }
                std::vector<std::string> expectedFieldTexts;
                for (const auto &field: sampleQpackFields())
                {
                    expectedFieldTexts.push_back(field.name + '=' + field.value);
                }
                if (probeTrace.frameKeys != expectedFieldTexts)
                {
                    return "生产编码器产出的头块解不回同样的字段（编解码互逆对照）";
                }

                // I6 双流对拍：编码器产出的两组字节（编码器流指令 + 引用它们的头块）分开喂给同一个
                // 解码器——头块先到时必须 Blocked 且一行不交付（半截产出即失真）；指令流补齐、按报出
                // 的流标识 resume 之后必须逐字段解回。resume 交付此前在模糊闭包里零覆盖：喂进去的
                // 从来只是单路字节流，「解除阻塞」这条 §2.2.1 正常路径从没走到过
                {
                    const std::vector<QpackHeaderField> pairedFields = sampleQpackFields();
                    QpackEncoder                        pairedEncoder(4096U, 4U, 4096U);
                    std::string                         pairedBlock;
                    std::string                         pairedEncoderStream;
                    if (!pairedEncoder.encodeFieldSection(1U, pairedFields, pairedBlock, pairedEncoderStream).has_value())
                    {
                        return "双流对拍：生产编码器组不出配对样本（I6 前提）";
                    }

                    QpackDecoder                        pairedDecoder(kQpackFuzzDecoderSettings);
                    std::vector<QpackHeaderField>       pairedDecoded;
                    std::string                         pairedDecoderStream;
                    const std::span<const std::uint8_t> pairedBlockBytes(reinterpret_cast<const std::uint8_t *>(pairedBlock.data()), pairedBlock.size());
                    const auto                          pairedFirst = pairedDecoder.decodeFieldSection(1U, pairedBlockBytes, pairedDecoded, pairedDecoderStream);
                    if (!pairedFirst.has_value())
                    {
                        return "双流对拍：头块先到时报错（应先 Blocked，I6）";
                    }
                    if (*pairedFirst != QpackFieldSectionDecodeStatus::Blocked || !pairedDecoded.empty())
                    {
                        return "双流对拍：指令流未到就交出了字段（I6）";
                    }

                    std::vector<std::uint64_t>          unblockedStreamIds;
                    std::string                         feedDecoderStream;
                    const std::span<const std::uint8_t> pairedStreamBytes(reinterpret_cast<const std::uint8_t *>(pairedEncoderStream.data()), pairedEncoderStream.size());
                    if (!pairedDecoder.feedEncoderStream(pairedStreamBytes, unblockedStreamIds, feedDecoderStream).has_value())
                    {
                        return "双流对拍：配对指令流喂不进（I6）";
                    }

                    bool isResumed = false;
                    for (const std::uint64_t streamId: unblockedStreamIds)
                    {
                        std::string resumeDecoderStream;
                        const auto  pairedResumed = pairedDecoder.resumeBlockedFieldSection(streamId, pairedDecoded, resumeDecoderStream);
                        if (!pairedResumed.has_value() || *pairedResumed != QpackFieldSectionDecodeStatus::Decoded)
                        {
                            return "双流对拍：指令流补齐后续解没有交付（I6）";
                        }
                        isResumed = true;
                    }
                    if (!isResumed || pairedDecoder.hasBlockedFieldSection(1U))
                    {
                        return "双流对拍：指令流喂完却没有走通续解（I6）";
                    }

                    std::vector<std::string> pairedFieldTexts;
                    for (const auto &field: pairedDecoded)
                    {
                        pairedFieldTexts.push_back(field.name + '=' + field.value);
                    }
                    std::vector<std::string> expectedPairedFieldTexts;
                    for (const auto &field: pairedFields)
                    {
                        expectedPairedFieldTexts.push_back(field.name + '=' + field.value);
                    }
                    if (pairedFieldTexts != expectedPairedFieldTexts)
                    {
                        return "双流对拍：续解出的字段与编码输入不同（I6）";
                    }
                }
                return {};
            }

            case Target::PerMessageDeflate:
            {
                RunTrace trace;
                // 「拒绝收场」对这对自由函数取「本轮至少撞上一个拒绝面」：要约未接受，或两级解压上限下
                // 都没解出来。防空转断言据此确认拒绝分支确实被覆盖（这个目标没有单一的三态终态）
                bool isAnyRejectionObserved = false;

                // ① 扩展要约解析：同一份头值两次必须同一结论；接受与拒绝各自的伴随产物必须自洽
                const PerMessageDeflateNegotiation firstNegotiation  = negotiatePerMessageDeflate(input);
                const PerMessageDeflateNegotiation secondNegotiation = negotiatePerMessageDeflate(input);
                if (firstNegotiation.accepted != secondNegotiation.accepted || firstNegotiation.responseValue != secondNegotiation.responseValue ||
                    firstNegotiation.window.has_value() != secondNegotiation.window.has_value() ||
                    (firstNegotiation.window.has_value() && (firstNegotiation.window->compressBits != secondNegotiation.window->compressBits ||
                                                             firstNegotiation.window->decompressBits != secondNegotiation.window->decompressBits)))
                {
                    return "同一份要约头两次协商结论不同（确定性）";
                }
                if (firstNegotiation.accepted)
                {
                    if (!firstNegotiation.window.has_value() || firstNegotiation.responseValue.empty())
                    {
                        return "接受要约却不给窗口位数或不回参数";
                    }
                    const int compressBits   = firstNegotiation.window->compressBits;
                    const int decompressBits = firstNegotiation.window->decompressBits;
                    if (compressBits < kWebSocketMinimumWindowBits || compressBits > kWebSocketDefaultWindowBits || decompressBits < kWebSocketMinimumWindowBits ||
                        decompressBits > kWebSocketDefaultWindowBits)
                    {
                        return "接受要约却给出区间外的窗口位数（§7.1 只允许 8..15）";
                    }
                } else
                {
                    isAnyRejectionObserved = true;
                    if (!firstNegotiation.responseValue.empty() || firstNegotiation.window.has_value())
                    {
                        return "拒绝要约却留下回执或窗口位数";
                    }
                }

                // ② 解压闸门：压缩比可以做到几百倍，输出上限是这一层唯一的防爆闸——解成功就绝不允许越过它
                for (const std::size_t capBytes: {std::size_t{64U}, std::size_t{64U * 1024U}})
                {
                    const auto firstInflated  = inflateWebSocketMessage(input, capBytes);
                    const auto secondInflated = inflateWebSocketMessage(input, capBytes);
                    if (firstInflated.has_value() != secondInflated.has_value() || (firstInflated.has_value() && *firstInflated != *secondInflated))
                    {
                        return "同一份负载两次解压结论不同（确定性）";
                    }
                    if (firstInflated.has_value())
                    {
                        if (firstInflated->size() > capBytes)
                        {
                            return "解压越过输出上限：防爆闸失效";
                        }
                        trace.frameKeys.push_back(std::format("inflate-ok={}", firstInflated->size()));
                    } else
                    {
                        isAnyRejectionObserved = true;
                    }
                }

                // ③ 往返：本端压出去的必须解得回原负载（两个函数互为逆运算是这一对的契约）
                for (const int windowBits: {kWebSocketMinimumWindowBits, kWebSocketDefaultWindowBits})
                {
                    const auto deflated = deflateWebSocketMessage(input, windowBits);
                    if (!deflated.has_value())
                    {
                        continue; // 压缩失败是文档允许的结论（调用方原样发送），不是违例
                    }
                    const auto roundTripped = inflateWebSocketMessage(*deflated, input.size() + 64U, windowBits);
                    if (!roundTripped.has_value() || *roundTripped != input)
                    {
                        return "压缩再解压与原负载不同（编解码互逆）";
                    }
                    trace.frameKeys.push_back(std::format("roundtrip-ok={}", windowBits));
                }
                trace.isEndedInError = isAnyRejectionObserved;
                collect(trace);
                return {};
            }

            case Target::HttpResponse:
            {
                HttpResponseParser wholeParser;
                const RunTrace     whole = driveHttpResponse(wholeParser, input, input.size(), errorText);
                if (!errorText.empty())
                {
                    return errorText;
                }

                HttpResponseParser byteWiseParser;
                const RunTrace     byteWise = driveHttpResponse(byteWiseParser, input, 1U, errorText);
                if (!errorText.empty())
                {
                    return errorText;
                }
                if (whole.frameKeys != byteWise.frameKeys || whole.isEndedInError != byteWise.isEndedInError || whole.isEndedNeedMore != byteWise.isEndedNeedMore)
                {
                    return "整体喂与逐字节喂结论不同（I3）";
                }
                collect(whole);

                // I4 粘滞 + I6：失败之后继续喂任何字节必须一个字节不吃且结论不变，「收齐」形状不得出现
                if (whole.isEndedInError)
                {
                    if (wholeParser.feed("tail-more-bytes") != 0U || !wholeParser.hasFailed())
                    {
                        return "错误态没粘住，或还在消费字节（I4/I6）";
                    }
                    if (wholeParser.isComplete() || wholeParser.isHeadComplete())
                    {
                        return "失败态却给出「收齐」形状（I6）";
                    }
                }

                // I5 复位可用：同一个解析器 reset() 后必须还能解出一条完整响应
                wholeParser.reset();
                const std::string probe = validInput(target);
                std::string       probeError;
                const RunTrace    probeTrace = driveHttpResponse(wholeParser, probe, probe.size(), probeError);
                if (!probeError.empty())
                {
                    return probeError;
                }
                if (probeTrace.isEndedInError || !wholeParser.isComplete() || wholeParser.result().statusCode != 200)
                {
                    return "reset() 后合法响应解不出（I5）";
                }
                return {};
            }

            case Target::Count:
                break;
        }
        return "未知目标";
    }
} // namespace AsynGyanis::Net::Fuzz

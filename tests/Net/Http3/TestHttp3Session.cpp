// HTTP/3 会话层的用例：本端单向流的绑定、SETTINGS 的产出、请求到 Router
// 的映射，以及隧道与限额这套状态机。前几条只驱动会话本身——单向流的开流口与流数据出口都是测试给的假实现，因此不涉及 QUIC 与真实
// UDP。后面的真字节用例由测试自带的字节级对端（Http3ClientPeer）驱动：请求按 RFC 9114/9204 排成帧、响应按帧解回来，中间同样不经
// UDP。为什么对端是自己写的：链接进来的第三方实现与被测代码同属一次构建、可以一起改软，那种「跨实现裁判」迟早只剩名字。字节合不合规范的判定因此在进程外做（scripts/h3_acceptance.py
// 与 scripts/quic_cross_check.sh 用 aioquic 真握手真编解），本文件留的是状态机与业务映射的回归判据。
#include "Net/Http3/Http3Session.h"

#include "Platform/FileSystem/FileSystem.h"

#include "CoreTestSupport.h"

#include "Core/Coroutine/Task.h"
#include "Net/Http/HttpRequestId.h"
#include "Net/Http/Router.h"
#include "Net/Http/StaticFileService.h"
#include "Net/Http3/Http3Frame.h"
#include "Net/Http3/Qpack.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 本端（服务端）发起的单向流号序列：RFC 9000 §2.1 规定服务端发起的单向流号 ≡ 3 (mod 4)
        constexpr std::int64_t kFirstServerUnidirectionalStreamId = 3;
        constexpr std::int64_t kUnidirectionalStreamIdStep        = 4;

        /// 对端（客户端）发起的单向流号序列：≡ 2 (mod 4)，依次是控制流 2、QPACK 编码流 6、解码流 10
        constexpr std::int64_t kClientControlStreamId      = 2;
        constexpr std::int64_t kClientQpackEncoderStreamId = 6;
        constexpr std::int64_t kClientQpackDecoderStreamId = 10;

        /// 客户端发起的双向流号序列：≡ 0 (mod 4)，第一条就是承载首个请求的那条
        constexpr std::int64_t kFirstRequestStreamId = 0;

        /// HTTP/3 的单向流类型：控制流是 0（RFC 9114 §6.2.1）
        constexpr std::uint8_t kControlStreamType = 0x00;

        /// SETTINGS 帧的帧类型（RFC 9114 §7.2.4）
        constexpr std::uint8_t kSettingsFrameType = 0x04;

        /// 请求的 :scheme 取值：h3 只跑在 TLS 上（放在静态存储期，供伪头构造引用）
        constexpr const char *kRequestScheme = "https";

        /// 记下会话交给出口的一段流数据
        struct CapturedStreamData
        {
            std::int64_t              streamId{0};        ///< 流号
            std::vector<std::uint8_t> bytes;              ///< 字节
            bool                      isEndStream{false}; ///< 是否收尾
        };

        /**
         * @brief 按「服务端单向流号依次递增」给出流号的假开流口
         */
        class FakeStreamOpener
        {
        public:
            FakeStreamOpener() = default;

            /// 给出一条新的本端单向流号
            [[nodiscard]] std::int64_t operator()()
            {
                const std::int64_t streamId = m_nextStreamId;
                m_nextStreamId += kUnidirectionalStreamIdStep;
                m_openedStreamIds.push_back(streamId);
                return streamId;
            }

            /// 已经被开出来的流号（按开流顺序）
            [[nodiscard]] const std::vector<std::int64_t> &openedStreamIds() const noexcept
            {
                return m_openedStreamIds;
            }

        private:
            std::int64_t              m_nextStreamId{kFirstServerUnidirectionalStreamId}; ///< 下一条流号
            std::vector<std::int64_t> m_openedStreamIds;                                  ///< 已开出的流号
        };

        /**
         * @brief 测试侧的 HTTP/3 客户端：按 RFC 9114/9204 自己排字节，不链接第三方 h3 实现
         *
         * @details 只做三件事：把请求编成 HEADERS/DATA 帧分趟交出去、把会话回的字节按帧读回来、
         *          把解出的字段与正文攒成一份可比对的响应视图。头块用本层的 QpackEncoder 编、
         *          QpackDecoder 解，因此这里的判据是「会话的状态机与业务映射对不对」；
         *          「字节合不合规范」由**进程外**的独立实现裁定——`scripts/h3_acceptance.py` 与
         *          `scripts/quic_cross_check.sh` 拿 aioquic 真握手、真编解。第三方实现不再链接进测试
         *          可执行体：链接进来的裁判与被测代码同属一次构建、可以一起改软，那种「跨实现」迟早只剩名字。
         */
        class Http3ClientPeer
        {
        public:
            /// 解出来的响应
            struct DecodedResponse
            {
                int status{0}; ///< :status（最后一条，即最终响应）
                /// 按到达顺序记下的全部 :status：信息性响应（100/103）会排在最终响应之前
                std::vector<int>                   statuses;
                std::map<std::string, std::string> headers; ///< 其余头部（同名只留最后一条）
                /// 全部响应字段按到达顺序逐条记下：可重复头（Set-Cookie）只有这里能数出条数
                std::vector<std::pair<std::string, std::string>> headerFields;
                /// 每个字段段（一个 HEADERS 帧）单独一份：头段与尾段的分界只有这里看得出来，
                /// 上面那张表是拉平的——而「尾部字段确实排在正文之后、由第二个段带出来」正是要钉的东西
                std::vector<std::vector<std::pair<std::string, std::string>>> fieldSections;
                std::string                                                   body;              ///< 正文
                bool                                                          isComplete{false}; ///< 是否收到了收尾
                /// 排字节或解字节时撞到的第一句报错：判据失败时用它分清「服务端没回」与「回了但解不开」
                std::string decodeError;

                /// 数某个头名出现了几次
                [[nodiscard]] std::size_t countOf(const std::string &name) const
                {
                    return static_cast<std::size_t>(std::ranges::count(headerFields, name, &std::pair<std::string, std::string>::first));
                }
            };

            /**
             * @brief 造一条测试侧的 h3 对端：先把控制流与 QPACK 两条流的开头排进待发队列
             * @param maximumFieldSectionSizeByteCount 写进本端 SETTINGS_MAX_FIELD_SECTION_SIZE 的取值，
             *        0 表示不报这项（对端因此不受约束）；用例用它通告一个很小的上限，看服务端怎么处置
             * @details 这三段字节随第一次取写一同交给会话，与真实客户端「连上就先报自我约束」同一趟
             */
            explicit Http3ClientPeer(const std::uint64_t maximumFieldSectionSizeByteCount = 0) :
                m_answerDecoder(QpackDecoderSettings{
                        .maximumTableCapacityByteCount = 0, .maximumBlockedStreamCount = 0, .maximumFieldSectionSizeByteCount = maximumFieldSectionSizeByteCount})
            {
                // 本端 SETTINGS 报两项自我约束：动态表容量 0、阻塞流 0（RFC 9204 §5）。容量 0 让服务端
                // 只能用静态表与字面量，响应头块因此不依赖任何编码器流指令，也就没有「先等表补齐」这条路
                Http3SettingsFrame settingsFrame;
                settingsFrame.settings.emplace_back(Http3SettingId::QpackMaxTableCapacity, 0U);
                settingsFrame.settings.emplace_back(Http3SettingId::QpackBlockedStreams, 0U);
                if (maximumFieldSectionSizeByteCount != 0)
                {
                    settingsFrame.settings.emplace_back(Http3SettingId::MaxFieldSectionSize, maximumFieldSectionSizeByteCount);
                }
                std::string controlStreamBytes;
                appendHttp3StreamTypeHeader(controlStreamBytes, Http3StreamType::Control);
                appendHttp3Frame(controlStreamBytes, settingsFrame);
                m_pendingWrites.push_back(PendingWrite{kClientControlStreamId, toStreamBytes(controlStreamBytes), false});

                // QPACK 的两条流在容量 0 下一条指令都不产，但流本身要按 RFC 9114 §6.2 报出类型
                std::string encoderStreamBytes;
                appendHttp3StreamTypeHeader(encoderStreamBytes, Http3StreamType::QpackEncoder);
                m_pendingWrites.push_back(PendingWrite{kClientQpackEncoderStreamId, toStreamBytes(encoderStreamBytes), false});
                std::string decoderStreamBytes;
                appendHttp3StreamTypeHeader(decoderStreamBytes, Http3StreamType::QpackDecoder);
                m_pendingWrites.push_back(PendingWrite{kClientQpackDecoderStreamId, toStreamBytes(decoderStreamBytes), false});
            }

            Http3ClientPeer(const Http3ClientPeer &) = delete;

            Http3ClientPeer &operator=(const Http3ClientPeer &) = delete;

            /**
             * @brief 提交一条请求，并把由此产生的全部待发字节取出来
             * @param method 方法原文
             * @param path 路径（:path）
             * @param authority 权威主机（:authority）
             * @param requestStreamId 请求所在的双向流号：一条请求一条流是 HTTP/3 的规矩，
             *        同一对象上再发一条就要换号（客户端发起的双向流是 0、4、8…）
             * @param extraHeaders 伪头之后追加的普通头（本文件用来带 x-request-id）
             * @return std::vector<CapturedStreamData> 按流号分好的待发字节（含控制流与请求流）
             */
            std::vector<CapturedStreamData> submitRequest(const std::string &method, const std::string &path, const std::string &authority,
                                                          const std::int64_t                                      requestStreamId = kFirstRequestStreamId,
                                                          const std::vector<std::pair<std::string, std::string>> &extraHeaders    = {})
            {
                queueRequestHead(method, path, authority, {}, extraHeaders, requestStreamId, true);
                return drainPendingWrites();
            }

            /**
             * @brief 提交一条带正文的请求：正文按批分成多条 DATA 帧
             * @param method 方法原文
             * @param path 路径（:path）
             * @param authority 权威主机（:authority）
             * @param body 正文
             * @param chunkByteCount 每批的字节数（分批到达就是这样造出来的）
             * @param extraHeaders 伪头之后追加的普通头（如 expect、content-length）：本对端只按显式
             *        给出的字段编头块，不会自己补
             * @return true 头块与各批正文都排进了待发队列；false 头块没能编出来（原因看 response().decodeError）
             * @note 只排队不取字节：要靠 takeNextWriteStep() 一段一段送到服务端，才能测「正文收齐之前
             *       处理器已经进去」这类边收边读的路径
             */
            bool submitRequestWithBody(const std::string &method, const std::string &path, const std::string &authority, std::string body, const std::size_t chunkByteCount,
                                       const std::vector<std::pair<std::string, std::string>> &extraHeaders = {})
            {
                const std::size_t stepByteCount = chunkByteCount == 0 ? body.size() : chunkByteCount;
                // 没有正文就没有 DATA 帧，收尾只能压在头那一趟上，否则这条流永远等不到 END_STREAM
                queueRequestHead(method, path, authority, {}, extraHeaders, kFirstRequestStreamId, body.empty());
                for (std::size_t offset = 0; offset < body.size(); offset += stepByteCount)
                {
                    const std::size_t remainingByteCount = body.size() - offset;
                    const std::size_t thisStepByteCount  = (stepByteCount < remainingByteCount) ? stepByteCount : remainingByteCount;
                    queueDataFrame(kFirstRequestStreamId, body.substr(offset, thisStepByteCount), offset + thisStepByteCount >= body.size());
                }
                return m_response.decodeError.empty();
            }

            /**
             * @brief 提交一条「请求头 → 整段正文 → 尾段收尾」的请求
             * @param method 方法原文
             * @param path 路径（:path）
             * @param authority 权威主机（:authority）
             * @param body 正文
             * @param trailerFields 尾段字段，按到达顺序
             * @return std::vector<CapturedStreamData> 按流号分好的待发字节
             */
            std::vector<CapturedStreamData> submitRequestWithBodyAndTrailers(const std::string &method, const std::string &path, const std::string &authority,
                                                                             const std::string &body, const std::vector<QpackHeaderField> &trailerFields)
            {
                std::vector<std::pair<std::string, std::string>> extraHeaders;
                if (!body.empty())
                {
                    extraHeaders.emplace_back("content-length", std::to_string(body.size()));
                }
                queueRequestHead(method, path, authority, {}, extraHeaders, kFirstRequestStreamId, body.empty());
                if (!body.empty())
                {
                    queueDataFrame(kFirstRequestStreamId, body, false);
                }
                queueTrailerSection(trailerFields, kFirstRequestStreamId);
                return drainPendingWrites();
            }

            /**
             * @brief 只取一段待发字节：把请求分步送到服务端，模拟正文随时间到达
             * @return CapturedStreamData 本次的片段；没有待发字节时 streamId 为 -1
             */
            CapturedStreamData takeNextWriteStep()
            {
                if (m_pendingWrites.empty())
                {
                    CapturedStreamData emptyStep;
                    emptyStep.streamId = -1;
                    return emptyStep;
                }
                PendingWrite write = std::move(m_pendingWrites.front());
                m_pendingWrites.pop_front();
                return CapturedStreamData{write.streamId, std::move(write.bytes), write.isEndStream};
            }

            /**
             * @brief 提交一条「不带任何请求正文」的扩展 CONNECT：递交即 END_STREAM
             * @param path 路径（:path）
             * @param authority 权威主机（:authority）
             * @param extraHeaders 伪头之后追加的普通头（本用例用来带 sec-websocket-extensions）
             * @return std::vector<CapturedStreamData> 按流号分好的待发字节
             * @note 与 submitWebSocketTunnel 的区别只有一处：头那一趟就收尾——「头与 END_STREAM 同一趟
             *       到达」正是这条路径
             */
            std::vector<CapturedStreamData> submitEndedWebSocketTunnel(const std::string &path, const std::string &authority,
                                                                       const std::vector<std::pair<std::string, std::string>> &extraHeaders = {})
            {
                queueRequestHead("CONNECT", path, authority, "websocket", extraHeaders, kFirstRequestStreamId, true);
                return drainPendingWrites();
            }

            /**
             * @brief 提交一条扩展 CONNECT（RFC 9220）请求，并把第一条 WebSocket 帧当作请求数据随后发出
             * @param path 路径（:path）
             * @param authority 权威主机（:authority）
             * @param firstWebSocketFrame 隧道建立后要发的第一条 WebSocket 帧字节
             * @return std::vector<CapturedStreamData> 按流号分好的待发字节（含请求头与随后的帧）
             * @note 头那一趟**不能**收尾：对端之后还要在同一条流上发 WebSocket 帧，
             *       服务端会把收尾之后的 DATA 判成 H3_FRAME_UNEXPECTED
             */
            std::vector<CapturedStreamData> submitWebSocketTunnel(const std::string &path, const std::string &authority, const std::string &firstWebSocketFrame)
            {
                queueRequestHead("CONNECT", path, authority, "websocket", {}, kFirstRequestStreamId, false);
                queueDataFrame(kFirstRequestStreamId, firstWebSocketFrame, false);
                return drainPendingWrites();
            }

            /**
             * @brief 在已建立的隧道上再发一条 WebSocket 帧（同一条流上的后续 DATA）
             * @param webSocketFrame 帧字节
             * @return std::vector<CapturedStreamData> 按流号分好的待发字节
             */
            std::vector<CapturedStreamData> sendWebSocketFrame(const std::string &webSocketFrame)
            {
                // 隧道到对端关流为止都开着：每一趟都不收尾
                queueDataFrame(kFirstRequestStreamId, webSocketFrame, false);
                return drainPendingWrites();
            }

            /**
             * @brief 把服务端回的字节喂进来解出响应
             * @param streamId 这段字节所在的流号
             * @param data 字节（可以只到半帧，剩下的等下一趟）
             * @param isEndStream 这一趟是否把该流收尾
             */
            void receive(const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
            {
                // 只解请求流（客户端发起的双向流 ≡ 0 mod 4）：单向流上是会话的控制流与 QPACK 流，
                // 本对端通告的是容量 0，那些字节没有一样需要解
                if (streamId % 4 != 0)
                {
                    return;
                }

                const auto        existingReader = m_frameReaders.find(streamId);
                Http3FrameReader &reader         = existingReader != m_frameReaders.end()
                                                           ? *existingReader->second
                                                           : *m_frameReaders.emplace(streamId, std::make_unique<Http3FrameReader>(kPeerFrameByteLimit)).first->second;

                if (const auto fed = reader.feed(data); !fed.has_value())
                {
                    recordDecodeError("响应帧解不开：" + fed.error().message);
                    return;
                }
                while (true)
                {
                    const auto nextFrame = reader.nextFrame();
                    if (!nextFrame.has_value())
                    {
                        recordDecodeError("响应帧不合布局：" + nextFrame.error().message);
                        return;
                    }
                    if (!nextFrame->has_value())
                    {
                        break;
                    }
                    handleResponseFrame(streamId, **nextFrame);
                }

                if (!isEndStream)
                {
                    return;
                }
                m_response.isComplete = true;
                // 收尾时缓冲里不该剩半截帧：那是「最后一个帧被截断」，真实对端按 §7.1 要判错
                if (const std::size_t residualByteCount = reader.pendingByteCount(); residualByteCount != 0)
                {
                    recordDecodeError(std::format("响应流收尾时还剩 {} 字节未成帧", residualByteCount));
                }
            }

            /// 解出来的响应
            [[nodiscard]] const DecodedResponse &response() const noexcept
            {
                return m_response;
            }

        private:
            /// 一段待交给会话的流数据：一次「写」一段
            struct PendingWrite
            {
                std::int64_t              streamId{0};        ///< 流号
                std::vector<std::uint8_t> bytes;              ///< 该趟的字节
                bool                      isEndStream{false}; ///< 这一趟是否把流收尾
            };

            /// 单帧「帧头 + 载荷」的上限：够装下用例里最大的头块与正文段
            static constexpr std::size_t kPeerFrameByteLimit = 64U * 1024U;

            /**
             * @brief 把一段文本缓冲转成流字节
             * @param bytes 二进制安全的文本缓冲
             * @return std::vector<std::uint8_t> 同样的字节
             */
            [[nodiscard]] static std::vector<std::uint8_t> toStreamBytes(const std::string &bytes)
            {
                return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
            }

            /**
             * @brief 把一帧的「类型 + 长度 + 载荷」排成字节并追加到待发队列
             * @param streamId 该帧所在的流号
             * @param frameType 帧类型的线上取值（§7.1）
             * @param payload 帧载荷原文
             * @param isEndStream 这一趟是否把流收尾
             */
            void queueFrame(const std::int64_t streamId, const Http3FrameType frameType, const std::string &payload, const bool isEndStream)
            {
                std::string frameBytes;
                appendHttp3FrameWithPayload(frameBytes, frameType, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size()));
                m_pendingWrites.push_back(PendingWrite{streamId, toStreamBytes(frameBytes), isEndStream});
            }

            /**
             * @brief 造出请求的伪头字段行
             * @param method 方法原文
             * @param path 路径
             * @param authority 权威主机
             * @param protocol :protocol 取值，空串表示不带（普通请求都不带）
             * @return std::vector<QpackHeaderField> 按伪头应在前的顺序给出
             */
            [[nodiscard]] static std::vector<QpackHeaderField> makePseudoFields(const std::string &method, const std::string &path, const std::string &authority,
                                                                                const std::string &protocol)
            {
                std::vector<QpackHeaderField> headerFields{QpackHeaderField{":method", method}, QpackHeaderField{":scheme", kRequestScheme},
                                                           QpackHeaderField{":authority", authority}, QpackHeaderField{":path", path}};
                if (!protocol.empty())
                {
                    // 扩展 CONNECT 用（RFC 9220）：伪头必须排在普通头之前，追加在末尾即可
                    headerFields.push_back(QpackHeaderField{":protocol", protocol});
                }
                return headerFields;
            }

            /**
             * @brief 把一条请求的头块编成 HEADERS 帧排进待发队列
             * @param method 方法原文
             * @param path 路径（:path）
             * @param authority 权威主机（:authority）
             * @param protocol :protocol 取值，空串表示普通请求
             * @param extraHeaders 伪头之后追加的普通头
             * @param requestStreamId 承载该请求的双向流号
             * @param isEndStream 头这一趟是否顺带收尾
             */
            void queueRequestHead(const std::string &method, const std::string &path, const std::string &authority, const std::string &protocol,
                                  const std::vector<std::pair<std::string, std::string>> &extraHeaders, const std::int64_t requestStreamId, const bool isEndStream)
            {
                std::vector<QpackHeaderField> headerFields = makePseudoFields(method, path, authority, protocol);
                for (const auto &[name, value]: extraHeaders)
                {
                    headerFields.push_back(QpackHeaderField{name, value});
                }

                // 每个头块单独一个编码器：容量 0 下没有跨头块的表状态要继承
                QpackEncoder encoder(0, 0, 0);
                std::string  headerBlock;
                std::string  encoderStreamBytes;
                const auto   encoded =
                        encoder.encodeFieldSection(static_cast<std::uint64_t>(requestStreamId), std::span<const QpackHeaderField>(headerFields), headerBlock, encoderStreamBytes);
                if (!encoded.has_value())
                {
                    recordDecodeError("请求头块没能编出来：" + encoded.error().message);
                    return;
                }
                if (!encoderStreamBytes.empty())
                {
                    // 只用静态表与字面量就不该有编码器流指令（RFC 9204 §2.2.2.2）：出现了说明容量 0 没被守住
                    recordDecodeError("本端通告容量 0，请求头块却产生了 QPACK 编码器流指令");
                }
                queueFrame(requestStreamId, Http3FrameType::Headers, headerBlock, isEndStream);
            }

            /**
             * @brief 排一条尾段（trailer section）：正文之后再来一枚 HEADERS，并以此收尾
             * @details 尾段里没有伪头（RFC 9114 §4.3），其余编码与头段同一套：容量 0 下只用字面量。
             * @param trailerFields 尾段字段，按到达顺序
             * @param requestStreamId 承载这条请求的流号
             */
            void queueTrailerSection(const std::vector<QpackHeaderField> &trailerFields, const std::int64_t requestStreamId)
            {
                QpackEncoder encoder(0, 0, 0);
                std::string  headerBlock;
                std::string  encoderStreamBytes;
                const auto   encoded =
                        encoder.encodeFieldSection(static_cast<std::uint64_t>(requestStreamId), std::span<const QpackHeaderField>(trailerFields), headerBlock, encoderStreamBytes);
                if (!encoded.has_value())
                {
                    recordDecodeError("尾段字段没能编出来：" + encoded.error().message);
                    return;
                }
                queueFrame(requestStreamId, Http3FrameType::Headers, headerBlock, true);
            }

            /**
             * @brief 把一段正文编成 DATA 帧排进待发队列
             * @param streamId 承载正文的流号
             * @param payload 正文
             * @param isEndStream 这一趟是否把流收尾
             */
            void queueDataFrame(const std::int64_t streamId, const std::string &payload, const bool isEndStream)
            {
                queueFrame(streamId, Http3FrameType::Data, payload, isEndStream);
            }

            /// 反复取待发字节直到没有，同一条流的几趟合并成一条
            std::vector<CapturedStreamData> drainPendingWrites()
            {
                std::vector<CapturedStreamData>     chunks;
                std::map<std::int64_t, std::size_t> chunkIndexByStreamId;
                while (!m_pendingWrites.empty())
                {
                    PendingWrite write = std::move(m_pendingWrites.front());
                    m_pendingWrites.pop_front();
                    // 同一条流可能分几次产出（头一块、正文一块），合并成一条，免得下游把「同一流的
                    // 第二次产出」当成收尾之后的意外字节
                    if (const auto existing = chunkIndexByStreamId.find(write.streamId); existing != chunkIndexByStreamId.end())
                    {
                        CapturedStreamData &chunk = chunks[existing->second];
                        chunk.bytes.insert(chunk.bytes.end(), write.bytes.begin(), write.bytes.end());
                        chunk.isEndStream = chunk.isEndStream || write.isEndStream;
                        continue;
                    }
                    chunkIndexByStreamId.emplace(write.streamId, chunks.size());
                    chunks.push_back(CapturedStreamData{write.streamId, std::move(write.bytes), write.isEndStream});
                }
                return chunks;
            }

            /**
             * @brief 处置从请求流上解出来的一帧
             * @param streamId 该帧所在的流号
             * @param frame 帧
             */
            void handleResponseFrame(const std::int64_t streamId, const Http3Frame &frame)
            {
                if (const auto *headers = std::get_if<Http3HeadersFrame>(&frame); headers != nullptr)
                {
                    std::vector<QpackHeaderField> fields;
                    std::string                   decoderStreamBytes;
                    const auto decoded = m_answerDecoder.decodeFieldSection(static_cast<std::uint64_t>(streamId), headers->encodedFieldSection, fields, decoderStreamBytes);
                    if (!decoded.has_value())
                    {
                        recordDecodeError("响应头块解不开：" + decoded.error().message);
                        return;
                    }
                    if (*decoded == QpackFieldSectionDecodeStatus::Blocked)
                    {
                        recordDecodeError("响应头块引用了动态表，而本端通告的是容量 0");
                        return;
                    }
                    m_response.fieldSections.emplace_back();
                    for (const auto &field: fields)
                    {
                        noteResponseField(field);
                    }
                    return;
                }
                if (const auto *body = std::get_if<Http3DataFrame>(&frame); body != nullptr)
                {
                    m_response.body.append(reinterpret_cast<const char *>(body->payload.data()), body->payload.size());
                }
                // 其余帧（SETTINGS/GOAWAY/未知类型）按 §9「必须忽略」处置：本对端只关心请求流上的响应
            }

            /**
             * @brief 记下一条响应字段：:status 单独计数，其余按到达顺序入表
             * @param field 一条字段行
             */
            void noteResponseField(const QpackHeaderField &field)
            {
                if (field.name == ":status")
                {
                    m_response.status = std::atoi(field.value.c_str());
                    m_response.statuses.push_back(m_response.status);
                    return;
                }
                m_response.headerFields.emplace_back(field.name, field.value);
                m_response.fieldSections.back().emplace_back(field.name, field.value);
                m_response.headers[field.name] = field.value;
            }

            /**
             * @brief 只留下第一条报错：后面的失败多半是它的连带后果
             * @param message 中文可操作文案
             */
            void recordDecodeError(std::string message)
            {
                if (m_response.decodeError.empty())
                {
                    m_response.decodeError = std::move(message);
                }
            }

            /// 待交给会话的字节：一条「写」一段，取一步交一步，正文分批到达就是这样造出来的
            std::deque<PendingWrite> m_pendingWrites;
            /// 每条请求流一个帧读取器：响应的头与正文可能分好几趟到齐
            std::map<std::int64_t, std::unique_ptr<Http3FrameReader>> m_frameReaders;
            /// 解响应头块用的解码器：按本端通告的自我约束建表（容量 0，即只认静态表与字面量）
            QpackDecoder    m_answerDecoder;
            DecodedResponse m_response;
        };

        /**
         * @brief 把一条「即时完成」的协程推到结束
         * @details 与 tests/Net/Http 里的既有口径一致：用例中的处理器都不等 I/O，因此首次 resume()
         *          就该跑完；这里多推进几次是为了容下 await 链上的多次让出
         * @param task 待推进的协程
         */
        void resumeUntilReady(Core::Task<> &task)
        {
            for (int resumeIndex = 0; resumeIndex < 16 && !task.isReady(); ++resumeIndex)
            {
                task.handle().resume();
            }
        }
    } // namespace

    /**
     * @brief 会话建立时开三条本端单向流并在控制流上产出 SETTINGS
     */
    TEST(Http3Session, BindsControlAndQpackStreamsThenEmitsSettings)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        // 会话按值存开流口，要观察它到底开了哪些流号就得把本对象按引用交进去（否则填的是副本）
        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                                 return data.size();
                             });

        ASSERT_TRUE(session.isUsable()) << "三条本端单向流都开得出来，会话却不是可用状态";
        ASSERT_EQ(opener.openedStreamIds().size(), 3U) << "会话应当正好开三条本端单向流（控制流 + QPACK 编码流 + QPACK 解码流）";

        session.flushPendingStreamData();

        ASSERT_FALSE(sentStreamData.empty()) << "会话建好后一段字节都没产出：SETTINGS 没有发出去";
        const CapturedStreamData &settingsFrame = sentStreamData.front();
        EXPECT_EQ(settingsFrame.streamId, opener.openedStreamIds().front()) << "SETTINGS 应当产在控制流上（也就是第一条开出来的单向流）";
        ASSERT_GE(settingsFrame.bytes.size(), 2U) << "控制流上第一段字节太短，装不下流类型与帧类型";
        EXPECT_EQ(settingsFrame.bytes[0], kControlStreamType) << "单向流的第一字节应当是流类型，控制流为 0";
        EXPECT_EQ(settingsFrame.bytes[1], kSettingsFrameType) << "控制流上的第一个帧应当是 SETTINGS";
    }

    /**
     * @brief 吃下对端控制流上的 SETTINGS 之后会话仍可用
     */
    TEST(Http3Session, ConsumesPeerSettingsAndStaysUsable)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        // 会话按值存开流口，要观察它到底开了哪些流号就得把本对象按引用交进去（否则填的是副本）
        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                                 return data.size();
                             });
        ASSERT_TRUE(session.isUsable());
        session.flushPendingStreamData();

        // 客户端发起的单向流号 ≡ 2 (mod 4)，第一条就是控制流 2：内容是流类型 0x00 + 长度为 0 的 SETTINGS
        const std::vector<std::uint8_t> peerControlStreamBytes{kControlStreamType, kSettingsFrameType, 0x00};
        session.onStreamData(kClientControlStreamId, peerControlStreamBytes, false);

        EXPECT_FALSE(session.isBroken()) << "吃下对端合法的 SETTINGS 不该把会话弄坏";
        EXPECT_TRUE(session.isUsable());
    }

    /**
     * @brief 开不出本端单向流时会话如实不可用，且不会往出口写任何字节
     */
    TEST(Http3Session, IsUnusableAndSilentWhenNoUnidirectionalStreamCanBeOpened)
    {
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session([] { return std::int64_t{-1}; },
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                                 return data.size();
                             });

        EXPECT_FALSE(session.isUsable()) << "单向流都开不出来，会话不该报可用";
        session.flushPendingStreamData();
        EXPECT_TRUE(sentStreamData.empty()) << "会话不可用时不该往出口写任何字节";
    }

    /**
     * @brief 真字节往返：客户端提的请求经 QPACK 编出来，服务端解出来交给 Router，响应再解回客户端
     * @details 头块由对端真编、响应由它真解，因此这条用例同时钉住「映射对不对」与
     *          「两端字节能不能互通」——比只调 add* 接口的单元断言强一层
     */
    TEST(Http3Session, DispatchesRequestThroughRouterAndAnswersWithRealBytes)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                                 return data.size();
                             });

        // 处理器收到的请求：断言它的字段就是「h1/h2 上同一份业务代码会看到的那一份」
        HttpMethod  observedMethod{HttpMethod::UNKNOWN};
        std::string observedUri;
        std::string observedHost;
        std::string observedVersion;
        Router      router;
        router.get("/hello",
                   [&observedMethod, &observedUri, &observedHost, &observedVersion](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                   {
                       observedMethod  = request.method();
                       observedUri     = std::string(request.uri());
                       observedHost    = request.getHeader("host").value_or("");
                       observedVersion = request.httpVersion();
                       response.setStatus(200);
                       response.setHeader("content-type", "text/plain");
                       response.setBody("hi");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;

        const std::vector<CapturedStreamData> requestChunks = peer.submitRequest("GET", "/hello", "example.com");
        ASSERT_FALSE(requestChunks.empty()) << "客户端没能产出任何字节（请求根本没编出来）";
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        ASSERT_FALSE(sentStreamData.empty()) << "服务端一个字节都没回：响应没发出去";
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        EXPECT_EQ(observedMethod, HttpMethod::GET) << "方法没有从 :method 映射过来";
        EXPECT_EQ(observedUri, "/hello") << ":path 没有映射成请求 URI";
        EXPECT_EQ(observedHost, "example.com") << ":authority 没有补齐成 host 头";
        EXPECT_EQ(observedVersion, "HTTP/3") << "请求版本号不是 HTTP/3";
        EXPECT_EQ(peer.response().status, 200) << "客户端没解出 200";
        EXPECT_EQ(peer.response().body, "hi") << "正文没有回到客户端";
        EXPECT_TRUE(peer.response().isComplete) << "响应没有收尾";
        const auto contentTypeHeader = peer.response().headers.find("content-type");
        ASSERT_NE(contentTypeHeader, peer.response().headers.end()) << "业务设的响应头没送到";
        EXPECT_EQ(contentTypeHeader->second, "text/plain");
        const auto contentLengthHeader = peer.response().headers.find("content-length");
        ASSERT_NE(contentLengthHeader, peer.response().headers.end()) << "缺正文长度时应当按实际长度补上 content-length";
        EXPECT_EQ(contentLengthHeader->second, "2");
    }

    /**
     * @brief 畸形请求头：回 400 而不是作废整条连接，也不把请求交给业务
     * @details RFC 9114 §4.1.2 允许服务端在重置之前先答一个错。这条把「连接层判定 → 会话作答」
     *          这一段接起来测——连接层已单测过会发通知，此处钉的是通知真的变成了一个能解开的响应。
     *          非法字节由本层自己的 QPACK 编码器直接造（对端的提交入口只编用例点名的字段，带不出畸形字段名），
     *          作答则由测试侧的解码器解回来。
     */
    TEST(Http3Session, AnswersMalformedRequestHeadWithFourHundredAndKeepsConnection)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                                 return data.size();
                             });
        ASSERT_TRUE(session.isUsable());
        session.flushPendingStreamData();

        bool   isHandlerReached = false;
        Router router;
        router.get("/hello",
                   [&isHandlerReached](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       isHandlerReached = true;
                       response.setStatus(200);
                       co_return;
                   });
        session.attachRouter(router);

        std::vector<QpackHeaderField> fieldLines = {
                QpackHeaderField{":method", "GET"},  QpackHeaderField{":scheme", "https"},         QpackHeaderField{":authority", "example.com"},
                QpackHeaderField{":path", "/hello"}, QpackHeaderField{"connection", "keep-alive"}, ///< 连接特定字段：RFC 9114 §4.2 明确禁止
        };
        std::string  headerBlock;
        std::string  encoderStreamBytes;
        QpackEncoder encoder(0, 0, 0);
        ASSERT_TRUE(encoder.encodeFieldSection(0, std::span<const QpackHeaderField>(fieldLines), headerBlock, encoderStreamBytes).has_value());
        ASSERT_TRUE(encoderStreamBytes.empty()) << "只用静态表就不该产生编码器流指令，否则这条用例的前提变了";

        // 帧由帧层自己编：长度域是变长整数，手写字节会在载荷超过单字节档时写出自相矛盾的帧
        Http3HeadersFrame headersFrame;
        headersFrame.encodedFieldSection = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(headerBlock.data()), headerBlock.size());
        std::string requestBytes;
        appendHttp3Frame(requestBytes, headersFrame);
        session.onStreamData(0, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(requestBytes.data()), requestBytes.size()), true);

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        EXPECT_FALSE(session.isBroken()) << "一个畸形请求不该把整条连接判死";
        EXPECT_FALSE(isHandlerReached) << "畸形的请求不能交到业务手里";
        ASSERT_FALSE(sentStreamData.empty()) << "没有作答：对端只能挂到空闲超时";
        const bool isAnswerOnRequestStream = std::ranges::any_of(sentStreamData, [](const CapturedStreamData &chunk) { return chunk.streamId == 0; });
        EXPECT_TRUE(isAnswerOnRequestStream) << "作答没出现在流 0 上：一共只回了 " << sentStreamData.size() << " 段，全是别的流";

        // 作答由本层的帧读取器与 QPACK 解码器解回来：这条要钉的是「会话真的回了一份能解开、
        // 且收尾完整的 400」。这条用例没走对端（畸形字节要绕过它的提交入口），因此这里的解回
        // 只作观察用；跨实现的字节对齐归 scripts/h3_acceptance.py 那套进程外探针判
        std::string answerBytes;
        bool        isAnswerEnded = false;
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            if (chunk.streamId != 0)
            {
                continue;
            }
            answerBytes.append(reinterpret_cast<const char *>(chunk.bytes.data()), chunk.bytes.size());
            isAnswerEnded = isAnswerEnded || chunk.isEndStream;
        }
        ASSERT_FALSE(answerBytes.empty());

        Http3FrameReader frameReader(64U * 1024U);
        ASSERT_TRUE(frameReader.feed(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(answerBytes.data()), answerBytes.size())).has_value());
        QpackDecoder answerDecoder(QpackDecoderSettings{.maximumTableCapacityByteCount = 4096, .maximumBlockedStreamCount = 100, .maximumFieldSectionSizeByteCount = 64U * 1024U});
        std::string  statusValue;
        std::string  answerBody;
        std::string  decoderStreamBytes;
        while (true)
        {
            const auto nextFrame = frameReader.nextFrame();
            ASSERT_TRUE(nextFrame.has_value()) << nextFrame.error().message;
            if (!nextFrame->has_value())
            {
                break;
            }
            if (const auto *headers = std::get_if<Http3HeadersFrame>(&**nextFrame); headers != nullptr)
            {
                std::vector<QpackHeaderField> answerFields;
                const auto                    decoded = answerDecoder.decodeFieldSection(0, headers->encodedFieldSection, answerFields, decoderStreamBytes);
                ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
                EXPECT_TRUE(*decoded == QpackFieldSectionDecodeStatus::Decoded) << "作答不该依赖动态表";
                for (const auto &field: answerFields)
                {
                    if (field.name == ":status")
                    {
                        statusValue = field.value;
                    }
                }
            } else if (const auto *data = std::get_if<Http3DataFrame>(&**nextFrame); data != nullptr)
            {
                answerBody.append(reinterpret_cast<const char *>(data->payload.data()), data->payload.size());
            }
        }

        EXPECT_EQ(statusValue, "400") << "作答的状态码不是 400";
        EXPECT_FALSE(answerBody.empty()) << "400 应当带上命中的规则，方便对端与运维定位";
        EXPECT_TRUE(isAnswerEnded) << "作答要把这条流收尾，不能让对端等正文";
    }

    /**
     * @brief HEAD 只收头部：正文一个字节都不发，content-length 仍按完整正文给出
     * @details h1/h2 都在发送那一刻把正文换成空（Router 明确把这件事留给会话），h3 此前照发正文，
     *          严格的对端会把它判成畸形响应（RFC 9110 §9.3.2）
     */
    TEST(Http3Session, SuppressesBodyForHeadRequests)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                                 return data.size();
                             });

        Router router;
        router.get("/bench",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setHeader("content-type", "text/plain");
                       response.setBody("OK");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;

        const std::vector<CapturedStreamData> requestChunks = peer.submitRequest("HEAD", "/bench", "example.com");
        ASSERT_FALSE(requestChunks.empty()) << "客户端没能产出任何字节（请求根本没编出来）";
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        ASSERT_FALSE(sentStreamData.empty()) << "服务端一个字节都没回：响应没发出去";
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        EXPECT_EQ(peer.response().status, 200) << "客户端没解出 200";
        EXPECT_TRUE(peer.response().body.empty()) << "HEAD 响应不该带正文（RFC 9110 §9.3.2）";
        EXPECT_TRUE(peer.response().isComplete) << "HEAD 响应没有收尾";
        const auto contentLengthHeader = peer.response().headers.find("content-length");
        ASSERT_NE(contentLengthHeader, peer.response().headers.end()) << "HEAD 响应仍要给出 content-length";
        EXPECT_EQ(contentLengthHeader->second, "2") << "content-length 必须等于 GET 会发出的那份正文长度";
    }

    /**
     * @brief 某个字段段里是否出现了这一对字段名与取值
     * @param section 一个字段段的字段表（DecodedResponse::fieldSections 的一项）
     * @param name 字段名
     * @param value 字段取值
     * @return true 该段里有这条字段
     */
    [[nodiscard]] bool hasFieldIn(const std::vector<std::pair<std::string, std::string>> &section, const std::string &name, const std::string &value)
    {
        return std::ranges::find(section, std::pair{name, value}) != section.end();
    }

    /**
     * @brief 走一条完整的 GET 往返，把服务端交出的字节喂回客户端
     * @details 下面几条「响应形状」用例只差业务往响应里写了什么，走完的步子完全一样，
     *          因此把提交请求、喂会话、pump、回喂客户端这四步收在这里
     * @param session 被测会话（由调用方持有，会话不可搬运）
     * @param peer 测试侧的客户端连接
     * @param sentStreamData 会话出口的字节收集容器
     * @param path 请求路径
     * @return 客户端解出来的响应
     */
    Http3ClientPeer::DecodedResponse answerOneGet(Http3Session &session, Http3ClientPeer &peer, std::vector<CapturedStreamData> &sentStreamData, const std::string &path)
    {
        const std::vector<CapturedStreamData> requestChunks = peer.submitRequest("GET", path, "example.com");
        EXPECT_FALSE(requestChunks.empty()) << "客户端没能产出任何字节（请求根本没编出来）";
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        EXPECT_FALSE(sentStreamData.empty()) << "服务端一个字节都没回：响应没发出去";
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        return peer.response();
    }

    /// 一条被本端收口的流：流号与写进 RESET_STREAM / STOP_SENDING 的应用错误码
    struct AbortedStream
    {
        std::int64_t  streamId{0};             ///< 被收口的流
        std::uint64_t applicationErrorCode{0}; ///< RFC 9114 §8.1 那一档的错误码
    };

    /**
     * @brief 造一个只挂了 writer 的会话：出口把字节按流收进 sentStreamData，开流口按本端单向流递增
     * @param opener 假开流口
     * @param sentStreamData 会话出口的字节收集容器
     * @param requestIdGenerator request-id 生成器（可空）
     * @param aborter 流收口出口（可空）：给了就能断言「本端有没有把这条流交代给传输层」
     * @return Http3Session 可按值搬走的会话
     */
    Http3Session makeSession(FakeStreamOpener &opener, std::vector<CapturedStreamData> &sentStreamData,
                             std::shared_ptr<AsynGyanis::Net::HttpRequestIdGenerator> requestIdGenerator = nullptr, Http3Session::StreamAborter aborter = {})
    {
        return Http3Session(
                std::ref(opener),
                [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                {
                    sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                    // 假出口一律全收：传输层的待发上界归 QuicStreamLayer 的用例测，
                    // 这里只关心「协议层交了什么字节」
                    return data.size();
                },
                {}, nullptr, nullptr, std::move(requestIdGenerator), std::move(aborter));
    }

    /**
     * @brief 造一条带掩码的 WebSocket 文本帧
     * @details 客户端→服务端的帧必须带掩码（RFC 6455 §5.3），测试侧自己拼一份比借用本端的编码器更独立
     * @param textPayload 负载
     * @param maskBytes 4 字节掩码
     * @return std::vector<std::uint8_t> 帧字节（含首字节、长度、掩码与掩蔽后的负载）
     */
    std::vector<std::uint8_t> makeMaskedTextFrame(const std::string_view textPayload, const std::array<std::uint8_t, 4> &maskBytes)
    {
        std::vector<std::uint8_t> frame{0x81U, static_cast<std::uint8_t>(0x80U | textPayload.size())};
        frame.insert(frame.end(), maskBytes.begin(), maskBytes.end());
        for (std::size_t payloadIndex = 0; payloadIndex < textPayload.size(); ++payloadIndex)
        {
            frame.push_back(static_cast<std::uint8_t>(textPayload[payloadIndex]) ^ maskBytes[payloadIndex % maskBytes.size()]);
        }
        return frame;
    }

    /**
     * @brief 可重复头逐条上线，且整段字段行的次序就是业务的设置顺序
     * @details 单值视图是「一名一值」，可重复头只在 headerValues() 里逐条给出：采集时直接用视图，
     *          业务设的第二条 Cookie 会静默消失（h1/h2 都会发全）。按名回查虽然能把值取全，
     *          次序却由那张视图的哈希顺序决定，同名多条会被归到一起——与 h1 的 appendHead 不一致。
     */
    TEST(Http3Session, SendsEveryValueOfRepeatableResponseHeaders)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.get("/cookies",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       // 交错设置：中间夹一条别的头，才能把「同名归组」与「按设置顺序发出」区分开
                       response.setHeader("set-cookie", "first=1");
                       response.setHeader("x-trace", "abc");
                       response.setHeader("set-cookie", "second=2");
                       response.setBody("ok");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;

        const Http3ClientPeer::DecodedResponse response = answerOneGet(session, peer, sentStreamData, "/cookies");
        EXPECT_EQ(response.countOf("set-cookie"), 2U) << "可重复响应头只剩一条，第二条被单值视图吃掉了";
        const std::vector<std::pair<std::string, std::string>> trackedFields = [&response]
        {
            std::vector<std::pair<std::string, std::string>> values;
            for (const auto &[name, value]: response.headerFields)
            {
                if (name == "set-cookie" || name == "x-trace")
                {
                    values.emplace_back(name, value);
                }
            }
            return values;
        }();
        // 逐条比对而不是只数条数：三条的相对次序正是「按设置顺序上线」这条契约
        EXPECT_EQ(trackedFields, (std::vector<std::pair<std::string, std::string>>{{"set-cookie", "first=1"}, {"x-trace", "abc"}, {"set-cookie", "second=2"}}))
                << "多条同名头的先后顺序要跟着业务的设置顺序，中间夹的头不能被归到后面";
    }

    /**
     * @brief 钉住：h3 的尾部字段发成正文之后的第二个字段段，且收尾由它带出来
     * @details RFC 9114 §4.3 与 h2 同源：尾段就是一个排在最后一个 DATA 之后的普通字段段，而它之后
     *          什么都不剩（FIN 只能跟着它）。三件事一并钉：段数、字段落在哪一段、头段带着 trailer 声明。
     *          h1/h2/h3 三条通路的这几条断言长得一样是刻意的——同一份业务代码换个协议，
     *          不该看到不同形状的报文（尾部字段的合规判定唯一落在 HttpResponse::addTrailerField）。
     */
    TEST(Http3Session, SendsTrailerFieldsInAFieldSectionAfterTheBody)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.get("/trailing",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       static_cast<void>(response.addTrailerField("x-checksum", "abc123"));
                       static_cast<void>(response.addTrailerField("x-rows", "2"));
                       response.setStatus(200);
                       response.setBody("trailed-body");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;

        const Http3ClientPeer::DecodedResponse response = answerOneGet(session, peer, sentStreamData, "/trailing");
        EXPECT_TRUE(response.decodeError.empty()) << "本端交出的字节连自家对端都解不开：" << response.decodeError;
        EXPECT_EQ(response.status, 200);
        EXPECT_EQ(response.body, "trailed-body");
        EXPECT_TRUE(response.isComplete) << "尾部字段没带 FIN：这条流在会话看来还没收尾";
        ASSERT_EQ(response.fieldSections.size(), 2U) << "应当恰好两个字段段：头段与尾段";

        EXPECT_TRUE(hasFieldIn(response.fieldSections[0], "trailer", "x-checksum, x-rows")) << "头段没声明这些字段";
        EXPECT_FALSE(hasFieldIn(response.fieldSections[0], "x-checksum", "abc123")) << "尾部字段混进了头段，身份就错了";
        EXPECT_TRUE(hasFieldIn(response.fieldSections[1], "x-checksum", "abc123"));
        EXPECT_TRUE(hasFieldIn(response.fieldSections[1], "x-rows", "2"));
        EXPECT_TRUE(response.fieldSections[1].front().first != ":status") << "尾段不许含伪头（RFC 9114 §7.2.4）";
    }

    /**
     * @brief 业务没写 date 时按 h1/h2 同一口径补上（RFC 9110 §6.1 要求源服务器给出）
     */
    TEST(Http3Session, AddsDateHeaderWhenHandlerOmitsIt)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.get("/dated",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setHeader("content-type", "text/plain");
                       response.setBody("ok");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;

        const Http3ClientPeer::DecodedResponse response   = answerOneGet(session, peer, sentStreamData, "/dated");
        const auto                             dateHeader = response.headers.find("date");
        ASSERT_NE(dateHeader, response.headers.end()) << "h1/h2 都会自动补 date，h3 漏给会让客户端自己做缓存判定";
        EXPECT_TRUE(dateHeader->second.ends_with("GMT")) << "date 必须是 IMF-fixdate 形态：" << dateHeader->second;
    }

    /**
     * @brief 有正文却没设媒体类型时按纯文本下发（与 HttpResponse::appendHead 的缺省一致）
     */
    TEST(Http3Session, DefaultsContentTypeWhenBodyIsPresent)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.get("/plain",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setBody("hi");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;

        const Http3ClientPeer::DecodedResponse response          = answerOneGet(session, peer, sentStreamData, "/plain");
        const auto                             contentTypeHeader = response.headers.find("content-type");
        ASSERT_NE(contentTypeHeader, response.headers.end()) << "有正文却没设类型，h1/h2 会补 text/plain";
        EXPECT_EQ(contentTypeHeader->second, "text/plain");
    }

    /**
     * @brief 越界的状态码改回 500，而不是把整条流废掉
     * @details setStatus 不校验取值范围，而 :status 必须是三位十进制（RFC 9114 §4.3.2）：
     *          原样交出去会被自己的连接层拒收，这条响应一个字节都发不出去，对端只能干等
     */
    TEST(Http3Session, MapsOutOfRangeStatusCodeBackToFiveHundred)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.get("/bogus",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(1000);
                       response.setBody("boom");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;

        const Http3ClientPeer::DecodedResponse response = answerOneGet(session, peer, sentStreamData, "/bogus");
        EXPECT_EQ(response.status, 500) << "越界状态码没有改回 500，而是把这条流的响应废掉了";
        EXPECT_EQ(response.body, "boom");
        EXPECT_TRUE(response.isComplete) << "改回 500 之后这条流仍要正常收尾";
    }

    /**
     * @brief 带 Expect: 100-continue 且声明了正文长度的请求，先收到 100 再收到最终响应
     * @details h1 与 h2 都会先回一个 100 催对端把正文发完（RFC 9110 §10.1.1）；h3 此前不接这个头，
     *          严格等 100 的对端只能靠自己的 expect 超时兜底。信息性响应是一条不带 END_STREAM
     *          的 HEADERS（RFC 9114 §5.3.2），随后才是最终响应
     */
    TEST(Http3Session, AnswersContinueInformationallyBeforeTheFinalResponse)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.post("/upload",
                    [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                    {
                        response.setStatus(201);
                        response.setBody(std::string(request.body()));
                        co_return;
                    });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", "abcd", 4, {{"expect", "100-continue"}, {"content-length", "4"}}))
                << "客户端没能提交这条带正文的请求";
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        const Http3ClientPeer::DecodedResponse response = peer.response();
        ASSERT_EQ(response.statuses.size(), 2U) << "应当有一条信息性响应加一条最终响应，实际收到的状态码序列不符";
        EXPECT_EQ(response.statuses[0], 100) << "第一条应是 100 Continue：对端在等它才敢发正文";
        EXPECT_EQ(response.statuses[1], 201) << "最终响应要照旧给出";
        EXPECT_EQ(response.body, "abcd") << "正文没有完整交给业务";
        EXPECT_TRUE(response.isComplete) << "这条流没有收尾";
    }

    /**
     * @brief 没有 Expect 的请求不会平白收到一个 100
     * @details 拒绝面：100 是给「等着被催」的对端的，给别的请求塞一条会让它多解一段头块
     */
    TEST(Http3Session, DoesNotSendContinueWhenExpectHeaderIsAbsent)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.post("/upload",
                    [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        response.setStatus(201);
                        response.setBody("done");
                        co_return;
                    });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", "abcd", 4, {{"content-length", "4"}})) << "客户端没能提交这条带正文的请求";
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        const Http3ClientPeer::DecodedResponse response = peer.response();
        ASSERT_EQ(response.statuses.size(), 1U) << "没带 Expect 的请求不该收到信息性响应";
        EXPECT_EQ(response.statuses[0], 201);
    }

    /**
     * @brief 业务处理器抛异常时回 500，且会话与后续请求都不受影响
     * @details 异常此前没人接，会穿出 pump()、打断 QuicServer 的收报文循环——整台服务此后
     *          不再处理任何报文。这条用例同时钉住「回 500」与「下一条请求照样正常」两件事
     */
    TEST(Http3Session, Answers500WhenHandlerThrowsAndStaysUsable)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                                 return data.size();
                             });

        Router router;
        router.get("/boom", [](HttpRequest &, HttpResponse &) -> Core::Task<> { throw std::runtime_error("intentional handler failure"); });
        router.get("/hello",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setBody("hi");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;

        // 第一条：处理器抛异常，必须是 500（而不是没有响应、也不是异常穿出去）
        for (const CapturedStreamData &chunk: peer.submitRequest("GET", "/boom", "example.com"))
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        // 响应必须真的排出去：异常若穿出 pump()，这里一个字节都不会有（status 会是 0）
        ASSERT_FALSE(sentStreamData.empty()) << "处理器抛异常后一条响应都没发：异常把 pump() 带走了";
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        EXPECT_EQ(peer.response().status, 500) << "处理器抛异常没有回 500";
        EXPECT_FALSE(peer.response().body.empty()) << "500 应当带一条可读的正文";

        // 第二条：会话仍然可用，正常请求照常 200（换一条请求流：0 号那条已经用过了）
        // 测试侧的 peer 只记一份响应、正文会跨请求累加，因此按「新增的那一段」核对
        const std::size_t bodyLengthBeforeSecondRequest = peer.response().body.size();
        sentStreamData.clear();
        for (const CapturedStreamData &chunk: peer.submitRequest("GET", "/hello", "example.com", kFirstRequestStreamId + 4))
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> secondPumpTask = session.pump();
        resumeUntilReady(secondPumpTask);
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        EXPECT_EQ(peer.response().status, 200) << "抛过异常之后会话不再服务后续请求";
        ASSERT_GE(peer.response().body.size(), bodyLengthBeforeSecondRequest);
        EXPECT_EQ(peer.response().body.substr(bodyLengthBeforeSecondRequest), "hi") << "第二条响应的正文";
    }

    /**
     * @brief 接上采集端后，h3 的请求数、状态码类与耗时如实落账
     * @details 与 h1/h2 同一套口径：收齐的请求计一条、响应按状态码类归档、耗时进直方图。
     *          这一条原先钉的是「耗时刻意不记」（理由是会话没有可信的请求起始戳）——那个理由在
     *          代码里站不住：请求收齐并交付业务的那一刻就是戳，h2 也正是从那一刻起算的。把 h3 排除
     *          在直方图外的代价是混合部署下的延迟读数只覆盖两条 TCP 通道，而看的人不知道。
     *          处理器里睡 2 毫秒是为了让「耗时是真量出来的」这句断言有下界可钉：本用例没有事件循环，
     *          阻塞的就是驱动会话的这一条线程，不影响任何判定。
     */
    TEST(Http3Session, ReportsRequestsAndStatusClassesToMetricsCollector)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        const auto                      metrics = std::make_shared<HttpMetricsCollector>();

        Http3Session session(
                std::ref(opener),
                [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                {
                    sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                    return data.size();
                },
                Http3Session::StreamCrediter{}, metrics);

        Router router;
        router.get("/hello",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       std::this_thread::sleep_for(std::chrono::milliseconds{2});
                       response.setStatus(200);
                       response.setBody("hi");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;

        for (const CapturedStreamData &chunk: peer.submitRequest("GET", "/hello", "example.com"))
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        // 服务端的字节要真的喂回客户端，才谈得上「这条请求被正常应答」
        ASSERT_FALSE(sentStreamData.empty()) << "服务端一个字节都没回：响应没发出去";
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        ASSERT_EQ(peer.response().status, 200) << "用例前提：这条请求应当被正常应答";

        const HttpServerStats snapshot = metrics->snapshot();
        EXPECT_EQ(snapshot.totalRequestCount, 1U) << "h3 的请求没有计入请求数";
        EXPECT_EQ(snapshot.status2xxCount, 1U) << "h3 的 200 响应没有计入 2xx";
        EXPECT_EQ(snapshot.badRequestCount, 0U);
        // 耗时是真量出来的：落了一个样本，且不小于处理器里那 2 毫秒
        EXPECT_EQ(snapshot.latencySampleCount(), 1U) << "h3 的响应没有落进耗时直方图";
        EXPECT_GE(snapshot.totalLatencyMicroseconds, 2000U) << "耗时不像从「收下请求」量起的：" << snapshot.totalLatencyMicroseconds << " 微秒";
    }

    /**
     * @brief 正文越界的 h3 请求：既计请求数，也计一条「坏请求」，响应状态码记 4xx
     */
    TEST(Http3Session, ReportsOversizeRequestAsBadRequestToMetricsCollector)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        const auto                      metrics = std::make_shared<HttpMetricsCollector>();

        Http3Session session(
                std::ref(opener),
                [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                {
                    sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                    return data.size();
                },
                Http3Session::StreamCrediter{}, metrics);

        HttpParserLimits limits;
        limits.maximumBodySize = 8;
        session.setParserLimits(limits);

        Router router;
        router.post("/upload",
                    [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        response.setStatus(200);
                        response.setBody("uploaded");
                        co_return;
                    });
        session.attachRouter(router);

        Http3ClientPeer   peer;
        const std::string oversizeBody = "0123456789abcdef"; // 16 字节，超过上限 8
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", oversizeBody, oversizeBody.size()));
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }
        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        ASSERT_EQ(peer.response().status, 413) << "用例前提：这条请求应当被 413 拒绝";

        const HttpServerStats snapshot = metrics->snapshot();
        EXPECT_EQ(snapshot.totalRequestCount, 1U) << "被 413 拒掉的请求同样是收齐的 h3 请求，要计入请求数";
        EXPECT_EQ(snapshot.badRequestCount, 1U) << "正文越界应当计一条坏请求（与 h2 同一口径）";
        EXPECT_EQ(snapshot.status4xxCount, 1U) << "413 属于 4xx 类";
    }

    /**
     * @brief 钉住：h3 的请求正文受 HttpParserLimits::maximumBodySize 约束，越界回 413 且不交给业务
     * @details h3 此前完全没有正文上限（h1 有在途预算、h2 有 413），一条 POST 就能把内存吃光。
     *          上限调到 8 字节触发，避免用例为了越界真去分配默认上限那么大的缓冲。
     */
    TEST(Http3Session, RejectsOversizeRequestBodyWith413)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                                 return data.size();
                             });

        HttpParserLimits limits;
        limits.maximumBodySize = 8;
        session.setParserLimits(limits);

        bool   isHandlerEntered = false;
        Router router;
        router.post("/upload",
                    [&isHandlerEntered](HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        isHandlerEntered = true;
                        response.setStatus(200);
                        response.setBody("uploaded");
                        co_return;
                    });
        session.attachRouter(router);

        Http3ClientPeer peer;

        const std::string oversizeBody = "0123456789abcdef"; // 16 字节，超过上限 8
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", oversizeBody, oversizeBody.size()));
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        EXPECT_FALSE(isHandlerEntered) << "正文越界的请求不该交给业务";
        EXPECT_EQ(peer.response().status, 413) << "正文越界必须回 413（与 h1/h2 同一口径）";
        EXPECT_EQ(peer.response().body, "Payload Too Large");
    }

    /**
     * @brief 跑一条「头 → 正文 → 尾段收尾」的请求，把处理器报回来的字符串交给断言
     * @param registerRoute 往路由器上挂那条路（普通或流式由调用方决定）
     * @param body 请求正文
     * @return std::pair<std::string, int> 处理器写进响应体的内容与 :status
     */
    template<typename RouteRegistrar>
    std::pair<std::string, int> serveRequestWithTrailers(RouteRegistrar registerRoute, const std::string &body)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                                 return data.size();
                             });

        Router router;
        registerRoute(router);
        session.attachRouter(router);

        Http3ClientPeer                     peer;
        const std::vector<QpackHeaderField> trailerFields{QpackHeaderField{"x-checksum", "abc123"}};
        for (const CapturedStreamData &step: peer.submitRequestWithBodyAndTrailers("POST", "/echo-tail", "example.com", body, trailerFields))
        {
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        return {peer.response().body, peer.response().status};
    }

    /**
     * @brief 钉住：h3 尾段字段落进请求的 trailer 一档，且**不**出现在头部（非流式派发）
     * @details 改造前尾段的字段与普通头部走同一条 addHeader：正文之后到达的字段于是有了头部的身份，
     *          业务按头部读到的值与线上「这是尾部」的事实不符。现在两处都验：trailer 档读得到，
     *          头部读不到。删掉会话里的分流（让它回到 addHeader）这条就红。
     */
    TEST(Http3Session, DeliversTrailerSectionIntoTheTrailerStore)
    {
        const auto [body, status] = serveRequestWithTrailers(
                [](Router &router)
                {
                    router.post("/echo-tail",
                                [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                {
                                    response.setStatus(200);
                                    response.setBody("tf=" + request.getTrailerField("x-checksum").value_or("-") +
                                                     "|h=" + (request.getHeader("x-checksum").has_value() ? "yes" : "no"));
                                    co_return;
                                });
                },
                "abc");

        EXPECT_EQ(status, 200);
        EXPECT_EQ(body, "tf=abc123|h=no") << "尾段字段没有落到 trailer 一档，或同时混进了头部";
    }

    /**
     * @brief 钉住：流式派发的请求也在正文读完之后看到尾段字段
     * @details 这条走的是第二个落点：请求记录在头收齐时就从 m_incomingRequests 搬进
     *          m_streamingRequests，尾段到达时只能落在搬走后的那一份上。漏了那一支，
     *          流式路由的处理器会读到空（而字段被塞进一条无人认领的新记录里）。
     */
    TEST(Http3Session, DeliversTrailerSectionToStreamingRoute)
    {
        const auto [body, status] = serveRequestWithTrailers(
                [](Router &router)
                {
                    router.postStreaming("/echo-tail",
                                         [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                                         {
                                             std::size_t bodyByteCount = 0;
                                             while (co_await request.bodyStream()->readNext())
                                             {
                                                 bodyByteCount += request.bodyStream()->chunk().size();
                                             }
                                             response.setStatus(200);
                                             response.setBody("bytes=" + std::to_string(bodyByteCount) + "|tf=" + request.getTrailerField("x-checksum").value_or("-"));
                                             co_return;
                                         });
                },
                "abcdefghijkl");

        EXPECT_EQ(status, 200);
        EXPECT_EQ(body, "bytes=12|tf=abc123") << "流式路由没读到尾段字段，或正文按批交付被改动";
    }

    /**
     * @brief 把请求正文的字节按 DATA 帧归还给 QUIC 的接收窗口
     * @details read_stream2 的消费计数不含 DATA 负载，正文那部分必须单独归还；漏了这条，
     *          正文一大就会把接收窗口用光（对端随后被流控卡住，而本端并不知道为什么）
     */
    TEST(Http3Session, CreditsRequestBodyBytesBackToTheTransport)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        std::size_t                     creditedByteCount{0};

        Http3Session session(
                std::ref(opener),
                [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                {
                    sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                    return data.size();
                },
                [&creditedByteCount](const std::int64_t, const std::size_t consumedByteCount) { creditedByteCount += consumedByteCount; });

        const std::vector<std::uint8_t> payload{'a', 's', 'y', 'n'};
        session.addRequestHeader(kFirstRequestStreamId, ":method", "POST", false);
        session.addRequestHeader(kFirstRequestStreamId, ":path", "/upload", false);
        session.addRequestBody(kFirstRequestStreamId, payload);

        EXPECT_EQ(creditedByteCount, payload.size()) << "请求正文的字节没有被归还给接收窗口";
    }

    /**
     * @brief h3 上的流式响应（startChunkedResponse + writeChunk）逐块送到客户端
     * @details 顺带钉住两条：流式响应不带 content-length（长度此刻还不知道），以及各块都真的到了
     *          客户端手里
     */
    TEST(Http3Session, StreamsChunkedResponseToTheClient)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                                 return data.size();
                             });

        Router router;
        router.get("/stream",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.startChunkedResponse(200);
                       response.setHeader("content-type", "text/event-stream");
                       if (!co_await response.writeChunk("data: one\n\n"))
                       {
                           co_return;
                       }
                       static_cast<void>(co_await response.writeChunk("data: two\n\n"));
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer                       peer;
        const std::vector<CapturedStreamData> requestChunks = peer.submitRequest("GET", "/stream", "example.com");
        ASSERT_FALSE(requestChunks.empty());
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        EXPECT_EQ(peer.response().status, 200) << "流式响应的状态码没有送到";
        EXPECT_EQ(peer.response().body, "data: one\n\ndata: two\n\n") << "流式响应的各块没有完整到达客户端";
        EXPECT_TRUE(peer.response().isComplete) << "流式响应没有收尾";
        const auto contentTypeHeader = peer.response().headers.find("content-type");
        ASSERT_NE(contentTypeHeader, peer.response().headers.end());
        EXPECT_EQ(contentTypeHeader->second, "text/event-stream");
        EXPECT_EQ(peer.response().headers.count("content-length"), 0U) << "流式响应的长度此刻还不知道，不该带 content-length";
    }

    /**
     * @brief 流式正文路由在 h3 上真的边收边读：处理器在正文收齐之前就拿到了前几批
     * @details 断言分两步下：先把请求分步送到服务端，只送到「头部 + 第一批」时就要求处理器已经
     *          进去且读到了第一批（这是「不等整份正文」的直接证据）；再把余下的送完，要求它读到
     *          全部字节、按批交付，并且响应是 200。
     */
    TEST(Http3Session, StreamsRequestBodyToTheHandlerBeforeTheBodyIsComplete)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                                 return data.size();
                             });

        constexpr std::size_t    kChunkByteCount = 4;
        const std::string        body            = "abcdefghijkl"; // 共 3 批
        std::vector<std::size_t> observedChunkByteCounts;
        bool                     isHandlerEntered = false;

        Router router;
        router.postStreaming("/upload",
                             [&observedChunkByteCounts, &isHandlerEntered](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                             {
                                 isHandlerEntered = true;
                                 while (co_await request.bodyStream()->readNext())
                                 {
                                     observedChunkByteCounts.push_back(request.bodyStream()->chunk().size());
                                 }
                                 response.setStatus(200);
                                 response.setBody("uploaded");
                                 co_return;
                             });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", body, kChunkByteCount));

        // 只送到「处理器拿到第一批」为止：此刻正文还远没收齐，处理器却已经进去了
        bool isFirstChunkObserved = false;
        for (std::size_t stepIndex = 0; stepIndex < 16 && !isFirstChunkObserved; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
            isFirstChunkObserved = isHandlerEntered && !observedChunkByteCounts.empty();
        }
        EXPECT_TRUE(isFirstChunkObserved) << "正文收齐之前处理器没拿到第一批：这条路径不是边收边读";
        ASSERT_FALSE(observedChunkByteCounts.empty());
        EXPECT_LT(observedChunkByteCounts.front(), body.size()) << "第一批就是整份正文：说明还是整段收全之后才派的发";

        // 把余下的送完（含收尾），处理器应当读到全部正文
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        std::size_t totalObservedByteCount = 0;
        for (const std::size_t chunkByteCount: observedChunkByteCounts)
        {
            totalObservedByteCount += chunkByteCount;
        }
        EXPECT_EQ(totalObservedByteCount, body.size()) << "处理器读到的正文总量与发出去的不一致";
        EXPECT_GE(observedChunkByteCounts.size(), 2U) << "正文没有按批交付：边收边读没有真正成立";

        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        EXPECT_EQ(peer.response().status, 200) << "流式正文路由应当正常服务，而不是回错";
        EXPECT_EQ(peer.response().body, "uploaded") << "处理器的响应没有回到客户端";
    }

    /**
     * @brief 钉住：走流式正文路由的 h3 请求也要计入采集端（请求数、状态码类、耗时）
     * @details 这条漏斗原先一笔都不落账：普通请求的记账写在收齐后派发的那一段里，而流式路由是
     *          「头部收齐即派发」、由本流自己的协程服务到底，绕过了那一段。后果是「只挂流式上传路由」
     *          的 h3 服务在 /metrics 上看着像没有流量——请求确实被收下并答了，计数却全是零。
     *          h1/h2 两条通道的流式路由都在同一条派发路径上记账，因此这是 h3 独有的漏项。
     */
    TEST(Http3Session, CountsStreamingBodyRouteRequestIntoMetricsCollector)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        const auto                      metrics = std::make_shared<HttpMetricsCollector>();

        Http3Session session(
                std::ref(opener),
                [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                {
                    sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                    return data.size();
                },
                Http3Session::StreamCrediter{}, metrics);

        Router router;
        router.postStreaming("/upload",
                             [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                             {
                                 while (co_await request.bodyStream()->readNext())
                                 {
                                 }
                                 response.setStatus(200);
                                 response.setBody("uploaded");
                                 co_return;
                             });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", "streaming-upload-body", 8));
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        ASSERT_EQ(peer.response().status, 200) << "用例前提：这条流式请求应当被正常应答";

        const HttpServerStats snapshot = metrics->snapshot();
        EXPECT_EQ(snapshot.totalRequestCount, 1U) << "流式正文路由的请求没计入请求数：这类服务在指标上像没有流量";
        EXPECT_EQ(snapshot.status2xxCount, 1U) << "流式正文路由的 200 没计入状态码类";
        EXPECT_EQ(snapshot.latencySampleCount(), 1U) << "流式正文路由的响应没落进耗时直方图";
    }
    /**
     * @brief 对端取消（RESET_STREAM）一条正在收正文的流：会话在下一个安全点把它整条回收
     * @details 会话层自己看不到 QUIC 层的重置信号。少了承载层这一路通知，被取消的请求会连同已攒下的
     *          正文一直留在会话里（对端还能靠归还的 STREAMS 额度反复重来），等正文的处理器更是永远
     *          等不到唤醒。这条用例钉住三件事：处理器被唤醒收尾、请求不再被应答、计入单流取消
     */
    TEST(Http3Session, ReclaimsStreamCancelledByPeer)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        const auto                      metrics = std::make_shared<HttpMetricsCollector>();

        Http3Session session(
                std::ref(opener),
                [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                {
                    sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                    return data.size();
                },
                Http3Session::StreamCrediter{}, metrics);

        constexpr std::size_t    kChunkByteCount = 4;
        const std::string        body            = "abcdefghijkl"; // 共 3 批
        std::vector<std::size_t> observedChunkByteCounts;
        bool                     isHandlerEntered  = false;
        bool                     isHandlerFinished = false;

        Router router;
        router.postStreaming("/upload",
                             [&observedChunkByteCounts, &isHandlerEntered, &isHandlerFinished](HttpRequest &request, HttpResponse &) -> Core::Task<>
                             {
                                 isHandlerEntered = true;
                                 while (co_await request.bodyStream()->readNext())
                                 {
                                     observedChunkByteCounts.push_back(request.bodyStream()->chunk().size());
                                 }
                                 // 流被取消后 readNext() 必须终止循环；挂在这里就是「等待者永不唤醒」那个缺陷
                                 isHandlerFinished = true;
                                 co_return;
                             });
        session.attachRouter(router);

        Http3ClientPeer peer;
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", body, kChunkByteCount));

        // 只送到「处理器进入且读到第一批」为止：此刻正文还没收完，这条流是活的
        bool isFirstChunkObserved = false;
        for (std::size_t stepIndex = 0; stepIndex < 16 && !isFirstChunkObserved; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
            isFirstChunkObserved = isHandlerEntered && !observedChunkByteCounts.empty();
        }
        ASSERT_TRUE(isFirstChunkObserved) << "用例前提：处理器应当在正文收齐之前就拿到第一批";

        // 对端放弃这条请求：承载层把 RESET_STREAM 转成这一声通知，回收发生在下一个安全点（pump）
        session.cancelStreamByPeer(kFirstRequestStreamId);
        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        EXPECT_TRUE(isHandlerFinished) << "流被取消后等正文的处理器没有醒过来：等待者被留在了已摘掉的记录上";
        const bool hasRequestStreamBytes = std::ranges::any_of(sentStreamData, [](const CapturedStreamData &sent) { return sent.streamId == kFirstRequestStreamId; });
        EXPECT_FALSE(hasRequestStreamBytes) << "被取消的流不该再发出任何响应字节";
        const HttpServerStats snapshot = metrics->snapshot();
        EXPECT_EQ(snapshot.streamCancelledCount, 1U) << "被对端取消的流没有计入单流取消";
    }


    /**
     * @brief 扩展 CONNECT 与 END_STREAM 同一趟到达时，隧道建立即收尾、响应正常收完
     * @details 头收齐那一刻流号只进了「待建隧道」集合，随后的 END_STREAM 此前被这条分支直接丢掉：
     *          隧道建成后业务永远挂在 receive() 上、对端也拿不到响应收尾。这里钉住「头与 END_STREAM
     *          同趟到达」这条路径——客户端的响应必须收尾（isComplete）。
     *          顺带在这里钉隧道的采集账：隧道那条请求计入请求数，应答（h3 按 RFC 9220 是 2xx，
     *          没有 101 这一档）也要落状态码类与耗时——原先 `continue` 直接跳过了整个落账分支，
     *          于是只挂 WebSocket 的 h3 服务在 /metrics 上是「有请求、没响应」。
     */
    TEST(Http3Session, ClosesTunnelWhenConnectAndEndStreamArriveTogether)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        const auto                      metrics = std::make_shared<HttpMetricsCollector>();

        Http3Session session(
                std::ref(opener),
                [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                {
                    sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                    return data.size();
                },
                Http3Session::StreamCrediter{}, metrics);

        bool   isBusinessFinished = false;
        Router router;
        router.get("/chat",
                   [&isBusinessFinished](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.upgradeToWebSocket(
                               [&isBusinessFinished](WebSocketPeer &peer) -> Core::Task<>
                               {
                                   // 对端一上来就收尾：receive() 必须立刻返回「没有更多消息」，
                                   // 而不是永远挂着
                                   while (const auto message = co_await peer.receive())
                                   {
                                       static_cast<void>(message);
                                   }
                                   isBusinessFinished = true;
                                   co_return;
                               });
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;

        const std::vector<CapturedStreamData> requestChunks = peer.submitEndedWebSocketTunnel("/chat", "example.com");
        ASSERT_FALSE(requestChunks.empty()) << "扩展 CONNECT 请求没编出来";
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        EXPECT_EQ(peer.response().status, 200) << "隧道没有以 2xx 应答";
        EXPECT_TRUE(peer.response().isComplete) << "对端已收尾的隧道没有跟着收口：响应永远收不完（业务也醒不过来）";
        EXPECT_TRUE(isBusinessFinished) << "隧道收口后业务没有醒来收尾";
        EXPECT_FALSE(session.hasOutstandingWork()) << "同趟收尾的隧道收口后仍留在账上：承载层会一直认为这条连接有在途工作";

        const HttpServerStats snapshot = metrics->snapshot();
        EXPECT_EQ(snapshot.totalRequestCount, 1U) << "隧道这条请求没计入请求数";
        EXPECT_EQ(snapshot.status2xxCount, 1U) << "隧道的 2xx 应答没落进状态码类：h3 没有 101 这一档";
        EXPECT_EQ(snapshot.latencySampleCount(), 1U) << "隧道的响应没落进耗时直方图";
    }

    /**
     * @brief 承载连接没了：还挂在 receive() 上的隧道业务要醒来收尾，会话随后才算空闲
     * @details 传输层收口不会逐条流发信号，这条流上没有「对端取消」那一路通知；少了 abandon 这一步，
     *          会话被摘掉时连着业务协程帧一起销毁，等待之后的收尾永不执行。第二次调用把「业务已跑完
     *          且流已关闭」的记录摘走，承载层据此才敢收连接——两条断言分别钉住唤醒与收敛
     */
    TEST(Http3Session, WakesTunnelBusinessWhenTheCarryingConnectionIsGone)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        bool   isBusinessFinished = false;
        Router router;
        router.get("/chat",
                   [&isBusinessFinished](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.upgradeToWebSocket(
                               [&isBusinessFinished](WebSocketPeer &peer) -> Core::Task<>
                               {
                                   while (const auto message = co_await peer.receive())
                                   {
                                       static_cast<void>(message);
                                   }
                                   isBusinessFinished = true;
                                   co_return;
                               });
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        // 带一条帧而不带 END_STREAM：隧道建起来、业务读完这一条，然后挂在 receive() 上等下一条
        const std::array<std::uint8_t, 4>     mask{0x11U, 0x22U, 0x33U, 0x44U};
        const std::vector<std::uint8_t>       firstFrameBytes = makeMaskedTextFrame("hello", mask);
        const std::vector<CapturedStreamData> requestChunks   = peer.submitWebSocketTunnel("/chat", "example.com", std::string(firstFrameBytes.begin(), firstFrameBytes.end()));
        ASSERT_FALSE(requestChunks.empty()) << "扩展 CONNECT 请求没编出来";
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        ASSERT_TRUE(session.hasOutstandingWork()) << "隧道没建起来，这条用例就没东西可唤醒";

        session.abandonPendingStreams();
        EXPECT_TRUE(isBusinessFinished) << "承载连接收口时没唤醒挂在 receive() 上的隧道业务";

        session.abandonPendingStreams();
        EXPECT_FALSE(session.hasOutstandingWork()) << "业务跑完后会话仍报「有在途工作」：承载层会一直不敢收这条连接";
    }

    /**
     * @brief 对端事后用 END_STREAM 收隧道：业务醒过来收尾，会话也要跟着报「手上没活」
     * @details 这条路径上两个动作缺一不可：唤醒经 `finishRequest()` 推到 `pump()` 的安全点做（当场叫醒
     *          等于在连接层回调里重入连接层），记录要在那一趟里摘掉——留着它 `hasOutstandingWork()` 就永远
     *          为真，单连接请求条数到量后的排空收口与优雅停机等的都是这条已经死掉的隧道
     */
    TEST(Http3Session, ReapsTunnelClosedByPeersLaterEndStream)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        bool   isBusinessFinished = false;
        Router router;
        router.get("/chat",
                   [&isBusinessFinished](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.upgradeToWebSocket(
                               [&isBusinessFinished](WebSocketPeer &peer) -> Core::Task<>
                               {
                                   while (const auto message = co_await peer.receive())
                                   {
                                       static_cast<void>(message);
                                   }
                                   isBusinessFinished = true;
                                   co_return;
                               });
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        // 只带一条帧、不带 END_STREAM：隧道建起来，业务读完这一条后挂在 receive() 上等下一条
        const std::array<std::uint8_t, 4>     mask{0x11U, 0x22U, 0x33U, 0x44U};
        const std::vector<std::uint8_t>       firstFrameBytes       = makeMaskedTextFrame("hello", mask);
        const std::size_t                     sentCountAfterConnect = sentStreamData.size();
        const std::vector<CapturedStreamData> requestChunks = peer.submitWebSocketTunnel("/chat", "example.com", std::string(firstFrameBytes.begin(), firstFrameBytes.end()));
        ASSERT_FALSE(requestChunks.empty()) << "扩展 CONNECT 请求没编出来";
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> firstPumpTask = session.pump();
        resumeUntilReady(firstPumpTask);
        ASSERT_FALSE(isBusinessFinished) << "用例前提：业务要还挂在 receive() 上";
        ASSERT_TRUE(session.hasOutstandingWork()) << "用例前提：隧道没建起来就没东西可收口";

        // 对端事后收尾：走真实的连接层入口（空正文 + END_STREAM），而不是直接调 finishRequest()
        session.onStreamData(kFirstRequestStreamId, {}, true);
        EXPECT_FALSE(isBusinessFinished) << "收到对端收尾的那一趟就把业务叫醒：那是在连接层的回调里重入连接层";

        Core::Task<> secondPumpTask = session.pump();
        resumeUntilReady(secondPumpTask);
        EXPECT_TRUE(isBusinessFinished) << "安全点没有兑现收尾：业务永远挂在 receive() 上";

        for (std::size_t writtenIndex = sentCountAfterConnect; writtenIndex < sentStreamData.size(); ++writtenIndex)
        {
            const CapturedStreamData &written = sentStreamData[writtenIndex];
            if (written.streamId == kFirstRequestStreamId)
            {
                peer.receive(written.streamId, written.bytes, written.isEndStream);
            }
        }
        EXPECT_TRUE(peer.response().isComplete) << "本端没跟着交出 END_STREAM：对端那条流永远收不完";
        EXPECT_FALSE(session.hasOutstandingWork()) << "隧道已收口却还留在账上：排空收口与优雅停机等的就是这条死流";
    }

    /**
     * @brief 被唤醒之后还要再挂一次的业务：隧道记录不能被摘走，等它真跑完才摘
     * @details 摘账的判据是「业务跑完 且 流已关闭」两个标记，缺一不可。这条用例把「流已关闭但业务
     *          还没跑完」这一档单独造出来（处理器在 receive() 返回终点之后又挂起一次），因为协程帧
     *          由隧道记录持有——提前摘表等于把还在别人手里的句柄连帧一起销毁
     */
    TEST(Http3Session, KeepsTunnelRecordAliveWhileBusinessStillSuspended)
    {
        struct SuspendUntilResumed
        {
            std::coroutine_handle<> *slot; ///< 用例手里那只句柄的落点：等它被外部 resume

            [[nodiscard]] bool await_ready() const noexcept
            {
                return false;
            }
            void await_suspend(const std::coroutine_handle<> waiter) const noexcept
            {
                *slot = waiter;
            }
            static void await_resume() noexcept
            {
            }
        };

        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        std::coroutine_handle<> handlerWaiter{};
        bool                    isBusinessFinished = false;
        Router                  router;
        router.get("/chat",
                   [&isBusinessFinished, &handlerWaiter](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.upgradeToWebSocket(
                               [&isBusinessFinished, &handlerWaiter](WebSocketPeer &peer) -> Core::Task<>
                               {
                                   while (const auto message = co_await peer.receive())
                                   {
                                       static_cast<void>(message);
                                   }
                                   // 收尾还要做一次异步动作（真业务里是刷最后一段帧或等落盘）：
                                   // 此刻流已关闭、业务没跑完，记录必须还在
                                   co_await SuspendUntilResumed{&handlerWaiter};
                                   isBusinessFinished = true;
                                   co_return;
                               });
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer                       peer;
        const std::array<std::uint8_t, 4>     mask{0x11U, 0x22U, 0x33U, 0x44U};
        const std::vector<std::uint8_t>       firstFrameBytes = makeMaskedTextFrame("hello", mask);
        const std::vector<CapturedStreamData> requestChunks   = peer.submitWebSocketTunnel("/chat", "example.com", std::string(firstFrameBytes.begin(), firstFrameBytes.end()));
        ASSERT_FALSE(requestChunks.empty()) << "扩展 CONNECT 请求没编出来";
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> firstPumpTask = session.pump();
        resumeUntilReady(firstPumpTask);
        ASSERT_TRUE(session.hasOutstandingWork()) << "用例前提：隧道要已经建起来并挂着业务";

        session.onStreamData(kFirstRequestStreamId, {}, true);
        Core::Task<> secondPumpTask = session.pump();
        resumeUntilReady(secondPumpTask);
        ASSERT_FALSE(isBusinessFinished) << "用例前提：业务要在「唤醒之后又挂起一次」的位置上";
        ASSERT_TRUE(handlerWaiter != nullptr) << "业务没挂起来，这条用例就没东西可保命";
        EXPECT_TRUE(session.hasOutstandingWork()) << "流关闭就把记录摘走了：还挂在处理器协程里的帧被连帧销毁，下一次 resume 用的是已释放内存";

        handlerWaiter.resume();
        EXPECT_TRUE(isBusinessFinished) << "恢复之后业务没跑完";
        // 业务是在泵之外跑完的：下一趟泵收尾时要把这条记录摘掉，否则账又留下一个恒真的「有在途工作」
        Core::Task<> thirdPumpTask = session.pump();
        resumeUntilReady(thirdPumpTask);
        EXPECT_FALSE(session.hasOutstandingWork()) << "业务跑完之后记录仍留在账上：摘账只挂在收口那一趟，漏了迟到的那一批";
    }

    /**
     * @brief 正文超出全局在途预算时回 503，且不交给业务（与 h1/h2 同一口径）
     */
    TEST(Http3Session, Answers503WhenInflightBodyBudgetIsExhausted)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        const auto                      budget = std::make_shared<HttpMemoryBudget>(8); // 只够 8 字节正文

        Http3Session session(
                std::ref(opener),
                [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                {
                    sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                    return data.size();
                },
                Http3Session::StreamCrediter{}, nullptr, budget);

        bool   isHandlerEntered = false;
        Router router;
        router.post("/upload",
                    [&isHandlerEntered](HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        isHandlerEntered = true;
                        response.setStatus(200);
                        response.setBody("uploaded");
                        co_return;
                    });
        session.attachRouter(router);

        Http3ClientPeer peer;

        const std::string oversizeBody = "0123456789abcdef"; // 16 字节，超过预算 8
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", oversizeBody, oversizeBody.size()));
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        EXPECT_FALSE(isHandlerEntered) << "超出全局预算的请求不该交给业务";
        EXPECT_EQ(peer.response().status, 503) << "全局在途预算不足必须回 503（与 h1/h2 同一口径）";
        EXPECT_EQ(budget->reservedByteCount(), 0U) << "被拒的请求不该占着额度（记录析构即归还）";
    }

    /**
     * @brief 一条还没收齐正文的流被摘掉时，已缓冲的那一段要立刻把全局额度还回来
     * @details 这是一处跨连接的放大：客户端每条连接只发半截正文再取消，攒够预算就把整台服务器的
     *          在途正文限额占死，之后所有带正文的请求都按 503 收口。收齐、派发、被拒三条路都测过，
     *          「没收齐就被摘」这一路此前没人直测。摘记录走的是 `dropRequest()`——传输层报上来的
     *          取消（`cancelStreamByPeer()`）只排个号，回收要等活的连接对象把通知送回来
     */
    TEST(Http3Session, ReturnsBudgetWhenStreamIsDroppedMidBody)
    {
        FakeStreamOpener opener;
        const auto       budget = std::make_shared<HttpMemoryBudget>(100);

        Http3Session session(
                std::ref(opener), [](const std::int64_t, const std::span<const std::uint8_t> data, const bool) { return data.size(); }, Http3Session::StreamCrediter{}, nullptr,
                budget);

        const std::vector<std::uint8_t> partialBody(40, 'z');
        session.addRequestHeader(kFirstRequestStreamId, ":method", "POST", false);
        session.addRequestHeader(kFirstRequestStreamId, ":path", "/upload", false);
        session.addRequestHeader(kFirstRequestStreamId, "content-length", "60", false);
        session.addRequestBody(kFirstRequestStreamId, partialBody);
        // 不调 finishRequest：正文只到了 40/60，这条请求还躺在待服务记录里

        EXPECT_EQ(budget->reservedByteCount(), partialBody.size()) << "已缓冲的正文没占着额度，这条用例也就测不到归还";

        session.dropRequest(kFirstRequestStreamId);
        EXPECT_EQ(budget->reservedByteCount(), 0U) << "被摘掉的流仍占着全局额度：半截正文加一次取消就能把限额占死，当前占用 " << budget->reservedByteCount();
    }

    /**
     * @brief 已经收齐、还排在派发队列里的请求被摘掉时，账也要跟着走
     * @details 收齐那一刻正文的额度从待服务记录搬进了派发记录，摘除点因此有两处；只还前一处
     *          的话，「对端在派发之前就重置了流」这一路照样漏账
     */
    TEST(Http3Session, ReturnsBudgetWhenQueuedRequestIsDroppedBeforeDispatch)
    {
        FakeStreamOpener opener;
        const auto       budget = std::make_shared<HttpMemoryBudget>(100);

        Http3Session session(
                std::ref(opener), [](const std::int64_t, const std::span<const std::uint8_t> data, const bool) { return data.size(); }, Http3Session::StreamCrediter{}, nullptr,
                budget);

        const std::vector<std::uint8_t> fullBody(40, 'z');
        session.addRequestHeader(kFirstRequestStreamId, ":method", "POST", false);
        session.addRequestHeader(kFirstRequestStreamId, ":path", "/upload", false);
        session.addRequestHeader(kFirstRequestStreamId, "content-length", "40", false);
        session.addRequestBody(kFirstRequestStreamId, fullBody);
        session.finishRequest(kFirstRequestStreamId);
        // 不 pump：请求停在派发队列里，额度已经搬到那条记录上

        ASSERT_TRUE(session.hasOutstandingWork()) << "请求没排进派发队列，这条用例测不到排队那一处摘除点";
        EXPECT_EQ(budget->reservedByteCount(), fullBody.size()) << "排队中的正文没占着额度，用例也就测不到归还";

        session.dropRequest(kFirstRequestStreamId);
        EXPECT_EQ(budget->reservedByteCount(), 0U) << "排队记录被摘掉后仍占着 " << budget->reservedByteCount() << " 字节全局额度";
    }

    /**
     * @brief 钉住：流式处理器丢下的正文，摘记录前要把接收窗口还给对端
     * @details 承载层把 DATA 载荷的归还留给上层（`creditConsumedBytes` 只补帧开销、明确扣掉载荷），
     *          h2 侧由 `finishStreamingRequestBody()` 兑现；h3 的流式记录是被收尾步骤摘掉的，
     *          不在摘除点还账就等于「业务不看正文 → 这些字节永久占着对端的连接级 MAX_DATA」，
     *          攒够一轮整条连接就收不进东西了
     */
    TEST(Http3Session, ReturnsWindowForBodyAbandonedByStreamingHandler)
    {
        FakeStreamOpener                                  opener;
        std::vector<CapturedStreamData>                   sentStreamData;
        std::vector<std::pair<std::int64_t, std::size_t>> creditedChunks;

        Http3Session session(
                std::ref(opener),
                [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                {
                    sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                    return data.size();
                },
                [&creditedChunks](const std::int64_t streamId, const std::size_t consumedByteCount) { creditedChunks.emplace_back(streamId, consumedByteCount); }, nullptr);

        bool   isHandlerEntered = false;
        Router router;
        router.postStreaming("/streaming-upload",
                             [&isHandlerEntered](HttpRequest &, HttpResponse &response) -> Core::Task<>
                             {
                                 isHandlerEntered = true;
                                 // 刻意不碰 request.bodyStream()：这就是「处理器提前作答」的常见形状
                                 response.setStatus(200);
                                 response.setBody("done");
                                 co_return;
                             });
        session.attachRouter(router);

        Http3ClientPeer peer;

        const std::string body(4096, 'z');
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/streaming-upload", "example.com", body, body.size()));
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        for (int pumpRound = 0; pumpRound < 6 && session.hasOutstandingWork(); ++pumpRound)
        {
            Core::Task<> pumpTask = session.pump();
            resumeUntilReady(pumpTask);
        }
        EXPECT_FALSE(session.hasOutstandingWork()) << "记录没被收尾摘掉：这条用例测不到摘除点还账";
        ASSERT_TRUE(isHandlerEntered) << "流式路由没把请求交给业务，这条用例就没走到摘除点";

        std::size_t creditedByteCount = 0;
        for (const auto &[streamId, consumedByteCount]: creditedChunks)
        {
            static_cast<void>(streamId);
            creditedByteCount += consumedByteCount;
        }
        EXPECT_GE(creditedByteCount, body.size()) << "摘掉流式记录时没把正文占掉的接收窗口还回去（还回的只会是帧开销）";
    }

    /**
     * @brief 流式路由答一个无正文响应之后，这条流式记录要能被收尾摘掉
     * @details 无正文的应答只由 `submitResponseHead` 记下「本端已收尾」，关闭通知要等尾字节真的交给
     *          传输层才发得出。业务协程答完就把 isServeFinished 置上，摘除点另一半场（isStreamClosed）
     *          则来自承载层的 onStreamClosed——不 flush 就永远等不到它，记录连同它占着的窗口额度一起
     *          挂到连接收口，排空判定也随之永久为真
     */
    TEST(Http3Session, ReapsStreamingRecordAfterBodylessResponse)
    {
        FakeStreamOpener opener;
        Http3Session     session(
                std::ref(opener), [](const std::int64_t, const std::span<const std::uint8_t> data, const bool) { return data.size(); },
                [](const std::int64_t, const std::size_t) {}, nullptr);

        bool   isHandlerEntered = false;
        Router router;
        router.postStreaming("/streaming-no-content",
                             [&isHandlerEntered](HttpRequest &, HttpResponse &response) -> Core::Task<>
                             {
                                 isHandlerEntered = true;
                                 // 204 是「有响应、没正文」：走的是只交头部就收尾的那条支路
                                 response.setStatus(204);
                                 co_return;
                             });
        session.attachRouter(router);

        Http3ClientPeer peer;

        const std::string body(2048, 'q');
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/streaming-no-content", "example.com", body, body.size()));
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        for (int pumpRound = 0; pumpRound < 6 && session.hasOutstandingWork(); ++pumpRound)
        {
            Core::Task<> pumpTask = session.pump();
            resumeUntilReady(pumpTask);
        }
        ASSERT_TRUE(isHandlerEntered) << "流式路由没把请求交给业务，这条用例就没走到收尾";
        EXPECT_FALSE(session.hasOutstandingWork()) << "无正文应答之后流式记录没被摘掉：收尾的字节没交给传输层，关闭通知就发不出来";
    }

    /**
     * @brief 排队中与服务中的正文都要占着全局额度，直到这一条服务完才归还
     * @details 额度若在「请求被排进待派发队列」时就归还，排队的正文与正在跑处理器的正文都不再被记账，
     *          多条流各自压一份正文就能把实际占用推过上限——这道限额要挡的正是这个。
     *          判据取两处：请求收齐、还没 pump 时的占用，以及处理器进门时看到的占用
     */
    TEST(Http3Session, KeepsInflightBudgetHeldWhileRequestIsQueuedAndServed)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        const auto                      budget = std::make_shared<HttpMemoryBudget>(16); // 够一条 10 字节正文，再只剩 6

        Http3Session session(
                std::ref(opener),
                [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                {
                    sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                    return data.size();
                },
                Http3Session::StreamCrediter{}, nullptr, budget);

        std::size_t reservedAtHandlerEntry = 0;
        Router      router;
        router.post("/upload",
                    [&reservedAtHandlerEntry, budget](HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        // 处理器跑起来时，它自己那份正文必须还记在账上
                        reservedAtHandlerEntry = budget->reservedByteCount();
                        response.setStatus(200);
                        response.setBody("uploaded");
                        co_return;
                    });
        session.attachRouter(router);

        Http3ClientPeer peer;

        const std::string body = "0123456789"; // 10 字节，在 16 的预算之内
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", body, body.size()));
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        // 请求已收齐、还排在待派发队列里：正文仍在内存里，额度不能先还
        EXPECT_EQ(budget->reservedByteCount(), body.size()) << "请求还在排队，额度就已经归还了";

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        EXPECT_EQ(peer.response().status, 200);
        EXPECT_EQ(reservedAtHandlerEntry, body.size()) << "处理器跑起来时没看到自己那份正文占着额度";
        EXPECT_EQ(budget->reservedByteCount(), 0U) << "这一条服务完之后额度应当整份归还";
    }

    /**
     * @brief h3 上跑 WebSocket：扩展 CONNECT（RFC 9220）建隧道，帧在流上原样收发
     * @details 隧道建立之后这条流上跑的就是 WebSocket 帧本身（不是 h3 正文），因此这里手工造一个带
     *          掩码的文本帧喂进去，断言业务把同样的负载回显回来——回显帧由服务端发出，不带掩码，
     *          负载逐字节可比
     */
    TEST(Http3Session, TunnelsWebSocketOverExtendedConnect)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;

        Http3Session session(std::ref(opener),
                             [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                             {
                                 sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                                 return data.size();
                             });

        Router router;
        router.get("/chat",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.upgradeToWebSocket(
                               [](WebSocketPeer &peer) -> Core::Task<>
                               {
                                   // 隧道不做任何 h3 编解码：收到一条就原样回一条
                                   while (const auto message = co_await peer.receive())
                                   {
                                       if (!co_await peer.sendText(message->payload))
                                       {
                                           co_return;
                                       }
                                   }
                                   co_return;
                               });
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        // 隧道建立后要发的帧：带掩码的文本帧（构造器见 makeMaskedTextFrame）
        const std::string                 payload = "hello";
        const std::array<std::uint8_t, 4> mask{0x11U, 0x22U, 0x33U, 0x44U};
        const std::vector<std::uint8_t>   firstFrameBytes = makeMaskedTextFrame(payload, mask);
        const std::string                 webSocketFrameText(firstFrameBytes.begin(), firstFrameBytes.end());

        const std::vector<CapturedStreamData> requestChunks = peer.submitWebSocketTunnel("/chat", "example.com", webSocketFrameText);
        ASSERT_FALSE(requestChunks.empty()) << "扩展 CONNECT 请求没编出来";
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        std::size_t fedServerChunkCount = 0;
        for (; fedServerChunkCount < sentStreamData.size(); ++fedServerChunkCount)
        {
            const CapturedStreamData &written = sentStreamData[fedServerChunkCount];
            if (written.streamId == kFirstRequestStreamId)
            {
                peer.receive(written.streamId, written.bytes, written.isEndStream);
            }
        }
        ASSERT_EQ(peer.response().status, 200) << "隧道没有以 2xx 应答（RFC 9220 里没有 101）";

        // 回显：服务端写出的字节喂给客户端，它解出的 DATA 负载就是回显的 WebSocket 帧
        for (; fedServerChunkCount < sentStreamData.size(); ++fedServerChunkCount)
        {
            const CapturedStreamData &written = sentStreamData[fedServerChunkCount];
            if (written.streamId == kFirstRequestStreamId)
            {
                peer.receive(written.streamId, written.bytes, written.isEndStream);
            }
        }

        const std::string_view echoedFrame = peer.response().body;
        ASSERT_GE(echoedFrame.size(), 2U + payload.size()) << "隧道里没有回显帧：业务没收到帧，或出向帧没发出去";
        EXPECT_EQ(static_cast<std::uint8_t>(echoedFrame[0]), 0x81U) << "回显的不是文本帧";
        EXPECT_EQ(static_cast<std::size_t>(static_cast<std::uint8_t>(echoedFrame[1])), payload.size()) << "回显帧的长度不对";
        EXPECT_EQ(echoedFrame.substr(2, payload.size()), payload) << "回显的负载与发出去的不一致";

        // 第二条帧：首条交付完时出向队列已空、库正等着新数据，这一条能不能出去取决于新数据到达时
        // 有没有把库叫回来（读回调报过「暂时没有」之后，只有 resume_stream 才会再叫它来取）
        const std::string               secondPayload         = "world!";
        const std::size_t               sentCountBeforeSecond = sentStreamData.size();
        const std::vector<std::uint8_t> secondFrameBytes      = makeMaskedTextFrame(secondPayload, mask);
        for (const CapturedStreamData &chunk: peer.sendWebSocketFrame(std::string(secondFrameBytes.begin(), secondFrameBytes.end())))
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        for (std::size_t writtenIndex = sentCountBeforeSecond; writtenIndex < sentStreamData.size(); ++writtenIndex)
        {
            const CapturedStreamData &written = sentStreamData[writtenIndex];
            if (written.streamId == kFirstRequestStreamId)
            {
                peer.receive(written.streamId, written.bytes, written.isEndStream);
            }
        }

        const std::string_view bothEchoes = peer.response().body;
        ASSERT_EQ(bothEchoes.size(), (2U + payload.size()) + (2U + secondPayload.size())) << "第二条帧没有回显：首条交付完之后的出向帧没发出去";
        EXPECT_EQ(bothEchoes.substr(2U + payload.size() + 2U, secondPayload.size()), secondPayload) << "第二帧的回显负载与发出去的不一致";
    }

    /**
     * @brief 隧道建立时把 permessage-deflate 的协商结论回给对端（README 声称三条通道共用这套协商）
     * @details 只看应答头里那一行：压缩本身由 WebSocketPeer 负责，它的压缩收发已有
     *          TestWebSocketSession 的用例钉住，这里重复一遍不会多出信息
     */
    TEST(Http3Session, EchoesNegotiatedPerMessageDeflateOnTheTunnel)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.get("/chat",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.upgradeToWebSocket([](WebSocketPeer &) -> Core::Task<> { co_return; });
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer                       peer;
        const std::vector<CapturedStreamData> requestChunks =
                peer.submitEndedWebSocketTunnel("/chat", "example.com", {{"sec-websocket-extensions", "permessage-deflate; client_max_window_bits"}});
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        ASSERT_EQ(peer.response().status, 200) << "扩展 CONNECT 应当以 200 应答";
        const auto extensionsHeader = peer.response().headers.find("sec-websocket-extensions");
        ASSERT_NE(extensionsHeader, peer.response().headers.end()) << "对端提了 permessage-deflate，应答里却没回协商结论";
        EXPECT_TRUE(extensionsHeader->second.starts_with("permessage-deflate")) << extensionsHeader->second;
    }

    /**
     * @brief 拒绝面：对端没提扩展时，应答头里不该凭空出现 sec-websocket-extensions
     */
    TEST(Http3Session, OmitsExtensionHeaderWhenPeerOffersNothing)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.get("/chat",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.upgradeToWebSocket([](WebSocketPeer &) -> Core::Task<> { co_return; });
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;
        for (const CapturedStreamData &chunk: peer.submitEndedWebSocketTunnel("/chat", "example.com"))
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        ASSERT_EQ(peer.response().status, 200) << "扩展 CONNECT 应当以 200 应答";
        EXPECT_EQ(peer.response().headers.count("sec-websocket-extensions"), 0U) << "没协商扩展却回一行，对端会以为要按压缩帧收";
    }

    /**
     * @brief 有生成器时业务与响应头读到的是同一个 request-id，且流式响应的头部也带得上
     * @details 流式响应的头部在处理器第一次写块时就上线了，等处理器返回再设已经来不及——
     *          因此回显必须发生在派发之前，这条用例钉的正是这个时机
     */
    TEST(Http3Session, EchoesRequestIdOnStreamingResponseHead)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData, std::make_shared<HttpRequestIdGenerator>());

        std::string observedRequestId;
        Router      router;
        router.get("/stream",
                   [&observedRequestId](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                   {
                       observedRequestId = std::string(request.requestId());
                       response.startChunkedResponse(200);
                       static_cast<void>(co_await response.writeChunk("data: one\n\n"));
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer                        peer;
        const Http3ClientPeer::DecodedResponse response = answerOneGet(session, peer, sentStreamData, "/stream");

        EXPECT_FALSE(observedRequestId.empty()) << "生成器在场时业务必须读到落定好的 request-id";
        const auto requestIdHeader = response.headers.find("x-request-id");
        ASSERT_NE(requestIdHeader, response.headers.end()) << "响应头里没有 x-request-id";
        EXPECT_EQ(requestIdHeader->second, observedRequestId) << "响应回显的必须正是业务读到的那一份，不能各生成一个";
    }

    /**
     * @brief 客户端带来的链路 id 原样沿用（与 h1/h2 同口径），换掉就断了关联
     */
    TEST(Http3Session, AdoptsClientSuppliedRequestId)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData, std::make_shared<HttpRequestIdGenerator>());

        Router router;
        router.get("/whoami",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setBody("ok");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer                       peer;
        const std::vector<CapturedStreamData> requestChunks = peer.submitRequest("GET", "/whoami", "example.com", kFirstRequestStreamId, {{"x-request-id", "trace-me"}});
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        const auto requestIdHeader = peer.response().headers.find("x-request-id");
        ASSERT_NE(requestIdHeader, peer.response().headers.end());
        EXPECT_EQ(requestIdHeader->second, "trace-me") << "客户端给的合法 id 被换掉了，上游的链路关联就此断掉";
    }

    /**
     * @brief 拒绝面：没接生成器时不该凭空造一个 x-request-id
     */
    TEST(Http3Session, OmitsRequestIdWhenNoGeneratorIsShared)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        Router router;
        router.get("/plain",
                   [](HttpRequest &request, HttpResponse &response) -> Core::Task<>
                   {
                       EXPECT_TRUE(request.requestId().empty()) << "用例前提：没生成器时 id 保持空";
                       response.setStatus(200);
                       response.setBody("ok");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;

        const Http3ClientPeer::DecodedResponse response = answerOneGet(session, peer, sentStreamData, "/plain");
        EXPECT_EQ(response.headers.count("x-request-id"), 0U) << "空 id 也要回显，等于给对端一个空头";
    }

    /**
     * @brief 单连接请求条数到量后 h3 会排空：答完在途的，就不再受理新流
     * @details h1/h2 早就有 maximumRequestsPerConnection（回完当前响应即收口），h3 此前无上限：
     *          一条长连接可以被无限期复用。这里把上限调到 2，避免用例真去刷满默认值
     */
    TEST(Http3Session, DrainsConnectionAfterThePerRequestLimit)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session = makeSession(opener, sentStreamData);

        const auto limits                    = std::make_shared<HttpServerLimits>();
        limits->maximumRequestsPerConnection = 2;
        session.setServerLimits(limits);

        Router router;
        router.get("/limit",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setBody("ok");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;

        // 一条请求一轮：提交、喂会话、派发、把服务端的字节交回客户端
        const auto serveOne = [&](const std::int64_t requestStreamId)
        {
            sentStreamData.clear();
            const std::vector<CapturedStreamData> requestChunks = peer.submitRequest("GET", "/limit", "example.com", requestStreamId);
            for (const CapturedStreamData &chunk: requestChunks)
            {
                session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
            }
            Core::Task<> pumpTask = session.pump();
            resumeUntilReady(pumpTask);
            for (const CapturedStreamData &chunk: sentStreamData)
            {
                peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
            }
        };

        serveOne(kFirstRequestStreamId);
        EXPECT_EQ(peer.response().status, 200);
        EXPECT_FALSE(session.isDrainedAndFinished()) << "只答了一条，远没到上限";

        serveOne(4);
        EXPECT_EQ(peer.response().status, 200) << "上限这条响应本身必须照常答完";
        EXPECT_TRUE(session.isDrainedAndFinished()) << "到量之后应当已通告排空且手上没活";

        // 第三条走新流号：排空之后不该再有它的任何字节
        serveOne(8);
        EXPECT_FALSE(std::ranges::any_of(sentStreamData, [](const CapturedStreamData &chunk) { return chunk.streamId == 8; })) << "GOAWAY 之后的新流不该被处理，更不该往回写东西";
    }

    /**
     * @brief 排空通告之后的新流要显式取消，错误码是 H3_REQUEST_REJECTED（RFC 9114 §5.2 的 SHOULD）
     * @details 只「不处理」的话对端看不出这条流被判死了，只能等连接收尾；显式取消让客户端立刻
     *          知道该换一条连接重试。上一条用例只管服务端不回字节，这里核对的是它有没有交代给传输层
     */
    TEST(Http3Session, CancelsStreamsArrivingAfterTheDrainAnnouncement)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        std::vector<AbortedStream>      abortedStreams;
        Http3Session session = makeSession(opener, sentStreamData, nullptr, [&abortedStreams](const std::int64_t streamId, const std::uint64_t applicationErrorCode)
                                           { abortedStreams.push_back(AbortedStream{streamId, applicationErrorCode}); });

        const auto limits                    = std::make_shared<HttpServerLimits>();
        limits->maximumRequestsPerConnection = 1;
        session.setServerLimits(limits);

        Router router;
        router.get("/hello",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setBody("hi");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer peer;

        // 第一条：答完之后连接就该通告排空
        for (const CapturedStreamData &chunk: peer.submitRequest("GET", "/hello", "example.com", kFirstRequestStreamId))
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> firstPumpTask = session.pump();
        resumeUntilReady(firstPumpTask);
        ASSERT_TRUE(session.isDrainedAndFinished()) << "用例前提：这一条答完就该通告排空";
        ASSERT_TRUE(abortedStreams.empty()) << "正常答完的流不该被收口";

        // 第二条：通告之后的新流，只喂进来、不该有响应，但要被显式取消
        sentStreamData.clear();
        for (const CapturedStreamData &chunk: peer.submitRequest("GET", "/late", "example.com", kFirstRequestStreamId + 4))
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> latePumpTask = session.pump();
        resumeUntilReady(latePumpTask);

        EXPECT_FALSE(std::ranges::any_of(sentStreamData, [](const CapturedStreamData &chunk) { return chunk.streamId == kFirstRequestStreamId + 4; }))
                << "通告之后的新流不该回任何字节";
        ASSERT_EQ(abortedStreams.size(), 1U) << "这条流没有被显式取消：对端只能等到连接收尾才知道结果";
        EXPECT_EQ(abortedStreams.front().streamId, kFirstRequestStreamId + 4);
        EXPECT_EQ(abortedStreams.front().applicationErrorCode, 0x010bU) << "拒绝一条排空后的请求该用 H3_REQUEST_REJECTED";
    }

    /**
     * @brief 正文越界时立刻回 413，不等对端收尾；回完还请对端别再发正文
     * @details 此前 h3 的 413 排在「请求收齐」之后：客户端一边分批 dribble 一边等，响应永远不来，
     *          这条流就这么挂着。h2 的判据是 isReadyToServe 里带上 isBodyTooLarge，这里对齐它，
     *          并在响应完整交给传输层之后请对端停发（h2 那边同样是发完才 abortStream）
     */
    TEST(Http3Session, AnswersPayloadTooLargeBeforeTheBodyEndsAndAsksPeerToStop)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        std::vector<AbortedStream>      abortedStreams;
        Http3Session session = makeSession(opener, sentStreamData, nullptr, [&abortedStreams](const std::int64_t streamId, const std::uint64_t applicationErrorCode)
                                           { abortedStreams.push_back(AbortedStream{streamId, applicationErrorCode}); });

        HttpParserLimits parserLimits;
        parserLimits.maximumBodySize = 8;
        session.setParserLimits(parserLimits);

        bool   isHandlerEntered = false;
        Router router;
        router.post("/upload",
                    [&isHandlerEntered](HttpRequest &, HttpResponse &response) -> Core::Task<>
                    {
                        isHandlerEntered = true;
                        response.setStatus(200);
                        response.setBody("uploaded");
                        co_return;
                    });
        session.attachRouter(router);

        Http3ClientPeer   peer;
        const std::string oversizeBody(32, 'x');
        ASSERT_TRUE(peer.submitRequestWithBody("POST", "/upload", "example.com", oversizeBody, 4));

        // 只喂到「还没收尾」为止：最后那片带着 END_STREAM，喂进去就测不出「不等收尾」这件事
        for (std::size_t stepIndex = 0; stepIndex < 32; ++stepIndex)
        {
            const CapturedStreamData step = peer.takeNextWriteStep();
            if (step.streamId == -1 || step.isEndStream)
            {
                break;
            }
            session.onStreamData(step.streamId, step.bytes, step.isEndStream);
        }

        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        ASSERT_FALSE(sentStreamData.empty()) << "服务端一个字节都没回：413 没发出去";
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        EXPECT_EQ(peer.response().status, 413) << "正文越界没能在请求收尾之前就回掉";
        EXPECT_TRUE(peer.response().isComplete) << "413 的响应要收尾";
        EXPECT_FALSE(isHandlerEntered) << "越界的请求不该交给业务";
        ASSERT_EQ(abortedStreams.size(), 1U) << "响应已发出，却没请对端停发剩余正文";
        EXPECT_EQ(abortedStreams.front().applicationErrorCode, 0x0100U) << "这是无错的收尾请求（H3_NO_ERROR），不是错误";
    }

    /**
     * @brief 请求一直收不齐：过 readTimeout 就收口这条流，不连坐同连接的其它请求
     * @details h1/h2 撞到这个时限是掐掉整条连接，h3 多路复用是常态，只处置那一条流。时限取 1 毫秒，
     *          再往里注入一个未来的「本拍时刻」，用例因此是确定的，不靠睡眠等超时
     */
    TEST(Http3Session, AbortsRequestThatStopsArrivingWithinTheReadDeadline)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        std::vector<AbortedStream>      abortedStreams;
        Http3Session session = makeSession(opener, sentStreamData, nullptr, [&abortedStreams](const std::int64_t streamId, const std::uint64_t applicationErrorCode)
                                           { abortedStreams.push_back(AbortedStream{streamId, applicationErrorCode}); });

        const auto limits   = std::make_shared<HttpServerLimits>();
        limits->readTimeout = std::chrono::milliseconds{1};
        session.setServerLimits(limits);

        bool   isHandlerEntered = false;
        Router router;
        router.get("/hello",
                   [&isHandlerEntered](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       isHandlerEntered = true;
                       response.setStatus(200);
                       response.setBody("hi");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer                       peer;
        const std::vector<CapturedStreamData> requestChunks = peer.submitRequest("GET", "/hello", "example.com");
        ASSERT_FALSE(requestChunks.empty());
        // 请求流号取常量：peer 交出的第一段往往是本端单向流（控制流 2、编码器流 6）的字节
        const std::int64_t requestStreamId = kFirstRequestStreamId;
        // 头交进去、但强行不收尾：这条请求永远差一个 END_STREAM
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, false);
        }

        session.expireStaleRequests(std::chrono::steady_clock::now() + std::chrono::seconds{5});

        ASSERT_EQ(abortedStreams.size(), 1U) << "过点的请求没有被收口：它会一直占着这条流";
        EXPECT_EQ(abortedStreams.front().streamId, requestStreamId);
        EXPECT_EQ(abortedStreams.front().applicationErrorCode, 0x010cU) << "本端放弃一条请求该用 H3_REQUEST_CANCELLED";
        EXPECT_TRUE(std::ranges::none_of(sentStreamData, [requestStreamId](const CapturedStreamData &chunk) { return chunk.streamId == requestStreamId; }))
                << "没收齐的请求不该回任何字节（没有 :method/:path 可派发的半成品响应）";
        EXPECT_FALSE(isHandlerEntered) << "过点的请求不该交给业务";
        EXPECT_FALSE(session.hasOutstandingWork()) << "过点的流要连同记账一起摘掉，否则排空永远等不完";
    }

    /**
     * @brief 还在 readTimeout 之内的请求照常能答：时限判定不能把活着的请求误杀
     */
    TEST(Http3Session, KeepsRequestThatIsStillWithinTheReadDeadline)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        std::vector<AbortedStream>      abortedStreams;
        Http3Session session = makeSession(opener, sentStreamData, nullptr, [&abortedStreams](const std::int64_t streamId, const std::uint64_t applicationErrorCode)
                                           { abortedStreams.push_back(AbortedStream{streamId, applicationErrorCode}); });

        const auto limits   = std::make_shared<HttpServerLimits>();
        limits->readTimeout = std::chrono::seconds{60};
        session.setServerLimits(limits);

        Router router;
        router.get("/hello",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setBody("hi");
                       co_return;
                   });
        session.attachRouter(router);

        Http3ClientPeer                       peer;
        const std::vector<CapturedStreamData> requestChunks = peer.submitRequest("GET", "/hello", "example.com");
        ASSERT_FALSE(requestChunks.empty());
        // 请求流号取常量：peer 交出的第一段往往是本端单向流（控制流 2、编码器流 6）的字节
        const std::int64_t requestStreamId = kFirstRequestStreamId;
        for (const CapturedStreamData &chunk: requestChunks)
        {
            session.onStreamData(chunk.streamId, chunk.bytes, false);
        }

        // 本拍时刻就取「现在」：一分钟的预算不该被判定过点
        session.expireStaleRequests(std::chrono::steady_clock::now());
        EXPECT_TRUE(abortedStreams.empty()) << "没到时限的请求被误杀了";

        // 再把这条流收尾：请求照常走完整轮，拿到 200
        const std::vector<std::uint8_t> noBytes;
        session.onStreamData(requestStreamId, std::span<const std::uint8_t>(noBytes), true);
        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);
        ASSERT_FALSE(sentStreamData.empty()) << "服务端一个字节都没回：响应没发出去";
        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        EXPECT_EQ(peer.response().status, 200) << "差一个收尾的请求，补上收尾之后就该正常应答";
    }

    /**
     * @brief 钉住：本端没能交出响应时只复位那一条流，会话与整条连接继续服务其它请求
     * @details 这里用「响应头段越过对端通告的 SETTINGS_MAX_FIELD_SECTION_SIZE」造出一次本端失误。
     *          原先这条出口是 markBroken，承载层随之 closeNow 整条 QUIC 连接——一处本端失误把同连接上
     *          别人在途的请求一起带走；h2 同一处已按流级收（RFC 9114 §4.2.2、§8.1）。
     */
    TEST(Http3Session, ResetsOnlyTheStreamWhoseResponseCannotBeSubmitted)
    {
        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        std::vector<AbortedStream>      abortedStreams;
        Http3Session session = makeSession(opener, sentStreamData, nullptr, [&abortedStreams](const std::int64_t streamId, const std::uint64_t applicationErrorCode)
                                           { abortedStreams.push_back(AbortedStream{streamId, applicationErrorCode}); });

        Router router;
        router.get("/wide",
                   [](HttpRequest &, HttpResponse &response) -> Core::Task<>
                   {
                       response.setStatus(200);
                       response.setHeader("x-wide", std::string(400U, 'v'));
                       response.setBody("ok");
                       co_return;
                   });
        session.attachRouter(router);

        // 对端通告只肯收 60 字节的头段：响应头段远超它，本端因此不作答这条流
        Http3ClientPeer peer{60U};
        static_cast<void>(answerOneGet(session, peer, sentStreamData, "/wide"));

        EXPECT_FALSE(session.isBroken()) << "错在本端也只该作废一条流，不该把整条会话判死";
        ASSERT_EQ(abortedStreams.size(), 1U) << "那条没能答出的流要被交代一次复位，对端才不必干等";
        EXPECT_EQ(abortedStreams.front().applicationErrorCode, static_cast<std::uint64_t>(Http3ErrorCode::InternalError));
    }

    /**
     * @brief 钉住：静态文件兜底路由在 h3 这条通道上生效（会话层，不经 UDP）
     * @details 静态服务原先只长在明文 HTTP 上，本轮把配置本体收进 StaticFileService 让三条通道共用
     *          同一份实现。这里钉 h3 这一端的落点：正文由映射交给连接层之后，客户端要能拿回那段字节，
     *          且 content-length 与 content-type 这类同源头部不缺——少了任何一步都说明「路由登记了但
     *          这条通道的正文路径接不上」。ETag 一并钉：它是条件请求的键，只在明文侧验过等于没验。
     */
    TEST(Http3Session, ServesStaticFileFromInstalledFallbackRoute)
    {
        AsynGyanis::TestSupport::TemporaryDirectory site("H3StaticSite");
        const std::string                           fileContent = "hello-h3-static-body";
        ASSERT_TRUE(site.writeBinaryFile("greeting.txt", fileContent));

        FakeStreamOpener                opener;
        std::vector<CapturedStreamData> sentStreamData;
        Http3Session                    session(std::ref(opener),
                                                [&sentStreamData](const std::int64_t streamId, const std::span<const std::uint8_t> data, const bool isEndStream)
                                                {
                                 sentStreamData.push_back(CapturedStreamData{streamId, std::vector<std::uint8_t>(data.begin(), data.end()), isEndStream});
                                 return data.size();
                                                });

        Router            router;
        StaticFileService staticFiles;
        // 限额给 0（关闭映射缓存）：本条要测的是路由与正文通路，不是缓存的条数账
        staticFiles.install(router, 0);
        staticFiles.setDirectory(Platform::FileSystem::utf8FromPath(site.path()));
        session.attachRouter(router);

        Http3ClientPeer peer;
        for (const CapturedStreamData &chunk: peer.submitRequest("GET", "/greeting.txt", "example.com"))
        {
            session.onStreamData(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }
        Core::Task<> pumpTask = session.pump();
        resumeUntilReady(pumpTask);

        for (const CapturedStreamData &chunk: sentStreamData)
        {
            peer.receive(chunk.streamId, chunk.bytes, chunk.isEndStream);
        }

        ASSERT_EQ(peer.response().status, 200) << "h3 上的静态文件没有回 200";
        EXPECT_EQ(peer.response().body, fileContent) << "h3 上取回的静态正文与文件内容不一致";
        const auto contentType = peer.response().headers.find("content-type");
        ASSERT_TRUE(contentType != peer.response().headers.end()) << "静态响应少了 content-type";
        EXPECT_EQ(contentType->second.find("text/plain"), 0U) << "content-type 不是按扩展名推出来的那一串：" << contentType->second;
        EXPECT_TRUE(peer.response().headers.contains("etag")) << "静态响应少了 ETag：条件请求在 h3 上没有键可用";
    }

} // namespace AsynGyanis::Net

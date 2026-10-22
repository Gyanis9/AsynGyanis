/**
 * @file Http3SessionResponses.cpp
 * @brief HTTP/3 会话的整段响应、流式响应与收尾
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 整段响应提交、流式响应头/正文/尾段、产出背压与收尾都在这里；请求解析、
 *          路由派发、隧道与连接驱动留在 Http3Session.cpp。
 */

#include "Net/Http3/Http3Session.h"
#include "Net/Http3/Http3SessionSupport.h"

#include "Base/Log/LogMacros.h"
#include "Net/Http/Message/HttpChunkFrame.h"
#include "Net/Http/Message/HttpDate.h"
#include "Net/Http/Message/HttpHeaderRules.h"
#include "Net/Http3/Qpack.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using Http3SessionDetail::kContentLengthHeaderName;
        using Http3SessionDetail::kContentTypeHeaderName;
        using Http3SessionDetail::kDateHeaderName;
        using Http3SessionDetail::kDefaultContentTypeValue;
        using Http3SessionDetail::kStatusHeaderName;
        using Http3SessionDetail::kTrailerHeaderName;

        /// 把字符串按字节交给只认「指针 + 长度」的接口，不留零终止的假设
        [[nodiscard]] std::span<const std::uint8_t> asBytes(const std::string_view text) noexcept
        {
            return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
        }

        /// 越界的状态码上线时会被连接层拒收，整条流就此发不出东西：改回 500 至少让对端拿到一份能读的
        /// 响应。判据与 h1 共用同一份（`normalizeWireStatusCode`），这里只负责本通道的那条日志
        [[nodiscard]] int normalizeWireStatusCode(const int responseStatus, const std::int64_t streamId)
        {
            const int wireStatus = AsynGyanis::Net::normalizeWireStatusCode(responseStatus);
            if (wireStatus != responseStatus)
            {
                LOG_ERROR_FMT("Http3Session: 响应状态码 {} 越界（应为 100..999），流 {} 已改回 500", responseStatus, streamId);
            }
            return wireStatus;
        }

        /**
         * @brief 把 HttpResponse 摊平成 h3 要交的字段行，口径与 Http2Session 的采集器一致
         * @details 三条默认补齐（类型/长度/日期）与「可重复头逐条展开」必须与 h1/h2 逐字相同，
         *          否则同一份业务代码换个协议就会少发 Set-Cookie 或少发 date。
         * @param streamId 只用于日志
         * @param response 业务写好的响应
         * @param isStreamingResponse true 表示正文由后续 DATA 逐段给出，此刻算不出长度
         * @return 以 :status 开头的字段行
         */
        [[nodiscard]] std::vector<QpackHeaderField> collectResponseFieldLines(const std::int64_t streamId, const HttpResponse &response, const bool isStreamingResponse)
        {
            std::vector<QpackHeaderField> fieldLines;
            // 常见情形是「响应自己的头 + 后面补齐的三条」；给一个够用的起点，宁可留一点余量，
            // 也不为了算准数量先去把单值视图建出来（那是每次查询都要重建的哈希表）
            fieldLines.reserve(8U);

            const int              wireStatusCode         = normalizeWireStatusCode(response.status(), streamId);
            const bool             isBodylessStatus       = HttpResponse::isBodylessStatusCode(wireStatusCode);
            const std::string_view responseBody           = response.body();
            bool                   hasContentTypeHeader   = false;
            bool                   hasContentLengthHeader = false;
            bool                   hasDateHeader          = false;

            fieldLines.push_back(QpackHeaderField{.name = std::string(kStatusHeaderName), .value = std::to_string(wireStatusCode)});

            // 按权威记录的设置顺序逐条取，与 h1 的 appendHead、h2 的采集器走同一条路径：
            // 先建单值视图再按名回查，既多付一张哈希表和每头一个临时 vector，
            // 也让同名多条被按名归组、次序取决于哈希表
            response.forEachHeaderField(
                    [&](const std::string_view headerName, const std::string_view headerValue)
                    {
                        if (isConnectionSpecificHeaderName(headerName))
                        {
                            // h3 禁止连接特定字段（RFC 9114 §4.2）：HttpResponse 按 h1 口径可能带上它们，
                            // 带上会被对端判成报文格式错误，整条响应作废
                            LOG_DEBUG_FMT("Http3Session: 流 {} 的响应已丢弃 HTTP/3 禁止的连接特定头「{}」", streamId, headerName);
                            return;
                        }
                        if (isStreamingResponse && headerName == kContentLengthHeaderName)
                        {
                            // 流式响应的长度由 DATA 帧的总长给出：这个数字与随后陆续发出的正文对不上，
                            // 留着反而让对端按它定界、把后面的段当多余字节
                            LOG_DEBUG_FMT("Http3Session: 流 {} 的流式响应正文长度由 DATA 给出，已丢弃 content-length 响应头", streamId);
                            return;
                        }
                        if (response.hasTrailerFields() && headerName == kTrailerHeaderName)
                        {
                            // 声明按已登记的尾部字段生成（见本函数末尾）：两条并存对端读到的就是两份可能
                            // 不一致的承诺——与 h1 的 appendHead、h2 的采集器同一条处置
                            return;
                        }
                        if (headerName == kContentTypeHeaderName)
                        {
                            hasContentTypeHeader = true;
                        } else if (headerName == kContentLengthHeaderName)
                        {
                            hasContentLengthHeader = true;
                        } else if (headerName == kDateHeaderName)
                        {
                            hasDateHeader = true;
                        }

                        // 自设的正文长度在这里定稿，判据与 h1 的 appendHead、h2 的采集器同一份：声明与
                        // 真正进 DATA 的字节不符就由声明让位（RFC 9114 §4.2 与 §4.1 沿用 h2 那条判据）
                        if (headerName == kContentLengthHeaderName)
                        {
                            switch (response.declaredContentLengthDisposition())
                            {
                                case ContentLengthSendDisposition::Omit:
                                    LOG_DEBUG_FMT("Http3Session: 流 {} 的响应按状态码不该声明正文长度，已丢弃自设的 content-length", streamId);
                                    return;
                                case ContentLengthSendDisposition::UseBodyLength:
                                    fieldLines.push_back(QpackHeaderField{.name = std::string(kContentLengthHeaderName), .value = std::to_string(response.body().size())});
                                    return;
                                case ContentLengthSendDisposition::KeepDeclared:
                                    break;
                            }
                        }

                        fieldLines.push_back(QpackHeaderField{std::string(headerName), std::string(headerValue)});
                    });

            if (!hasContentTypeHeader && !responseBody.empty())
            {
                fieldLines.push_back(QpackHeaderField{.name = std::string(kContentTypeHeaderName), .value = std::string(kDefaultContentTypeValue)});
            }
            // 没有长度对端就只能靠 END_STREAM 定界；1xx/204/304 补出去是让它白等一段正文
            if (!hasContentLengthHeader && !isStreamingResponse && !isBodylessStatus)
            {
                fieldLines.push_back(QpackHeaderField{.name = std::string(kContentLengthHeaderName), .value = std::to_string(responseBody.size())});
            }
            if (!hasDateHeader)
            {
                // 日期视图指向线程内的缓存，下一轮就可能被覆盖：这里必须落一份自己的拷贝
                fieldLines.push_back(QpackHeaderField{.name = std::string(kDateHeaderName), .value = std::string(currentHttpDateText())});
            }
            if (response.hasTrailerFields())
            {
                // 与 h1/h2 同一条承诺：正文之后还有这些字段（RFC 9110 §6.5.1）。三条通路的声明都取自
                // HttpResponse::trailerDeclarationValue()，拼法只有一处
                fieldLines.push_back(QpackHeaderField{.name = std::string(kTrailerHeaderName), .value = response.trailerDeclarationValue()});
            }
            return fieldLines;
        }

        /**
         * @brief 把响应登记的尾部字段摊平成 h3 的尾段字段行（不含 :status 与任何伪头）
         * @details 字段形状已在 HttpResponse::addTrailerField 一处把住，这里只做容器转换；
         *          尾段自己的合规判定由 Http3Connection 的校验器做（它才是能拒绝伪头的那一层）。
         * @param response 业务写好的响应
         * @return 按登记顺序排列的字段行；没登记时为空表，调用方据此根本不交尾段
         */
        [[nodiscard]] std::vector<QpackHeaderField> collectResponseTrailerFieldLines(const HttpResponse &response)
        {
            std::vector<QpackHeaderField> fieldLines;
            if (!response.hasTrailerFields())
            {
                return fieldLines;
            }
            fieldLines.reserve(4U);
            response.forEachTrailerField([&fieldLines](const std::string_view name, const std::string_view value)
                                         { fieldLines.push_back(QpackHeaderField{std::string(name), std::string(value)}); });
            return fieldLines;
        }

    } // namespace

    void Http3Session::finalizeResponseForHttp3(const std::int64_t streamId, HttpResponse &response)
    {
        // 升级只能走 RFC 9220 的扩展 CONNECT（:method=CONNECT + :protocol=websocket）：普通请求上
        // 登记升级会产出一条既不是 101 也不是隧道的响应，对端无从处理，因此明确回 500 并留下日志
        if (response.isWebSocketUpgradeRequested())
        {
            LOG_ERROR_FMT("Http3Session: 流 {} 上的处理器要求 WebSocket 升级，但这条流不是扩展 CONNECT（RFC 9220），已改回 500", streamId);
            response.reset();
            response.setStatus(500);
            response.setBody("HTTP/3 上的 WebSocket 升级需要扩展 CONNECT 请求（RFC 9220）");
        }
    }

    void Http3Session::attachChunkSender(const std::int64_t streamId, HttpResponse &response)
    {
        // 捕获 &response：发送口只在处理器运行期间被调用，而处理器就活在这次路由的栈帧里
        response.setChunkSender(
                [this, streamId, &response](const std::string_view chunk) -> Core::Task<bool>
                {
                    // HEAD：响应只有头部，正文段一字节都不发（头部由收尾路径与 END_STREAM
                    // 一起发出）。发出去会被对端当成下一条报文的开头
                    if (response.isStreamingBodySuppressed())
                    {
                        co_return true;
                    }
                    // 这块路径保持「按需建表」：首个写入块发生在状态建立之前，
                    // 只查不建会让第一块直接失败（另一条路径——隧道帧——才必须只查不建）
                    co_return co_await sendStreamingChunk(streamId, streamingResponseFor(streamId), response, chunk);
                });

        // 中间响应（1xx）的出口：本流上一个不收尾的头块（RFC 9114 §4.1）。
        // 装配时机与流式发送口同一处，false 含义也一致：没写成、这条流已不可用
        response.setInformationalWriter([this, streamId](const int statusCode, const std::vector<HttpResponse::InformationalHeaderField> &fields) -> Core::Task<bool>
                                        { co_return sendInformationalResponse(streamId, statusCode, fields); });
    }

    std::shared_ptr<Http3Session::StreamingResponse> Http3Session::findStreamingResponse(const std::int64_t streamId) const noexcept
    {
        const auto entry = m_streamingResponses.find(streamId);
        return entry == m_streamingResponses.end() ? nullptr : entry->second;
    }

    std::shared_ptr<Http3Session::StreamingResponse> Http3Session::streamingResponseFor(const std::int64_t streamId)
    {
        std::shared_ptr<StreamingResponse> &entry = m_streamingResponses[streamId];
        if (entry == nullptr)
        {
            entry = std::make_shared<StreamingResponse>();
        }
        return entry;
    }

    bool Http3Session::submitStreamingResponseHead(const std::int64_t streamId, StreamingResponse &state, HttpResponse &response)
    {
        if (m_connection == nullptr)
        {
            return false;
        }

        // 流式路径按「正文由 DATA 逐段给出」采集：content-length 一律不出，date 照补
        std::vector<QpackHeaderField> fieldLines = collectResponseFieldLines(streamId, response, true);

        // 只交响应头、不结束这条流：正文随后一段一段推过来
        if (const auto submitted = m_connection->submitResponseHead(streamId, fieldLines, false); !submitted)
        {
            handleResponseSubmissionFailure(streamId, "提交流式响应头", submitted.error().message, toHttp3ErrorCode(submitted.error().kind));
            return false;
        }

        state.isHeadSent = true;
        // 头部上线也是产出进展：一条只回头部就收尾的响应（HEAD、204）不会在正文那侧刷新时限
        refreshProduceDeadline(streamId);
        return true;
    }

    Core::Task<bool> Http3Session::sendStreamingChunk(const std::int64_t streamId, std::shared_ptr<StreamingResponse> state, HttpResponse &response, const std::string_view chunk)
    {
        // 承载侧的流已经关闭：这一段写不出去，按失败收手
        if (state->isStreamClosed)
        {
            co_return false;
        }
        if (!state->isHeadSent)
        {
            // 首个段落是 HttpResponse::writeChunk() 推上来的 HTTP/1.1 头部文本（状态行 + 头部块）：
            // 内容是 h1 线格式，不是 h3 要发的头块，这次调用只当「头部该上线了」的信号，
            // 真正发出的字段按响应对象现取；它自身那些字节不进正文（与 h2 侧同一口径）
            co_return submitStreamingResponseHead(streamId, *state, response);
        }

        // 其余段落是 h1 分块帧成帧的正文（RFC 9112 §7.1）：h3 里没有分块帧这一层，只把帧里的负载收下
        co_return co_await pushStreamingResponseBody(streamId, state, chunkFramePayload(chunk));
    }

    Core::Task<bool> Http3Session::pushStreamingResponseBody(const std::int64_t streamId, const std::shared_ptr<StreamingResponse> &state, const std::string_view bytes)
    {
        // 状态不在（流已被重置）或承载侧的流已经关闭：写多少都出不去，直接按失败收手
        if (state == nullptr || state->isStreamClosed || m_connection == nullptr || m_isBroken)
        {
            co_return false;
        }

        if (!bytes.empty())
        {
            if (const auto appended = m_connection->appendResponseBody(streamId, asBytes(bytes), false); !appended)
            {
                handleResponseSubmissionFailure(streamId, "追加流式响应正文", appended.error().message, toHttp3ErrorCode(appended.error().kind));
                co_return false;
            }
        }
        // 推完就往外送：连接层把这流的待发字节一次性交给传输层，本端不留副本也不等人来取
        flushPendingStreamData();
        // 字节真的被连接层接走了，这就是「产出有进展」：把时限往后推一格。慢消费者堵在发送口外
        // 时生产者可一直挂在这里，推不动的那一段才是产出预算要收口的对象
        refreshProduceDeadline(streamId);

        // 与流式响应同一道闸：生产者跑得比网络快就挂起等排空；流一关闭立即收手
        while (!state->isStreamClosed && streamingResponsePendingByteCount(streamId) > kStreamingResponseBufferByteCount)
        {
            co_await ResponseSpaceAwaiter(*this, streamId, state);
        }
        co_return !state->isStreamClosed;
    }

    void Http3Session::finishStreamingResponse(const std::int64_t streamId, HttpResponse &response, const bool isBodyComplete)
    {
        const std::shared_ptr<StreamingResponse> state = streamingResponseFor(streamId);
        if (!state->isHeadSent)
        {
            if (!submitStreamingResponseHead(streamId, *state, response))
            {
                return; // 提交失败时已经记过日志
            }
        }
        if (state->isFinished || m_connection == nullptr || m_isBroken)
        {
            return; // 已经收过口（或会话已作废）：再交一次 END_STREAM 只会报错
        }

        // 收尾：交出「正文到此为止」，连接层把最后一段与 END_STREAM 一起送出去。登记过尾部字段时
        // 这次收尾改由尾段那个帧承担（RFC 9114 §4.3：尾段之后什么都不剩），位置与 h1 的
        // 「终止块 + 字段段 + 空行」、h2 的尾部头块一一对应
        state->isFinished = true;
        const std::vector<QpackHeaderField> trailerFieldLines =
                isBodyComplete && !response.isStreamingBodySuppressed() ? collectResponseTrailerFieldLines(response) : std::vector<QpackHeaderField>{};
        if (!trailerFieldLines.empty())
        {
            if (const auto submitted = m_connection->submitResponseTrailers(streamId, trailerFieldLines); !submitted)
            {
                handleResponseSubmissionFailure(streamId, "提交响应尾段", submitted.error().message, toHttp3ErrorCode(submitted.error().kind));
                return;
            }
            flushPendingStreamData();
            return;
        }
        if (const auto appended = m_connection->appendResponseBody(streamId, std::span<const std::uint8_t>{}, true); !appended)
        {
            handleResponseSubmissionFailure(streamId, "收尾流式响应", appended.error().message, toHttp3ErrorCode(appended.error().kind));
            return;
        }
        flushPendingStreamData();
    }

    void Http3Session::submitResponse(const std::int64_t streamId, const HttpResponse &response, const bool isHeadRequest)
    {
        if (m_connection == nullptr)
        {
            return;
        }

        const std::string_view body = response.body();
        // 无正文的状态码（1xx/204/304）不给出 content-length；HEAD 与其余状态码都要给长度，只是正文不发
        const bool isBodylessStatus = HttpResponse::isBodylessStatusCode(response.status());
        // HEAD：正文一个字节都不发（与 h2 的 isHeadRequest ? {} : body 同一处置），content-length
        // 下面仍按完整正文长度给出——这正是 HEAD 的语义（RFC 9110 §9.3.2）
        const bool hasBody = !isBodylessStatus && !isHeadRequest && !body.empty();

        // 头部与 h1/h2 逐字同源：采集器负责丢连接特定字段、逐条展开可重复头并补齐类型/长度/日期
        const std::vector<QpackHeaderField> fieldLines = collectResponseFieldLines(streamId, response, false);

        // 没有正文时交完头就收尾；有正文则头先走（不结束流），紧接一次把整段正文推过去并收尾。
        // 带尾部字段时收尾交给尾段那个帧（RFC 9114 §4.3 里尾段之后什么都不剩），头与正文都不许带 FIN
        const std::vector<QpackHeaderField> trailerFieldLines = isHeadRequest ? std::vector<QpackHeaderField>{} : collectResponseTrailerFieldLines(response);
        const bool                          hasTrailers       = !trailerFieldLines.empty();

        // 没有正文时交完头就收尾；有正文则头先走（不结束流），紧接一次把整段正文推过去并收尾
        if (const auto submitted = m_connection->submitResponseHead(streamId, fieldLines, !hasBody && !hasTrailers); !submitted)
        {
            handleResponseSubmissionFailure(streamId, "提交响应头", submitted.error().message, toHttp3ErrorCode(submitted.error().kind));
            return;
        }
        if (hasBody)
        {
            if (const auto appended = m_connection->appendResponseBody(streamId, asBytes(body), !hasTrailers); !appended)
            {
                handleResponseSubmissionFailure(streamId, "提交响应正文", appended.error().message, toHttp3ErrorCode(appended.error().kind));
                return;
            }
        }
        if (hasTrailers)
        {
            if (const auto submitted = m_connection->submitResponseTrailers(streamId, trailerFieldLines); !submitted)
            {
                // 走到这里正文已经交出，这条响应因此是「发了一半就断」：原因记下来，按既有口径把
                // 该中止的流中止掉（handleResponseSubmissionFailure 里带着线上错误码）
                handleResponseSubmissionFailure(streamId, "提交响应尾段", submitted.error().message, toHttp3ErrorCode(submitted.error().kind));
            }
        }
    }
} // namespace AsynGyanis::Net

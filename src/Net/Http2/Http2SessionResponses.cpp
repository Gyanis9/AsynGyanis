/**
 * @file Http2SessionResponses.cpp
 * @brief HTTP/2 会话的响应头组装、响应写出与流式收尾
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details 把 HttpResponse 折成 HPACK 头块、按状态码补齐 content-length/date/content-type、
 *          发出头部/正文/尾部，以及流式响应逐段写出与收尾都在这里；请求派发、帧驱动与
 *          WebSocket 隧道留在 Http2Session.cpp。
 */

#include "Net/Http2/Http2Session.h"
#include "Net/Http2/Http2SessionSupport.h"

#include "Base/Log/LogMacros.h"
#include "Net/Http/Message/HttpChunkFrame.h"
#include "Net/Http/Message/HttpDate.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Net
{
    namespace
    {
        using Http2SessionDetail::kContentLengthHeaderName;
        using Http2SessionDetail::kContentTypeHeaderName;
        using Http2SessionDetail::kDateHeaderName;
        using Http2SessionDetail::kDefaultContentTypeValue;
        using Http2SessionDetail::kTrailerHeaderName;

        /**
         * @brief 判断状态码是否不该自动补 content-length
         * @details 与 HttpResponse::mustNotDeclareContentLength() 同口径：1xx / 204 / 304 都不补。
         *          304 可以携带 content-length，但那只是「取回同一资源时 200 会发出的长度」，
         *          而 RFC 9112 §6.2 规定自动补出的必须是 0，两者冲突时 MUST NOT 禁止取值
         * @param statusCode 响应状态码
         * @return true 表示不能补 content-length
         */
        bool mustNotDeclareContentLength(const int statusCode) noexcept
        {
            return statusCode / 100 == 1 || statusCode == 204 || statusCode == 304;
        }
    } // namespace

    std::vector<HpackHeaderField> Http2Session::collectResponseHeaderFields(const HttpResponse &response)
    {
        std::vector<HpackHeaderField> headerFields;
        // 常见情形是「响应自己的头 + 后面补齐的三条」；这里给一个够用的起点，宁可留一点余量，
        // 也不为了算准数量先去把单值视图建出来（那是每次查询都要重建的哈希表）
        headerFields.reserve(8U);
        bool hasContentTypeHeader   = false;
        bool hasContentLengthHeader = false;
        bool hasDateHeader          = false;

        // 流式响应的正文由各 DATA 帧给出，长度在收尾前未知：content-length 与 DATA 负载总长必须一致
        // （RFC 9113 §8.1.1），此刻算不出正确值，因此这条路径一律不写它（h1 的分块模式同样不写）
        const bool isStreamingResponse = response.isChunkedResponse();

        // 按权威记录的设置顺序逐条取，与 h1 的 appendHead 同一条路径：
        // 先建单值视图再按名回查，既多付一张哈希表，又会因遍历顺序不稳而让同一份响应两次编码给出不同次序
        response.forEachHeaderField(
                [&](const std::string_view headerName, const std::string_view headerValue)
                {
                    if (isConnectionSpecificHeaderName(headerName))
                    {
                        // 业务常按 HTTP/1.1 的习惯设 connection: close；HTTP/2 里它一律非法（§8.2.2），
                        // 丢掉它而不是让整条响应被连接层拒绝
                        LOG_DEBUG_FMT("Http2Session: 已丢弃 HTTP/2 禁止的连接特定响应头「{}」", headerName);
                        return;
                    }
                    if (isStreamingResponse && headerName == kContentLengthHeaderName)
                    {
                        // 流式模式按 h1 契约本就不该有这条头（startChunkedResponse 已删过一遍）：业务后补的
                        // 一样丢掉，否则一个恰好等于某段长度的数字会让对端按它定界、把后续 DATA 当多余字节
                        LOG_DEBUG_FMT("Http2Session: 流式响应的正文长度由 DATA 帧给出，已丢弃 content-length 响应头");
                        return;
                    }
                    if (response.hasTrailerFields() && headerName == kTrailerHeaderName)
                    {
                        // 声明按已登记的尾部字段生成（见本函数末尾）：业务自设的那一条若与它并存，对端读到的
                        // 就是两份可能不一致的承诺——与 h1 的 appendHead 同一条处置
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

                    // 自设的正文长度在这里定稿，判据与 h1 的 appendHead 同一份：声明与真正进 DATA 帧的
                    // 字节不符就由声明让位，留着对端只会把这条流判成畸形（RFC 9113 §8.1.1）
                    if (headerName == kContentLengthHeaderName)
                    {
                        switch (response.declaredContentLengthDisposition())
                        {
                            case ContentLengthSendDisposition::Omit:
                                LOG_DEBUG_FMT("Http2Session: 流响应按状态码不该声明正文长度，已丢弃自设的 content-length");
                                return;
                            case ContentLengthSendDisposition::UseBodyLength:
                                headerFields.push_back(HpackHeaderField{.name = std::string(kContentLengthHeaderName), .value = std::to_string(response.body().size())});
                                return;
                            case ContentLengthSendDisposition::KeepDeclared:
                                break;
                        }
                    }

                    headerFields.push_back(HpackHeaderField{std::string(headerName), std::string(headerValue)});
                });

        // 自动补齐与 HttpResponse::appendHead() 同口径：有正文却漏设媒体类型按纯文本下发，
        // 长度按正文实际字节数补，日期缺省补当前时刻
        const std::string_view responseBody = response.body();
        if (!hasContentTypeHeader && !responseBody.empty())
        {
            headerFields.push_back(HpackHeaderField{.name = std::string(kContentTypeHeaderName), .value = std::string(kDefaultContentTypeValue)});
        }
        if (!hasContentLengthHeader && !isStreamingResponse && !mustNotDeclareContentLength(response.status()))
        {
            headerFields.push_back(HpackHeaderField{.name = std::string(kContentLengthHeaderName), .value = std::to_string(responseBody.size())});
        }
        if (!hasDateHeader)
        {
            // 定长文本按秒缓存在线程局部，这里显式构造拥有者（头字段要持有值）
            headerFields.push_back(HpackHeaderField{.name = std::string(kDateHeaderName), .value = std::string(currentHttpDateText())});
        }
        if (response.hasTrailerFields())
        {
            // 与 h1 同一条承诺：正文之后还会有这些字段（RFC 9110 §6.5.1）。三条出站通路的声明都取自
            // HttpResponse::trailerDeclarationValue()，拼法只有一处，不会出现某一版漏了某个字段名
            headerFields.push_back(HpackHeaderField{.name = std::string(kTrailerHeaderName), .value = response.trailerDeclarationValue()});
        }
        return headerFields;
    }

    std::vector<HpackHeaderField> Http2Session::collectResponseTrailerFields(const HttpResponse &response)
    {
        std::vector<HpackHeaderField> trailerFields;
        if (!response.hasTrailerFields())
        {
            return trailerFields;
        }
        trailerFields.reserve(4U);
        response.forEachTrailerField([&trailerFields](const std::string_view name, const std::string_view value)
                                     { trailerFields.push_back(HpackHeaderField{std::string(name), std::string(value)}); });
        return trailerFields;
    }

    Core::Task<Http2ResponseSendStatus> Http2Session::sendResponse(const std::uint32_t streamId, const HttpResponse &response, const bool isHeadRequest)
    {
        // 状态码越界（HttpResponse::setStatus 不校验取值范围）时由助手改回 500：连接层只接受 100..999，
        // 把越界值原样交出去会让这条流一个字节都发不出去，对端只能等到超时
        const std::uint32_t wireStatusCode = normalizeWireStatusCode(response.status(), streamId);

        const std::vector<HpackHeaderField> headerFields = collectResponseHeaderFields(response);
        // HEAD 只发头：正文视图换成空，头部里的 content-length 仍按完整正文补齐。同一处置还要盖住
        // 「按定义没有正文」的状态码（1xx/204/304，RFC 9110 §6.3）：RFC 9113 §8.1.1 明说这类响应可以
        // 带着非零 content-length 而正文不进 DATA 帧。204/304 由 `Router::finalizeResponse()` 在上游
        // 就把正文清了，所以这一格真正兜住的是以 1xx 作最终状态那条——Router 不清它，而 h1 的
        // 序列化层与 h3 的 submitResponse 都按状态码挡着，h2 此前只挡 HEAD：一条流式契约之外的
        // 合法写法就会发出「1xx 还带正文」的报文，对端按 §8.1.1 判畸形是站得住的
        const bool             isBodylessResponse = HttpResponse::isBodylessStatusCode(static_cast<int>(wireStatusCode));
        const std::string_view responseBody       = (isHeadRequest || isBodylessResponse) ? std::string_view{} : response.body();
        const bool             isBodyEmpty        = responseBody.empty();
        // 尾部字段随 HEAD 一并省掉：HEAD 的响应按定义没有正文，也就没有「正文之后」（RFC 9110 §9.3.2）。
        // 头部的 trailer 声明仍保留——它描述的是同一条报文若以 GET 请求会带回什么
        const std::vector<HpackHeaderField> trailerFields = isHeadRequest ? std::vector<HpackHeaderField>{} : collectResponseTrailerFields(response);
        const bool                          hasTrailers   = !trailerFields.empty();

        std::string errorText;
        // 有尾部头块时收尾不归这里两处：RFC 9113 §8.1 规定尾部头块必须自带 END_STREAM，那么头部与
        // 最后一片 DATA 就都不许带它，否则这条流在尾部字段之前就已经结束了
        const Http2ResponseSendStatus headersStatus = m_connection.sendResponseHeaders(streamId, wireStatusCode, headerFields, isBodyEmpty && !hasTrailers, &errorText);
        if (headersStatus != Http2ResponseSendStatus::Sent)
        {
            // 对端取消这条流的日志由调用方按结论统一记，这里只管其余失败（连接不可用、用法错误）
            if (headersStatus != Http2ResponseSendStatus::StreamNotWritable)
            {
                LOG_ERROR_FMT("Http2Session: 流 {} 的响应头未能排入待发字节，该流不会再有响应。原因：{}", streamId, errorText);
            }
            co_return headersStatus;
        }
        if (!isBodyEmpty)
        {
            const Http2ResponseSendStatus bodyStatus = m_connection.sendResponseData(streamId, responseBody, !hasTrailers, &errorText);
            if (bodyStatus != Http2ResponseSendStatus::Sent)
            {
                if (bodyStatus != Http2ResponseSendStatus::StreamNotWritable)
                {
                    LOG_ERROR_FMT("Http2Session: 流 {} 的响应正文未能排入待发字节，该响应不完整。原因：{}", streamId, errorText);
                }
                co_return bodyStatus;
            }
        }
        if (hasTrailers)
        {
            const Http2ResponseSendStatus trailerStatus = m_connection.sendResponseTrailers(streamId, trailerFields, &errorText);
            if (trailerStatus != Http2ResponseSendStatus::Sent)
            {
                // 走到这里说明正文已经上线，这条响应因此是「发了一半就断」：连接层已经把该中止的流
                // 按 INTERNAL_ERROR 中止掉了，这里只把原因记下来并按同一结论交回调用方
                LOG_ERROR_FMT("Http2Session: 流 {} 的尾部头块未能排入待发字节，该响应不完整。原因：{}", streamId, errorText);
                co_return trailerStatus;
            }
        }
        co_return Http2ResponseSendStatus::Sent;
    }

    std::uint32_t Http2Session::normalizeWireStatusCode(const int responseStatus, const std::uint32_t streamId)
    {
        // 100..999 是 RFC 9110 §15 的状态码区间（也是连接层的接受范围）：越界值上线等于把这条流废掉
        if (responseStatus < 100 || responseStatus > 999)
        {
            LOG_ERROR_FMT("Http2Session: 响应状态码 {} 越界（应为 100..999），流 {} 已改回 500", responseStatus, streamId);
            return 500U;
        }
        return static_cast<std::uint32_t>(responseStatus);
    }

    std::string_view Http2Session::chunkFramePayload(const std::string_view chunkFrame)
    {
        // 剥离逻辑已上收到 Net/Http/HttpChunkFrame：h2 与 h3 面对的是同一份 h1 分块帧，
        // 各自实现一遍只会让两边的边界判定慢慢走偏。这里保留一个转发的成员函数，叫法不变
        return Net::chunkFramePayload(chunkFrame);
    }

    bool Http2Session::sendInformationalResponse(const std::uint32_t streamId, const int statusCode, const std::vector<HttpResponse::InformationalHeaderField> &fields)
    {
        std::vector<HpackHeaderField> headerFields;
        headerFields.reserve(fields.size());
        for (const HttpResponse::InformationalHeaderField &field: fields)
        {
            // h2 的头部名必须全小写（RFC 9113 §8.2）：大写名会被对端判成协议错误并 reset 这条流。
            // 调用方写的是 HTTP/1.1 那套大小写随意的习惯，归一落在这一层，而不是让每个处理器记住
            // 每种协议的规矩
            std::string lowerCaseName;
            lowerCaseName.reserve(field.first.size());
            for (const char character: field.first)
            {
                lowerCaseName.push_back(toLowerAscii(character));
            }
            headerFields.push_back(HpackHeaderField{std::move(lowerCaseName), field.second});
        }

        std::string errorText;
        // endStream 恒为 false：中间响应不收尾这条流，最终响应还在后面
        const Http2ResponseSendStatus sendStatus = m_connection.sendResponseHeaders(streamId, static_cast<std::uint32_t>(statusCode), headerFields, false, &errorText);
        if (sendStatus != Http2ResponseSendStatus::Sent)
        {
            LOG_ERROR_FMT("Http2Session: 流 {} 的 {} 中间响应未能排入待发字节。原因：{}", streamId, statusCode, errorText);
            return false;
        }
        // 只落状态码类，不落延迟样本：中间响应不是「这条请求的答复」。路由上下文拿不到，按哨兵记账
        if (m_metrics != nullptr)
        {
            m_metrics->countResponseStatus(statusCode, kHttpRouteUnmatchedLabel);
        }
        return true;
    }

    Core::Task<bool> Http2Session::sendStreamingSegment(const std::uint32_t streamId, HttpResponse &response, const std::string_view segment)
    {
        // HEAD：一段正文都不发（含头部）——头部与 END_STREAM 由 finishStreamingResponse 一起发出，
        // 只发头部不发正文正是 HEAD 的语义
        if (response.isStreamingBodySuppressed())
        {
            co_return true;
        }

        // 本侧已判定写不出去：与 h1 契约一致，短路返回 false 且不新增日志（首次失败已经交代过原因）
        if (m_isConnectionUnusable)
        {
            co_return false;
        }

        std::string errorText;
        if (!response.hasSentChunkedHead())
        {
            // 首个段落是 HttpResponse::writeChunk() 推上来的 HTTP/1.1 头部文本（状态行 + 头部块），
            // 内容是 h1 线格式而不是 h2 要发的头块：这次调用只当「头部该上线了」的信号，
            // 真正发出的字段按响应对象现取（HPACK 编码与连接特定头剥离都在下一跳完成）。
            // 不带 END_STREAM：正文段随后还要发，与 h1 侧「头部随首段正文上线」同一时机
            const std::vector<HpackHeaderField> headerFields = collectResponseHeaderFields(response);
            const Http2ResponseSendStatus       headersStatus =
                    m_connection.sendResponseHeaders(streamId, normalizeWireStatusCode(response.status(), streamId), headerFields, false, &errorText);
            if (headersStatus != Http2ResponseSendStatus::Sent)
            {
                // 对端取消这条流的日志由 serveOneRequest() 按服务结论统一记，这里只管其余失败
                if (headersStatus != Http2ResponseSendStatus::StreamNotWritable)
                {
                    LOG_ERROR_FMT("Http2Session: 流 {} 的流式响应头未能排入待发字节，该流不会再有响应。原因：{}", streamId, errorText);
                }
                co_return false;
            }
        } else
        {
            // 上界先行：窗口何时放开完全由对端决定，对端长期不发 WINDOW_UPDATE 时再入队就是无界内存。
            // 到顶即失败，业务据此停写（h1 侧不设这道闸是因为套接字写满会自然挂住生产者）
            if (!hasSendQueueRoom(streamId))
            {
                LOG_ERROR_FMT("Http2Session: 流 {} 的待发排队已触顶（单流上限 {} 字节、整条连接上限 {} 字节，"
                              "对端长期未发 WINDOW_UPDATE），本段不再入队，业务应停止写入",
                              streamId, kStreamingSendQueueLimitByteCount, kStreamingSendQueueConnectionLimitByteCount);
                co_return false;
            }

            // 其余段落是 HttpResponse::writeChunk() 按 h1 分块帧成帧的正文（RFC 9112 §7.1）：HTTP/2 里
            // 没有分块帧这一层（transfer-encoding 属禁止头，RFC 9113 §8.2.2），只把帧里的负载发成 DATA 帧
            const std::string_view        payload    = chunkFramePayload(segment);
            const Http2ResponseSendStatus bodyStatus = m_connection.sendResponseData(streamId, payload, false, &errorText);
            if (bodyStatus != Http2ResponseSendStatus::Sent)
            {
                if (bodyStatus != Http2ResponseSendStatus::StreamNotWritable)
                {
                    LOG_ERROR_FMT("Http2Session: 流 {} 的流式正文段未能排入待发字节，该响应不完整。原因：{}", streamId, errorText);
                }
                co_return false;
            }
        }

        // 当场写出：流式响应的要点是「边写边到」，把数据攒在待发缓冲里等主循环下一轮就等于推迟一段。
        // 窗口不足时连接层已把整段排进该流的发送队列（返回 true），这一次没有字节可写也不算失败：
        // 等对端 WINDOW_UPDATE 到达后由连接层在同一入口内续发，正是 writeChunk 文档里「入队而非失败」的含义
        co_return co_await flushOutgoingBytes();
    }

    bool Http2Session::hasSendQueueRoom(const std::uint32_t streamId) const noexcept
    {
        // 两道闸一起判：单流那道挡「一条流卡死」，连接那道挡「每条流各卡一点」——后者是并发流数的乘法
        return m_connection.pendingResponseByteCount(streamId) <= kStreamingSendQueueLimitByteCount &&
               m_connection.totalPendingResponseByteCount() <= kStreamingSendQueueConnectionLimitByteCount;
    }

    Core::Task<Http2ResponseSendStatus> Http2Session::finishStreamingResponse(const std::uint32_t streamId, HttpResponse &response, const bool isBodyComplete)
    {
        // 本侧已判定写不出去：不再重试，也不重复记日志（与 h1 契约一致）
        if (m_isConnectionUnusable)
        {
            co_return Http2ResponseSendStatus::ConnectionUnavailable;
        }

        std::string errorText;
        // 尾部字段随正文一起收尾：HEAD（流式正文被抑制）那一路没有正文，也就没有「正文之后」，补一个
        // 尾部头块反而会让对端多等一帧它没要的字段；业务中途抛异常时正文只发了一半，那些值本来就不
        // 对应发出去的那段正文，补上去等于给对端一个必然对不上的承诺（h1 同一条处置）
        const std::vector<HpackHeaderField> trailerFields =
                isBodyComplete && !response.isStreamingBodySuppressed() ? collectResponseTrailerFields(response) : std::vector<HpackHeaderField>{};
        const bool hasTrailers = !trailerFields.empty();
        if (!response.hasSentChunkedHead())
        {
            // 一段正文都没写出来（业务只调了 startChunkedResponse()）：头部与 END_STREAM 一起发出，
            // 对端因此拿到一条没有正文的完整响应，而不是挂在一条永远收不满的消息上；
            // 与 h1 侧「空流式响应补出头部与终止块」是同一个位置。有尾部字段时收尾交给那个尾部头块
            const std::vector<HpackHeaderField> headerFields = collectResponseHeaderFields(response);
            const Http2ResponseSendStatus       headersStatus =
                    m_connection.sendResponseHeaders(streamId, normalizeWireStatusCode(response.status(), streamId), headerFields, !hasTrailers, &errorText);
            if (headersStatus != Http2ResponseSendStatus::Sent)
            {
                // 对端取消这条流的日志由 serveOneRequest() 按服务结论统一记，这里只管其余失败
                if (headersStatus != Http2ResponseSendStatus::StreamNotWritable)
                {
                    LOG_ERROR_FMT("Http2Session: 流 {} 的空流式响应头未能排入待发字节，该流不会再有响应。原因：{}", streamId, errorText);
                }
                co_return headersStatus;
            }
            if (hasTrailers)
            {
                const Http2ResponseSendStatus trailerStatus = m_connection.sendResponseTrailers(streamId, trailerFields, &errorText);
                if (trailerStatus != Http2ResponseSendStatus::Sent)
                {
                    LOG_ERROR_FMT("Http2Session: 流 {} 的尾部头块未能排入待发字节，该响应不完整。原因：{}", streamId, errorText);
                    co_return trailerStatus;
                }
            }
            co_return co_await flushOutgoingBytes() ? Http2ResponseSendStatus::Sent : Http2ResponseSendStatus::ConnectionUnavailable;
        }

        if (hasTrailers)
        {
            // 尾部头块自带 END_STREAM，那条流的本端方向就由它收尾：不再补零长 DATA（那会先把流关掉，
            // 尾部字段就没有位置了）。位置与 h1 的「终止块 + 字段段 + 空行」对应
            const Http2ResponseSendStatus trailerStatus = m_connection.sendResponseTrailers(streamId, trailerFields, &errorText);
            if (trailerStatus != Http2ResponseSendStatus::Sent)
            {
                if (trailerStatus != Http2ResponseSendStatus::StreamNotWritable)
                {
                    LOG_ERROR_FMT("Http2Session: 流 {} 的尾部头块未能排入待发字节，对端收不到消息结尾。原因：{}", streamId, errorText);
                }
                co_return trailerStatus;
            }
            co_return co_await flushOutgoingBytes() ? Http2ResponseSendStatus::Sent : Http2ResponseSendStatus::ConnectionUnavailable;
        }

        // 头部已随首段上线：补一个零长 DATA 帧带 END_STREAM（RFC 9113 §6.1 允许零长 DATA，且不占流控窗口），
        // 消息边界由末片给出——h1 侧补的是 `0\r\n\r\n` 终止块，位置与语义都对应
        const Http2ResponseSendStatus endStreamStatus = m_connection.sendResponseData(streamId, std::string_view{}, true, &errorText);
        if (endStreamStatus != Http2ResponseSendStatus::Sent)
        {
            if (endStreamStatus != Http2ResponseSendStatus::StreamNotWritable)
            {
                LOG_ERROR_FMT("Http2Session: 流 {} 的流式响应末片未能排入待发字节，对端收不到消息结尾。原因：{}", streamId, errorText);
            }
            co_return endStreamStatus;
        }
        co_return co_await flushOutgoingBytes() ? Http2ResponseSendStatus::Sent : Http2ResponseSendStatus::ConnectionUnavailable;
    }
} // namespace AsynGyanis::Net

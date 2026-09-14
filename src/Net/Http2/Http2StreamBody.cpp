#include "Net/Http2/Http2StreamBody.h"

#include <utility>

namespace AsynGyanis::Net
{
    void Http2StreamBody::reset(ConsumeHandler consumeHandler)
    {
        // 内容只清不缩：这条连接上后续的流接着复用这块缓冲
        m_bytes.clear();
        m_pendingFlowControlByteCount = 0;
        m_totalReceivedByteCount      = 0;
        m_consumeHandler              = std::move(consumeHandler);
        m_isPeerFinished              = false;
        m_isBroken                    = false;
        m_isBodyTooLarge              = false;
    }

    void Http2StreamBody::setConsumeHandler(ConsumeHandler consumeHandler)
    {
        m_consumeHandler = std::move(consumeHandler);
    }

    void Http2StreamBody::append(const std::string_view data, const std::size_t flowControlByteCount, const bool endStream)
    {
        m_bytes.append(data);
        m_pendingFlowControlByteCount += flowControlByteCount;
        m_totalReceivedByteCount += data.size();
        if (endStream)
        {
            m_isPeerFinished = true;
        }
    }

    void Http2StreamBody::markBroken() noexcept
    {
        m_isBroken = true;
    }

    void Http2StreamBody::markBodyTooLarge() noexcept
    {
        m_isBroken       = true;
        m_isBodyTooLarge = true;
    }

    void Http2StreamBody::consumePending() noexcept
    {
        discardBufferedBody();
    }

    std::size_t Http2StreamBody::pendingByteCount() const noexcept
    {
        return m_bytes.size();
    }

    std::size_t Http2StreamBody::totalReceivedByteCount() const noexcept
    {
        return m_totalReceivedByteCount;
    }

    bool Http2StreamBody::isBodyTooLarge() const noexcept
    {
        return m_isBodyTooLarge;
    }

    void Http2StreamBody::discardBufferedBody() noexcept
    {
        // 消费即还窗口：按帧负载原长（含 padding）报量——只按应用数据报量会让对端窗口
        // 少还 padding 那部分，长期下来窗口被 padding 一点点吃掉
        if (m_pendingFlowControlByteCount != 0 && m_consumeHandler)
        {
            m_consumeHandler(m_pendingFlowControlByteCount);
            m_pendingFlowControlByteCount = 0;
        }
        m_bytes.clear();
    }

    std::string_view Http2StreamBody::bufferedBodyView() const noexcept
    {
        return m_bytes;
    }

    bool Http2StreamBody::isComplete() const noexcept
    {
        return m_isPeerFinished;
    }

    bool Http2StreamBody::isBroken()
    {
        return m_isBroken;
    }

    std::string_view Http2StreamBody::completedBody()
    {
        // h2 的正文不经请求对象中转（request.body() 在流式路径上始终为空），
        // 收齐那一刻到达的最后一批字节仍在缓冲里、由 bufferedBodyView() 正常交付
        return {};
    }
} // namespace AsynGyanis::Net

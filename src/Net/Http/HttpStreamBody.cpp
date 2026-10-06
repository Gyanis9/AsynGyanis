#include "Net/Http/HttpStreamBody.h"

#include <utility>

namespace AsynGyanis::Net
{
    void HttpStreamBody::reset(ConsumeHandler consumeHandler)
    {
        // 内容只清不缩：这条连接上后续的流接着复用这块缓冲
        m_bytes.clear();
        m_pendingFlowControlByteCount = 0;
        m_totalReceivedByteCount      = 0;
        m_consumeHandler              = std::move(consumeHandler);
        // 声明值随会话的交接口进来，也必须随复位出去：漏清的话下一条流会拿上一条的
        // content-length 判自己的体量，短了的那条被说成截断、满了的那条反过来
        m_declaredBodyByteCount = std::nullopt;
        m_isPeerFinished        = false;
        m_isBroken              = false;
        m_isBodyTooLarge        = false;
    }

    void HttpStreamBody::setConsumeHandler(ConsumeHandler consumeHandler)
    {
        m_consumeHandler = std::move(consumeHandler);
    }

    void HttpStreamBody::setBodyArrivedHandler(BodyArrivedHandler bodyArrivedHandler)
    {
        m_bodyArrivedHandler = std::move(bodyArrivedHandler);
    }

    void HttpStreamBody::setDeclaredBodyByteCount(const std::optional<std::size_t> declaredByteCount) noexcept
    {
        m_declaredBodyByteCount = declaredByteCount;
    }

    void HttpStreamBody::append(const std::string_view data, const std::size_t flowControlByteCount, const bool endStream)
    {
        m_bytes.append(data);
        m_pendingFlowControlByteCount += flowControlByteCount;
        m_totalReceivedByteCount += data.size();
        if (endStream)
        {
            m_isPeerFinished = true;
        }

        // 先改状态再通知：等待者被唤醒后会立刻回头看状态，通知里带着「到了什么」反而容易写错
        if (m_bodyArrivedHandler)
        {
            m_bodyArrivedHandler();
        }
    }

    void HttpStreamBody::markBroken() noexcept
    {
        m_isBroken = true;
        if (m_bodyArrivedHandler)
        {
            m_bodyArrivedHandler();
        }
    }

    void HttpStreamBody::markBodyTooLarge() noexcept
    {
        m_isBroken       = true;
        m_isBodyTooLarge = true;
        if (m_bodyArrivedHandler)
        {
            m_bodyArrivedHandler();
        }
    }

    void HttpStreamBody::consumePending() noexcept
    {
        discardBufferedBody();
    }

    std::size_t HttpStreamBody::pendingByteCount() const noexcept
    {
        return m_bytes.size();
    }

    std::size_t HttpStreamBody::totalReceivedByteCount() const noexcept
    {
        return m_totalReceivedByteCount;
    }

    bool HttpStreamBody::isBodyTooLarge() const noexcept
    {
        return m_isBodyTooLarge;
    }

    bool HttpStreamBody::hasBodyLengthMismatch() const noexcept
    {
        // 收尾了才对得上账：还没收尾时「实收少于声明」是正常的进行中状态，不是不符
        return m_isPeerFinished && m_declaredBodyByteCount.has_value() && m_totalReceivedByteCount != *m_declaredBodyByteCount;
    }

    void HttpStreamBody::discardBufferedBody() noexcept
    {
        // 消费即还窗口：按承载给出的流控报量归还——只按应用数据报量会让对端窗口一点点被吃掉
        // （h2 的帧头与 padding 就是被吃掉的那部分）
        if (m_pendingFlowControlByteCount != 0 && m_consumeHandler)
        {
            m_consumeHandler(m_pendingFlowControlByteCount);
            m_pendingFlowControlByteCount = 0;
        }
        m_bytes.clear();
    }

    std::string_view HttpStreamBody::bufferedBodyView() const noexcept
    {
        return m_bytes;
    }

    bool HttpStreamBody::isComplete() const noexcept
    {
        return m_isPeerFinished;
    }

    bool HttpStreamBody::isBroken()
    {
        return m_isBroken;
    }

    bool HttpStreamBody::isTruncated() const noexcept
    {
        // 「收尾了而字节没到位」才是截断：多发的那一向正文是全的，交给业务的只是比声明多出来的零头
        return m_isPeerFinished && m_declaredBodyByteCount.has_value() && m_totalReceivedByteCount < *m_declaredBodyByteCount;
    }

    std::string_view HttpStreamBody::completedBody()
    {
        // 正文不经请求对象中转（流式路径上 request.body() 始终为空），收尾那一刻到达的最后一批
        // 字节仍在缓冲里、由 bufferedBodyView() 正常交付
        return {};
    }
} // namespace AsynGyanis::Net

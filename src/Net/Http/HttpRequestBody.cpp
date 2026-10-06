#include "Net/Http/HttpRequestBody.h"

#include "Net/Http/HttpBodySource.h"

#include <string>
#include <utility>

namespace AsynGyanis::Net
{
    void HttpRequestBody::attach(HttpBodySource &source, Pump pump)
    {
        m_source                 = &source;
        m_pump                   = std::move(pump);
        m_chunk                  = {};
        m_isChunkOutstanding     = false;
        m_isCompleteBodyConsumed = false;
        m_isFinished             = false;
    }

    Core::Task<bool> HttpRequestBody::readNext()
    {
        if (m_isFinished || m_source == nullptr || !m_pump)
        {
            co_return false;
        }

        // 上一段已交付完毕：把它从来源的缓冲里丢掉，腾出的容量给下一批数据复用；
        // 首次调用没有「上一段」，因此不做丢弃（此刻缓冲里可能已有随头部一起到达的正文）
        if (m_isChunkOutstanding)
        {
            m_source->discardBufferedBody();
            m_isChunkOutstanding = false;
        }
        m_chunk = {};

        while (true)
        {
            // 正文中途的失败（解析错误、承载被取消）按流终止处理：原因已粘滞在来源上，
            // 会话在收尾时收口连接
            if (m_source->isBroken())
            {
                m_isFinished = true;
                co_return false;
            }

            if (const std::string_view buffered = m_source->bufferedBodyView(); !buffered.empty())
            {
                m_chunk              = buffered;
                m_isChunkOutstanding = true;
                co_return true;
            }

            if (m_source->isComplete())
            {
                // 收齐那一刻正文已整体搬进请求对象：普通路由一次性在这里交付全量；
                // 流式路由则补交最后一批尚未经流交付的残余（此前各批已被丢弃/取走）。
                // 空正文或已交付过即到流终点
                if (!m_isCompleteBodyConsumed)
                {
                    m_isCompleteBodyConsumed = true;
                    if (const std::string_view completeBody = m_source->completedBody(); !completeBody.empty())
                    {
                        m_chunk = completeBody;
                        co_return true;
                    }
                }
                m_isFinished = true;
                co_return false;
            }

            if (!co_await m_pump())
            {
                m_isFinished = true;
                co_return false;
            }
        }
    }

    std::string_view HttpRequestBody::chunk() const noexcept
    {
        return m_chunk;
    }

    bool HttpRequestBody::isTruncated() const noexcept
    {
        // 只有流已经终止才谈得上「没拿齐」：还在逐段交付时差着字节是正常状态，不是截断
        if (!m_isFinished || m_source == nullptr)
        {
            return false;
        }

        // 两条判据取或，是因为「不完整」在线上就有两种形状：对端按规矩收尾了却少发字节（只有
        // 来源自己判得出，它同时是「已收尾」——所以 !isComplete() 在这一格看不出来），以及
        // 根本没收尾就断了（承载断开、流被重置、正文解析失败）。三条通道因此对同一个形状给同一个答案
        return m_source->isTruncated() || !m_source->isComplete();
    }
} // namespace AsynGyanis::Net

#include "Net/Http/HttpRequestBody.h"

#include "Net/Http/HttpParser.h"

#include <string>
#include <utility>

namespace AsynGyanis::Net
{
    void HttpRequestBody::attach(HttpParser &parser, Pump pump)
    {
        m_parser                 = &parser;
        m_pump                   = std::move(pump);
        m_chunk                  = {};
        m_isChunkOutstanding     = false;
        m_isCompleteBodyConsumed = false;
        m_isFinished             = false;
    }

    Core::Task<bool> HttpRequestBody::readNext()
    {
        if (m_isFinished || m_parser == nullptr || !m_pump)
        {
            co_return false;
        }

        // 上一段已交付完毕：把它从解析器缓冲里丢掉，腾出的容量给下一批数据复用；
        // 首次调用没有「上一段」，因此不做丢弃（此刻缓冲里可能已有随头部一起到达的正文）
        if (m_isChunkOutstanding)
        {
            m_parser->discardBufferedBody();
            m_isChunkOutstanding = false;
        }
        m_chunk = {};

        while (true)
        {
            // 正文中途的解析失败按流终止处理：错误已粘滞在解析器上，会话在收尾时收口连接
            if (m_parser->hasError())
            {
                m_isFinished = true;
                co_return false;
            }

            if (const std::string_view buffered = m_parser->bufferedBodyView(); !buffered.empty())
            {
                m_chunk              = buffered;
                m_isChunkOutstanding = true;
                co_return true;
            }

            if (m_parser->isComplete())
            {
                // 收齐那一刻正文已随 commitMessage 移交请求对象：普通路由一次性在这里交付全量；
                // 流式路由则补交最后一批尚未经流交付的残余（此前各批已被丢弃/取走）。
                // 空正文或已交付过即到流终点
                if (!m_isCompleteBodyConsumed)
                {
                    m_isCompleteBodyConsumed = true;
                    if (const std::string_view completeBody = m_parser->request().body(); !completeBody.empty())
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
} // namespace AsynGyanis::Net

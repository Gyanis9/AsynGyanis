#include "Net/Http/HttpSession.h"

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /**
         * @brief 按 ASCII 表把字符转小写，非字母原样返回
         * @param character 待转换字符
         * @return char 转换结果
         * @note 不用 std::tolower：那个受 locale 影响（土耳其语环境下 'I' 会变成 0x69 之外的东西），
         *       而 HTTP 头部名按 ASCII 定义，必须与区域设置无关
         */
        constexpr char asciiToLower(const char character)
        {
            return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
        }

        /**
         * @brief 判断字符是否为头部值的空白（SP/HT）
         * @param character 待判定字符
         * @return true 是可在两端裁剪的空白
         */
        constexpr bool isHeaderWhitespace(const char character)
        {
            return character == ' ' || character == '\t';
        }

        /**
         * @brief 裁掉字符串两端的空白
         * @param text 原始视图
         * @return std::string_view 裁剪后的视图，不复制字节
         */
        std::string_view trimHeaderWhitespace(std::string_view text)
        {
            std::size_t beginPosition = 0;
            while (beginPosition < text.size() && isHeaderWhitespace(text[beginPosition]))
            {
                ++beginPosition;
            }
            std::size_t endPosition = text.size();
            while (endPosition > beginPosition && isHeaderWhitespace(text[endPosition - 1]))
            {
                --endPosition;
            }
            return text.substr(beginPosition, endPosition - beginPosition);
        }

        /**
         * @brief 把视图按 ASCII 小写复制到 std::string（用于比较头部名与值）
         * @param text 原始视图
         * @return std::string 小写化的副本
         */
        std::string asciiToLowerCopy(std::string_view text)
        {
            std::string result;
            result.reserve(text.size());
            for (const char character : text)
            {
                result.push_back(asciiToLower(character));
            }
            return result;
        }
    } // namespace

    HttpSession::HttpSession(Core::AsyncSocket socket, Router &router, std::shared_ptr<const HttpServerLimits> limits,
                             std::shared_ptr<HttpMetricsCollector> metrics,
                             std::shared_ptr<HttpRequestIdGenerator> requestIdGenerator) :
        Core::Connection(std::move(socket)),
        m_router(router),
        // 空配置按默认限额执行：让只关心协议的调用方不必显式传一份配置，会话内也不必到处判空
        m_limits(limits != nullptr ? std::move(limits) : std::make_shared<const HttpServerLimits>()),
        // 统计对象与生成器允许为空：这两种空值都表示「本会话不采集」，是明确的关闭语义，
        // 而不是待填补的缺省——因此不在构造里补一份新的，否则统计会散进没人读的对象里
        m_metrics(std::move(metrics)),
        m_requestIdGenerator(std::move(requestIdGenerator))
    {
        // 接收窗口不在这里分配：真正开始读之前它一直是空的，第一次读时按固定大小一次性分配
    }

    Core::Task<> HttpSession::start()
    {
        /**
         * @brief 退出时无条件收口的守卫
         *
         * @details 事务循环内部把读侧的传输失败折成 -1、把写侧的传输失败折成 false，但协议与实现类
         *          异常（例如解析器未消费字节却要求更多输入）仍会穿过本函数。若只在正常返回路径调用
         *          close()，这条路径就会漏掉：连接仍被标记为存活、取消源收不到停止请求、描述符要等
         *          对象析构才归还。协程帧销毁时会析构局部对象，因此用 RAII 守卫覆盖全部退出路径；
         *          基类 close() 幂等，重复调用是空操作。
         */
        struct ConnectionCloser
        {
            HttpSession *session = nullptr; ///< 需要在退出时收口的会话

            ~ConnectionCloser()
            {
                if (session != nullptr)
                {
                    session->close();
                }
            }
        } closer{this};

        // 谓词提成命名局部：它要以 const std::function 引用的形式活过整个 co_await，
        // 直接传临时量就把正确性押在「挂起中的全表达式结束时才析构临时量」这条规则上，
        // 读代码的人不易一眼确认；放在本协程帧里则一目了然
        const std::function<bool()> alivePredicate = [this]()
        {
            return isAlive();
        };

        // 事务循环与 HTTPS 共用同一份模板实现，差别只在传输层对象、「连接是否存活」的谓词、
        // 限额配置与可选的采集端；把 *this 传进去是为了让循环按相位刷新本连接的空闲截止时间
        co_await detail::httpKeepAliveLoop(socket(), cancelable(), m_router, m_parser, m_receiveBuffer, alivePredicate,
                                          *this, *m_limits, m_metrics.get(), m_requestIdGenerator.get());

        // 不再在此处 close()：统一交给上面的守卫，正常路径与异常路径只有一处收口
        co_return;
    }

    void HttpSession::onIdleTimeoutClosed() noexcept
    {
        // 只累加计数：清扫协程已经把「哪条连接、误差多大」写进了日志，这里再写一条只会让它翻倍。
        // 未持有统计对象时什么都不做——那表示调用方只关心协议本身
        if (m_metrics != nullptr)
        {
            m_metrics->countTimeoutClosedConnection();
        }
    }

    bool HttpSession::shouldKeepAlive(const HttpRequest &request, const HttpResponse &response)
    {
        // ---- 第 1 优先级：请求显式 close。客户端的明确指令不可被任何一侧的响应头反转 ----
        if (detail::headerValueListContainsToken(request.headerValues("connection"), "close"))
        {
            return false;
        }

        // ---- 第 2 优先级：响应显式 close。中间件或 handler 主动收口时同样不可被保活 ----
        if (detail::headerValueListContainsToken(response.headerValues("connection"), "close"))
        {
            return false;
        }

        // ---- 第 3 优先级：请求显式 keep-alive。对 HTTP/1.0 是「要求保活」，对 1.1 只是重申默认 ----
        if (detail::headerValueListContainsToken(request.headerValues("connection"), "keep-alive"))
        {
            return true;
        }

        // ---- 第 4 优先级：按协议版本的默认值。1.0/0.9 逐请求断连，1.1 起默认持久连接 ----
        const std::string_view version = request.httpVersion();
        const bool isHttp10OrOlder = version.starts_with("HTTP/1.0") || version.starts_with("HTTP/0.9") || version.empty();
        return !isHttp10OrOlder;
    }

    namespace detail
    {
        ConnectionCancelForwarder::ConnectionCancelForwarder(Core::Cancelable &cancelable, HttpRequest &request) :
            m_stopCallback(cancelable.stopToken(), RequestCancelForwarder{&request})
        {
            // 构造即注册：路由器与中间件跑完之后本对象析构注销，回调的作用域恰好等于「本次请求」
        }

        bool headerValueListContainsToken(const std::vector<std::string> &headerValueList, const std::string_view expectedToken)
        {
            for (const std::string &headerValue : headerValueList)
            {
                std::string_view remainder(headerValue);

                // 同一个头名里可以用逗号列多个值（"Connection: keep-alive, X"），逐个比对
                while (!remainder.empty())
                {
                    const std::size_t commaPosition = remainder.find(',');
                    const std::string_view currentToken = trimHeaderWhitespace(remainder.substr(0, commaPosition));

                    if (currentToken.size() == expectedToken.size() && asciiToLowerCopy(currentToken) == expectedToken)
                    {
                        return true;
                    }

                    if (commaPosition == std::string_view::npos)
                    {
                        break;
                    }
                    remainder = remainder.substr(commaPosition + 1);
                }
            }
            return false;
        }

        void writeParseErrorResponse(HttpResponse &response, const HttpParseErrorKind errorKind)
        {
            // 状态码与英文正文由解析器给出的类别决定；中文说明留在注释与日志里，
            // 协议字段里塞非 ASCII 字节会让对端按自己的编码猜
            switch (errorKind)
            {
                case HttpParseErrorKind::HeaderTooLarge:
                    // 431：头部过大，与 nginx 的 large_client_header_buffers 行为一致
                    response.setStatus(431);
                    response.setBody("Request Header Fields Too Large");
                    break;
                case HttpParseErrorKind::BodyTooLarge:
                    // 413：形态合法但体量越界（声明的长度、分块块大小与解码后的实收字节数都在此列）
                    response.setStatus(413);
                    response.setBody("Payload Too Large");
                    break;
                case HttpParseErrorKind::Malformed:
                case HttpParseErrorKind::None:
                default:
                    // 报文读不懂：给 400 兜底而不是让响应停在默认 200。
                    // None 不该走到这里（只有 Error 才调用本函数），同样按 400 收口
                    response.setStatus(400);
                    response.setBody("Bad Request");
                    break;
            }

            response.setHeader("content-type", "text/plain");
            response.setHeader("connection", "close");
        }

    } // namespace detail
} // namespace AsynGyanis::Net

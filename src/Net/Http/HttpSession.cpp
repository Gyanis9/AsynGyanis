/**
 * @file HttpSession.cpp
 * @brief HTTP 会话实现：报文定界、保持活跃判定与事务循环的支撑逻辑
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

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
         * @brief 头部块未收齐时的哨兵值
         */
        constexpr std::size_t kNotFoundPosition = std::string_view::npos;

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

        /**
         * @brief 严格十进制解析：只接受纯数字，溢出即失败
         * @details 不接受 '+'、前导空白、十六进制与任何分隔符：Content-Length 的语法就是 1*DIGIT，
         *          放宽等于给「同一含义多种写法」的走私留门。
         * @param text 待解析片段
         * @param resultValue 输出解析结果
         * @return true 解析成功
         * @return false 空串、含非数字字符或数值溢出
         */
        bool parseDecimalLength(std::string_view text, std::size_t &resultValue)
        {
            if (text.empty() || text.size() > 20)
            {
                // 超过 20 位必然溢出 64 位无符号数，先按长度粗筛掉，省掉逐位判溢出
                return false;
            }

            std::size_t accumulator = 0;
            for (const char digitCharacter : text)
            {
                if (digitCharacter < '0' || digitCharacter > '9')
                {
                    return false;
                }
                const std::size_t digitValue = static_cast<std::size_t>(digitCharacter - '0');
                if (accumulator > (std::numeric_limits<std::size_t>::max() - digitValue) / 10)
                {
                    return false;
                }
                accumulator = accumulator * 10 + digitValue;
            }

            resultValue = accumulator;
            return true;
        }

        /**
         * @brief 查找头部块结束位置（即正文起点）
         * @details 同时接受 CRLF CRLF 与裸 LF LF 两种形态：llhttp 对仅用 LF 分隔的老客户端
         *          是宽容的，定界器若只认 CRLF 就会与解析器对边界的判断不一致。
         *          两种形态都从同一个起点线性扫描，取更早出现的那个结束位置。
         * @param data   本条报文起点
         * @param length 已可读字节数
         * @return std::size_t 头部块长度（含结尾空行）；未找到时返回 kNotFoundPosition
         */
        std::size_t findHeaderBlockEnd(const char *data, const std::size_t length)
        {
            for (std::size_t index = 0; index + 1 < length; ++index)
            {
                // CRLF CRLF：占四个字节，因此要求 index+3 仍在界内
                if (data[index] == '\r' && index + 3 < length &&
                    data[index + 1] == '\n' && data[index + 2] == '\r' && data[index + 3] == '\n')
                {
                    return index + 4;
                }

                // 裸 LF LF：占两个字节
                if (data[index] == '\n' && data[index + 1] == '\n')
                {
                    return index + 2;
                }
            }
            return kNotFoundPosition;
        }
    } // namespace

    HttpSession::HttpSession(Core::AsyncSocket socket, Router &router) :
        Core::Connection(std::move(socket)),
        m_router(router),
        m_receiveBuffer(detail::kInitialReceiveBufferLength)
    {
        // 构造期就分配好初值大小的缓冲：会话协程第一次挂起前不该再做内存分配，
        // 之后仅在头部块不够大时按几何级数扩容
    }

    Core::Task<> HttpSession::start()
    {
        // 谓词提成命名局部：它要以 const std::function 引用的形式活过整个 co_await，
        // 直接传临时量就把正确性押在「挂起中的全表达式结束时才析构临时量」这条规则上，
        // 读代码的人不易一眼确认；放在本协程帧里则一目了然
        const std::function<bool()> alivePredicate = [this]()
        {
            return isAlive();
        };

        // 事务循环与 HTTPS 共用同一份模板实现，差别只在传输层对象与「连接是否存活」的谓词
        co_await detail::httpKeepAliveLoop(
                socket(), cancelable(), m_router, m_parser, m_receiveBuffer, alivePredicate);

        // 不论循环从哪条路径退出都要关连接：基类 close() 幂等，重复调用只是空操作
        close();
        co_return;
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

        FramingResult frameRequestMessage(const char *data, const std::size_t length)
        {
            FramingResult result;

            const std::size_t headerBlockEnd = findHeaderBlockEnd(data, length);
            if (headerBlockEnd == kNotFoundPosition)
            {
                // 还没收齐结尾空行：已缓冲的字节数已经超过头部块上限就直接判超限，否则继续读
                result.outcome = length > kMaximumHeaderBlockLength ? FrameOutcome::HeaderBlockTooLarge : FrameOutcome::NeedMore;
                return result;
            }

            if (headerBlockEnd > kMaximumHeaderBlockLength)
            {
                result.outcome = FrameOutcome::HeaderBlockTooLarge;
                return result;
            }

            // ---- 逐行扫头部：只为两件事——正文长度与是否分块。其余语义交给解析器 ----
            std::string_view headerBlock(data, headerBlockEnd);
            std::size_t declaredContentLength = 0;
            bool hasContentLength = false;
            bool hasChunkedTransferEncoding = false;

            // 跳过请求行：它没有 ':' 也无从影响正文长度
            if (const std::size_t firstLineFeed = headerBlock.find('\n'); firstLineFeed != std::string_view::npos)
            {
                headerBlock = headerBlock.substr(firstLineFeed + 1);
            }

            while (!headerBlock.empty())
            {
                const std::size_t lineFeed = headerBlock.find('\n');
                std::string_view currentLine = lineFeed == std::string_view::npos ? headerBlock : headerBlock.substr(0, lineFeed);
                if (!currentLine.empty() && currentLine.back() == '\r')
                {
                    currentLine = currentLine.substr(0, currentLine.size() - 1);
                }

                // 空行就是头部块的结尾标志，之后的字节属于正文，一律不再当头部解析
                if (currentLine.empty())
                {
                    break;
                }

                const std::size_t colonPosition = currentLine.find(':');

                // 没有 ':' 的行不是头部字段：可能是 obs-fold 续行（老式折行），
                // 它不影响正文长度，llhttp 才是判定报文合法性的地方，这里不重复裁决
                if (colonPosition != std::string_view::npos)
                {
                    const std::string fieldName = asciiToLowerCopy(trimHeaderWhitespace(currentLine.substr(0, colonPosition)));
                    const std::string_view rawFieldValue = trimHeaderWhitespace(currentLine.substr(colonPosition + 1));

                    if (fieldName == "content-length")
                    {
                        std::size_t currentValue = 0;
                        if (!parseDecimalLength(rawFieldValue, currentValue))
                        {
                            result.outcome = FrameOutcome::ContentLengthInvalid;
                            return result;
                        }

                        // 同一条报文里出现两个不一致的 Content-Length：这是请求走私最经典的形态，
                        // 代理链上下游会因此对边界各判各的，唯一安全的处置是整条拒收
                        if (hasContentLength && declaredContentLength != currentValue)
                        {
                            result.outcome = FrameOutcome::ContentLengthInvalid;
                            return result;
                        }

                        declaredContentLength = currentValue;
                        hasContentLength      = true;

                        // 声明值超上限：现在就拒，一个正文字节都不用收进内存
                        if (declaredContentLength > kMaximumDeclaredBodyLength)
                        {
                            result.outcome = FrameOutcome::ContentLengthTooLarge;
                            return result;
                        }
                    } else if (fieldName == "transfer-encoding")
                    {
                        const std::string fieldValue = asciiToLowerCopy(rawFieldValue);
                        if (fieldValue.find("chunked") != std::string::npos)
                        {
                            hasChunkedTransferEncoding = true;
                        }
                    }
                }

                if (lineFeed == std::string_view::npos)
                {
                    break;
                }
                headerBlock = headerBlock.substr(lineFeed + 1);
            }

            // 两种「正文到哪里结束」的判据同时出现，必须拒收：留哪一个都是走私窗口
            if (hasChunkedTransferEncoding && hasContentLength)
            {
                result.outcome = FrameOutcome::ContentLengthInvalid;
                return result;
            }

            // 分块正文不做帧定界（理由见 frameRequestMessage 的 @details），按 411 收口
            if (hasChunkedTransferEncoding)
            {
                result.outcome = FrameOutcome::ChunkedBodyNotSupported;
                return result;
            }

            result.outcome            = FrameOutcome::Complete;
            result.headerBlockLength  = headerBlockEnd;
            result.messageLength      = headerBlockEnd + (hasContentLength ? declaredContentLength : 0);
            return result;
        }

        void writeFramingErrorResponse(HttpResponse &response, const FrameOutcome outcome)
        {
            // 状态码与英文原因短语由 HttpResponse 侧统一给出，这里只写 ASCII 正文：
            // 中文说明留在注释与日志里，协议字段里塞非 ASCII 字节会让对端按自己的编码猜
            switch (outcome)
            {
                case FrameOutcome::HeaderBlockTooLarge:
                    // 431：头部过大，与 nginx 的 large_client_header_buffers 行为一致
                    response.setStatus(431);
                    response.setBody("Request Header Fields Too Large");
                    break;
                case FrameOutcome::ContentLengthTooLarge:
                    response.setStatus(413);
                    response.setBody("Payload Too Large");
                    break;
                case FrameOutcome::ContentLengthInvalid:
                    // 400：谎报或重复声明长度，报文本身不合法，不与「体量太大」混为一谈
                    response.setStatus(400);
                    response.setBody("Bad Request: Invalid Content-Length");
                    break;
                case FrameOutcome::ChunkedBodyNotSupported:
                    // 411 Length Required：RFC 9110 §15.5.8，服务器拒绝处理缺少 Content-Length 的请求
                    response.setStatus(411);
                    response.setBody("Length Required");
                    break;
                case FrameOutcome::NeedMore:
                case FrameOutcome::Complete:
                default:
                    // 这两种不是错误结论，调用方不会带它们进来；给个 400 兜底而不是让响应停在默认 200
                    response.setStatus(400);
                    response.setBody("Bad Request");
                    break;
            }

            response.setHeader("content-type", "text/plain");
        }
    } // namespace detail
} // namespace AsynGyanis::Net

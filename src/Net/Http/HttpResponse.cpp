#include "Net/Http/HttpResponse.h"

#include "Net/Http/HttpDate.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        // ====================================================================
        // 序列化预留量：全部由 constexpr 常量按报文结构推导，避免出现无出处的魔法数字。
        // 预留只是容量提示，估少了 std::string 会自行扩容，不影响正确性，只影响分配次数
        // ====================================================================

        constexpr std::string_view kCrLf = "\r\n";                            ///< 报文行分隔符，HTTP 固定为 CR LF

        // 状态行结构：版本 SP 状态码 SP 原因短语 CRLF（RFC 9110 §3.1.2）
        constexpr std::size_t kCrLfLength = 2;                                ///< CRLF 占用字节数
        constexpr std::size_t kHttpVersionReserveLength = 16;                  ///< "HTTP/1.1" 实为 8 字节，取 16 容纳自定义版本串
        constexpr std::size_t kSingleSpaceLength = 1;                          ///< 状态行里的字段分隔空格
        constexpr std::size_t kStatusCodeTextLength = 3;                       ///< 状态码按规范恰为三位十进制
        constexpr std::size_t kLongestReasonPhraseLength = 29;                 ///< 表内最长原因短语 "Unavailable For Legal Reasons"
        constexpr std::size_t kStatusLineReserveLength =
                kHttpVersionReserveLength + kSingleSpaceLength + kStatusCodeTextLength + kSingleSpaceLength +
                kLongestReasonPhraseLength + kCrLfLength;                       ///< 状态行预留量 = 16 + 1 + 3 + 1 + 29 + 2 = 52

        // 单条头部结构：名 ":" SP 值 CRLF
        constexpr std::string_view kHeaderNameValueSeparator = ": ";           ///< 头部名与值之间的分隔符
        constexpr std::size_t kHeaderLineReserveLength =
                kHeaderNameValueSeparator.size() + kCrLfLength;                 ///< "name: value\r\n" 里除名与值之外的固定开销 = 2 + 2

        constexpr std::size_t kHeaderBlockTerminatorReserveLength = kCrLfLength;///< 头部块收尾的空白行

        // 自动补出的头部：与下面的名字常量同处定义，改名字时不会漏改预留量
        constexpr std::string_view kContentTypeHeaderName = "content-type";     ///< 媒体类型头部名（小写形态）
        constexpr std::string_view kContentLengthHeaderName = "content-length"; ///< 正文长度头部名（小写形态）
        constexpr std::string_view kDateHeaderName = "date";                    ///< 日期头部名（小写形态）
        constexpr std::string_view kAutoContentTypeHeader = "content-type: text/plain\r\n";      ///< 未设媒体类型且有正文时补出的整条头部
        constexpr std::string_view kAutoContentLengthHeaderPrefix = "content-length: ";          ///< 未设正文长度时补出的头部名前缀（含冒号与空格）
        constexpr std::string_view kAutoDateHeaderPrefix = "date: ";                             ///< 未设日期时补出的头部名前缀（含冒号与空格）

        constexpr std::size_t kMaximumUnsignedDecimalTextLength = std::numeric_limits<std::uint64_t>::max_digits10; ///< 64 位无符号十进制最长 20 位；有符号 int 在 appendDecimal 里按 max_digits10 + 2（含负号位）同理推导

        constexpr std::size_t kAutoContentTypeReserveLength = kAutoContentTypeHeader.size();     ///< "content-type: text/plain\r\n" 的实际字节数 = 26
        constexpr std::size_t kAutoContentLengthReserveLength =
                kAutoContentLengthHeaderPrefix.size() + kMaximumUnsignedDecimalTextLength + kCrLfLength; ///< 前缀 16 + 最多 20 位数字 + CRLF 2 = 38
        constexpr std::size_t kAutoDateReserveLength = kAutoDateHeaderPrefix.size() + kHttpDateTextLength + kCrLfLength; ///< 前缀 6 + 定长 29 + CRLF 2 = 37

        // 允许在同一报文里出现多条、且不得合并的头部名单（已归一化为小写）。
        // 与请求侧保持同一份判定口径：RFC 6265 规定多条 Set-Cookie 各表达一个独立 cookie
        constexpr std::array<std::string_view, 1> kRepeatableHeaderNames{"set-cookie"};

        /**
         * @brief 以十进制形式把整数追加到序列化缓冲
         * @details 缓冲长度按整数类型推导，杜绝「8 字节栈数组配 int」的写穿风险；
         *          to_chars 的 error_code 一并检查，失败时退回 std::to_string 兜底。
         * @tparam IntegerType 整数类型，须为整型
         * @param target 目标字符串，结果追加在其尾部
         * @param value  待写出的整数值
         */
        template <typename IntegerType>
        void appendDecimal(std::string &target, const IntegerType value)
        {
            static_assert(std::is_integral_v<IntegerType>, "appendDecimal 只接受整数类型");

            std::array<char, std::numeric_limits<IntegerType>::max_digits10 + 2> textBuffer{};
            const auto [pointer, errorCode] =
                    std::to_chars(textBuffer.data(), textBuffer.data() + textBuffer.size(), value);

            if (errorCode == std::errc())
            {
                // pointer 指向最后一个已写入字符之后，差值即实际字符数；
                // 这里不必再防减法越界——to_chars 成功时 pointer 必然落在 [first, last] 区间内
                target.append(textBuffer.data(), static_cast<std::size_t>(pointer - textBuffer.data()));
                return;
            }

            // 缓冲已按类型上限给足，正常平台走不到这里（唯一可能是 result_out_of_range）。
            // 真遇到了退回 std::to_string：宁可多一次堆分配，也绝不让 content-length 写成半截数字
            target.append(std::to_string(value));
        }

        /**
         * @brief 判断字符串是否是合法的 HTTP 头部字段名
         * @details 按 RFC 9110 §5.1 的 tchar 集合逐字符校验：`!#$%&'*+-.^_`|~` 加数字与字母。
         *          空白、冒号、控制字符都会让报文无法定界，一律拒绝；空名字同样非法。
         * @param fieldName 已归一化为小写的头部字段名
         * @return true 可作为头部字段名
         */
        bool isValidHeaderFieldName(const std::string &fieldName)
        {
            if (fieldName.empty())
            {
                return false;
            }

            const auto isTokenCharacter = [](const char character)
            {
                static constexpr std::string_view kTokenSeparators = "!#$%&'*+-.^_`|~";
                return std::isalnum(static_cast<unsigned char>(character)) != 0 || kTokenSeparators.find(character) != std::string_view::npos;
            };
            return std::ranges::all_of(fieldName, isTokenCharacter);
        }

        /**
         * @brief 判断头部字段值是否可以安全写入报文
         * @details 头部块以 CRLF 定界，值里出现 CR/LF 就等于让调用方自行结束头部块，
         *          是 HTTP 响应拆分的经典入口；NUL 与其它控制字符同样会撕裂报文。
         *          水平制表符是 RFC 允许在 OWS 中出现的空白，因此放行。
         * @param headerValue 头部字段值
         * @return true 可以写入
         */
        bool isSafeHeaderValue(const std::string &headerValue)
        {
            const auto isForbiddenCharacter = [](const char character)
            {
                // character 先转 unsigned char：把负值交给 std::iscntrl 是未定义行为
                const auto unsignedCharacter = static_cast<unsigned char>(character);
                return character == '\0' || (std::iscntrl(unsignedCharacter) != 0 && character != '\t');
            };
            return std::ranges::find_if(headerValue, isForbiddenCharacter) == headerValue.end();
        }
    } // namespace

    HttpResponse::HttpResponse() = default;

    void HttpResponse::setStatus(const int code)
    {
        m_status = code;
    }

    void HttpResponse::setHttpVersion(std::string version)
    {
        m_httpVersion = std::move(version);
    }

    int HttpResponse::status() const
    {
        return m_status;
    }

    void HttpResponse::lowercaseInPlace(std::string &name)
    {
        // 逐字符查 ASCII 表：std::tolower 只接受 unsigned char 或 EOF，
        // 把可能为负的 char 直接喂进去是未定义行为
        std::ranges::transform(name, name.begin(),
                               [](const unsigned char character)
                               {
                                   return static_cast<char>(std::tolower(character));
                               });
    }

    std::string HttpResponse::toCanonicalHeaderName(const std::string_view name)
    {
        std::string canonicalName(name);
        lowercaseInPlace(canonicalName);
        return canonicalName;
    }

    bool HttpResponse::isRepeatableHeaderName(const std::string_view canonicalName)
    {
        // 名单极小，线性比较比构造哈希集合划算
        return std::ranges::find(kRepeatableHeaderNames, canonicalName) != kRepeatableHeaderNames.end();
    }

    auto HttpResponse::findHeaderField(const std::string &canonicalName) -> HeaderFieldList::iterator
    {
        // 头部数量级为几十条，线性比较比再挂一张「名到迭代器」的索引表更划算，也少一份要维护的一致性
        return std::ranges::find_if(m_headerFields,
                                    [&canonicalName](const HeaderField &field)
                                    {
                                        return field.name == canonicalName;
                                    });
    }

    bool HttpResponse::setHeader(const std::string &name, const std::string &value)
    {
        // 头部名转小写后入库，判据也按小写形态给出
        const std::string canonicalName = toCanonicalHeaderName(name);

        // CR/LF/NUL 会让调用方提前结束头部块（HTTP 响应拆分），字段名里的空白与控制字符
        // 则产出线上非法报文——两种情况都拒写，且不改动任何已有状态
        if (!isValidHeaderFieldName(canonicalName) || !isSafeHeaderValue(value))
        {
            return false;
        }

        if (isRepeatableHeaderName(canonicalName))
        {
            // 可重复头部（Set-Cookie）的 set 语义退化为「追加一条」：既有调用方逐条 setHeader
            // 下发多个 cookie，覆盖式写法会静默丢掉前面的 cookie。每条各占一项，序列化时逐条上线
            m_headerFields.push_back(HeaderField{.name = canonicalName, .value = value});

            // 单值视图只留首条：headers()/getHeader() 的「一个名字一个值」契约不变，
            // 需要全部值请用 headerValues()
            m_headers.try_emplace(canonicalName, value);
            return true;
        }

        if (const auto iterator = findHeaderField(canonicalName); iterator != m_headerFields.end())
        {
            // 普通头部：原地覆盖值，条目位置仍停在首次设置处，
            // 这样序列化顺序不因反复改写而漂移
            iterator->value = value;
            m_headers[canonicalName] = value;
            return true;
        }

        m_headerFields.push_back(HeaderField{.name = canonicalName, .value = value});
        m_headers.emplace(canonicalName, value);
        return true;
    }

    std::optional<std::string> HttpResponse::getHeader(const std::string &name) const
    {
        if (const auto iterator = m_headers.find(toCanonicalHeaderName(name)); iterator != m_headers.end())
        {
            return iterator->second;
        }
        // 缺席是常态而非错误：可选头部查不到时交回空 optional
        return std::nullopt;
    }

    std::vector<std::string> HttpResponse::headerValues(const std::string &name) const
    {
        const std::string canonicalName = toCanonicalHeaderName(name);

        std::vector<std::string> values;
        // 按权威记录的设置顺序收集，与 toString() 的上线顺序一致
        for (const HeaderField &field : m_headerFields)
        {
            if (field.name == canonicalName)
            {
                values.push_back(field.value);
            }
        }
        return values;
    }

    const std::unordered_map<std::string, std::string> &HttpResponse::headers() const
    {
        return m_headers;
    }

    void HttpResponse::setBody(const std::string_view body)
    {
        // 两条正文存储互斥：换成堆正文之前先解除映射，否则 bodyView() 会继续读旧映射
        m_mappedBody = Platform::MemoryMappedFile{};
        m_mappedBodyOffset = 0;
        m_mappedBodyLength = 0;

        // string_view 不保证零终止也不拥有内存，落到成员前必须实体化一份
        m_body = std::string(body);
    }

    void HttpResponse::setMappedBody(Platform::MemoryMappedFile mappedFile)
    {
        // 整份文件等价于 [0, 映射字节数) 这个区间。长度必须先在移动之前取好：
        // 形参求值顺序未指定，边移动边取长度会读到已搬空的映射
        const std::size_t mappedLength = mappedFile.bytes().size();
        setMappedBody(std::move(mappedFile), 0, mappedLength);
    }

    void HttpResponse::setMappedBody(Platform::MemoryMappedFile mappedFile, const std::size_t offset, const std::size_t length)
    {
        const std::size_t availableLength = mappedFile.isValid() ? mappedFile.bytes().size() : 0;

        // 越界属于调用方的用法错误（重试无用），归入 logic_error 分支；静默钳制会让
        // content-length 与实际字节数悄悄不一致，那正是收端报文边界错位的源头
        if (offset > availableLength || length > availableLength - offset)
        {
            throw Base::InvalidArgumentException("HttpResponse::setMappedBody：映射正文区间越界，offset=" + std::to_string(offset) +
                                                 "，length=" + std::to_string(length) + "，映射字节数=" + std::to_string(availableLength) +
                                                 "；请先按 MemoryMappedFile::bytes().size() 校验区间，或改用整份映射的重载");
        }

        // 反向的互斥：映射正文接管后堆正文必须清空，避免 content-length 按残留字节数算错
        m_body.clear();
        m_mappedBody = std::move(mappedFile);
        m_mappedBodyOffset = offset;
        m_mappedBodyLength = length;
    }

    std::string_view HttpResponse::bodyView() const noexcept
    {
        if (m_mappedBody.isValid())
        {
            const std::span<const std::byte> mappedBytes = m_mappedBody.bytes();
            if (mappedBytes.empty() || m_mappedBodyLength == 0)
            {
                // 空文件映射不出可解引用的地址（data() 可能为空），长度为 0 的区间同理，
                // 两者都用空视图表示「正文 0 字节」，不构造 string_view(nullptr, 0)
                return {};
            }
            // 区间合法性已由 setMappedBody 拦住，这里直接按 offset/length 取子视图
            return std::string_view(reinterpret_cast<const char *>(mappedBytes.data() + m_mappedBodyOffset), m_mappedBodyLength);
        }
        return m_body;
    }

    std::string_view HttpResponse::autoDateText() const
    {
        // 只生成一次：同一响应多次序列化（serializeHead + toString）必须给出逐字一致的 date
        if (m_autoDateValue.empty())
        {
            m_autoDateValue = formatHttpDate(std::chrono::system_clock::now());
        }
        return m_autoDateValue;
    }

    std::string_view HttpResponse::body() const
    {
        return bodyView();
    }

    const char *HttpResponse::statusMessage(const int code)
    {
        // 只收录本框架会用到的状态码；未收录者给出空原因短语，
        // RFC 9110 §3.1.2 允许 reason-phrase 为空，状态行仍然合法
        switch (code)
        {
            case 100:
                return "Continue";
            case 101:
                return "Switching Protocols";
            case 102:
                return "Processing";
            case 103:
                return "Early Hints";
            case 200:
                return "OK";
            case 201:
                return "Created";
            case 202:
                return "Accepted";
            case 204:
                return "No Content";
            case 206:
                return "Partial Content";
            case 301:
                return "Moved Permanently";
            case 302:
                return "Found";
            case 304:
                return "Not Modified";
            case 307:
                return "Temporary Redirect";
            case 308:
                return "Permanent Redirect";
            case 400:
                return "Bad Request";
            case 401:
                return "Unauthorized";
            case 403:
                return "Forbidden";
            case 404:
                return "Not Found";
            case 405:
                return "Method Not Allowed";
            case 406:
                return "Not Acceptable";
            case 408:
                return "Request Timeout";
            case 409:
                return "Conflict";
            case 410:
                return "Gone";
            case 413:
                return "Payload Too Large";
            case 414:
                return "URI Too Long";
            case 415:
                return "Unsupported Media Type";
            case 416:
                return "Range Not Satisfiable";
            case 422:
                return "Unprocessable Content";
            case 429:
                return "Too Many Requests";
            case 451:
                return "Unavailable For Legal Reasons";
            case 500:
                return "Internal Server Error";
            case 501:
                return "Not Implemented";
            case 502:
                return "Bad Gateway";
            case 503:
                return "Service Unavailable";
            case 504:
                return "Gateway Timeout";
            case 505:
                return "HTTP Version Not Supported";
            default:
                return "";
        }
    }

    bool HttpResponse::carriesNoContent() const noexcept
    {
        // RFC 9110 §6.3：1xx、204、304 一律不含正文。这里按状态码在序列化层兜住，
        // 不能只指望上游 Router 的 finalizeResponse 先把正文清空——业务直接
        // setStatus(204) + setBody(...) 就会发出「没有 content-length 却带正文」的报文，
        // keep-alive 上的下一帧边界随之错位
        return m_status == 204 || m_status == 304 || (m_status >= 100 && m_status < 200);
    }

    bool HttpResponse::mustNotDeclareContentLength() const noexcept
    {
        // 204 与 1xx 响应不得带正文，自动补 content-length 会让收端把「接下来没有字节」
        // 当成一条额外承诺；304 则被 RFC 7230 明确允许携带，故不排除
        return m_status == 204 || (m_status >= 100 && m_status < 200);
    }

    std::size_t HttpResponse::headReserveLength() const
    {
        std::size_t reservedLength = kStatusLineReserveLength + kHeaderBlockTerminatorReserveLength;
        bool        hasContentTypeHeader = false;
        bool        hasContentLengthHeader = false;
        bool        hasDateHeader = false;
        for (const HeaderField &field : m_headerFields)
        {
            reservedLength += field.name.size() + field.value.size() + kHeaderLineReserveLength;

            // 名比对直接用库内的小写形态，无需再归一化
            if (field.name == kContentTypeHeaderName)
            {
                hasContentTypeHeader = true;
            } else if (field.name == kContentLengthHeaderName)
            {
                hasContentLengthHeader = true;
            } else if (field.name == kDateHeaderName)
            {
                hasDateHeader = true;
            }
        }
        if (!hasContentTypeHeader && !bodyView().empty())
        {
            reservedLength += kAutoContentTypeReserveLength;
        }
        if (!hasContentLengthHeader && !mustNotDeclareContentLength())
        {
            reservedLength += kAutoContentLengthReserveLength;
        }
        if (!hasDateHeader)
        {
            reservedLength += kAutoDateReserveLength;
        }
        return reservedLength;
    }

    void HttpResponse::appendHead(std::string &result) const
    {
        // ---- 状态行。版本取 setHttpVersion 传进来的值，与请求行版本保持一致 ----
        result.append(m_httpVersion);
        result.push_back(' ');
        appendDecimal(result, m_status);
        result.push_back(' ');
        result.append(statusMessage(m_status));
        result.append(kCrLf);

        // ---- 头部块。按权威记录的设置顺序逐条输出，不再遍历 unordered_map ----
        // 顺带解决了两件事：跨次运行顺序稳定；多条 Set-Cookie 各占一行且先设先发
        bool hasContentTypeHeader  = false;
        bool hasContentLengthHeader = false;
        bool hasDateHeader = false;
        for (const HeaderField &field : m_headerFields)
        {
            if (field.name == kContentTypeHeaderName)
            {
                hasContentTypeHeader = true;
            } else if (field.name == kContentLengthHeaderName)
            {
                hasContentLengthHeader = true;
            } else if (field.name == kDateHeaderName)
            {
                hasDateHeader = true;
            }

            result.append(field.name);
            result.append(kHeaderNameValueSeparator);
            result.append(field.value);
            result.append(kCrLf);
        }

        // ---- 补缺。调用方没写的几条由这里兜底，排在自设头部之后 ----
        const std::string_view responseBody = bodyView();
        if (!hasContentTypeHeader && !responseBody.empty())
        {
            // 有正文却漏设媒体类型时按纯文本下发：不会被浏览器当脚本执行，是最安全的兜底
            result.append(kAutoContentTypeHeader);
        }
        if (!hasContentLengthHeader && !mustNotDeclareContentLength())
        {
            // content-length 必须是正文的真实字节数，收端据此判定报文边界，错一个字节整条连接就错位
            result.append(kAutoContentLengthHeaderPrefix);
            appendDecimal(result, responseBody.size());
            result.append(kCrLf);
        }
        if (!hasDateHeader)
        {
            // Date 是 RFC 9110 §6.6.1 要求源服务器在几乎所有响应上都给出的头部
            result.append(kAutoDateHeaderPrefix);
            result.append(autoDateText());
            result.append(kCrLf);
        }

        result.append(kCrLf);
    }

    std::string HttpResponse::serializeHead() const
    {
        std::string result;
        result.reserve(headReserveLength());
        appendHead(result);
        return result;
    }

    std::string HttpResponse::toString() const
    {
        const std::string_view responseBody = bodyView();

        std::string result;
        // 头部与正文一次预留到位：估不准只是多一次扩容，不影响正确性
        result.reserve(headReserveLength() + (carriesNoContent() ? 0 : responseBody.size()));
        appendHead(result);
        if (!carriesNoContent())
        {
            result.append(responseBody);
        }
        return result;
    }

    HttpResponse HttpResponse::ok(std::string body)
    {
        HttpResponse response;
        response.setStatus(200);
        response.setBody(std::move(body));
        response.setHeader("content-type", "text/plain");
        return response;
    }

    HttpResponse HttpResponse::notFound()
    {
        HttpResponse response;
        response.setStatus(404);
        response.setBody("Not Found");
        response.setHeader("content-type", "text/plain");
        return response;
    }

    HttpResponse HttpResponse::serverError(std::string message)
    {
        HttpResponse response;
        response.setStatus(500);
        response.setBody(std::move(message));
        response.setHeader("content-type", "text/plain");
        return response;
    }

    void HttpResponse::reset()
    {
        m_status      = 200;
        m_httpVersion = "HTTP/1.1";

        // 两份存储一起清：只清一处会留下「视图里查得到、序列化里没有」的鬼条目
        m_headerFields.clear();
        m_headers.clear();

        // 正文同样是两条存储：堆串清空之外映射也要解除，
        // 否则复用响应对象时上一轮的文件会继续当正文发出去
        m_body.clear();
        m_mappedBody = Platform::MemoryMappedFile{};
        m_mappedBodyOffset = 0;
        m_mappedBodyLength = 0;

        // 自动 date 同样要清：否则复用响应时下一条报文会带上上一轮生成的日期
        m_autoDateValue.clear();
    }

} // namespace AsynGyanis::Net

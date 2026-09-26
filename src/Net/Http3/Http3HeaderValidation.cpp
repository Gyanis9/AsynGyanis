#include "Net/Http3/Http3HeaderValidation.h"

#include "Net/Http/HttpHeaderRules.h"

#include <cstddef>
#include <expected>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        /// 造一个判定失败：文案统一带上字段名实际值与规范章节，便于日志直接定位是哪一条规则
        Http3HeaderError makeHeaderError(const Http3HeaderErrorKind errorKind, std::string messageText)
        {
            return Http3HeaderError{.kind = errorKind, .message = std::move(messageText)};
        }

        /// RFC 3986 §3.1 的 scheme：首字符必须是字母，其后只允许字母数字与 + - .
        bool isSchemeCharacter(const char character) noexcept
        {
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '+' ||
                   character == '-' || character == '.';
        }

        /// 把字节写成可读文本再进日志：控制字符与高位字节原样打出来会污染终端，看不出问题在哪
        std::string printableFieldText(std::string_view text, const std::size_t maximumDisplayByteCount = 48)
        {
            const std::string_view excerpt = text.substr(0, maximumDisplayByteCount < text.size() ? maximumDisplayByteCount : text.size());
            std::string            result;
            result.reserve(excerpt.size() + 8);
            for (const char character: excerpt)
            {
                const auto byte = static_cast<unsigned char>(character);
                if (byte >= 0x20 && byte < 0x7F)
                {
                    result.push_back(character);
                    continue;
                }
                // 不可打印字节写成 \xNN：否则「值里夹了个 NUL」这类缺陷在日志里看着与正常值一模一样
                static constexpr char kHexDigits[] = "0123456789ABCDEF";
                result.append("\\x");
                result.push_back(kHexDigits[(byte >> 4) & 0x0FU]);
                result.push_back(kHexDigits[byte & 0x0FU]);
            }
            if (excerpt.size() < text.size())
            {
                result.append("…（已截断）");
            }
            return result;
        }
    } // namespace

    Http3ErrorCode toHttp3ErrorCode(const Http3HeaderErrorKind /*errorKind*/) noexcept
    {
        // 不按类别分码：RFC 9114 §4.1.2 把这里每一条都归成同一个流错误 H3_MESSAGE_ERROR，
        // 分码反而会让上层以为「某些畸形可以只回 400 不重置流」
        return Http3ErrorCode::MessageError;
    }

    Http3HeaderValidator::Http3HeaderValidator(const Http3MessageKind messageKind, const bool isExtendedConnectPermitted) noexcept :
        m_messageKind(messageKind), m_isExtendedConnectPermitted(isExtendedConnectPermitted)
    {
    }

    std::expected<void, Http3HeaderError> Http3HeaderValidator::beginHeaderBlock(const bool isTrailers) noexcept
    {
        // 一条消息只有一个头段；再来的必须是尾段。本实现不建模 1xx（服务端不产、收到即按非法序列拒），
        // 因为既有 h1/h2 路径也没有把中间响应交给业务的位置
        if (m_isHeadSectionDone && !isTrailers)
        {
            return std::unexpected(
                    makeHeaderError(Http3HeaderErrorKind::InvalidMessageSequence, "同一消息里出现了第二个头段：HTTP/3 只允许一个头段加至多一个尾段（RFC 9114 §4.1）"));
        }
        // 尾段只能出现在头段之后：单独一个尾段头块没有可归属的消息
        if (isTrailers && !m_isHeadSectionDone)
        {
            return std::unexpected(makeHeaderError(Http3HeaderErrorKind::InvalidMessageSequence, "头块序列非法：尾段（trailer section）出现在头段之前（RFC 9114 §4.1）"));
        }

        m_isTrailersSection = isTrailers;
        // 伪头的顺序标记与 content-length 都是**每个头段**独立判的：尾段不该带这些，带了要能报出来
        m_sawRegularField = false;
        return {};
    }

    std::expected<void, Http3HeaderError> Http3HeaderValidator::onHeaderField(const std::string_view name, std::string_view value)
    {
        if (name.empty())
        {
            return std::unexpected(makeHeaderError(Http3HeaderErrorKind::EmptyFieldName, "头字段名为空：无法构成合法的 field-name（RFC 9110 §5.1）"));
        }

        // 伪头以 ':' 开头，且只可能出现在头段开头一段里：单独一条分支处理，避免每个规则都判两次首字符
        if (name.front() == ':')
        {
            return onPseudoHeader(name.substr(1), value);
        }
        return onRegularField(name, value);
    }

    std::expected<void, Http3HeaderError> Http3HeaderValidator::onPseudoHeader(std::string_view name, std::string_view value)
    {
        // RFC 9114 §4.3：伪头不得出现在尾段里
        if (m_isTrailersSection)
        {
            return std::unexpected(
                    makeHeaderError(Http3HeaderErrorKind::PseudoHeaderInTrailers, "伪头 \":" + std::string(name) + "\" 出现在尾段里：尾段只允许普通字段（RFC 9114 §4.3）"));
        }
        // RFC 9114 §4.3：伪头必须全部排在普通字段之前
        if (m_sawRegularField)
        {
            return std::unexpected(makeHeaderError(Http3HeaderErrorKind::PseudoHeaderAfterField, "伪头 \":" + std::string(name) + "\" 出现在普通字段之后（RFC 9114 §4.3）"));
        }
        if (name.empty())
        {
            return std::unexpected(makeHeaderError(Http3HeaderErrorKind::UndefinedPseudoHeader, "伪头名只有一个冒号，没有任何名字"));
        }
        // 伪头名的字符集与字段名同一套（tchar），冒号之外不额外放行
        for (const char character: name)
        {
            if (!isTokenCharacter(static_cast<unsigned char>(character)) || (character >= 'A' && character <= 'Z'))
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::UndefinedPseudoHeader, "伪头名 \"" + std::string(name) + "\" 含非法字符或大写字母（RFC 9110 §5.1）"));
            }
        }
        if (auto fieldError = validateFieldValue(value); !fieldError)
        {
            return fieldError;
        }

        if (m_messageKind == Http3MessageKind::Response)
        {
            // 响应侧只定义了 :status 一个伪头（RFC 9114 §4.3.2），其余一律按未定义处理
            if (name != "status")
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::UndefinedPseudoHeader,
                                                       "响应里出现未定义的伪头 \":" + std::string(name) + "\"：响应只定义 :status（RFC 9114 §4.3.2）"));
            }
            if (m_statusCode.has_value())
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::DuplicatePseudoHeader, "响应里出现了第二个 :status（RFC 9114 §4.3.2）"));
            }
            // 状态码必须是恰好三位十进制：这不是「宽松解析」，三位是规范的硬要求
            if (value.size() != 3 || value[0] < '1' || value[0] > '9' || value[1] < '0' || value[1] > '9' || value[2] < '0' || value[2] > '9')
            {
                return std::unexpected(
                        makeHeaderError(Http3HeaderErrorKind::InvalidStatusValue, ":status 取值 \"" + printableFieldText(value) + "\" 不是三位十进制状态码（RFC 9114 §4.3.2）"));
            }
            m_statusCode = (value[0] - '0') * 100 + (value[1] - '0') * 10 + (value[2] - '0');
            return {};
        }

        if (name == "method")
        {
            if (!m_methodText.empty())
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::DuplicatePseudoHeader, "请求里出现了第二个 :method（RFC 9114 §4.3.1）"));
            }
            if (value.empty())
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::MissingPseudoHeader, ":method 取值为空：方法名不得为空串（RFC 9110 §9）"));
            }
            m_methodText.assign(value);
            return {};
        }
        if (name == "scheme")
        {
            if (!m_schemeText.empty())
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::DuplicatePseudoHeader, "请求里出现了第二个 :scheme（RFC 9114 §4.3.1）"));
            }
            if (value.empty() || !((value.front() >= 'a' && value.front() <= 'z') || (value.front() >= 'A' && value.front() <= 'Z')))
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::MissingPseudoHeader, ":scheme 取值为空或首字符不是字母：scheme 的语法见 RFC 3986 §3.1"));
            }
            for (const char character: value)
            {
                if (!isSchemeCharacter(character))
                {
                    return std::unexpected(
                            makeHeaderError(Http3HeaderErrorKind::MissingPseudoHeader, ":scheme 取值 \"" + printableFieldText(value) + "\" 含 RFC 3986 §3.1 之外的字符"));
                }
            }
            m_schemeText.assign(value);
            return {};
        }
        if (name == "authority")
        {
            if (!m_authorityText.empty())
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::DuplicatePseudoHeader, "请求里出现了第二个 :authority（RFC 9114 §4.3.1）"));
            }
            if (value.empty())
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::EmptyAuthority, ":authority 取值为空：出现就必须非空（RFC 9114 §4.3.1）"));
            }
            for (const char character: value)
            {
                // 权威段里出现这些字符说明对端把整条 URI 或带 userinfo 的目标塞了进来：
                // RFC 9114 §4.3.1 明确禁止 http/https 的 userinfo，而 '/' '?' '#' 会让路由读到越界的目标
                if (character == '@' || character == '/' || character == '\\' || character == '?' || character == '#' || character == ' ' || character == '\t')
                {
                    return std::unexpected(
                            makeHeaderError(Http3HeaderErrorKind::EmptyAuthority, ":authority 取值 \"" + printableFieldText(value) + "\" 含权威段不允许的字符（RFC 9114 §4.3.1）"));
                }
            }
            m_authorityText.assign(value);
            return {};
        }
        if (name == "path")
        {
            if (!m_pathText.empty())
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::DuplicatePseudoHeader, "请求里出现了第二个 :path（RFC 9114 §4.3.1）"));
            }
            if (value.empty())
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::EmptyPath, ":path 为空：http/https 的目标 URI 至少要带 \"/\"（RFC 9114 §4.3.1）"));
            }
            // OPTIONS 的星号形式是唯一不以 '/' 开头的合法取值（RFC 9110 §7.1）
            if (value != "*" && value.front() != '/')
            {
                return std::unexpected(
                        makeHeaderError(Http3HeaderErrorKind::EmptyPath, ":path 取值 \"" + printableFieldText(value) + "\" 不是以 / 开头的 path-absolute（RFC 9114 §4.3.1）"));
            }
            for (const char character: value)
            {
                // 片段标识（# 之后）不得出现在请求目标里（RFC 9110 §7.1），空格与控制字符同理
                if (character == '#' || character == ' ' || character == '\\' || static_cast<unsigned char>(character) < 0x21 || static_cast<unsigned char>(character) == 0x7F)
                {
                    return std::unexpected(makeHeaderError(Http3HeaderErrorKind::EmptyPath, ":path 取值 \"" + printableFieldText(value) + "\" 含请求目标不允许的字符"));
                }
            }
            m_pathText.assign(value);
            return {};
        }
        if (name == "protocol")
        {
            // RFC 9220 的扩展 CONNECT：只有对端 SETTINGS 里开了 ENABLE_CONNECT_PROTOCOL 才认这个伪头
            if (!m_isExtendedConnectPermitted)
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::UndefinedPseudoHeader,
                                                       "请求带了 :protocol，但对端没有用 SETTINGS_ENABLE_CONNECT_PROTOCOL=1 声明支持扩展 CONNECT（RFC 9220 §3.2）"));
            }
            if (!m_protocolText.empty())
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::DuplicatePseudoHeader, "请求里出现了第二个 :protocol（RFC 9220 §3.1.1）"));
            }
            if (value.empty())
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::UndefinedPseudoHeader, ":protocol 取值为空：扩展 CONNECT 必须写明协议名（RFC 9220 §3.1.1）"));
            }
            m_protocolText.assign(value);
            return {};
        }

        return std::unexpected(makeHeaderError(Http3HeaderErrorKind::UndefinedPseudoHeader, "请求里出现未定义的伪头 \":" + std::string(name) + "\"（RFC 9114 §4.3）"));
    }

    std::expected<void, Http3HeaderError> Http3HeaderValidator::onRegularField(const std::string_view name, std::string_view value)
    {
        if (auto nameError = validateFieldName(name); !nameError)
        {
            return nameError;
        }
        if (auto valueError = validateFieldValue(value); !valueError)
        {
            return valueError;
        }

        // 从这一刻起再出现伪头就是畸形：标记必须在所有拒绝判定之后才置，
        // 否则一个非法字段会把后面的合法伪头一起打死
        m_sawRegularField = true;

        // TE 是连接特定字段规则的唯一例外，且只允许 trailers 这一个值（RFC 9114 §4.2）
        if (equalsIgnoringCase(name, "te"))
        {
            if (!equalsIgnoringCase(trimOptionalWhitespace(value), "trailers"))
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::TeValueNotAllowed,
                                                       "TE 的取值 \"" + printableFieldText(value) + "\" 不是 trailers：HTTP/3 只放行这一个值（RFC 9114 §4.2）"));
            }
            return {};
        }
        // 其余连接特定字段一律禁止：h3 没有连接概念，带上会被对端判畸形
        if (isConnectionSpecificHeaderName(name))
        {
            return std::unexpected(
                    makeHeaderError(Http3HeaderErrorKind::ConnectionFieldProhibited, "头部含 HTTP/3 禁止的连接特定字段 \"" + std::string(name) + "\"（RFC 9114 §4.2）"));
        }

        // 尾段不得改变消息的长度与权威目标：content-length 与 host 只能在头段出现（RFC 9110 §6.5）。
        // 判在归位之前，否则下面两条分支会先 return 掉，这条规则就成了死代码
        if (m_isTrailersSection && (equalsIgnoringCase(name, "content-length") || equalsIgnoringCase(name, "host")))
        {
            return std::unexpected(makeHeaderError(Http3HeaderErrorKind::ProhibitedFieldInTrailers,
                                                   "尾段里出现了 \"" + std::string(name) + "\"：尾段不得携带消息长度与权威信息（RFC 9110 §6.5）"));
        }

        if (equalsIgnoringCase(name, "host"))
        {
            if (m_hasHostHeader && m_hostText != value)
            {
                // host 重复且取值不一致才判非法（RFC 9110 §7.2）：放任两份不同的 host，
                // 前端与本端可能各取一份，那就是请求走私的入口
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::AuthorityConflict, "请求里出现了取值不一致的多个 host 字段（RFC 9110 §7.2）"));
            }
            if (value.empty())
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::EmptyAuthority, "host 字段取值为空：出现就必须非空（RFC 9114 §4.3.1）"));
            }
            m_hasHostHeader = true;
            m_hostText.assign(value);
            return {};
        }

        if (equalsIgnoringCase(name, "content-length"))
        {
            std::size_t declaredLength = 0;
            // 复用 h1 侧那套解析：它会拒绝「3, 3」这类逗号拼接与非数字写法，与 RFC 9110 §8.6 同口径
            if (!parseContentLengthValue(value, declaredLength))
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::ContentLengthInvalid,
                                                       "content-length 取值 \"" + printableFieldText(value) + "\" 不是非负十进制整数（RFC 9110 §8.6）"));
            }
            if (m_contentLengthByteCount.has_value() && *m_contentLengthByteCount != declaredLength)
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::ContentLengthConflict, "出现了取值不一致的第二个 content-length（RFC 9110 §8.6）"));
            }
            m_contentLengthByteCount = declaredLength;
            return {};
        }

        // cookie 允许拆成多行（RFC 9114 §4.2.1），本判定器不合并：合并规则与 h1/h2 侧对重复字段名的
        // 处理方式必须一致，那部分归 HttpRequest::addHeader，不在协议层另开一份口径

        return {};
    }

    std::expected<void, Http3HeaderError> Http3HeaderValidator::validateFieldName(const std::string_view name) const
    {
        for (const char character: name)
        {
            if (!isTokenCharacter(static_cast<unsigned char>(character)))
            {
                return std::unexpected(
                        makeHeaderError(Http3HeaderErrorKind::IllegalFieldNameCharacter, "字段名 \"" + printableFieldText(name) + "\" 含 tchar 之外的字符（RFC 9110 §5.1）"));
            }
            // 大小写折叠只在字段名上从严：RFC 9114 §4.2 要求编码前转小写，带大写即畸形
            if (character >= 'A' && character <= 'Z')
            {
                return std::unexpected(
                        makeHeaderError(Http3HeaderErrorKind::UppercaseFieldName, "字段名 \"" + printableFieldText(name) + "\" 含大写字母：必须在编码前转为小写（RFC 9114 §4.2）"));
            }
        }
        return {};
    }

    std::expected<void, Http3HeaderError> Http3HeaderValidator::validateFieldValue(const std::string_view value) const
    {
        // CR/LF/NUL 一旦放行，字段值就能伪造出状态行或截断头部，这是响应拆分的最小形态
        if (!containsOnlyFieldValueCharacters(value))
        {
            return std::unexpected(
                    makeHeaderError(Http3HeaderErrorKind::IllegalFieldValueCharacter, "头字段取值含控制字符（RFC 9110 §5.6.2 只允许 HTAB、可见 ASCII 与 obs-text）"));
        }
        return {};
    }

    std::expected<void, Http3HeaderError> Http3HeaderValidator::endHeaderBlock() noexcept
    {
        m_isHeadSectionDone = true;
        if (m_isTrailersSection)
        {
            // 尾段没有必填项：伪头已在 onPseudoHeader 里打死，被禁字段已在 onRegularField 里打死
            m_isTrailersSection = false;
            return {};
        }

        if (m_messageKind == Http3MessageKind::Response)
        {
            if (!m_statusCode.has_value())
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::MissingPseudoHeader, "响应缺少 :status（RFC 9114 §4.3.2）"));
            }
            return {};
        }

        if (m_methodText.empty())
        {
            return std::unexpected(makeHeaderError(Http3HeaderErrorKind::MissingPseudoHeader, "请求缺少 :method（RFC 9114 §4.3.1）"));
        }

        const bool isConnectRequest  = m_methodText == "CONNECT";
        const bool isExtendedConnect = isConnectRequest && !m_protocolText.empty();
        if (isConnectRequest && !isExtendedConnect)
        {
            // 经典 CONNECT 必须省掉 :scheme 与 :path：带着就是畸形（RFC 9114 §4.4）。
            // 扩展 CONNECT（RFC 9220）反过来要有 :scheme/:path，因为隧道之上还要按路径路由
            if (!m_schemeText.empty() || !m_pathText.empty())
            {
                return std::unexpected(
                        makeHeaderError(Http3HeaderErrorKind::ProhibitedPseudoForMethod, "CONNECT 请求带了 :scheme 或 :path：经典 CONNECT 必须省略这两项（RFC 9114 §4.4）"));
            }
            if (m_authorityText.empty())
            {
                return std::unexpected(makeHeaderError(Http3HeaderErrorKind::MissingPseudoHeader, "CONNECT 请求缺少 :authority：它给出要连的目标主机与端口（RFC 9114 §4.4）"));
            }
            return {};
        }

        if (m_schemeText.empty())
        {
            return std::unexpected(makeHeaderError(Http3HeaderErrorKind::MissingPseudoHeader, "请求缺少 :scheme（RFC 9114 §4.3.1）"));
        }
        if (m_pathText.empty())
        {
            return std::unexpected(makeHeaderError(Http3HeaderErrorKind::MissingPseudoHeader, "请求缺少 :path（RFC 9114 §4.3.1）"));
        }

        // 权威来源的三条规则（RFC 9114 §4.3.1）：http/https 必须有其中一项；出现就必须非空；
        // 两项都在就得一致。非空已在各自分支判过，这里只判「有没有」与「一不一致」
        const bool schemeNeedsAuthority = equalsIgnoringCase(m_schemeText, "http") || equalsIgnoringCase(m_schemeText, "https");
        if (schemeNeedsAuthority && m_authorityText.empty() && !m_hasHostHeader)
        {
            return std::unexpected(
                    makeHeaderError(Http3HeaderErrorKind::MissingPseudoHeader, ":scheme 为 " + m_schemeText + " 的请求必须带 :authority 或 host（RFC 9114 §4.3.1）"));
        }
        if (!schemeNeedsAuthority && (!m_authorityText.empty() || m_hasHostHeader))
        {
            // 反向那条同样是 MUST NOT：目标 URI 里没有权威组件时还带 :authority，
            // 说明对端把路由信息塞进了不属于它的地方，收下只会让后续转发歧义
            return std::unexpected(makeHeaderError(Http3HeaderErrorKind::AuthorityConflict, ":scheme 为 " + m_schemeText + " 的请求不得带 :authority 或 host（RFC 9114 §4.3.1）"));
        }
        // 两项都在就得逐字一致（RFC 9114 §4.3.1）：不一致时代理与本端可能各取一份，
        // 与 h1 侧 Host/:authority 混用是同一类走私入口，所以不接受「取其中一份」的宽容做法
        if (!m_authorityText.empty() && m_hasHostHeader && m_authorityText != m_hostText)
        {
            return std::unexpected(makeHeaderError(Http3HeaderErrorKind::AuthorityConflict, ":authority 与 host 同时出现且取值不一致（RFC 9114 §4.3.1）"));
        }
        return {};
    }

    const std::string &Http3HeaderValidator::methodText() const noexcept
    {
        return m_methodText;
    }
    const std::string &Http3HeaderValidator::pathText() const noexcept
    {
        return m_pathText;
    }
    const std::string &Http3HeaderValidator::authorityText() const noexcept
    {
        return m_authorityText;
    }
    const std::string &Http3HeaderValidator::schemeText() const noexcept
    {
        return m_schemeText;
    }
    const std::string &Http3HeaderValidator::protocolText() const noexcept
    {
        return m_protocolText;
    }
    std::optional<int> Http3HeaderValidator::statusCode() const noexcept
    {
        return m_statusCode;
    }
    bool Http3HeaderValidator::hasHostHeader() const noexcept
    {
        return m_hasHostHeader;
    }
    std::optional<std::uint64_t> Http3HeaderValidator::contentLengthByteCount() const noexcept
    {
        return m_contentLengthByteCount;
    }
} // namespace AsynGyanis::Net

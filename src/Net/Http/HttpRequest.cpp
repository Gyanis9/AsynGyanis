#include "Net/Http/HttpRequest.h"

#include "Net/Http/HttpHeaderFieldStore.h"
#include "Net/Http/HttpHeaderRules.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    bool isContinueExpected(const std::string_view expectHeaderValue) noexcept
    {
        // RFC 9110 §10.1.1 只定义了一个期望值 100-continue；取值是逗号分隔的 token 列表，大小写不敏感
        constexpr std::string_view kContinueToken = "100-continue";
        std::size_t                tokenStart     = 0;
        while (tokenStart <= expectHeaderValue.size())
        {
            const std::size_t commaIndex = expectHeaderValue.find(',', tokenStart);
            const std::size_t tokenEnd   = commaIndex == std::string_view::npos ? expectHeaderValue.size() : commaIndex;

            // 逐个 token 去掉首尾空白（OWS 只可能是 SP / HTAB）后做大小写不敏感的全等比较
            if (equalsIgnoringCase(trimOptionalWhitespace(expectHeaderValue.substr(tokenStart, tokenEnd - tokenStart)), kContinueToken))
            {
                return true;
            }

            if (commaIndex == std::string_view::npos)
            {
                break;
            }
            tokenStart = commaIndex + 1;
        }
        return false;
    }

    namespace
    {
        // 一个完整的百分号转义序列形如 "%XY"，占 3 个字符
        constexpr std::size_t kPercentEscapeSequenceLength = 3;
    } // namespace

    HttpMethod HttpRequest::methodFromString(const std::string_view method)
    {
        // 只认报文上的大写原文：请求行里的方法本身就是区分大小写的 token，
        // 这里再做一次小写兼容反而会掩盖客户端的错误拼写，不如落到 UNKNOWN 让上层看见
        if (method == "GET")
            return HttpMethod::GET;
        if (method == "POST")
            return HttpMethod::POST;
        if (method == "PUT")
            return HttpMethod::PUT;
        if (method == "DELETE")
            return HttpMethod::DELETE;
        if (method == "PATCH")
            return HttpMethod::PATCH;
        if (method == "HEAD")
            return HttpMethod::HEAD;
        if (method == "OPTIONS")
            return HttpMethod::OPTIONS;
        return HttpMethod::UNKNOWN;
    }

    void HttpRequest::setMethod(const HttpMethod method)
    {
        m_method = method;
    }

    HttpMethod HttpRequest::method() const
    {
        return m_method;
    }

    void HttpRequest::setUri(std::string uri)
    {
        // 入参按值接收，所有权直接接管，避免再拷一份；调用方留下的是一次空壳移动
        m_uri = std::move(uri);
    }

    void HttpRequest::adoptStagedUri(std::string &stagedUri) noexcept
    {
        // 交换而不是移动：解析器接手本请求刚清空的那份缓冲，两边容量都留着给下一条报文用
        m_uri.swap(stagedUri);
    }

    const std::string &HttpRequest::uri() const
    {
        return m_uri;
    }

    void HttpRequest::setHttpVersion(std::string version)
    {
        m_httpVersion = std::move(version);
    }

    const std::string &HttpRequest::httpVersion() const
    {
        return m_httpVersion;
    }

    void HttpRequest::addHeader(const std::string_view key, const std::string_view value)
    {
        // 权威记录：线上每出现一条头部就原样留一档，可重复头部互不覆盖，顺序即到达顺序；
        // 单值视图只标脏，等真有人查询时再一次性建出来（多数请求路径从不查询它）
        m_headerStore.append(key, value);
    }

    void HttpRequest::addTrailerField(const std::string_view name, const std::string_view value)
    {
        // 第一条 trailer 才把存储建出来：不带尾部的请求（绝大多数）一次分配也不付
        if (!m_trailerStore.has_value())
        {
            m_trailerStore.emplace();
        }
        m_trailerStore->append(name, value);
    }

    std::optional<std::string> HttpRequest::getTrailerField(const std::string_view name) const
    {
        if (!m_trailerStore.has_value())
        {
            return std::nullopt;
        }
        return m_trailerStore->get(name);
    }

    bool HttpRequest::setHeader(const std::string_view key, const std::string_view value)
    {
        // 判据与 HttpResponse::setHeader 同一张表、同一条口径：名与值不合法就拒写且不动任何已有状态。
        // 两侧共用判据不是为了少写几行，而是为了让同一段头部在「收进来」与「发出去」之间不分裂
        if (!isValidHeaderFieldName(key) || !containsOnlyFieldValueCharacters(value))
        {
            return false;
        }

        if (HttpHeaderFieldStore::isRepeatableHeaderName(key))
        {
            // 可重复头部的 set 退化为追加一条，与响应侧同语义：覆盖式写法会静默丢掉前面的条目
            m_headerStore.append(key, value);
            return true;
        }

        m_headerStore.overwriteOrAppend(key, value);
        return true;
    }

    void HttpRequest::adoptStagedHeaders(HttpHeaderFieldStore &stagedHeaders) noexcept
    {
        m_headerStore.adoptFrom(stagedHeaders);
    }

    void HttpRequest::reserveHeaders(const std::size_t fieldCount, const std::size_t byteCount)
    {
        m_headerStore.reserve(fieldCount, byteCount);
    }

    std::optional<std::string> HttpRequest::getHeader(const std::string_view key) const
    {
        return m_headerStore.get(key);
    }

    std::optional<std::string> HttpRequest::firstHeaderValue(const std::string_view key) const
    {
        // 存储侧就地做大小写不敏感比较，这里不再归一化：长头部名不必为此现造一个副本
        return m_headerStore.firstValue(key);
    }

    std::optional<std::string_view> HttpRequest::firstHeaderValueView(const std::string_view key) const
    {
        return m_headerStore.firstValueView(key);
    }

    bool HttpRequest::hasHeader(const std::string_view key) const
    {
        return m_headerStore.contains(key);
    }

    std::size_t HttpRequest::headerFieldCount(const std::string_view key) const
    {
        return m_headerStore.countOf(key);
    }

    bool HttpRequest::hasHeaderValueToken(const std::string_view key, const std::string_view expectedToken) const
    {
        return m_headerStore.containsListToken(key, expectedToken);
    }

    std::vector<std::string> HttpRequest::headerValues(const std::string_view key) const
    {
        return m_headerStore.values(key);
    }

    const std::unordered_map<std::string, std::string> &HttpRequest::headers() const
    {
        return m_headerStore.singleValueView();
    }

    void HttpRequest::setBody(std::string body)
    {
        m_body = std::move(body);
    }

    void HttpRequest::adoptStagedBody(std::string &stagedBody) noexcept
    {
        // 覆盖语义与 setBody 一致（旧正文作废），区别只在两条缓冲交换：换出去的旧内容由调用方清零，
        // 两侧容量都留下来供下一条报文复用
        m_body.swap(stagedBody);
    }

    void HttpRequest::appendBody(const char *data, const size_t length)
    {
        // 直连追加，不做上限判断：字节来自 llhttp 的正文回调，必然非空；
        // 请求体的资源上限由 HttpParser 把守，这里再判一次只会白读一次长度
        m_body.append(data, length);
    }

    std::string_view HttpRequest::body() const
    {
        return m_body;
    }

    HttpRequestBody *HttpRequest::bodyStream() const noexcept
    {
        return m_bodyStream;
    }

    void HttpRequest::setBodyStream(HttpRequestBody *bodyStream) noexcept
    {
        m_bodyStream = bodyStream;
    }

    void HttpRequest::setRequestId(const std::string_view requestId)
    {
        // 原地写入而不是接管一个现造的串：请求对象按连接复用时容量留着，稳态一次堆分配也不碰
        m_requestId.assign(requestId.data(), requestId.size());
    }

    std::string_view HttpRequest::requestId() const noexcept
    {
        return m_requestId;
    }

    std::string_view HttpRequest::path() const
    {
        // 路径与查询串以第一个 '?' 为界；'?' 之前一律算路径，即使里面还有 '?' 也不切开。
        // 返回视图，指向本对象持有的 URI——调用方在请求对象存活期间内使用即可
        const std::string_view uriView(m_uri);
        if (const std::size_t queryPosition = uriView.find('?'); queryPosition != std::string_view::npos)
        {
            return uriView.substr(0, queryPosition);
        }
        return uriView;
    }

    std::string HttpRequest::percentDecode(const std::string_view source)
    {
        std::string decoded;
        // 解码后的长度不可能超过原文（"%41" 三字节换成一字节），reserve 取原文长度即够
        decoded.reserve(source.size());

        for (std::size_t index = 0; index < source.size(); ++index)
        {
            const char currentCharacter = source[index];

            // '+' 表示空格是 application/x-www-form-urlencoded 的既有约定，
            // 只在查询串解码里成立，故本函数只服务于 queryParams()
            if (currentCharacter == '+')
            {
                decoded.push_back(' ');
                continue;
            }

            if (currentCharacter == '%')
            {
                // 界内判据写成「剩余长度至少容得下一个完整转义序列」，
                // 一眼可核对：正好覆盖以 "%41" 结尾、最后三字节即一个序列的情形
                const std::size_t remainingLength = source.size() - index;
                if (remainingLength >= kPercentEscapeSequenceLength)
                {
                    const int highDigitValue = hexadecimalDigitValue(source[index + 1]);
                    const int lowDigitValue  = hexadecimalDigitValue(source[index + 2]);

                    // 两位都必须是合法十六进制，否则整个序列作废
                    if (highDigitValue >= 0 && lowDigitValue >= 0)
                    {
                        // 高四位左移后并上低四位，还原成一个字节；%00 这类空字节照样解出
                        decoded.push_back(static_cast<char>((highDigitValue << 4) | lowDigitValue));
                        // 本次迭代已吃掉两个跟随字符，循环再自增一次正好跨过整个序列
                        index += 2;
                        continue;
                    }
                }

                // 走到这里说明序列残缺或非法（"%%"、"%4"、"%ZZ"）：
                // 百分号按原文保留，其后的字符留给下一轮照常处理，既不吞字符也不报错
            }

            decoded.push_back(currentCharacter);
        }

        return decoded;
    }

    std::unordered_map<std::string, std::string> HttpRequest::queryParams() const
    {
        std::unordered_map<std::string, std::string> parameters;

        const std::size_t queryPosition = m_uri.find('?');
        if (queryPosition == std::string::npos)
        {
            // 没有 '?' 就没有查询串，交回空表而不是抛异常：路径查询是常规操作
            return parameters;
        }

        // '?' 之后的全部字符即查询串；URI 以 '?' 收尾时这里得到空视图，substr 不会越界
        const std::string_view queryText = std::string_view(m_uri).substr(queryPosition + 1);

        std::size_t pairStart = 0;
        while (pairStart < queryText.size())
        {
            // 先定本分对的右边界：'&' 之后的字节属于下一个分对，
            // 因此不会出现「把 a&b=c 里的 '=' 当成 a 的分隔符」那类串扰
            std::size_t pairEnd = queryText.find('&', pairStart);
            if (pairEnd == std::string_view::npos)
            {
                pairEnd = queryText.size();
            }

            const std::string_view currentPair = queryText.substr(pairStart, pairEnd - pairStart);
            // 步进跨过 '&'；pairEnd 已是末尾时 pairEnd + 1 大于 size，while 条件自然收束
            pairStart = pairEnd + 1;

            // 连续 '&&'、首尾 '&' 都会切出空分对，没有键也没有值，直接丢弃
            if (currentPair.empty())
            {
                continue;
            }

            // 只按第一个 '=' 切分：键侧不可能含 '='（'=' 是分隔符），
            // 值侧剩下的 '='（如 ?q=a=b）都是值的组成部分，不该再切
            std::string_view keyText = currentPair;
            std::string_view valueText;
            if (const std::size_t separatorPosition = currentPair.find('='); separatorPosition != std::string_view::npos)
            {
                keyText   = currentPair.substr(0, separatorPosition);
                valueText = currentPair.substr(separatorPosition + 1);
            }

            // 无 '=' 的分对（"?flag"）走上面这条路：valueText 保持默认空串，即「有键无值」
            if (keyText.empty())
            {
                // "?=v" 这种只有值没有键的分对没有可寻址意义，跳过以免污染出空键
                continue;
            }

            // 重复键后者覆盖前者：返回类型是 map，只能表达一键一值；
            // 需要全部值时调用方自行处理原始 URI
            parameters[percentDecode(keyText)] = percentDecode(valueText);
        }

        return parameters;
    }

    void HttpRequest::setParam(std::string key, std::string value)
    {
        m_params[std::move(key)] = std::move(value);
    }

    std::optional<std::string> HttpRequest::param(const std::string &key) const
    {
        // 路由参数名直接来自路由模板（":id" 里的 id），大小写敏感，故不做归一化
        if (const auto iterator = m_params.find(key); iterator != m_params.end())
        {
            return iterator->second;
        }
        return std::nullopt;
    }

    void HttpRequest::reset()
    {
        m_method = HttpMethod::UNKNOWN;
        m_uri.clear();
        m_httpVersion.clear();
        m_headerStore.clear();
        m_body.clear();
        // request-id 必须跟着清：它是上一条报文的身份，留着会让下一条报文冒用别人的标识
        m_requestId.clear();
        m_params.clear();

        // trailer 那一档整份撤走而不是清空：hasTrailerFields() 读的就是「有没有这一档」，
        // 留一份空存储会让下一条请求谎称自己带过尾部字段
        m_trailerStore.reset();

        // 取消源：只有**被触发过**才重建。触发过的源会让下一条请求一进来就是「已取消」，
        // 必须换掉；没触发过的直接沿用，省掉每请求一次停止状态的分配。
        // 沿用是安全的：取消源只被本次请求的中间件与处理函数短暂引用（它们随请求结束一并析构），
        // 因此复用等价于「换一个全新的源」——除非调用方把令牌留到了下一条请求（那是误用）
        if (m_cancelSource.stop_requested())
        {
            m_cancelSource = std::stop_source{};
        }
    }

    std::stop_token HttpRequest::cancelToken() const noexcept
    {
        return m_cancelSource.get_token();
    }

    std::stop_source &HttpRequest::cancelSource() noexcept
    {
        return m_cancelSource;
    }

    bool HttpRequest::requestCancel() const
    {
        // 幂等：已经取消过时返回 false，调用方据此区分「本次是首次取消」还是「重复取消」
        return m_cancelSource.request_stop();
    }

} // namespace AsynGyanis::Net

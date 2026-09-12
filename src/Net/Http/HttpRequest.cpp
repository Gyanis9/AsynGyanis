#include "Net/Http/HttpRequest.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
        // 同名普通头部合并时的分隔符，与 RFC 7230 §3.2.2 给出的字段值列表形式一致
        constexpr std::string_view kMergedHeaderSeparator = ", ";

        // 允许在同一报文里出现多条、且不得逗号合并的头部名单（已归一化为小写）。
        // 目前只有 set-cookie：RFC 6265 规定多条 Set-Cookie 各表达一个独立 cookie，
        // 而 cookie 值本身可以含逗号，一旦合并就再也切不回去。
        // 后续要支持 www-authenticate、link 这类同样可多条的头部，在此扩充即可。
        constexpr std::array<std::string_view, 1> kRepeatableHeaderNames{"set-cookie"};

        // 一个完整的百分号转义序列形如 "%XY"，占 3 个字符
        constexpr std::size_t kPercentEscapeSequenceLength = 3;

        /**
         * @brief 取十六进制字符对应的数值
         * @param character 待判定的字符
         * @return 0~15 的数值；不是十六进制字符时返回 -1 作为「非十六进制」标记
         */
        int hexadecimalDigitValue(const char character) noexcept
        {
            if (character >= '0' && character <= '9')
            {
                return character - '0';
            }
            if (character >= 'A' && character <= 'F')
            {
                return character - 'A' + 10;
            }
            if (character >= 'a' && character <= 'f')
            {
                return character - 'a' + 10;
            }
            return -1;
        }
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

    void HttpRequest::lowercaseInPlace(std::string &name)
    {
        // 逐字符查 ASCII 表，不用 std::tolower(char)：char 可能是负值，
        // 直接传进 <cctype> 是未定义行为，必须先转 unsigned char 再转回 char
        std::ranges::transform(name, name.begin(),
                               [](const unsigned char character)
                               {
                                   return static_cast<char>(std::tolower(character));
                               });
    }

    std::string HttpRequest::toCanonicalHeaderName(const std::string_view name)
    {
        std::string canonicalName(name);
        lowercaseInPlace(canonicalName);
        return canonicalName;
    }

    bool HttpRequest::isRepeatableHeaderName(const std::string_view canonicalName)
    {
        // 名单极小，线性比较比构造哈希集合划算
        return std::ranges::find(kRepeatableHeaderNames, canonicalName) != kRepeatableHeaderNames.end();
    }

    void HttpRequest::addHeader(std::string key, std::string value)
    {
        // HTTP 头部名大小写不敏感（RFC 9110 §5.1）：统一转小写入库，
        // 于是 Content-Type 与 content-type 命中同一条。入参本来就是调用方交出的副本，
        // 就地改写比再造一个字符串省一次分配——解析器每条头部都会走这个函数
        lowercaseInPlace(key);

        // 权威记录：线上每出现一条头部就原样留一档，可重复头部互不覆盖，顺序即到达顺序。
        // 单值视图不在这里维护：合并逻辑只有 rebuildSingleValueView() 一处实现，
        // 等真有人查询时再一次性建出来（多数请求路径从不查询它）
        m_headerFields.push_back(HeaderField{.name = key, .value = std::move(value)});
        m_isSingleValueViewStale = true;
    }

    void HttpRequest::rebuildSingleValueView() const
    {
        m_headers.clear();
        for (const HeaderField &field: m_headerFields)
        {
            if (isRepeatableHeaderName(field.name))
            {
                // 单值视图只保留首条，其余靠 headerValues() 逐条取；
                // try_emplace 而非 insert_or_assign，正是为了「后来的不覆盖首条」
                m_headers.try_emplace(field.name, field.value);
                continue;
            }

            // 普通头部同名多条时，按 RFC 7230 §3.2.2 的收件人规则以 ", " 合并到同一条，
            // 视图里的条目位置与键都不变——旧实现在这里造 set-cookie_1 之类的伪键，已废除
            if (const auto [iterator, isInserted] = m_headers.try_emplace(field.name, field.value); !isInserted)
            {
                iterator->second.append(kMergedHeaderSeparator);
                iterator->second.append(field.value);
            }
        }
        m_isSingleValueViewStale = false;
    }

    std::optional<std::string> HttpRequest::getHeader(const std::string &key) const
    {
        // 查询前先把过期视图重建出来：新增头部会把视图标脏，这里一次性补齐
        if (m_isSingleValueViewStale)
        {
            rebuildSingleValueView();
        }

        // 查询侧走同一套归一化规则，保证写入与读取对键的认定一致
        if (const auto iterator = m_headers.find(toCanonicalHeaderName(key)); iterator != m_headers.end())
        {
            return iterator->second;
        }
        // 未命中不是错误：可选头部缺席是常态，交给调用方用 optional 判定
        return std::nullopt;
    }

    std::vector<std::string> HttpRequest::headerValues(const std::string &key) const
    {
        const std::string canonicalName = toCanonicalHeaderName(key);

        std::vector<std::string> values;
        // 按线上到达顺序收集，读到的顺序与客户端发出的顺序一致
        for (const HeaderField &field : m_headerFields)
        {
            if (field.name == canonicalName)
            {
                values.push_back(field.value);
            }
        }
        return values;
    }

    const std::unordered_map<std::string, std::string> &HttpRequest::headers() const
    {
        // 同 getHeader：查询前先把过期视图重建出来
        if (m_isSingleValueViewStale)
        {
            rebuildSingleValueView();
        }
        return m_headers;
    }

    void HttpRequest::setBody(std::string body)
    {
        m_body = std::move(body);
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

    std::string HttpRequest::path() const
    {
        // 路径与查询串以第一个 '?' 为界；'?' 之前一律算路径，即使里面还有 '?' 也不切开
        if (const std::size_t queryPosition = m_uri.find('?'); queryPosition != std::string::npos)
        {
            return m_uri.substr(0, queryPosition);
        }
        return m_uri;
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
            std::string_view keyText   = currentPair;
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
        m_method      = HttpMethod::UNKNOWN;
        m_uri.clear();
        m_httpVersion.clear();
        m_headerFields.clear();
        m_headers.clear();
        m_isSingleValueViewStale = true;
        m_body.clear();
        m_params.clear();

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

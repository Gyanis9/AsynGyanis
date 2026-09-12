#include "Net/Http/HttpParser.h"

#include <array>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace AsynGyanis::Net
{
    HttpParser::HttpParser()
    {
        // 设置表是 llhttp_init 的必填参数，且其生命周期必须不短于解析器：
        // 这里把它按值存成成员，与解析器同生同死，不留悬垂设置的隐患
        llhttp_settings_init(&m_settings);

        // 回调类型是 C 函数指针，不能捕获 this，因此全部挂 static 成员函数，
        // 宿主对象经下面构造末尾的 m_parser.data 取回
        m_settings.on_message_begin         = onMessageBegin;
        m_settings.on_url                   = onUrl;
        m_settings.on_header_field          = onHeaderField;
        m_settings.on_header_value          = onHeaderValue;
        m_settings.on_header_value_complete = onHeaderValueComplete;
        m_settings.on_body                  = onBody;
        m_settings.on_message_complete      = onMessageComplete;

        llhttp_init(&m_parser, HTTP_REQUEST, &m_settings);
        m_parser.data = this;
    }

    HttpParser::~HttpParser() = default;

    ParseStatus HttpParser::parse(const char *data, const size_t length)
    {
        // 已经收齐就一个字节都不再吃：这些字节属于流水线里的下一条报文，
        // 喂进已完成的解析器会被当作新报文的开头，把上一条已定稿的结果改掉
        if (m_isComplete)
        {
            return ParseStatus::Done;
        }

        // 完成标记优先于本次返回码：极端情况下同一次调用里既触发了完成又撞上错误，
        // 此时上一条报文是完整的，按 Done 交给上层。至于缓冲区里排在它后面的剩余字节，
        // 本次调用一并不再消费也就此作废——本解析器不回报已消耗字节数，
        // 流水线续读得靠上层自己留缓冲（见 onMessageBegin() 的守卫）
        const llhttp_errno_t executionResult = llhttp_execute(&m_parser, data, length);

        if (m_isComplete)
        {
            return ParseStatus::Done;
        }

        // HPE_OK 只说明「本段字节合法、状态机还没走完」，不代表结束，因此落到 NeedMore
        if (executionResult == HPE_OK)
        {
            return ParseStatus::NeedMore;
        }

        // 其余返回码一律判错。llhttp 的错误是粘滞的：非暂停类错误一旦出现，
        // 在重新 llhttp_init 之前每次 execute 都会返回同一个错误码，
        // 所以调用方拿到 Error 后要么 reset() 要么断开连接，不能继续喂数据
        recordParseError(executionResult);
        return ParseStatus::Error;
    }

    void HttpParser::reset()
    {
        m_currentRequest.reset();
        clearMessageScratch();
        m_hasError        = false;
        m_isComplete      = false;
        m_isLimitExceeded = false;
        m_errorMessage.clear();

        // 用 llhttp_init 而不是 llhttp_reset：两者都能清掉粘滞错误，
        // 而 init 会把整个结构清零，其中也包括用户数据指针 data，
        // 所以必须紧接着重新绑定 this，否则回调里的宿主指针就成了空指针
        llhttp_init(&m_parser, HTTP_REQUEST, &m_settings);
        m_parser.data = this;
    }

    HttpRequest &HttpParser::request()
    {
        return m_currentRequest;
    }

    bool HttpParser::hasError() const
    {
        return m_hasError;
    }

    bool HttpParser::isLimitExceeded() const
    {
        return m_isLimitExceeded;
    }

    std::string HttpParser::errorMessage() const
    {
        return m_errorMessage;
    }

    void HttpParser::clearMessageScratch() noexcept
    {
        m_currentUrl.clear();
        m_currentHeaderField.clear();
        m_currentHeaderValue.clear();
        m_headerFieldCount  = 0;
        m_headerBlockLength = 0;
    }

    HttpParser *HttpParser::ownerFrom(llhttp_t *parser) noexcept
    {
        // data 由构造与 reset() 在 llhttp_init 之后重新绑定，回调触发时必然有效；
        // 这里只做 C 指针到宿主类型的转换，不做二次校验——校验失败也没有比崩溃更合适的处理
        return static_cast<HttpParser *>(parser->data);
    }

    void HttpParser::recordResourceLimitExceeded(llhttp_t *parser, const char *const llhttpReason, std::string detailMessage)
    {
        m_isLimitExceeded = true;
        m_hasError        = true;

        // 中文详情带具体上限数值，是 errorMessage() 的实际出口
        m_errorMessage = std::move(detailMessage);

        // llhttp 只保存 reason 指针、不拷贝内容，所以传进去的必须是静态期生命周期的字面量；
        // 上面那份中文详情绝不能以临时串 c_str() 的形式交给它，那会在 execute() 返回后变成悬垂指针
        llhttp_set_error_reason(parser, llhttpReason);
    }

    void HttpParser::recordParseError(const llhttp_errno_t executionResult)
    {
        m_hasError = true;

        // 超限路径已在回调里写好带数值的中文详情，不能被这里的通用文案覆盖
        if (m_isLimitExceeded && !m_errorMessage.empty())
        {
            return;
        }

        // llhttp 的 reason 是英文原文且指向内部存储，立刻拷进 std::string 再包一层中文外壳，
        // 指针本身不留存
        if (m_parser.reason != nullptr && m_parser.reason[0] != '\0')
        {
            m_errorMessage = std::format("HTTP 报文解析失败：{}（错误码 {}）", m_parser.reason, llhttp_errno_name(executionResult));
            return;
        }

        // 少数底层错误不会填 reason，留一条兜底文本，免得调用方只看到一个光秃秃的错误码
        m_errorMessage = std::format("HTTP 报文解析失败，未给出原因（错误码 {}）", llhttp_errno_name(executionResult));
    }

    int HttpParser::onMessageBegin(llhttp_t *parser)
    {
        HttpParser *const owner = ownerFrom(parser);

        // 流水线守卫：完成标记已置位说明本对象已收齐一条报文，状态机却又开始解析下一条，
        // 即上层还没来得及 reset()。此刻请求对象仍带着上一条的头部与正文，
        // 再往里填就是把两条报文串成一条。返回 -1 让 llhttp 立刻中止本次 execute，
        // 越界的剩余字节随之作废。这不会被误报成解析错误：parse() 先检查完成标记，
        // 只要报文收过一条就直接返回 Done，压根走不到错误映射
        if (owner->m_isComplete)
        {
            // 只登记 llhttp 侧的英文原因，本对象的中文错误文本保持不动；
            // llhttp 只存指针不拷贝内容，所以这里必须传静态字面量
            llhttp_set_error_reason(parser, "a new message started before the previous one was reset");
            return -1;
        }

        // 新报文起始就把上一条留下的暂存与计数归零：与其依赖上层记得 reset()，
        // 不如在协议给出的这个天然分界点上自清，这样才不会串数据
        owner->clearMessageScratch();
        return 0;
    }

    int HttpParser::onUrl(llhttp_t *parser, const char *data, const size_t length)
    {
        HttpParser *const owner = ownerFrom(parser);

        // URI 可能跨多次回调分片到达（分片边界由输入缓冲区决定），
        // 所以上限按「已累积 + 本次」的总量判定；只卡单次长度会漏过由许多小片拼出的超长 URI
        if (owner->m_currentUrl.size() + length > kMaximumUriLength)
        {
            owner->recordResourceLimitExceeded(parser, "request URI exceeds maximum allowed length",
                                               std::format("请求 URI 超出上限 {} 字节", kMaximumUriLength));
            // 返回 HPE_USER（非 0）让 llhttp 立刻停止解析，本段剩余字节不再被消费
            return HPE_USER;
        }

        owner->m_currentUrl.append(data, length);
        return 0;
    }

    int HttpParser::onHeaderField(llhttp_t *parser, const char *data, const size_t length)
    {
        HttpParser *const owner = ownerFrom(parser);

        // 头部名同样按总量卡上限。旧实现这里完全没有上限：
        // 一条无限延长的头部名就能让 append 不停扩容，把整条连接的内存吃干净
        if (owner->m_currentHeaderField.size() + length > kMaximumHeaderFieldNameLength)
        {
            owner->recordResourceLimitExceeded(parser, "request header field name exceeds maximum allowed length",
                                               std::format("请求头部名超出上限 {} 字节", kMaximumHeaderFieldNameLength));
            return HPE_USER;
        }

        // 头部块总长是第二道闸：单条名与值各自合规，架不住上百条头部叠出来的总量，
        // 因此按「名 + 值」的净字节累计再判一次。判定放在落账之前，拒绝路径不留半成品
        if (owner->m_headerBlockLength + length > kMaximumHeaderBlockLength)
        {
            owner->recordResourceLimitExceeded(parser, "request header block exceeds maximum allowed length",
                                               std::format("请求头部总长超出上限 {} 字节", kMaximumHeaderBlockLength));
            return HPE_USER;
        }

        owner->m_headerBlockLength += length;
        owner->m_currentHeaderField.append(data, length);
        return 0;
    }

    int HttpParser::onHeaderValue(llhttp_t *parser, const char *data, const size_t length)
    {
        HttpParser *const owner = ownerFrom(parser);

        // 头部值的上限判定与头部名同理：跨片累积后按总量卡，超限立刻中止而不是截断保存
        if (owner->m_currentHeaderValue.size() + length > kMaximumHeaderFieldValueLength)
        {
            owner->recordResourceLimitExceeded(parser, "request header field value exceeds maximum allowed length",
                                               std::format("请求头部值超出上限 {} 字节", kMaximumHeaderFieldValueLength));
            return HPE_USER;
        }

        if (owner->m_headerBlockLength + length > kMaximumHeaderBlockLength)
        {
            owner->recordResourceLimitExceeded(parser, "request header block exceeds maximum allowed length",
                                               std::format("请求头部总长超出上限 {} 字节", kMaximumHeaderBlockLength));
            return HPE_USER;
        }

        owner->m_headerBlockLength += length;
        owner->m_currentHeaderValue.append(data, length);

        // 注意：这里不入库。值也可能被拆成多片，落库统一推迟到 on_header_value_complete，
        // 否则一条被拆成两片的头部会生成两条错误记录
        return 0;
    }

    int HttpParser::onHeaderValueComplete(llhttp_t *parser)
    {
        HttpParser *const owner = ownerFrom(parser);

        // 条数上限：每条头部都要在有序记录与单值视图里各占一份，
        // 海量空值头部同样是内存放大，所以给一条与长度上限相互独立的硬上限
        if (owner->m_headerFieldCount >= kMaximumHeaderCount)
        {
            owner->recordResourceLimitExceeded(parser, "request header count exceeds maximum allowed limit",
                                               std::format("请求头部条数超出上限 {} 条", kMaximumHeaderCount));
            return HPE_USER;
        }

        // 到这里才拿到一条完整头部：名与值都已收齐，交给请求对象决定合并还是逐条保存
        //（可重复头部多条并存，普通头部按 ", " 合并，见 HttpRequest::addHeader）
        // 两条暂存把所有权交出去，随后立刻 clear 回确定的空串状态供下一条复用
        owner->m_currentRequest.addHeader(std::move(owner->m_currentHeaderField), std::move(owner->m_currentHeaderValue));
        owner->m_currentHeaderField.clear();
        owner->m_currentHeaderValue.clear();
        ++owner->m_headerFieldCount;
        return 0;
    }

    int HttpParser::onBody(llhttp_t *parser, const char *data, const size_t length)
    {
        HttpParser *const owner = ownerFrom(parser);

        // 正文按「已收 + 本次」的总量卡上限。正文是分片追加的，
        // 单看 Content-Length 头不足以设防：声明 1 字节然后狂发数据同样能撑爆内存
        if (owner->m_currentRequest.body().size() + length > kMaximumBodySize)
        {
            owner->recordResourceLimitExceeded(parser, "request body exceeds maximum allowed size",
                                               std::format("请求体超出上限 {} 字节", kMaximumBodySize));
            return HPE_USER;
        }

        owner->m_currentRequest.appendBody(data, length);
        return 0;
    }

    int HttpParser::onMessageComplete(llhttp_t *parser)
    {
        HttpParser *const owner = ownerFrom(parser);

        // 方法：llhttp 的 method 字段是 uint8_t，取值与 enum llhttp_method 一一对应。
        // 这里只映射框架支持的 7 个方法，其余（CONNECT、TRACE、M-SEARCH 等）统一落到 UNKNOWN
        HttpMethod currentMethod = HttpMethod::UNKNOWN;
        switch (parser->method)
        {
            case HTTP_GET:
                currentMethod = HttpMethod::GET;
                break;
            case HTTP_POST:
                currentMethod = HttpMethod::POST;
                break;
            case HTTP_PUT:
                currentMethod = HttpMethod::PUT;
                break;
            case HTTP_DELETE:
                currentMethod = HttpMethod::DELETE;
                break;
            case HTTP_PATCH:
                currentMethod = HttpMethod::PATCH;
                break;
            case HTTP_HEAD:
                currentMethod = HttpMethod::HEAD;
                break;
            case HTTP_OPTIONS:
                currentMethod = HttpMethod::OPTIONS;
                break;
            default:
                currentMethod = HttpMethod::UNKNOWN;
                break;
        }

        owner->m_currentRequest.setMethod(currentMethod);
        owner->m_currentRequest.setUri(std::move(owner->m_currentUrl));

        // 版本：两代主版本 × 四个次版本共 8 个组合按行主序摊平在静态表里查表，
        // 省掉每条请求一次 std::format 的开销；表外版本退回拼接
        static constexpr std::size_t kKnownMajorVersionCount = 2;
        static constexpr std::size_t kKnownMinorVersionCount = 4;
        static constexpr std::array<std::string_view, kKnownMajorVersionCount * kKnownMinorVersionCount> kHttpVersionTexts{
                "HTTP/0.9", "HTTP/0.1", "HTTP/0.2", "HTTP/0.3",
                "HTTP/1.0", "HTTP/1.1", "HTTP/1.2", "HTTP/1.3"};

        // 显式抬到 unsigned int：uint8_t 实际是 unsigned char，
        // 直接交给 std::format 在部分实现上会按字符而非数字处理
        const unsigned int majorVersion = parser->http_major;
        const unsigned int minorVersion = parser->http_minor;
        if (majorVersion < kKnownMajorVersionCount && minorVersion < kKnownMinorVersionCount)
        {
            // string_view 转 std::string：8 字节走短字符串优化，不产生堆分配
            owner->m_currentRequest.setHttpVersion(std::string(kHttpVersionTexts[majorVersion * kKnownMinorVersionCount + minorVersion]));
        }
        else
        {
            owner->m_currentRequest.setHttpVersion(std::format("HTTP/{}.{}", majorVersion, minorVersion));
        }

        // 完成标记是 parse() 判定 Done 的唯一依据：置位之后即便 llhttp 接着报错，
        // parse() 也先按完成处理（完成判定优先于返回码）
        owner->m_isComplete = true;

        // 返回 0 表示让状态机继续消费剩余字节。这里不用「回调返回 HPE_PAUSED」来止步：
        // 暂停在不同 llhttp 版本上的处理路径并不一致，拿它当流控手段风险过高。
        // 客户端流水线带来的第二条报文由 onMessageBegin() 的守卫拦下，
        // 不会把下一条的头部与正文混进这一条尚未复位的请求里
        return 0;
    }

} // namespace AsynGyanis::Net

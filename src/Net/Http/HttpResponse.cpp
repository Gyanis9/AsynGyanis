#include "Net/Http/HttpResponse.h"

#include "Net/Http/HttpDate.h"
#include "Net/Http/HttpHeaderFieldStore.h"
#include "Net/Http/HttpHeaderRules.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
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
        constexpr std::string_view kTransferEncodingHeaderName = "transfer-encoding"; ///< 传输编码头部名（小写形态）
        constexpr std::string_view kChunkedTransferEncodingValue = "chunked";   ///< 分块传输编码值：正文长度未知，边界由分块帧给出（RFC 9112 §6）
        constexpr std::size_t kMaximumChunkLengthHexDigits = sizeof(std::size_t) * 2; ///< 分块长度行的十六进制位数上限（每字节 2 位）
        constexpr std::string_view kAutoContentTypeHeader = "content-type: text/plain\r\n";      ///< 未设媒体类型且有正文时补出的整条头部
        constexpr std::string_view kAutoContentLengthHeaderPrefix = "content-length: ";          ///< 未设正文长度时补出的头部名前缀（含冒号与空格）
        constexpr std::string_view kAutoDateHeaderPrefix = "date: ";                             ///< 未设日期时补出的头部名前缀（含冒号与空格）

        constexpr std::size_t kMaximumUnsignedDecimalTextLength = std::numeric_limits<std::uint64_t>::max_digits10; ///< 64 位无符号十进制最长 20 位；有符号 int 在 appendDecimal 里按 max_digits10 + 2（含负号位）同理推导

        constexpr std::size_t kAutoContentTypeReserveLength = kAutoContentTypeHeader.size();     ///< "content-type: text/plain\r\n" 的实际字节数 = 26
        constexpr std::size_t kAutoContentLengthReserveLength =
                kAutoContentLengthHeaderPrefix.size() + kMaximumUnsignedDecimalTextLength + kCrLfLength; ///< 前缀 16 + 最多 20 位数字 + CRLF 2 = 38
        constexpr std::size_t kAutoDateReserveLength = kAutoDateHeaderPrefix.size() + kHttpDateTextLength + kCrLfLength; ///< 前缀 6 + 定长 29 + CRLF 2 = 37

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
         * @details 按 RFC 9110 §5.1 的 tchar 集合校验，与请求侧共用同一张表：两侧判定不一致时，
         *          同一段转发代码会在「收得进来、发不出去」之间分裂。空白、冒号与控制字符都
         *          让报文无法定界，一律拒绝；空名字同样非法。
         * @param fieldName 头部字段名，原样判定（字符集与大小写无关）
         * @return true 可作为头部字段名
         */
        bool isValidHeaderFieldName(const std::string_view fieldName)
        {
            if (fieldName.empty())
            {
                return false;
            }

            return containsOnlyTokenCharacters(fieldName);
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

    void HttpResponse::removeHeaderField(const std::string_view name)
    {
        // 视图不在这里维护：存储内部只标脏，下次查询由权威记录重建（否则会留下
        // 「查询查得到、序列化里没有」的鬼条目）
        m_headerStore.removeAll(name);
    }

    bool HttpResponse::setHeader(const std::string_view name, const std::string_view value)
    {
        // 名不再先折成一个小写副本：入库形态由存储内部负责，比对也就地做大小写不敏感，
        // 于是头部名一超过短字符串缓冲就不再为一次写入现造副本。字符集校验与大小写无关
        // （大写与小写同属 tchar），按原样判即可
        // CR/LF/NUL 会让调用方提前结束头部块（HTTP 响应拆分），字段名里的空白与控制字符
        // 则产出线上非法报文——两种情况都拒写，且不改动任何已有状态
        // 取值判据与请求侧同一张表：CR/LF/NUL 会撕裂报文（HTAB 是 RFC 允许的 OWS，放行），
        // 判定标准是 RFC 的字段值字符集，而不是 locale 决定的 iscntrl
        if (!isValidHeaderFieldName(name) || !containsOnlyFieldValueCharacters(value))
        {
            return false;
        }

        if (HttpHeaderFieldStore::isRepeatableHeaderName(name))
        {
            // 可重复头部（Set-Cookie）的 set 语义退化为「追加一条」：既有调用方逐条 setHeader
            // 下发多个 cookie，覆盖式写法会静默丢掉前面的 cookie。每条各占一项，序列化时逐条上线；
            // 单值视图只留首条（headers()/getHeader() 的「一个名字一个值」契约不变）
            m_headerStore.append(std::string(name), std::string(value));
            return true;
        }

        // 普通头部：同名就地覆盖，条目位置仍停在首次设置处，序列化顺序不因反复改写而漂移
        m_headerStore.overwriteOrAppend(name, value);
        return true;
    }

    std::optional<std::string> HttpResponse::getHeader(const std::string_view name) const
    {
        return m_headerStore.get(name);
    }

    bool HttpResponse::hasHeaderValueToken(const std::string_view name, const std::string_view expectedToken) const
    {
        // 存储侧就地做大小写不敏感比较，这里不再归一化：长头部名不必为此现造一个副本
        return m_headerStore.containsListToken(name, expectedToken);
    }

    std::vector<std::string> HttpResponse::headerValues(const std::string_view name) const
    {
        return m_headerStore.values(name);
    }

    void HttpResponse::suppressStreamingBody() noexcept
    {
        m_isStreamingBodySuppressed = true;
    }

    bool HttpResponse::isStreamingBodySuppressed() const noexcept
    {
        return m_isStreamingBodySuppressed;
    }

    const std::unordered_map<std::string, std::string> &HttpResponse::headers() const
    {
        return m_headerStore.singleValueView();
    }

    void HttpResponse::setBody(const std::string_view body)
    {
        assignBody(std::string(body));
    }

    void HttpResponse::setOwnedBody(std::string body)
    {
        // 正文所有权由调用方交出，直接移动进堆缓冲，省掉 setBody 的整块拷贝与再分配
        assignBody(std::move(body));
    }

    void HttpResponse::assignBody(std::string body)
    {
        beginHeapBodyStorage();
        m_body = std::move(body);
    }

    std::string &HttpResponse::prepareBodyBuffer(const std::size_t length)
    {
        // 与 setOwnedBody 共用同一套前置不变式（流式互斥、解除旧映射），差别只在正文就地写：
        // resize 到 length 之后，调用方读满这段即可，不必再经过任何一次正文拷贝或移动
        beginHeapBodyStorage();
        m_body.resize(length);
        return m_body;
    }

    void HttpResponse::beginHeapBodyStorage()
    {
        // 流式模式下正文由 writeChunk 逐段写出、长度对序列化层未知：再塞一份整块正文就是
        // 两份互相矛盾的正文表述，明确报错而不是静默丢弃调用方给的内容
        if (m_isChunked)
        {
            throw Base::LogicException("HttpResponse::setBody/setOwnedBody/prepareBodyBuffer：本响应已进入流式模式，"
                                       "正文只能由 writeChunk() 逐段写出，"
                                       "不能再设置整块正文；请去掉这次的整块正文调用，"
                                       "或不要调用 startChunkedResponse() 而改用普通响应");
        }

        // 两条正文存储互斥：换成堆正文之前先解除映射，否则 bodyView() 会继续读旧映射
        releaseMappedBody();

        // 刻意不动调用方显式设过的 content-length：HEAD 与静态文件服务靠「先声明长度、
        // 不读正文」省一次整文件 IO（见 HttpServer 的静态文件分支与 Router 的 HEAD 用例），
        // 这里删掉就等于把那份声明抹平。替换既有正文的中间件（如响应压缩）要自己清这条头
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
        // 按值入口只做一件事：把调用方交来的映射升成共享所有权，之后与共享入口走同一条存储路径。
        // 校验留在被调方，所以移动之前不取任何状态——形参求值顺序未指定，边移动边读会读到空壳
        setSharedMappedBody(std::make_shared<Platform::MemoryMappedFile>(std::move(mappedFile)), offset, length);
    }

    void HttpResponse::setSharedMappedBody(std::shared_ptr<const Platform::MemoryMappedFile> mappedFile,
                                           const std::size_t offset,
                                           const std::size_t length)
    {
        // 互斥判定放在最前：流式模式下的整块正文（无论来自堆还是映射）都不允许，
        // 也不该让调用方以为「越界检查通过了就能设」
        if (m_isChunked)
        {
            throw Base::LogicException("HttpResponse 的映射正文入口：本响应已进入流式模式，正文只能由 writeChunk() 逐段写出，"
                                       "不能再设置整块正文；请去掉这次的 setMappedBody() 调用，"
                                       "或不要调用 startChunkedResponse() 而改用普通响应");
        }

        const bool        isUsableMapping = mappedFile != nullptr && mappedFile->isValid();
        const std::size_t availableLength = isUsableMapping ? mappedFile->bytes().size() : 0;

        // 越界属于调用方的用法错误（重试无用），归入 logic_error 分支；静默钳制会让
        // content-length 与实际字节数悄悄不一致，那正是收端报文边界错位的源头
        if (offset > availableLength || length > availableLength - offset)
        {
            throw Base::InvalidArgumentException("HttpResponse 的映射正文入口：区间越界，offset=" + std::to_string(offset) +
                                                 "，length=" + std::to_string(length) + "，映射字节数=" + std::to_string(availableLength) +
                                                 "；请先按 MemoryMappedFile::bytes().size() 校验区间，或改用整份映射的重载");
        }

        // 反向的互斥：映射正文接管后堆正文必须清空，避免 content-length 按残留字节数算错
        m_body.clear();
        // content-length 与 setBody 同一口径：调用方显式声明的长度原样保留（区间响应
        // 正是「先声明区间长度、再交出映射」的写法），只有替换正文的中间件需要自己清
        // 无效映射（含空指针）归一成「没有映射正文」：读侧只需判一次指针，不必每次再问 isValid
        m_mappedBody = isUsableMapping ? std::move(mappedFile) : nullptr;
        m_mappedBodyOffset = offset;
        m_mappedBodyLength = length;
    }

#if !ASYN_PLATFORM_WIN32
    std::optional<HttpResponse::ZeroCopyBody> HttpResponse::zeroCopyBody() const noexcept
    {
        // 只有「有效映射 + 非空区间」才给得出可发送的描述：堆正文、空文件、流式响应
        // 都没有文件可交给内核搬运，一律以空值表示走普通发送路径
        if (m_mappedBody == nullptr || m_mappedBodyLength == 0)
        {
            return std::nullopt;
        }
        return ZeroCopyBody{m_mappedBody->nativeFileDescriptor(), m_mappedBodyOffset, m_mappedBodyLength};
    }
#endif

    void HttpResponse::releaseMappedBody() noexcept
    {
        // 只释放映射正文，不碰状态码与头部：会话在响应发出后调用，此时正文已不再需要，
        // 提前归还映射与文件句柄能避免空闲的 keep-alive 连接长期占着它们。
        // 这里是放掉本响应的那一份引用：若映射同时被缓存或其他在途响应持有，页不会消失
        m_mappedBody.reset();
        m_mappedBodyOffset = 0;
        m_mappedBodyLength = 0;
    }

    std::string_view HttpResponse::bodyView() const noexcept
    {
        if (m_mappedBody != nullptr)
        {
            const std::span<const std::byte> mappedBytes = m_mappedBody->bytes();
            if (mappedBytes.empty() || m_mappedBodyLength == 0)
            {
                // 空文件映射不出可解引用的地址（data() 可能为空），长度为 0 的区间同理，
                // 两者都用空视图表示「正文 0 字节」，不构造 string_view(nullptr, 0)
                return {};
            }
            // 区间合法性已由 setSharedMappedBody 拦住，这里直接按 offset/length 取子视图
            return std::string_view(reinterpret_cast<const char *>(mappedBytes.data() + m_mappedBodyOffset), m_mappedBodyLength);
        }
        return m_body;
    }

    std::string_view HttpResponse::autoDateText() const
    {
        // 只生成一次：同一响应多次序列化（serializeHead + toString）必须给出逐字一致的 date
        if (m_autoDateValue.empty())
        {
            // 文本本身按秒缓存在线程局部，这里只是把当轮那份定长文本抄进响应对象
            m_autoDateValue = currentHttpDateText();
        }
        return m_autoDateValue;
    }

    std::string_view HttpResponse::body() const
    {
        return bodyView();
    }

    void HttpResponse::setChunkSender(ChunkSender chunkSender)
    {
        // 装配即接管：回调的生命周期要求见声明处 @note，这里不做任何包装或校验
        m_chunkSender = std::move(chunkSender);
    }

    bool HttpResponse::isChunkedResponse() const noexcept
    {
        return m_isChunked;
    }

    bool HttpResponse::hasSentChunkedHead() const noexcept
    {
        return m_hasSentChunkedHead;
    }

    void HttpResponse::upgradeToWebSocket(WebSocketHandler handler)
    {
        // 与流式模式互斥：升级应答由握手模块逐字节生成，与「头部 + 分块正文」是两条互斥的报文形态，
        // 同时成立会让「这条响应到底怎么上线」变得不确定
        if (m_isChunked)
        {
            throw Base::LogicException("HttpResponse::upgradeToWebSocket：本响应已进入流式模式，无法再登记 WebSocket 升级；"
                                       "请去掉 startChunkedResponse()/writeChunk() 调用，升级应答由会话按握手结果发出");
        }

        // 与整块正文互斥：理由同上——正文会被序列化进响应，而升级应答里没有正文的位置
        if (!bodyView().empty())
        {
            throw Base::LogicException("HttpResponse::upgradeToWebSocket：本响应已经设置过整块正文，与 WebSocket 升级互斥；"
                                       "请去掉 setBody()/setMappedBody() 调用，升级应答由会话按握手结果发出");
        }

        // 空处理器是用法错误：会话会在升级成功后调用它，而空 std::function 抛的是 bad_function_call，
        // 既不是本框架的异常，也指不出是哪一处漏了装配
        if (!handler)
        {
            throw Base::InvalidArgumentException("HttpResponse::upgradeToWebSocket：升级处理器为空，连接升级后没有可运行的处理逻辑；"
                                                 "请传入形如 std::function<Core::Task<>(WebSocketPeer &)> 的处理器");
        }

        m_webSocketHandler = std::move(handler);
        m_isWebSocketUpgradeRequested = true;
    }

    bool HttpResponse::isWebSocketUpgradeRequested() const noexcept
    {
        return m_isWebSocketUpgradeRequested;
    }

    const WebSocketHandler &HttpResponse::webSocketHandler() const noexcept
    {
        return m_webSocketHandler;
    }

    void HttpResponse::startChunkedResponse(const int statusCode)
    {
        // 头部已经上线之后再进/改流式模式：对端已经按上一版状态行与头部读到了报文，
        // 这里改什么都到不了它手上，静默接受只会让调用方以为改成功了
        if (m_hasSentChunkedHead)
        {
            throw Base::LogicException("HttpResponse::startChunkedResponse：本次流式响应的头部已经写出，无法再改状态码；"
                                       "请把 startChunkedResponse() 与头部设置都放到首次 writeChunk() 之前");
        }

        // 1xx、204、304 的响应按 RFC 9110 §6.3 不得携带正文，流式写出没有意义
        if (isBodylessStatusCode(statusCode))
        {
            throw Base::LogicException("HttpResponse::startChunkedResponse：状态码 " + std::to_string(statusCode) +
                                       " 的响应不允许携带正文，无法流式写出；请改用 200 等允许正文的状态码，"
                                       "或改用不带正文的普通响应");
        }

        // 已经设过整块正文再来进流式模式：两份互相矛盾的正文表述，报错而不是把已设的正文悄悄丢掉
        if (!bodyView().empty())
        {
            throw Base::LogicException("HttpResponse::startChunkedResponse：本响应已经设置过整块正文，与流式模式互斥；"
                                       "请去掉 setBody()/setMappedBody() 调用，正文改用 writeChunk() 逐段写出");
        }

        m_status = statusCode;
        m_isChunked = true;
        m_hasSentChunkedHead = false;

        // content-length 与 transfer-encoding 不得并存（RFC 9112 §6.1）：收端若按前者定界，
        // 分块帧就会被当成正文，整条报文的边界随之错位。调用方先设下的那条一律删掉
        removeHeaderField(kContentLengthHeaderName);

        // 正文长度对发送方未知，消息边界改由分块帧界定（RFC 9112 §6）。
        // 名与值都是本类自己给出的合法字面量，setHeader 不会拒收，返回值无需再判
        setHeader(kTransferEncodingHeaderName, kChunkedTransferEncodingValue);
    }

    Core::Task<bool> HttpResponse::writeChunk(const std::string_view data)
    {
        // 两种用法错误都在这里拦下：没进流式模式、或没有连接可写（响应对象在会话之外构造）。
        // 静默丢弃会让对端挂在一条永远收不满的响应上，因此一律当场报错
        if (!m_isChunked)
        {
            throw Base::LogicException("HttpResponse::writeChunk：当前响应不在流式模式，无法写出正文段；"
                                       "请先调用 startChunkedResponse(statusCode) 再逐段 co_await writeChunk(...)");
        }
        if (!m_chunkSender)
        {
            throw Base::LogicException("HttpResponse::writeChunk：响应对象没有装配发送回调，正文段无处可写；"
                                       "本接口只对会话交给业务处理器的响应对象有效，请改回 setBody() 一次性给出正文");
        }

        // 空段不发：零长度块是终止块的语义（RFC 9112 §7.1），发出去等于告诉对端消息结束。
        // 调用方按「没数据就空转一轮」的节奏循环时，不必自己判空
        if (data.empty())
        {
            co_return true;
        }

        // 首段之前先把头部推出去：startChunkedResponse() 是同步接口，发不出去字节，只能把头部
        // 懒发送到第一次真正写出的时候 —— 正是这一点让正文能边写边到，而不是攒到处理器结束
        if (!m_hasSentChunkedHead)
        {
            const std::string serializedHead = serializeHead();
            if (!co_await m_chunkSender(serializedHead))
            {
                co_return false;
            }

            // 头部已在对端手里：此后状态码与头部都改不了，即使接下来的数据段发送失败
            m_hasSentChunkedHead = true;
        }

        // HEAD：响应只有头部（RFC 9112 §6.1 的口径在流式路径上同样成立）——头部已经在上面发出去，
        // 正文段一字节都不发。对端在头部后的第一个空行就认为消息结束，发出去的帧会被当成下一条报文
        if (m_isStreamingBodySuppressed)
        {
            co_return true;
        }

        // 帧 = <十六进制长度>\r\n<数据>\r\n。长度必须与数据逐字节相符，错一位对端就再也找不回
        // 帧边界；to_chars 而非流式格式化，既不受 locale 影响也不抛异常
        std::array<char, kMaximumChunkLengthHexDigits> lengthText{};
        const auto [lengthEnd, lengthError] =
                std::to_chars(lengthText.data(), lengthText.data() + lengthText.size(), data.size(), 16);
        if (lengthError != std::errc())
        {
            // 缓冲按 size_t 的最长十六进制位数给足，正常平台走不到这里；真遇到了宁可失败，
            // 也绝不发出一个长度不对的帧
            throw Base::LogicException("HttpResponse::writeChunk：分块长度无法写成十六进制文本，本段数据未发出；"
                                       "请检查本段长度是否超出平台可表示范围");
        }

        const std::size_t lengthTextLength = static_cast<std::size_t>(lengthEnd - lengthText.data());
        std::string frame;
        // 长度行 + 数据 + 收尾 CRLF 一次预留到位：每次 writeChunk 只分配这一块
        frame.reserve(lengthTextLength + kCrLfLength + data.size() + kCrLfLength);
        frame.append(lengthText.data(), lengthTextLength);
        frame.append(kCrLf);
        frame.append(data.data(), data.size());
        frame.append(kCrLf);

        co_return co_await m_chunkSender(frame);
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

    bool HttpResponse::isBodylessStatusCode(const int statusCode) noexcept
    {
        // RFC 9110 §6.3：1xx、204、304 一律不含正文。这里按状态码在序列化层兜住，
        // 不能只指望上游 Router 的 finalizeResponse 先把正文清空——业务直接
        // setStatus(204) + setBody(...) 就会发出「没有 content-length 却带正文」的报文，
        // keep-alive 上的下一帧边界随之错位
        return statusCode == 204 || statusCode == 304 || (statusCode >= 100 && statusCode < 200);
    }

    bool HttpResponse::carriesNoContent() const noexcept
    {
        return isBodylessStatusCode(m_status);
    }

    bool HttpResponse::mustNotDeclareContentLength() const noexcept
    {
        // 三种状态码都不自动补 content-length：
        // - 204 与 1xx 不得带正文，补出去是让收端白等一条正文（RFC 9112 §6.2 的 MUST NOT）；
        // - 304 虽然允许携带，但只允许取「同一请求的 200 会发出的正文长度」——此刻正文已被
        //   清空，自动补出来的是 0，正好落在 §6.2 的 MUST NOT 上。要带这个头就由知道真实长度
        //   的调用方显式设（静态文件路径就是这么做的）
        return m_status == 204 || m_status == 304 || (m_status >= 100 && m_status < 200);
    }

    std::size_t HttpResponse::headReserveLength() const
    {
        std::size_t reservedLength = kStatusLineReserveLength + kHeaderBlockTerminatorReserveLength;
        bool        hasContentTypeHeader = false;
        bool        hasContentLengthHeader = false;
        bool        hasDateHeader = false;
        for (const HttpHeaderFieldStore::HeaderField &field: m_headerStore.fields())
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
        // 流式响应不写 content-length（正文长度未知，边界由分块帧界定），这一段无需预留
        if (!m_isChunked && !hasContentLengthHeader && !mustNotDeclareContentLength())
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
        for (const HttpHeaderFieldStore::HeaderField &field: m_headerStore.fields())
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

            // content-length 与 transfer-encoding 不得并存（RFC 9112 §6.1）：流式响应一律不输出这条，
            // 既拦住调用方自己设的，也拦住序列化层按正文长度自动补的那条
            if (m_isChunked && field.name == kContentLengthHeaderName)
            {
                continue;
            }

            // 反方向同理：正文定界由本框架掌管（流式=分块，其余=content-length）。调用方自设的
            // transfer-encoding 在这里一律剥掉——留着它就会出现「既补 content-length 又声明
            // chunked」而正文并没有分块帧化，对端按哪条解都是错位（h2/h3 早已剥掉，与它们对齐）
            if (!m_isChunked && field.name == kTransferEncodingHeaderName)
            {
                continue;
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
        // 流式响应由分块帧界定正文边界，content-length 与 transfer-encoding 不得并存
        // （RFC 9112 §6.1），因此既不补也不输出自设的那条
        if (!m_isChunked && !hasContentLengthHeader && !mustNotDeclareContentLength())
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

    void HttpResponse::serializeHeadInto(std::string &out) const
    {
        // 只清不缩：调用方复用的缓冲保留上一轮的容量，稳定后 reserve 命中已有容量即空操作，
        // 于是整条保活连接只在第一条响应上碰一次分配器
        out.clear();
        out.reserve(headReserveLength());
        appendHead(out);
    }

    std::string HttpResponse::serializeHead() const
    {
        std::string result;
        serializeHeadInto(result);
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

        // 清权威记录并把视图标脏：下次查询会重建出空视图（只清一处会留下
        // 「视图里查得到、序列化里没有」的鬼条目）
        m_headerStore.clear();

        // 正文同样是两条存储：堆串清空之外映射也要解除，
        // 否则复用响应对象时上一轮的文件会继续当正文发出去
        m_body.clear();
        releaseMappedBody();

        // 自动 date 同样要清：否则复用响应时下一条报文会带上上一轮生成的日期
        m_autoDateValue.clear();

        // 流式模式标记一并复位：残留下去会让下一条报文也按分块定界，而对端等的是终止块。
        // 发送回调刻意不动：它绑定的是连接，不是本条报文
        m_isChunked = false;
        m_hasSentChunkedHead = false;

        // 升级意图与处理器一起清：留在复用对象上会把下一条报文也拖进升级分支
        m_webSocketHandler = WebSocketHandler{};
        m_isWebSocketUpgradeRequested = false;

        // 正文抑制标记同样复位：它属于「这一条报文」，下一条可能是 GET，正文必须照发
        m_isStreamingBodySuppressed = false;
    }

} // namespace AsynGyanis::Net

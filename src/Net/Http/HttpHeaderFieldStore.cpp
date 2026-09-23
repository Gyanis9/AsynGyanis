#include "Net/Http/HttpHeaderFieldStore.h"

#include "Net/Http/HttpHeaderRules.h"

#include <algorithm>
#include <array>
#include <cstring>
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
    } // namespace

    std::string_view HttpHeaderFieldStore::segmentOf(const std::vector<char> &bytes, const std::size_t offset,
                                                      const std::size_t length) noexcept
    {
        // 零长段直接交空视图：空缓冲的 data() 是空指针，给空指针加偏移本身就是错的，
        // 而「有记录但取值为空」是合法形态（`foo:` 这种空取值）
        if (length == 0)
        {
            return {};
        }
        return std::string_view{bytes.data() + offset, length};
    }

    HttpHeaderFieldStore::FieldRef HttpHeaderFieldStore::appendNameToBytes(const std::string_view name)
    {
        FieldRef ref;
        ref.nameOffset = m_bytes.size();
        ref.nameLength = name.size();
        // 小写折叠与写缓冲合成一趟：入参只是视图，不必先造一份归一化副本再拷进缓冲
        for (const char character: name)
        {
            m_bytes.push_back(toLowerAscii(character));
        }
        return ref;
    }

    void HttpHeaderFieldStore::appendValueToBytes(FieldRef &ref, const std::string_view value)
    {
        ref.valueOffset = m_bytes.size();
        ref.valueLength = value.size();
        m_bytes.insert(m_bytes.end(), value.begin(), value.end());
    }

    void HttpHeaderFieldStore::append(const std::string_view name, const std::string_view value)
    {
        // 头部名大小写不敏感（RFC 9110 §5.1）：统一折小写入库，于是 Content-Type 与 content-type
        // 命中同一条，序列化也按这份小写形态上线
        FieldRef ref = appendNameToBytes(name);
        appendValueToBytes(ref, value);
        m_fields.push_back(ref);
        m_isViewStale = true;
    }

    void HttpHeaderFieldStore::reserve(const std::size_t fieldCount, const std::size_t byteCount)
    {
        // 只长不缩：vector::reserve 对更小的值不做任何事，所以同一条存储跨报文复用时，
        // 暖到稳态的容量不会被一条短头部抹掉
        m_fields.reserve(fieldCount);
        m_bytes.reserve(m_bytes.size() + byteCount);
    }

    void HttpHeaderFieldStore::overwriteOrAppend(const std::string_view name, const std::string_view value)
    {
        if (const auto iterator = findField(name); iterator != m_fields.end())
        {
            // 条目位置不动，只改值的指向：反复改写不会让序列化顺序漂移。
            // 名字保持入库时那份小写形态不动：调用方用 "CONTENT-TYPE" 覆盖，改的仍是 content-type。
            // 同长就地覆盖；长度变了就把新值追加到缓冲末尾并改指，旧那一段留成空洞——
            // 一次改写的浪费，比给每条头部各养一个堆块便宜
            FieldRef &ref = *iterator;
            if (ref.valueLength == value.size() && !value.empty())
            {
                std::memcpy(m_bytes.data() + ref.valueOffset, value.data(), value.size());
            }
            else
            {
                appendValueToBytes(ref, value);
            }
        }
        else
        {
            // 只有新建条目才需要折小写（线上形态由入库名决定）
            append(name, value);
            return;
        }
        m_isViewStale = true;
    }

    void HttpHeaderFieldStore::removeAll(const std::string_view name)
    {
        const auto isSameName = [this, name](const FieldRef &ref)
        {
            return equalsIgnoringCase(nameOf(ref), name);
        };
        m_fields.erase(std::ranges::remove_if(m_fields, isSameName).begin(), m_fields.end());

        // 视图不在这里维护：标脏即可，下次查询由权威记录重建（否则会留下
        // 「查询查得到、序列化里没有」的鬼条目）。字节缓冲里被删那条的段同样留成空洞：
        // 头部存储是请求级寿命，压缩整块缓冲的代价比省下的那点字节更贵
        m_isViewStale = true;
    }

    std::optional<std::string> HttpHeaderFieldStore::get(const std::string_view name) const
    {
        // 直接在权威记录上线性找，而不是先重建单值视图再查哈希表：为取一个值而把整张表建出来，
        // 等于让「视图按需重建」这项优化在任何只读一两个头部的请求上失效（实测多付约 700 ns，
        // 与解析整条 h1 请求的耗时同量级）。视图留给真正要整表的调用方（headers()）。
        // 比较就地折 ASCII 大小写而不先造归一化副本：入库名已是小写，两侧折完结果一致，
        // 而名字一超过短字符串缓冲（15 字符），那次拷贝就是每条查询一次的堆分配
        //（CORS 预检读 access-control-request-method、握手读 sec-websocket-version 都落在这一格）
        const bool isRepeatableName = isRepeatableHeaderName(name);
        std::optional<std::string> collectedValue;
        for (const FieldRef &ref: m_fields)
        {
            if (!equalsIgnoringCase(nameOf(ref), name))
            {
                continue;
            }
            if (!collectedValue.has_value())
            {
                collectedValue.emplace(valueOf(ref));
                // 可重复头部（Set-Cookie）的单值口径是「首条」，且不得逗号合并：值本身可含逗号
                if (isRepeatableName)
                {
                    break;
                }
                continue;
            }
            // 普通头部同名多条按 RFC 7230 §3.2.2 以 ", " 合并，与 rebuildSingleValueView 同口径
            collectedValue->append(kMergedHeaderSeparator);
            collectedValue->append(valueOf(ref));
        }
        // 未命中不是错误：可选头部缺席是常态，交给调用方用 optional 判定
        return collectedValue;
    }

    std::optional<std::string> HttpHeaderFieldStore::firstValue(const std::string_view name) const
    {
        // 走同一条查找：owning 版只多一步「把找到的那段拷出来」，两条入口的匹配口径不会漂移
        const std::optional<std::string_view> valueView = firstValueView(name);
        if (!valueView.has_value())
        {
            return std::nullopt;
        }
        return std::string{*valueView};
    }

    std::optional<std::string_view> HttpHeaderFieldStore::firstValueView(const std::string_view name) const
    {
        for (const FieldRef &ref: m_fields)
        {
            if (equalsIgnoringCase(nameOf(ref), name))
            {
                // 首条原样交出，不参与合并：链路 id 这类头部同名多条时各表达一个独立来源
                return valueOf(ref);
            }
        }
        return std::nullopt;
    }

    bool HttpHeaderFieldStore::contains(const std::string_view name) const
    {
        // 只看有没有这条记录：取值可能上百字节，为一次存在性判定把它整个拷出来是纯浪费
        return firstValueView(name).has_value();
    }

    bool HttpHeaderFieldStore::containsListToken(const std::string_view name, const std::string_view expectedToken) const
    {
        for (const FieldRef &ref: m_fields)
        {
            if (!equalsIgnoringCase(nameOf(ref), name))
            {
                continue;
            }

            std::string_view remainder(valueOf(ref));
            // 同一个头名可以用逗号列多个 token（"Connection: keep-alive, Upgrade"），逐段比对
            while (!remainder.empty())
            {
                const std::size_t commaPosition = remainder.find(',');
                if (equalsIgnoringCase(trimOptionalWhitespace(remainder.substr(0, commaPosition)), expectedToken))
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

    std::vector<std::string> HttpHeaderFieldStore::values(const std::string_view name) const
    {
        std::vector<std::string> collectedValues;
        // 按加入顺序收集，读到的顺序与写入顺序一致
        for (const FieldRef &ref: m_fields)
        {
            if (equalsIgnoringCase(nameOf(ref), name))
            {
                collectedValues.emplace_back(valueOf(ref));
            }
        }
        return collectedValues;
    }

    const std::unordered_map<std::string, std::string> &HttpHeaderFieldStore::singleValueView() const
    {
        // 同 get()：查询前先把过期视图重建出来
        if (m_isViewStale)
        {
            rebuildSingleValueView();
        }
        return m_singleValues;
    }

    void HttpHeaderFieldStore::adoptFrom(HttpHeaderFieldStore &source) noexcept
    {
        m_fields.swap(source.m_fields);
        m_bytes.swap(source.m_bytes);
        // 单值视图不跟着换：那张表是「按名合并」的派生物，换过来也说不清它属于哪份记录，
        // 两边各自清掉，下次有人整表查询时由权威记录重建
        m_singleValues.clear();
        source.m_singleValues.clear();
        m_isViewStale = true;
        source.m_isViewStale = true;
    }

    void HttpHeaderFieldStore::clear() noexcept
    {
        m_fields.clear();
        // 只清内容、留着容量：这条缓冲在解析器与请求对象之间来回交换，容量一丢就等于
        // 每条报文重新向堆要一次内存
        m_bytes.clear();
        m_singleValues.clear();
        m_isViewStale = true;
    }

    void HttpHeaderFieldStore::lowercaseInPlace(std::string &name)
    {
        for (char &character: name)
        {
            character = toLowerAscii(character);
        }
    }

    std::string HttpHeaderFieldStore::toCanonicalHeaderName(const std::string_view name)
    {
        std::string canonicalName(name);
        lowercaseInPlace(canonicalName);
        return canonicalName;
    }

    bool HttpHeaderFieldStore::isRepeatableHeaderName(const std::string_view name)
    {
        // 名单极小，线性比较比构造哈希集合划算。登记形式是小写，而读侧不再先归一化查询名，
        // 故这里按 ASCII 大小写不敏感比：调用方传 "Set-Cookie" 也要认得出是可重复头部
        return std::ranges::any_of(kRepeatableHeaderNames,
                                   [name](const std::string_view registeredName)
                                   {
                                       return equalsIgnoringCase(registeredName, name);
                                   });
    }

    std::vector<HttpHeaderFieldStore::FieldRef>::iterator HttpHeaderFieldStore::findField(const std::string_view name)
    {
        // 头部数量级为几十条，线性比较比再挂一张「名到迭代器」的索引表更划算，也少一份要维护的一致性
        return std::ranges::find_if(m_fields,
                                    [this, name](const FieldRef &ref)
                                    {
                                        return equalsIgnoringCase(nameOf(ref), name);
                                    });
    }

    void HttpHeaderFieldStore::rebuildSingleValueView() const
    {
        m_singleValues.clear();
        for (const FieldRef &ref: m_fields)
        {
            const std::string_view name = nameOf(ref);
            const std::string_view value = valueOf(ref);
            if (isRepeatableHeaderName(name))
            {
                // 单值视图只保留首条，其余靠 values() 逐条取；
                // try_emplace 而非 insert_or_assign，正是为了「后来的不覆盖首条」。
                // 这里显式造串而不是交视图：视图到串的转换是 explicit 的，MSVC 的 try_emplace 不认
                m_singleValues.try_emplace(std::string{name}, std::string{value});
                continue;
            }

            // 普通头部同名多条时，按 RFC 7230 §3.2.2 的收件人规则以 ", " 合并到同一条，
            // 视图里的条目位置与键都不变（可重复头部也不会派生出伪键）
            if (const auto [iterator, isInserted] = m_singleValues.try_emplace(std::string{name}, std::string{value});
                !isInserted)
            {
                iterator->second.append(kMergedHeaderSeparator);
                iterator->second.append(value);
            }
        }
        m_isViewStale = false;
    }
} // namespace AsynGyanis::Net

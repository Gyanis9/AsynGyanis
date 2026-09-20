#include "Net/Http/HttpHeaderFieldStore.h"

#include "Net/Http/HttpHeaderRules.h"

#include <algorithm>
#include <array>
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

    void HttpHeaderFieldStore::append(std::string name, std::string value)
    {
        // 头部名大小写不敏感（RFC 9110 §5.1）：统一转小写入库，于是 Content-Type 与
        // content-type 命中同一条。入参本来就是调用方交出的副本，就地改写比再造一个字符串省一次分配
        lowercaseInPlace(name);
        m_fields.push_back(HeaderField{.name = std::move(name), .value = std::move(value)});
        m_isViewStale = true;
    }

    void HttpHeaderFieldStore::overwriteOrAppend(const std::string &canonicalName, const std::string &value)
    {
        if (const auto iterator = findField(canonicalName); iterator != m_fields.end())
        {
            // 原地覆盖值，条目位置仍停在首次设置处，这样序列化顺序不因反复改写而漂移
            iterator->value = value;
        } else
        {
            m_fields.push_back(HeaderField{.name = canonicalName, .value = value});
        }
        m_isViewStale = true;
    }

    void HttpHeaderFieldStore::removeAll(const std::string &canonicalName)
    {
        const auto isSameName = [&canonicalName](const HeaderField &field)
        {
            return field.name == canonicalName;
        };
        m_fields.erase(std::ranges::remove_if(m_fields, isSameName).begin(), m_fields.end());

        // 视图不在这里维护：标脏即可，下次查询由权威记录重建（否则会留下
        // 「查询查得到、序列化里没有」的鬼条目）
        m_isViewStale = true;
    }

    std::optional<std::string> HttpHeaderFieldStore::get(const std::string &name) const
    {
        // 查询侧走同一套归一化规则，保证写入与读取对键的认定一致
        const std::string canonicalName = toCanonicalHeaderName(name);

        // 直接在权威记录上线性找，而不是先重建单值视图再查哈希表：为取一个值而把整张表建出来，
        // 等于让「视图按需重建」这项优化在任何只读一两个头部的请求上失效（实测多付约 700 ns，
        // 与解析整条 h1 请求的耗时同量级）。视图留给真正要整表的调用方（headers()）
        const bool isRepeatableName = isRepeatableHeaderName(canonicalName);
        std::optional<std::string> collectedValue;
        for (const HeaderField &field: m_fields)
        {
            if (field.name != canonicalName)
            {
                continue;
            }
            if (!collectedValue.has_value())
            {
                collectedValue.emplace(field.value);
                // 可重复头部（Set-Cookie）的单值口径是「首条」，且不得逗号合并：值本身可含逗号
                if (isRepeatableName)
                {
                    break;
                }
                continue;
            }
            // 普通头部同名多条按 RFC 7230 §3.2.2 以 ", " 合并，与 rebuildSingleValueView 同口径
            collectedValue->append(kMergedHeaderSeparator);
            collectedValue->append(field.value);
        }
        // 未命中不是错误：可选头部缺席是常态，交给调用方用 optional 判定
        return collectedValue;
    }

    std::vector<std::string> HttpHeaderFieldStore::values(const std::string &name) const
    {
        const std::string canonicalName = toCanonicalHeaderName(name);

        std::vector<std::string> collectedValues;
        // 按加入顺序收集，读到的顺序与写入顺序一致
        for (const HeaderField &field: m_fields)
        {
            if (field.name == canonicalName)
            {
                collectedValues.push_back(field.value);
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

    void HttpHeaderFieldStore::clear() noexcept
    {
        m_fields.clear();
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

    bool HttpHeaderFieldStore::isRepeatableHeaderName(const std::string_view canonicalName)
    {
        // 名单极小，线性比较比构造哈希集合划算
        return std::ranges::find(kRepeatableHeaderNames, canonicalName) != kRepeatableHeaderNames.end();
    }

    HttpHeaderFieldStore::HeaderFieldList::iterator HttpHeaderFieldStore::findField(const std::string &canonicalName)
    {
        // 头部数量级为几十条，线性比较比再挂一张「名到迭代器」的索引表更划算，也少一份要维护的一致性
        return std::ranges::find_if(m_fields,
                                    [&canonicalName](const HeaderField &field)
                                    {
                                        return field.name == canonicalName;
                                    });
    }

    void HttpHeaderFieldStore::rebuildSingleValueView() const
    {
        m_singleValues.clear();
        for (const HeaderField &field: m_fields)
        {
            if (isRepeatableHeaderName(field.name))
            {
                // 单值视图只保留首条，其余靠 values() 逐条取；
                // try_emplace 而非 insert_or_assign，正是为了「后来的不覆盖首条」
                m_singleValues.try_emplace(field.name, field.value);
                continue;
            }

            // 普通头部同名多条时，按 RFC 7230 §3.2.2 的收件人规则以 ", " 合并到同一条，
            // 视图里的条目位置与键都不变（可重复头部也不会派生出伪键）
            if (const auto [iterator, isInserted] = m_singleValues.try_emplace(field.name, field.value); !isInserted)
            {
                iterator->second.append(kMergedHeaderSeparator);
                iterator->second.append(field.value);
            }
        }
        m_isViewStale = false;
    }
} // namespace AsynGyanis::Net

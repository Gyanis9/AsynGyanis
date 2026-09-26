/**
 * @file HttpHeaderFieldStore.h
 * @brief 请求与响应共用的头部字段存储：权威记录 + 单值视图
 * @author Gyanis
 * @date 2026-09-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details `HttpRequest` 与 `HttpResponse` 各需一套同样的头部存储：按加入顺序的权威记录、
 *          按需重建的单值视图、以及名字归一化与「可重复头部」名单。此前两处各写一份完全对称的
 *          实现，本类把它收成一份；两边的策略差异（请求只追加、响应带校验与覆盖）留在各自类里。
 */

#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief 头部字段存储
     *
     * @details 两份数据：m_fields + m_bytes 是按加入顺序的权威记录（可重复头部各占一项，名与值的
     *          字节全在一条连续缓冲里），m_singleValues 是名到值的单值视图，只在真正被整表查询时
     *          才重建——按名字取单值走 get()/values() 直接读权威记录，多数请求只读一两个头部，为它们
     *          建表等于白付若干次节点分配与字符串拷贝。
     */
    class HttpHeaderFieldStore
    {
    public:
        /**
         * @brief 一条头部记录在字节缓冲里的位置
         *
         * @details 名与值不各自持串，只记「在 m_bytes 的哪一段」：一条请求的头部因此共用一次缓冲增长，
         *          而不是每条名值各要一个堆块（MSVC 的短串内联也救不掉超长的取值）。偏移用 size_t
         *          而非定长窄类型，是为了不写「长度或偏移放不下就静默截断」那种隐性行为。
         */
        struct FieldRef
        {
            std::size_t nameOffset{0};  ///< 头部名在 m_bytes 中的起始偏移
            std::size_t nameLength{0};  ///< 头部名长度
            std::size_t valueOffset{0}; ///< 头部值在 m_bytes 中的起始偏移
            std::size_t valueLength{0}; ///< 头部值长度
        };

        /**
         * @brief 追加一条头部记录（名字归一化为小写后入库）
         * @details 名与值各一趟 memcpy 进同一条字节缓冲，小写折叠在写入时顺带做完，
         *          因此这里不产生任何临时串或逐字段堆块。
         * @param name 头部名，大小写不敏感（Content-Type 与 content-type 命中同一条）
         * @param value 头部值，原样入库
         */
        void append(std::string_view name, std::string_view value);

        /**
         * @brief 一次留够整块头部的容量，把逐条 append 期间的按倍扩容并成一次
         * @details 给「装配前就知道会有几条、多少字节」的调用方用（协议层解完一个头块之后）：
         *          不预留时记录表与字节缓冲要各自长好几轮，一条七字段的请求能付到十次分配。
         * @param fieldCount 预计的记录条数；实到条数超出时照常扩容
         * @param byteCount 预计要追加的名值字节数；超出时照常扩容
         */
        void reserve(std::size_t fieldCount, std::size_t byteCount);

        /**
         * @brief 覆盖或追加一条非可重复头部
         * @details 同名已有记录就地覆盖值，条目位置仍停在首次设置处；缺席则追加到末尾。
         *          覆盖时会一并清掉该名的其余记录——「set」之后这个名只对应一条，否则合并视图
         *          会把新旧两条一起带下去，改写等于没改干净。
         * @param name 头部名，大小写不敏感；新建条目时按小写形态入库
         * @param value 头部值
         */
        void overwriteOrAppend(std::string_view name, std::string_view value);

        /**
         * @brief 移除该名下的全部记录
         * @param name 头部名，大小写不敏感
         */
        void removeAll(std::string_view name);

        /**
         * @brief 取该名对应的单值（按权威记录算，不建单值视图）
         * @details 可重复头部取首条；普通头部同名多条按 RFC 7230 §3.2.2 以 ", " 合并，
         *          与 singleValueView() 同口径。
         * @param name 头部名，大小写不敏感
         * @return std::optional<std::string> 头部值；未命中时为空
         */
        [[nodiscard]] std::optional<std::string> get(std::string_view name) const;

        /**
         * @brief 取该名的首条取值（原样，不参与合并）
         * @details 与 values() 的首元素同值，但省掉为「一个值」构造整列 vector 的开销。
         * @param name 头部名，大小写不敏感
         * @return std::optional<std::string> 首条取值；缺席时为空
         */
        [[nodiscard]] std::optional<std::string> firstValue(std::string_view name) const;

        /**
         * @brief 取该名首条取值的视图，不拷贝也不分配
         * @details 给「读完就丢」的调用方（判取值形态、比对前缀）用：owning 版每查一条长头部
         *          就向堆要一次内存，而这些地方只要读几十字节。
         * @param name 头部名，大小写不敏感
         * @return std::optional<std::string_view> 首条取值；缺席时为空（空取值给出「存在且为空视图」，
         *         不与缺席混淆）
         * @note 视图指向存储内的字符串，只在本存储下次写入或清空之前有效；要跨过改写点就得自己拷走
         */
        [[nodiscard]] std::optional<std::string_view> firstValueView(std::string_view name) const;

        /**
         * @brief 判断该名是否出现过（不看取值）
         * @details 只要存在性的调用方用它：owning 的取值入口会为一次判定拷出整个值。
         * @param name 头部名，大小写不敏感
         * @return true 至少有一条该名的记录
         */
        [[nodiscard]] bool contains(std::string_view name) const;

        /**
         * @brief 判断该名的取值里是否出现了某个逗号分隔的 token（RFC 9110 §5.6.1）
         * @details 在存储内部逐段切分比对，不构造取值列表也不拷贝取值：Connection/Upgrade 这类
         *          判定每条请求要跑好几遍，而调用方只要一个布尔结果。
         * @param name 头部名，大小写不敏感
         * @param expectedToken 待查找的 token，大小写不敏感，段首尾的 OWS 会被裁掉
         * @return true 至少一条取值列出了该 token
         */
        [[nodiscard]] bool containsListToken(std::string_view name, std::string_view expectedToken) const;

        /**
         * @brief 取该名下的全部值，按加入顺序
         * @param name 头部名，大小写不敏感
         * @return std::vector<std::string> 全部取值；未命中时为空
         */
        [[nodiscard]] std::vector<std::string> values(std::string_view name) const;

        /**
         * @brief 该名下有几条记录（不参与合并，也不拷任何字节）
         * @details 「0 条」与「多于 1 条」是两种需要分开处理的情形：缺席可以补一个值，
         *          而多条同名普通头部是有歧义的输入（合并视图会把两条折成一条带逗号的取值），
         *          按名取值的入口都看不出这个差别。判定为链路上下文这类严格字段所必需，
         *          又不必为一次计数构造值列表。
         * @param name 头部名，大小写不敏感
         * @return std::size_t 记录条数；未命中为 0
         */
        [[nodiscard]] std::size_t countOf(std::string_view name) const;

        /**
         * @brief 取单值视图（名 → 合并后的值）
         * @details 可重复头部只留首条；普通头部同名多条按 RFC 7230 §3.2.2 以 ", " 合并。
         * @return const std::unordered_map<std::string, std::string>& 视图引用
         */
        [[nodiscard]] const std::unordered_map<std::string, std::string> &singleValueView() const;

        /// 按加入顺序遍历权威记录（序列化按它的顺序进行）
        template<typename Visitor>
            requires std::invocable<Visitor, std::string_view, std::string_view>
        void forEachField(const Visitor &visitor) const
        {
            // 基址每次遍历只判一次：空缓冲的 data() 是空指针，而那时不可能有记录可访，
            // 逐条再判就是序列化这种整表遍历里每条两次的白付分支
            const char *const base = m_bytes.empty() ? "" : m_bytes.data();
            for (const FieldRef &ref: m_fields)
            {
                visitor(std::string_view{base + ref.nameOffset, ref.nameLength}, std::string_view{base + ref.valueOffset, ref.valueLength});
            }
        }

        /**
         * @brief 接手另一份存储的权威记录，同时把自己现有的记录换过去
         * @details 只做容器交换，一个字节也不拷：解析器把头部暂存在自己的存储里，报文收齐那一刻
         *          用它换走请求对象刚被 reset() 清空的空壳缓冲。两条缓冲就此在「解析器 ↔ 请求」之间
         *          来回复用，容量都不丢。
         * @param source 内容要被接手的存储；返回时它持有本存储原先那份空记录（不是残留的旧头部）
         */
        void adoptFrom(HttpHeaderFieldStore &source) noexcept;

        /// 清空全部记录与视图
        void clear() noexcept;

        /// 是否一条记录都没有
        [[nodiscard]] bool empty() const noexcept
        {
            return m_fields.empty();
        }

        /**
         * @brief 把头部名就地改写为小写
         * @details 逐字符按 ASCII 表折叠：不用 std::tolower，那个受 locale 影响
         *          （土耳其语环境下 'I' 会折成非 ASCII 字节）。
         * @param name 待改写的字符串
         */
        static void lowercaseInPlace(std::string &name);

        /**
         * @brief 把头部名归一化成内部存储形式（小写）
         * @param name 原始头部名
         * @return std::string 归一化后的头部名
         */
        [[nodiscard]] static std::string toCanonicalHeaderName(std::string_view name);

        /**
         * @brief 判断头部名是否允许在同一报文里出现多条
         * @param name 头部名，大小写不敏感（名单里的登记形式是小写）
         * @return true 表示该头部禁止合并，必须逐条保留
         */
        [[nodiscard]] static bool isRepeatableHeaderName(std::string_view name);

    private:
        /**
         * @brief 按名找第一条记录（大小写不敏感）
         * @param name 头部名
         * @return std::vector<FieldRef>::iterator 命中位置；未命中为 end()
         */
        [[nodiscard]] std::vector<FieldRef>::iterator findField(std::string_view name);

        /**
         * @brief 取缓冲里的一段视图
         * @details 零长段直接交空视图：空 vector 的 data() 是空指针，给空指针加偏移本身就是错的，
         *          而「有记录但值取空」是合法形态（如 foo: 这种空取值）。
         * @param bytes 字节缓冲
         * @param offset 段起始偏移
         * @param length 段长度
         * @return std::string_view 该段的视图
         */
        static std::string_view segmentOf(const std::vector<char> &bytes, std::size_t offset, std::size_t length) noexcept;

        /// 取记录的名字段视图
        [[nodiscard]] std::string_view nameOf(const FieldRef &ref) const noexcept
        {
            return segmentOf(m_bytes, ref.nameOffset, ref.nameLength);
        }

        /// 取记录的取值段视图
        [[nodiscard]] std::string_view valueOf(const FieldRef &ref) const noexcept
        {
            return segmentOf(m_bytes, ref.valueOffset, ref.valueLength);
        }

        /**
         * @brief 把名字折成小写追加进字节缓冲，并返回它的位置
         * @param name 头部名原文
         * @return FieldRef 只填好了名字段的引用（值段由调用方补）
         */
        [[nodiscard]] FieldRef appendNameToBytes(std::string_view name);

        /**
         * @brief 把取值追加进字节缓冲
         * @param ref 待补的记录引用，就地写入值段的偏移与长度
         * @param value 头部值
         */
        void appendValueToBytes(FieldRef &ref, std::string_view value);

        /// 按需要重建单值视图（调用前视图已标脏）
        void rebuildSingleValueView() const;

        std::vector<FieldRef>                                m_fields;            ///< 权威记录，按加入顺序保存，决定序列化顺序
        std::vector<char>                                    m_bytes;             ///< 名与值的字节缓冲：整块头部只在这条缓冲要长时碰一次分配器
        mutable std::unordered_map<std::string, std::string> m_singleValues;      ///< 单值视图，首次查询时由权威记录建出
        mutable bool                                         m_isViewStale{true}; ///< 视图是否已过期（写入或清空后置位，查询前重建）
    };
} // namespace AsynGyanis::Net

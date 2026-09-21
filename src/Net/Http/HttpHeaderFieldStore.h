/**
 * @file HttpHeaderFieldStore.h
 * @brief 请求与响应共用的头部字段存储：权威记录 + 单值视图
 * @author Gyanis
 * @date 2026-09-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 *
 * @details `HttpRequest` 与 `HttpResponse` 各需一套同样的头部存储：按加入顺序的权威记录、
 *          按需重建的单值视图、以及名字归一化与「可重复头部」名单。此前两处各写一份完全对称的
 *          实现，本类把它收成一份；两边的策略差异（请求只追加、响应带校验与覆盖）留在各自类里。
 */

#pragma once

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
     * @details 两份数据：m_fields 是按加入顺序的权威记录（可重复头部各占一项），
     *          m_singleValues 是名到值的单值视图，只在真正被整表查询时才重建——按名字取单值
     *          走 get()/values() 直接读权威记录，多数请求只读一两个头部，为它们建表等于白付
     *          若干次节点分配与字符串拷贝。
     */
    class HttpHeaderFieldStore
    {
    public:
        /**
         * @brief 一条头部记录
         */
        struct HeaderField
        {
            std::string name;  ///< 已归一化为小写的头部名
            std::string value; ///< 头部值原文
        };

        using HeaderFieldList = std::vector<HeaderField>; ///< 权威记录的有序容器类型

        /**
         * @brief 追加一条头部记录（名字就地归一化为小写）
         * @param name 头部名，按值接收后就地改写
         * @param value 头部值
         */
        void append(std::string name, std::string value);

        /**
         * @brief 覆盖或追加一条非可重复头部
         * @details 同名已有记录就地覆盖值，条目位置仍停在首次设置处；缺席则追加到末尾。
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
         * @brief 取单值视图（名 → 合并后的值）
         * @details 可重复头部只留首条；普通头部同名多条按 RFC 7230 §3.2.2 以 ", " 合并。
         * @return const std::unordered_map<std::string, std::string>& 视图引用
         */
        [[nodiscard]] const std::unordered_map<std::string, std::string> &singleValueView() const;

        /// 权威记录（序列化按它的顺序进行）
        [[nodiscard]] const HeaderFieldList &fields() const noexcept
        {
            return m_fields;
        }

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
         * @return HeaderFieldList::iterator 命中位置；未命中为 end()
         */
        [[nodiscard]] HeaderFieldList::iterator findField(std::string_view name);

        /// 按需要重建单值视图（调用前视图已标脏）
        void rebuildSingleValueView() const;

        HeaderFieldList m_fields; ///< 权威记录，按加入顺序保存，决定序列化顺序
        mutable std::unordered_map<std::string, std::string> m_singleValues; ///< 单值视图，首次查询时由权威记录建出
        mutable bool m_isViewStale{true}; ///< 视图是否已过期（写入或清空后置位，查询前重建）
    };
} // namespace AsynGyanis::Net

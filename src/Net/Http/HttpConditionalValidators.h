/**
 * @file HttpConditionalValidators.h
 * @brief 条件请求的验证器比较（RFC 9110 §8.8.3 强/弱比较与 §13.1/§13.2 的两种列表语义）
 * @author Gyanis
 * @date 2026-10-02
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <string_view>

namespace AsynGyanis::Net
{
    /**
     * @brief 去掉弱验证器标记 `W/`
     * @details `W/` 必须是大写 W（§8.8.3 的固定写法），小写 `w/` 不是标记而是标签文本的一部分。
     * @param validator 待处理的验证器原文
     * @return std::string_view 去掉前缀后的标签；没有前缀时原样返回
     */
    [[nodiscard]] inline constexpr std::string_view stripWeakValidatorPrefix(const std::string_view validator) noexcept
    {
        return validator.starts_with("W/") ? validator.substr(2) : validator;
    }

    /**
     * @brief 判断验证器是不是弱验证器
     * @param validator 待判断的验证器原文
     * @return true 带 `W/` 前缀
     */
    [[nodiscard]] inline constexpr bool isWeakValidator(const std::string_view validator) noexcept
    {
        return validator.starts_with("W/");
    }

    namespace Detail
    {
        /**
         * @brief 逐个逗号切分验证器列表，把每一项交给判定函数
         * @details 两项判定共用这一段切分，差别只在回调里怎么比：弱比较要剥 `W/` 且允许 `*`，
         *          强比较不剥且 `*` 只在整值时有效。切分只裁首尾的 SP/HTAB（§5.6.4 的可观察空白）。
         * @param listValue 头部原文
         * @param predicate 收到「已裁空白的单项」，返回 true 即整表命中并停止
         * @return true 任一单项使 predicate 为真
         */
        template<typename Predicate>
        inline constexpr bool anyValidatorInList(const std::string_view listValue, const Predicate predicate)
        {
            std::string_view remainder = listValue;
            while (true)
            {
                const std::size_t commaPosition = remainder.find(',');
                std::string_view  candidate     = remainder.substr(0, commaPosition);
                // 首尾空白：SP 与 HTAB 两种，与 HttpHeaderRules 里的字段值可观察空白同一口径
                while (!candidate.empty() && (candidate.front() == ' ' || candidate.front() == '\t'))
                {
                    candidate.remove_prefix(1);
                }
                while (!candidate.empty() && (candidate.back() == ' ' || candidate.back() == '\t'))
                {
                    candidate.remove_suffix(1);
                }
                if (predicate(candidate))
                {
                    return true;
                }
                if (commaPosition == std::string_view::npos)
                {
                    return false;
                }
                remainder = remainder.substr(commaPosition + 1);
            }
        }
    } // namespace Detail

    /**
     * @brief `If-None-Match` 的**弱比较**（§13.1.2）
     * @details 弱比较忽略 `W/` 标记，因此 `W/"x"` 与 `"x"` 算同一标签；`*` 作为整值或列表中的
     *          任一项都命中任意资源。
     * @param listValue 头部原文（可以是逗号分隔的列表）
     * @param entityTag 本资源当前的强标签
     * @return true 命中（调用方据此回 304）
     */
    [[nodiscard]] inline constexpr bool weakEntityTagListMatches(const std::string_view listValue, const std::string_view entityTag)
    {
        return Detail::anyValidatorInList(listValue,
                                          [entityTag](const std::string_view candidate) { return candidate == "*" || stripWeakValidatorPrefix(candidate) == entityTag; });
    }

    /**
     * @brief `If-Match` 的**强比较**（§13.2.1 与 §8.8.3.2）
     * @details 与弱比较的两处差别是规范要求的，不是疏忽：① 带 `W/` 的标签**永不**强匹配（连 `"x"`
     *          与 `W/"x"` 也不等）；② `*` 只在整值为 `*` 时命中，作为列表一项时不代表任意资源。
     * @param listValue 头部原文
     * @param entityTag 本资源当前的强标签
     * @param resourceExists 目标资源是否存在：`If-Match: *` 表达的是「只要它还在」
     * @return true 条件成立；false 时按 §13.2.2 回 412
     */
    [[nodiscard]] inline constexpr bool strongEntityTagListMatches(const std::string_view listValue, const std::string_view entityTag, const bool resourceExists)
    {
        std::string_view whole = listValue;
        while (!whole.empty() && (whole.front() == ' ' || whole.front() == '\t'))
        {
            whole.remove_prefix(1);
        }
        while (!whole.empty() && (whole.back() == ' ' || whole.back() == '\t'))
        {
            whole.remove_suffix(1);
        }
        if (whole == "*")
        {
            return resourceExists;
        }
        return Detail::anyValidatorInList(whole, [entityTag](const std::string_view candidate)
                                          { return !candidate.empty() && candidate != "*" && !isWeakValidator(candidate) && candidate == entityTag; });
    }
} // namespace AsynGyanis::Net

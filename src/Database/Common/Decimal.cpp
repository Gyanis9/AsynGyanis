/**
 * @file Decimal.cpp
 * @brief 精确十进制小数的解析与标度整数构造
 * @author Gyanis
 * @date 2026-10-18
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */
#include "Database/Common/Decimal.h"

#include <utility>

namespace AsynGyanis::Database
{
    std::optional<Decimal> Decimal::tryParse(const std::string_view text) noexcept
    {
        std::size_t offset = 0;
        if (!text.empty() && text.front() == '-')
        {
            offset = 1;
        }

        // 整数位至少一位：'.', 与 ".5" 都不认（引擎不会这样交回，接受只是多造一种等价写法）
        std::size_t integerDigits = 0;
        while (offset + integerDigits < text.size() && text[offset + integerDigits] >= '0' && text[offset + integerDigits] <= '9')
        {
            ++integerDigits;
        }
        if (integerDigits == 0)
        {
            return std::nullopt;
        }
        offset += integerDigits;

        std::size_t fractionDigits = 0;
        if (offset < text.size() && text[offset] == '.')
        {
            ++offset;
            while (offset + fractionDigits < text.size() && text[offset + fractionDigits] >= '0' && text[offset + fractionDigits] <= '9')
            {
                ++fractionDigits;
            }
            // 有小数点就必须跟至少一位数字："1." 在引擎侧各有不同含义，这里不猜
            if (fractionDigits == 0)
            {
                return std::nullopt;
            }
            offset += fractionDigits;
        }

        // 只允许上面这条文法吃掉全部字节："1e3"、"1_000"、" 1"、"1 "、"1.2.3"、"NaN" 都从这里被挡掉
        if (offset != text.size())
        {
            return std::nullopt;
        }

        return Decimal{std::string(text)};
    }

    Decimal Decimal::fromScaledInteger(const std::int64_t unscaledValue, const std::uint32_t scale)
    {
        // 幅度按无符号取：-INT64_MIN 在有符号域里会溢出，而 INT64_MIN 本身是一个合法金额
        const bool          isNegative    = unscaledValue < 0;
        const auto          unsignedValue = static_cast<std::uint64_t>(unscaledValue);
        const std::uint64_t magnitude     = isNegative ? (std::uint64_t{0} - unsignedValue) : unsignedValue;

        std::string digits = std::to_string(magnitude);
        std::string text;
        // 负零不存在：-0 与 0 是同一个数，写成 "-0.000" 只是把符号留在文本里骗人
        if (isNegative && magnitude != 0)
        {
            text += '-';
        }
        if (scale == 0)
        {
            return Decimal{text + digits};
        }

        // 位数不足时补前导零到「小数点左边至少还有一位」：5 / 标度 3 → "0.005"
        while (digits.size() <= scale)
        {
            digits.insert(digits.begin(), '0');
        }
        digits.insert(digits.end() - static_cast<std::ptrdiff_t>(scale), '.');
        return Decimal{text + digits};
    }

    std::uint32_t Decimal::scale() const noexcept
    {
        const std::size_t dotPosition = m_text.find('.');
        if (dotPosition == std::string::npos)
        {
            return 0;
        }
        return static_cast<std::uint32_t>(m_text.size() - dotPosition - 1);
    }

} // namespace AsynGyanis::Database

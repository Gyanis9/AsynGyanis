/**
 * @file PaddedFieldText.h
 * @brief 定宽字段追加：把一段文本追加进缓冲并按需右补空格
 * @author Gyanis
 * @date 2026-09-22
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

namespace AsynGyanis::Base
{
    /// 等级字段的最小宽度：`logLevelToString` 本就固定交回 5 字符，补齐只对将来改名有意义
    inline constexpr std::size_t kLevelFieldWidth = 5U;

    /// Debug 版式里「文件:行号」字段的最小宽度：短名对齐便于人眼扫读
    inline constexpr std::size_t kSourceLocationFieldWidth = 13U;

    /**
     * @brief 追加一段文本并把它右补齐到给定最小宽度，口径与 `std::format` 的 `{:<N}` 逐字节一致
     * @details `{:<N}` 量的是**显示宽度**：ASCII 一格一字，非 ASCII 的宽字符（中日韩）算两格。
     *          自己查那张宽度表不值得，因此分两条路：整段都是 ASCII 时码元数就是显示宽度，就地补空格；
     *          掺了非 ASCII（源文件名带中文）才退回 `std::format` 那一份实现。版式里只有「文件:行号」
     *          可能走到第二条，等级名恒为 ASCII。
     * @param out 目标缓冲，现有内容保留
     * @param text 待追加的字段文本
     * @param minimumWidth 补齐后的最小显示宽度；已达到的原样输出，不截断
     */
    inline void appendPaddedField(std::string &out, const std::string_view text, const std::size_t minimumWidth)
    {
        const bool isPlainAscii = std::ranges::all_of(text, [](const char character)
        {
            return static_cast<unsigned char>(character) < 0x80U;
        });
        if (!isPlainAscii)
        {
            static_cast<void>(std::format_to(std::back_inserter(out), "{:<{}}", text, minimumWidth));
            return;
        }

        out.append(text);
        // 循环条件即「还差几格」，无需先算差值（也就没有无符号减法下溢的可能）
        for (std::size_t written = text.size(); written < minimumWidth; ++written)
        {
            out.push_back(' ');
        }
    }
} // namespace AsynGyanis::Base

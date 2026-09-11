/**
 * @file ParserPosition.h
 * @brief 解析错误发生处的行列与字节偏移信息
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <format>
#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 解析位置
     *
     * @details 行号与列号一律从 1 起始，便于直接对照编辑器显示；offset 为字节偏移，
     *          供需要精确定位到原始缓冲区的使用方使用。
     */
    struct ParserPosition
    {
        std::size_t lineNumber{1};   ///< 行号，从 1 起始
        std::size_t columnNumber{1}; ///< 列号，从 1 起始
        std::size_t offset{0};       ///< 相对输入起始的字节偏移

        /**
         * @brief 生成人类可读的位置描述
         * @return std::string 形如 "line 3, column 12" 的文本
         */
        [[nodiscard]] std::string describe() const
        {
            return std::format("line {}, column {}", lineNumber, columnNumber);
        }
    };
} // namespace AsynGyanis::Base

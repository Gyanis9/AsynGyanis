/**
 * @file ParserError.cpp
 * @brief 文本解析失败异常，携带出错位置
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Parser/ParserError.h"

#include <format>
#include <string>

namespace AsynGyanis::Base
{
    ParserError::ParserError(const std::string &message, const ParserPosition &position, const std::source_location &sourceLocation) :
        Exception(std::format("解析错误（{}）：{}", position.describe(), message), sourceLocation)
        , m_position(position)
        , m_reason(message)
    {
    }

    const ParserPosition &ParserError::position() const noexcept
    {
        return m_position;
    }

    const std::string &ParserError::reason() const noexcept
    {
        return m_reason;
    }
} // namespace AsynGyanis::Base

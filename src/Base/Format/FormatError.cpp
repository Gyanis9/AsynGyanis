/**
 * @file FormatError.cpp
 * @brief 文本解析失败异常，携带出错位置
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Format/FormatError.h"

#include <format>
#include <string>

namespace AsynGyanis::Base
{
    FormatError::FormatError(const std::string &message, const TextPosition &position, const std::source_location &sourceLocation) :
        FormatError(FormatErrorKind::None, message, position, sourceLocation)
    {
    }

    FormatError::FormatError(const FormatErrorKind kind, const std::string &message, const TextPosition &position, const std::source_location &sourceLocation) :
        Exception(std::format("解析错误（{}）：{}", position.describe(), message), sourceLocation)
        , m_position(position)
        , m_reason(message)
        , m_kind(kind)
    {
    }

    const TextPosition &FormatError::position() const noexcept
    {
        return m_position;
    }

    const std::string &FormatError::reason() const noexcept
    {
        return m_reason;
    }

    FormatErrorKind FormatError::kind() const noexcept
    {
        return m_kind;
    }
} // namespace AsynGyanis::Base

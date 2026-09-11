/**
 * @file Exception.cpp
 * @brief 项目统一异常基类，记录异常消息与抛出位置
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Exception/Exception.h"

#include <format>

namespace AsynGyanis::Base
{
    Exception::Exception(const std::string &message, const std::source_location &sourceLocation) :
        std::runtime_error(formatMessage(message, sourceLocation)), m_location(sourceLocation)
    {
    }

    const std::source_location &Exception::location() const noexcept
    {
        return m_location;
    }

    std::string Exception::formatMessage(const std::string &message, const std::source_location &sourceLocation)
    {
        return std::format("[异常] {} [{}:{} in {}]", message, sourceLocation.file_name(), sourceLocation.line(), sourceLocation.function_name());
    }
} // namespace AsynGyanis::Base

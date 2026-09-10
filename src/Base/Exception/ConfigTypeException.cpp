/**
 * @file ConfigTypeException.cpp
 * @brief 配置值类型不匹配异常的实现
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Exception/ConfigTypeException.h"

#include <string>

namespace AsynGyanis::Base
{
    ConfigTypeException::ConfigTypeException(const std::string &key, const std::string &expectedType, const std::string &actualType, const std::source_location &sourceLocation) :
        ConfigException("Type mismatch for key '" + key + "': expected " + expectedType + ", got " + actualType, sourceLocation)
        , m_key(key)
        , m_expectedType(expectedType)
        , m_actualType(actualType)
    {
    }

    const std::string &ConfigTypeException::key() const noexcept
    {
        return m_key;
    }

    const std::string &ConfigTypeException::expectedType() const noexcept
    {
        return m_expectedType;
    }

    const std::string &ConfigTypeException::actualType() const noexcept
    {
        return m_actualType;
    }
} // namespace AsynGyanis::Base

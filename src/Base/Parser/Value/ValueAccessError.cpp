/**
 * @file ValueAccessError.cpp
 * @brief 文档值访问失败异常：类型不匹配或成员不存在
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Parser/Value/ValueAccessError.h"

#include <format>
#include <string>
#include <utility>

namespace AsynGyanis::Base
{
    ValueAccessError::ValueAccessError(const std::string &key, const std::string &expectedType, const std::string &actualType, const std::source_location &sourceLocation) :
        ValueAccessError(std::format("键 '{}' 类型不匹配：期望 {}，实际 {}", key, expectedType, actualType),
                         key, expectedType, actualType, sourceLocation)
    {
    }

    ValueAccessError ValueAccessError::missingMember(const std::string &key, const std::source_location &sourceLocation)
    {
        return {
                std::format("成员不存在：'{}'", key), key, std::string{}, std::string{},
                sourceLocation
        };
    }

    ValueAccessError::ValueAccessError(const std::string &         message,
                                       std::string                 key,
                                       std::string                 expectedType,
                                       std::string                 actualType,
                                       const std::source_location &sourceLocation) :
        Exception(message, sourceLocation)
        , m_key(std::move(key))
        , m_expectedType(std::move(expectedType))
        , m_actualType(std::move(actualType))
    {
    }

    const std::string &ValueAccessError::key() const noexcept
    {
        return m_key;
    }

    const std::string &ValueAccessError::expectedType() const noexcept
    {
        return m_expectedType;
    }

    const std::string &ValueAccessError::actualType() const noexcept
    {
        return m_actualType;
    }
} // namespace AsynGyanis::Base

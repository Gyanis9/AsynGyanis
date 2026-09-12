/**
 * @file FormatValueType.cpp
 * @brief 文档值的类型枚举与类型名映射
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Format/Value/FormatValueType.h"

namespace AsynGyanis::Base
{
    const char *typeName(const FormatValueType type) noexcept
    {
        switch (type)
        {
            case FormatValueType::Null:
                return "null";
            case FormatValueType::Bool:
                return "bool";
            case FormatValueType::Int:
                return "int";
            case FormatValueType::Double:
                return "double";
            case FormatValueType::String:
                return "string";
            case FormatValueType::Array:
                return "array";
            case FormatValueType::Object:
                return "object";
            case FormatValueType::UInt:
                return "uint";
            default:
                return "unknown";
        }
    }
} // namespace AsynGyanis::Base

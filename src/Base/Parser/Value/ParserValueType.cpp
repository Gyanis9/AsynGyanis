/**
 * @file ParserValueType.cpp
 * @brief 文档值的类型枚举与类型名映射
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Parser/Value/ParserValueType.h"

namespace AsynGyanis::Base
{
    const char *typeName(const ParserValueType type) noexcept
    {
        switch (type)
        {
            case ParserValueType::Null:
                return "null";
            case ParserValueType::Bool:
                return "bool";
            case ParserValueType::Int:
                return "int";
            case ParserValueType::Double:
                return "double";
            case ParserValueType::String:
                return "string";
            case ParserValueType::Array:
                return "array";
            case ParserValueType::Object:
                return "object";
            default:
                return "unknown";
        }
    }
} // namespace AsynGyanis::Base

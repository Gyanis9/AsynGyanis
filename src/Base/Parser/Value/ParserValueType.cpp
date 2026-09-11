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

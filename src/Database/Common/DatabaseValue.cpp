#include "Database/Common/DatabaseValue.h"

#include <type_traits>
#include <variant>

namespace AsynGyanis::Database
{
    const char *databaseValueTypeName(const DatabaseValue &value) noexcept
    {
        // 用 visit 按实际类型分支，避免手写 variant 下标常量：
        // 一旦上面的 DatabaseValue 调整顺序，下标映射会静默错位
        return std::visit(
            [](const auto &typedValue) -> const char *
            {
                using ValueType = std::remove_cvref_t<decltype(typedValue)>;
                if constexpr (std::is_same_v<ValueType, std::monostate>)
                {
                    return "Null";
                }
                else if constexpr (std::is_same_v<ValueType, bool>)
                {
                    return "Bool";
                }
                else if constexpr (std::is_same_v<ValueType, std::int64_t>)
                {
                    return "Int64";
                }
                else if constexpr (std::is_same_v<ValueType, double>)
                {
                    return "Double";
                }
                else if constexpr (std::is_same_v<ValueType, std::string>)
                {
                    return "String";
                }
                else if constexpr (std::is_same_v<ValueType, std::vector<std::string>>)
                {
                    return "List";
                }
                else if constexpr (std::is_same_v<ValueType, std::vector<std::uint8_t>>)
                {
                    return "Bytes";
                }
                else
                {
                    return "Hash";
                }
            },
            value);
    }

} // namespace AsynGyanis::Database

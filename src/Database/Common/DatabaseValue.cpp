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
                []<typename T0>([[maybe_unused]] const T0 &typedValue) -> const char *
                {
                    using ValueType = std::remove_cvref_t<T0>;
                    if constexpr (std::is_same_v<ValueType, std::monostate>)
                    {
                        return "Null";
                    } else if constexpr (std::is_same_v<ValueType, bool>)
                    {
                        return "Bool";
                    } else if constexpr (std::is_same_v<ValueType, std::int64_t>)
                    {
                        return "Int64";
                    } else if constexpr (std::is_same_v<ValueType, double>)
                    {
                        return "Double";
                    } else if constexpr (std::is_same_v<ValueType, std::string>)
                    {
                        return "String";
                    } else if constexpr (std::is_same_v<ValueType, std::vector<std::string>>)
                    {
                        return "List";
                    } else if constexpr (std::is_same_v<ValueType, std::vector<std::uint8_t>>)
                    {
                        return "Bytes";
                    } else if constexpr (std::is_same_v<ValueType, Decimal>)
                    {
                        // 这一支必须显式写出来：末尾的 else 是给哈希用的兜底，新增备选不写就会被
                        // 它静默吞掉，于是日志与断言把一个十进制数报成 "Hash"
                        return "Decimal";
                    } else if constexpr (std::is_same_v<ValueType, Timestamp>)
                    {
                        // 同上：两个日期时间备选都要点名，且不能同名——同名会让上面的撞判据静默放过
                        return "Timestamp";
                    } else if constexpr (std::is_same_v<ValueType, LocalDateTime>)
                    {
                        return "LocalDateTime";
                    } else
                    {
                        return "Hash";
                    }
                },
                value);
    }

} // namespace AsynGyanis::Database

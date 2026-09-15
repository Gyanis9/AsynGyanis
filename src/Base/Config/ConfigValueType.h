/**
 * @file ConfigValueType.h
 * @brief 配置模块的值类型别名与配置文件后缀、键路径等通用工具
 * @author Gyanis
 * @date 2026-09-15
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Config/ConfigValue.h"

#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Base
{
    /**
     * @brief 配置值类型枚举
     *
     * @details 即 nlohmann_json 的原生 value_t（null / boolean / number_integer / number_unsigned /
     *          number_float / string / array / object / binary / discarded）。原生 JSON 把非负整数
     *          放进 number_unsigned、负整数放进 number_integer，因此比较整数类型时两者要一并考虑。
     */
    using ConfigValueType = ConfigValue::value_t;

    /**
     * @brief 将配置值类型枚举转换为可读字符串。
     * @param type 配置值类型枚举。
     * @return const char* 类型名称（null/bool/int/uint/double/string/array/object/binary/discarded），未知取值返回 "unknown"。
     */
    [[nodiscard]] const char *typeName(ConfigValueType type) noexcept;

    /**
     * @brief 判断文件路径是否为 YAML 配置文件后缀。
     * @param filePath 待检查的文件路径。
     * @return bool 当后缀为 .yaml 或 .yml（大小写不敏感）时返回 true。
     */
    [[nodiscard]] bool isYamlFile(std::string_view filePath) noexcept;

    /**
     * @brief 判断文件路径是否为 JSON 配置文件后缀。
     * @param filePath 待检查的文件路径。
     * @return bool 当后缀为 .json（大小写不敏感）时返回 true。
     */
    [[nodiscard]] bool isJsonFile(std::string_view filePath) noexcept;

    /**
     * @brief 判断文件路径是否为受支持的配置文件（JSON 或 YAML）。
     * @param filePath 待检查的文件路径。
     * @return bool 当后缀为 .json/.yaml/.yml（大小写不敏感）时返回 true。
     */
    [[nodiscard]] bool isConfigFile(std::string_view filePath) noexcept;

    /**
     * @brief 使用指定分隔符拆分配置键字符串。
     * @details 连续分隔符与首尾分隔符产生的空片段会被忽略。
     * @param key 待拆分的原始键字符串。
     * @param delimiter 分隔字符。
     * @return std::vector<std::string> 拆分后的键片段列表。
     */
    std::vector<std::string> splitKey(std::string_view key, char delimiter = '.');
} // namespace AsynGyanis::Base

/**
 * @file ConfigValueType.h
 * @brief 配置模块的值类型别名与配置文件后缀、键路径等通用工具
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Parser/Value/ParserValueType.h"

#include <string>
#include <string_view>
#include <vector>

namespace AsynGyanis::Base
{
    /**
     * @brief 配置值类型的历史别名
     *
     * @details 值模型已上收到 Parser（Base/Parser/Value/ParserValue.h），配置侧保留
     *          原有名字以免既有代码与用例改名；两者是同一个类型，不存在第二份实现。
     */
    using ConfigValueType = ParserValueType;

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

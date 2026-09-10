/**
 * @file ConfigSchema.h
 * @brief 配置 schema 约束条目定义与校验入口
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Config/ConfigValidationResult.h"
#include "Base/Config/ConfigValue.h"
#include "Base/Config/ConfigValueType.h"

#include <optional>
#include <string>
#include <vector>

namespace AsynGyanis::Base
{
    /**
     * @brief 单条配置项的 schema 约束。
     * @details 用于启动与热加载时的自检：类型不符、必需键缺失、数值越界都会记入错误。
     */
    struct ConfigSchemaEntry
    {
        std::string                    key;             ///< 配置键
        std::optional<ConfigValueType> expectedType;    ///< 期望类型，空表示不限制
        bool                           required{false}; ///< 是否为必需键
        std::optional<double>          minimum;         ///< 数值下限（仅对 Int/Double 生效）
        std::optional<double>          maximum;         ///< 数值上限（仅对 Int/Double 生效）
    };

    /// 配置 schema：约束条目列表
    using ConfigSchema = std::vector<ConfigSchemaEntry>;

    /**
     * @brief 对指定配置字典执行 schema 全部约束（存在性/类型/数值范围）。
     * @details 供热重载提交路径与 setSchema/validateSchema 共用的纯函数，
     *          不修改传入字典，也不产生日志。
     * @param values 扁平化配置字典。
     * @param schema 约束条目列表。
     * @return ConfigValidationResult 校验结果。
     */
    ConfigValidationResult runSchemaValidation(const ConfigKeyValueMap &values, const ConfigSchema &schema);
} // namespace AsynGyanis::Base

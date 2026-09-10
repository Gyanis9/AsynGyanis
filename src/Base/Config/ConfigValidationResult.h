/**
 * @file ConfigValidationResult.h
 * @brief 配置 schema 校验结果结构
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <string>
#include <vector>

namespace AsynGyanis::Base
{
    /**
     * @brief 配置 schema 校验结果。
     *
     * @details 一次性收集全部约束的违反情况，errors 每条对应一个键的问题，
     *          避免首个错误即中断导致排查需要反复重启。
     */
    struct ConfigValidationResult
    {
        bool                     valid{true}; ///< 全部约束满足时为 true
        std::vector<std::string> errors;      ///< 校验失败描述列表（每条对应一个键的问题）

        /**
         * @brief 隐式转换为 bool，表示校验是否通过。
         * @return 校验通过返回 true。
         */
        explicit operator bool() const noexcept
        {
            return valid;
        }
    };
} // namespace AsynGyanis::Base

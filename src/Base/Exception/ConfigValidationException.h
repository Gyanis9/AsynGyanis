/**
 * @file ConfigValidationException.h
 * @brief 配置项校验失败异常
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/ConfigException.h"

#include <source_location>
#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 配置项校验失败异常
     *
     * @details schema 校验（必需键缺失、类型不符、数值越界）以异常形式上报时使用本类，
     *          批量场景请优先使用 ConfigValidationResult 收集全部错误。
     */
    class ConfigValidationException : public ConfigException
    {
    public:
        /**
         * @brief 构造配置项校验失败异常
         * @details 调用 ConfigException("配置键 '键' 校验失败：原因", sourceLocation)，
         *          把出错键与违反的约束原因分开保存。
         * @param key 校验失败的配置键
         * @param reason 校验失败原因描述
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        ConfigValidationException(const std::string &key, const std::string &reason, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 获取校验失败的配置键
         * @details 派生类新增的访问器（非重写），返回构造时传入的原始键文本。
         * @return const std::string& 配置键常量引用
         */
        [[nodiscard]] const std::string &key() const noexcept;

    private:
        std::string m_key; ///< 校验失败的配置键
    };
} // namespace AsynGyanis::Base

/**
 * @file ConfigException.h
 * @brief 配置模块异常基类
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Exception/Exception.h"

#include <source_location>
#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 配置模块异常基类
     *
     * @details 所有配置相关异常（文件读取、解析、键缺失、类型不符、校验失败）
     *          都派生自本类，便于上层一次性捕获配置错误。
     */
    class ConfigException : public Exception
    {
    public:
        /**
         * @brief 构造配置模块异常
         * @details 相较基类仅在消息前附加 "配置错误：" 领域前缀。
         * @param message 配置错误描述
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit ConfigException(const std::string &message, const std::source_location &sourceLocation = std::source_location::current());
    };
} // namespace AsynGyanis::Base

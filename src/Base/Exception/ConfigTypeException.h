/**
 * @file ConfigTypeException.h
 * @brief 配置值类型不匹配异常
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
     * @brief 配置值类型不匹配异常
     *
     * @details ConfigValue 的强类型访问（as<T>() / asBool() 等）在底层变体
     *          持有其他类型时抛出，消息同时给出期望类型与实际类型。
     */
    class ConfigTypeException : public ConfigException
    {
    public:
        /**
         * @brief 构造配置值类型不匹配异常
         * @details 调用 ConfigException("Type mismatch for key '键': expected 期望类型, got 实际类型", sourceLocation)，
         *          三个字段各自保留，便于上层按类型做统计或提示。
         * @param key 发生类型错误的配置键
         * @param expectedType 调用方期望的类型名称
         * @param actualType 配置值实际持有的类型名称
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        ConfigTypeException(const std::string &key, const std::string &expectedType, const std::string &actualType, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 获取发生类型错误的配置键
         * @details 派生类新增的访问器（非重写）；构造 ConfigValue 内部抛出时可能为占位名 "&lt;unknown&gt;"。
         * @return const std::string& 配置键常量引用
         */
        [[nodiscard]] const std::string &key() const noexcept;

        /**
         * @brief 获取调用方期望的类型名称
         * @details 派生类新增的访问器（非重写），取自 typeNameOf&lt;T&gt;()。
         * @return const std::string& 期望类型名称常量引用
         */
        [[nodiscard]] const std::string &expectedType() const noexcept;

        /**
         * @brief 获取配置值实际持有的类型名称
         * @details 派生类新增的访问器（非重写），取自 typeName(type())。
         * @return const std::string& 实际类型名称常量引用
         */
        [[nodiscard]] const std::string &actualType() const noexcept;

    private:
        std::string m_key;          ///< 发生类型错误的配置键
        std::string m_expectedType; ///< 调用方期望的类型名称
        std::string m_actualType;   ///< 配置值实际持有的类型名称
    };
} // namespace AsynGyanis::Base

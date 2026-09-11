/**
 * @file ValueAccessError.h
 * @brief 文档值访问失败异常：类型不匹配或成员不存在
 * @author Gyanis
 * @date 2026-09-11
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
     * @brief 文档值访问失败异常
     *
     * @details 属于 Parser 值模型自身的错误，因此直接派生自项目异常基类而非配置异常，
     *          使 JSON/YAML 以及后续格式在 Config 之外也能安全抛出同一异常。
     *          两类失败共用本类型：强类型访问（as&lt;T&gt;() 等）与实际持有类型不符；
     *          按键或下标取成员时目标不存在。
     */
    class ValueAccessError : public Exception
    {
    public:
        /**
         * @brief 构造类型不匹配异常
         * @details 消息形如 "键 'a.b' 类型不匹配：期望 int，实际 string"，
         *          三个字段各自保留，便于上层做提示或统计。
         * @param key 发生类型错误的键或下标描述
         * @param expectedType 调用方期望的类型名称
         * @param actualType 文档值实际持有的类型名称
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        ValueAccessError(const std::string &         key,
                         const std::string &         expectedType,
                         const std::string &         actualType,
                         const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 构造成员不存在异常
         * @details 消息形如 "成员不存在：'a.b'"，期望与实际类型均为空串。
         * @param key 缺失的键或越界下标描述
         * @param sourceLocation 异常抛出位置，默认取调用点
         * @return ValueAccessError 构造好的异常对象
         */
        [[nodiscard]] static ValueAccessError missingMember(const std::string &key, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 获取发生错误的键或下标描述
         * @details 新增访问器（非重写）；由 ParserValue 内部抛出时可能为占位名 "&lt;unknown&gt;"。
         * @return const std::string& 键名常量引用
         */
        [[nodiscard]] const std::string &key() const noexcept;

        /**
         * @brief 获取调用方期望的类型名称
         * @details 新增访问器（非重写），取自 typeNameOf&lt;T&gt;()；成员不存在时为空串。
         * @return const std::string& 期望类型名称常量引用
         */
        [[nodiscard]] const std::string &expectedType() const noexcept;

        /**
         * @brief 获取文档值实际持有的类型名称
         * @details 新增访问器（非重写），取自 typeName(type())；成员不存在时为空串。
         * @return const std::string& 实际类型名称常量引用
         */
        [[nodiscard]] const std::string &actualType() const noexcept;

    private:
        /**
         * @brief 以完整消息构造异常的内部通道
         * @param message 已拼装好的异常消息
         * @param key 键或下标描述
         * @param expectedType 期望类型名称，可为空
         * @param actualType 实际类型名称，可为空
         * @param sourceLocation 异常抛出位置
         */
        ValueAccessError(const std::string &         message,
                         std::string                 key,
                         std::string                 expectedType,
                         std::string                 actualType,
                         const std::source_location &sourceLocation);

        std::string m_key;          ///< 发生错误的键或下标描述
        std::string m_expectedType; ///< 调用方期望的类型名称
        std::string m_actualType;   ///< 文档值实际持有的类型名称
    };
} // namespace AsynGyanis::Base

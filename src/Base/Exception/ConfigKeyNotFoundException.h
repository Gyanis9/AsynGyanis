/**
 * @file ConfigKeyNotFoundException.h
 * @brief 配置键不存在异常
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
     * @brief 配置键不存在异常
     *
     * @details 在扁平配置字典或嵌套对象/数组中按点号路径查找失败时抛出，
     *          数组越界时下标会以 "[index]" 形式作为键记录。
     */
    class ConfigKeyNotFoundException : public ConfigException
    {
    public:
        /**
         * @brief 构造配置键不存在异常
         * @details 调用 ConfigException("配置键不存在：'键'", sourceLocation)，
         *          并把原始键单独保存供 key() 读取。
         * @param key 查找失败的配置键（点号路径或 "[下标]"）
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        explicit ConfigKeyNotFoundException(const std::string &key, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 获取查找失败的配置键
         * @details 派生类新增的访问器（非重写），返回构造时传入的原始键文本。
         * @return const std::string& 配置键常量引用
         */
        [[nodiscard]] const std::string &key() const noexcept;

    private:
        std::string m_key; ///< 查找失败的配置键
    };
} // namespace AsynGyanis::Base

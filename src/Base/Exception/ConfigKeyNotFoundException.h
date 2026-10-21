/**
 * @file ConfigKeyNotFoundException.h
 * @brief 配置键不存在异常
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "AsynGyanisExport.h"

#include "Base/Exception/ConfigException.h"

#include <source_location>
#include <string>

namespace AsynGyanis::Base
{
    /**
     * @brief 配置键不存在异常
     *
     * @details 在扁平配置字典（点号路径就是键本身）中按点号路径查找失败时抛出；
     *          Base 不解析嵌套对象/数组的路径，也不使用 "[index]" 形式的下标。
     */
    class ASYN_BASE_API ConfigKeyNotFoundException : public ConfigException
    {
    public:
        /**
         * @brief 构造配置键不存在异常
         * @details 调用 ConfigException("配置键不存在：'键'", sourceLocation)，
         *          并把原始键单独保存供 key() 读取。
         * @param key 查找失败的配置键（扁平字典中的点号路径）
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

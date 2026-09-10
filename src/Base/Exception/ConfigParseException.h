/**
 * @file ConfigParseException.h
 * @brief 配置文件解析失败异常
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
     * @brief 配置文件解析失败异常
     *
     * @details 文件可打开但内容不符合 YAML/JSON 语法时抛出，
     *          与 ConfigFileException（IO 层失败）区分开。
     */
    class ConfigParseException : public ConfigException
    {
    public:
        /**
         * @brief 构造配置文件解析失败异常
         * @details 调用 ConfigException("Parse error in '路径': 原因", sourceLocation)，
         *          消息明确标识为解析阶段错误。
         * @param filePath 解析失败的配置文件路径
         * @param reason 解析器给出的失败原因描述
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        ConfigParseException(const std::string &filePath, const std::string &reason, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 获取解析失败的配置文件路径
         * @details 派生类新增的访问器（非重写），返回构造时保存的文件路径原值。
         * @return const std::string& 文件路径常量引用
         */
        [[nodiscard]] const std::string &filePath() const noexcept;

    private:
        std::string m_filePath; ///< 解析失败的配置文件路径
    };
} // namespace AsynGyanis::Base

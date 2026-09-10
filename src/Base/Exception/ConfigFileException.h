/**
 * @file ConfigFileException.h
 * @brief 配置文件读写失败异常
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
     * @brief 配置文件读写失败异常
     *
     * @details 用于文件不存在、无权限、无法打开等 IO 层面的配置错误，
     *          附带出错文件路径便于定位。
     */
    class ConfigFileException : public ConfigException
    {
    public:
        /**
         * @brief 构造配置文件读写失败异常
         * @details 调用 ConfigException("File '路径': 原因", sourceLocation)，
         *          在配置异常前缀之外再附加具体文件路径。
         * @param filePath 出错的配置文件路径
         * @param reason 失败原因描述
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        ConfigFileException(const std::string &filePath, const std::string &reason, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 获取出错的配置文件路径
         * @details 派生类新增的访问器（非重写），返回构造时保存的文件路径原值。
         * @return const std::string& 文件路径常量引用
         */
        [[nodiscard]] const std::string &filePath() const noexcept;

    private:
        std::string m_filePath; ///< 出错的配置文件路径
    };
} // namespace AsynGyanis::Base

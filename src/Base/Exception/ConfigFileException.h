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
     * @details 用于文件不存在、无权限、无法打开等 IO 层面的配置错误。
     * @note 本仓库自身的配置加载路径**不走这个类型**：那一条把错误汇进
     *       ConfigLoadResult::errors（见 ConfigManager::loadConfigFile）。本类是留给
     *       调用方按异常形状处理配置 IO 错误的公开出口，不是内部通道的残留，
     *       因此「src 里没人抛」不等于可以删——删它是砍公开 API。
     */
    class ConfigFileException : public ConfigException
    {
    public:
        /**
         * @brief 构造配置文件读写失败异常
         * @details 在配置异常前缀之外再附加具体文件路径。
         * @param filePath 出错的配置文件路径
         * @param reason 失败原因描述
         * @param sourceLocation 异常抛出位置，默认取调用点
         */
        ConfigFileException(const std::string &filePath, const std::string &reason, const std::source_location &sourceLocation = std::source_location::current());

        /**
         * @brief 获取出错的配置文件路径
         * @return const std::string& 文件路径常量引用
         */
        [[nodiscard]] const std::string &filePath() const noexcept;

    private:
        std::string m_filePath; ///< 出错的配置文件路径
    };
} // namespace AsynGyanis::Base

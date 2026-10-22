/**
 * @file ConfigDocument.h
 * @brief 配置文件的读取与解析（内部接口）
 * @author Gyanis
 * @date 2026-10-22
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "Base/Config/ConfigValue.h"

#include <filesystem>
#include <string>

namespace AsynGyanis::Base::ConfigDocument
{
    /**
     * @brief 读取并解析一份配置文件，按扩展名选择 JSON 或 YAML
     * @param filePath 配置文件路径
     * @param document 输出：根值；空文件或空文档为 null
     * @param error 输出：失败时的完整中文文案（已带文件与语法类别）
     * @return true 读取与解析都成功；false 失败且 error 已填好
     */
    [[nodiscard]] bool load(const std::filesystem::path &filePath, ConfigValue &document, std::string &error);
} // namespace AsynGyanis::Base::ConfigDocument

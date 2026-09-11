/**
 * @file ConfigFileException.cpp
 * @brief 配置文件读写失败异常
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Exception/ConfigFileException.h"

#include <string>

namespace AsynGyanis::Base
{
    ConfigFileException::ConfigFileException(const std::string &filePath, const std::string &reason, const std::source_location &sourceLocation) :
        ConfigException("文件 '" + filePath + "'：" + reason, sourceLocation)
        , m_filePath(filePath)
    {
    }

    const std::string &ConfigFileException::filePath() const noexcept
    {
        return m_filePath;
    }
} // namespace AsynGyanis::Base

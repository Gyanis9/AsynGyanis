/**
 * @file ConfigParseException.cpp
 * @brief 配置文件解析失败异常
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Exception/ConfigParseException.h"

#include <string>

namespace AsynGyanis::Base
{
    ConfigParseException::ConfigParseException(const std::string &filePath, const std::string &reason, const std::source_location &sourceLocation) :
        ConfigException("解析错误：'" + filePath + "'：" + reason, sourceLocation)
        , m_filePath(filePath)
    {
    }

    const std::string &ConfigParseException::filePath() const noexcept
    {
        return m_filePath;
    }
} // namespace AsynGyanis::Base

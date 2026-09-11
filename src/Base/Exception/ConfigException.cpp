/**
 * @file ConfigException.cpp
 * @brief 配置模块异常基类
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Exception/ConfigException.h"

#include <string>

namespace AsynGyanis::Base
{
    ConfigException::ConfigException(const std::string &message, const std::source_location &sourceLocation) :
        Exception("配置错误：" + message, sourceLocation)
    {
    }
} // namespace AsynGyanis::Base

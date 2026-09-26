#include "Base/Exception/ConfigException.h"

#include <string>

namespace AsynGyanis::Base
{
    ConfigException::ConfigException(const std::string &message, const std::source_location &sourceLocation) : Exception("配置错误：" + message, sourceLocation)
    {
    }
} // namespace AsynGyanis::Base

#include "Base/Exception/ConfigException.h"

#include <string>

namespace AsynGyanis::Base
{
    ConfigException::ConfigException(const std::string &message, const std::source_location &sourceLocation) :
        Exception("Config error: " + message, sourceLocation)
    {
    }
} // namespace AsynGyanis::Base

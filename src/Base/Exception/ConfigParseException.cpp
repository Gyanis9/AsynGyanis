#include "Base/Exception/ConfigParseException.h"

#include <string>

namespace AsynGyanis::Base
{
    ConfigParseException::ConfigParseException(const std::string &filePath, const std::string &reason, const std::source_location &sourceLocation) :
        ConfigException("Parse error in '" + filePath + "': " + reason, sourceLocation)
        , m_filePath(filePath)
    {
    }

    const std::string &ConfigParseException::filePath() const noexcept
    {
        return m_filePath;
    }
} // namespace AsynGyanis::Base

#include "Base/Exception/ConfigValidationException.h"

#include <string>

namespace AsynGyanis::Base
{
    ConfigValidationException::ConfigValidationException(const std::string &key, const std::string &reason, const std::source_location &sourceLocation) :
        ConfigException("配置键 '" + key + "' 校验失败：" + reason, sourceLocation)
        , m_key(key)
    {
    }

    const std::string &ConfigValidationException::key() const noexcept
    {
        return m_key;
    }
} // namespace AsynGyanis::Base

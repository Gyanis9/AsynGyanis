#include "Base/Exception/ConfigKeyNotFoundException.h"

#include <string>

namespace AsynGyanis::Base
{
    ConfigKeyNotFoundException::ConfigKeyNotFoundException(const std::string &key, const std::source_location &sourceLocation) :
        ConfigException("配置键不存在：'" + key + "'", sourceLocation), m_key(key)
    {
    }

    const std::string &ConfigKeyNotFoundException::key() const noexcept
    {
        return m_key;
    }
} // namespace AsynGyanis::Base

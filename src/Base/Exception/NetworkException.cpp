#include "Base/Exception/NetworkException.h"

#include <string>
#include <utility>

namespace AsynGyanis::Base
{
    NetworkException::NetworkException(const std::string &context, std::string remoteAddress, const std::source_location &sourceLocation) :
        SystemException(context, sourceLocation)
        , m_remoteAddress(std::move(remoteAddress))
    {
    }

    NetworkException::NetworkException(const std::string &context, const std::error_code errorCode, const std::string &remoteAddress, const std::source_location &sourceLocation) :
        SystemException(context + " (remote: " + remoteAddress + ")", errorCode, sourceLocation)
        , m_remoteAddress(remoteAddress)
    {
    }

    const std::string &NetworkException::remoteAddress() const noexcept
    {
        return m_remoteAddress;
    }
} // namespace AsynGyanis::Base

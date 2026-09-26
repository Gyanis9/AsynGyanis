#include "Base/Exception/NetworkException.h"

#include <string>
#include <utility>

namespace AsynGyanis::Base
{
    NetworkException::NetworkException(const std::string &context, std::string remoteAddress, const std::source_location &sourceLocation) :
        // 同一次失败不该因「调用方手上有没有错误码」而给出两种 what()：对端地址照同一口径写进
        // 上下文，错误码走 lastErrnoErrorCode 那条唯一通道（本重载只可能来自非显式传码的调用点）
        NetworkException(context, lastErrnoErrorCode(), remoteAddress, sourceLocation)
    {
    }

    NetworkException::NetworkException(const std::string &context, std::error_code errorCode, const std::string &remoteAddress, const std::source_location &sourceLocation) :
        SystemException(context + " (remote: " + remoteAddress + ")", errorCode, sourceLocation), m_remoteAddress(remoteAddress)
    {
    }

    const std::string &NetworkException::remoteAddress() const noexcept
    {
        return m_remoteAddress;
    }
} // namespace AsynGyanis::Base

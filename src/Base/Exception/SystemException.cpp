#include "Base/Exception/SystemException.h"

#include <cerrno>
#include <string>
#include <utility>

namespace AsynGyanis::Base
{
    SystemException::SystemException(const std::string &context, const std::source_location &sourceLocation) :
        SystemException(context, std::error_code(errno, std::system_category()), sourceLocation)
    {
        // errno 在委托构造的参数求值期读取，无法在本体内补救读取时序，
        // 只能提示调用方优先用显式 error_code 的重载
    }

    SystemException::SystemException(const std::string &context, const std::error_code errorCode, const std::source_location &sourceLocation) :
        Exception(context + ": [" + std::to_string(errorCode.value()) + "] " + errorCode.message(), sourceLocation)
        , m_errorCode(errorCode)
    {
    }

    const std::error_code &SystemException::errorCode() const noexcept
    {
        return m_errorCode;
    }

    int SystemException::nativeError() const noexcept
    {
        return m_errorCode.value();
    }
} // namespace AsynGyanis::Base

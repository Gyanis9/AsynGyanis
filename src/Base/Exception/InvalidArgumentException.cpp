#include "Base/Exception/InvalidArgumentException.h"

#include "Base/Exception/ExceptionMessage.h"

namespace AsynGyanis::Base
{
    InvalidArgumentException::InvalidArgumentException(const std::string &message, const std::source_location &sourceLocation) :
        std::invalid_argument(Detail::formatExceptionMessage(message, sourceLocation)), m_location(sourceLocation),
        // 跳过 1 帧（captureStackTrace 自身已跳过）：首个保留帧即抛出点
        m_stackTrace(captureStackTrace(1))
    {
    }

    const std::source_location &InvalidArgumentException::location() const noexcept
    {
        return m_location;
    }

    const CapturedStackTrace &InvalidArgumentException::stackTrace() const noexcept
    {
        return m_stackTrace;
    }

} // namespace AsynGyanis::Base

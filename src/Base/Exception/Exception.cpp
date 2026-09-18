#include "Base/Exception/Exception.h"

#include "Base/Exception/ExceptionMessage.h"

namespace AsynGyanis::Base
{
    Exception::Exception(const std::string &message, const std::source_location &sourceLocation) :
        std::runtime_error(Detail::formatExceptionMessage(message, sourceLocation)), m_location(sourceLocation),
        // 跳过 1 帧（captureStackTrace 自身已跳过）：首个保留帧即抛出点；派生类构造函数
        // 多出的帧会出现在最前面，属可接受的噪声，换来的是不必让每个派生类各捕获一次
        m_stackTrace(captureStackTrace(1))
    {
    }

    const std::source_location &Exception::location() const noexcept
    {
        return m_location;
    }

    const CapturedStackTrace &Exception::stackTrace() const noexcept
    {
        return m_stackTrace;
    }

} // namespace AsynGyanis::Base

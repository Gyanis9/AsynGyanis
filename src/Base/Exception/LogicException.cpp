#include "Base/Exception/LogicException.h"

#include "Base/Exception/ExceptionMessage.h"

namespace AsynGyanis::Base
{
    LogicException::LogicException(const std::string &message, const std::source_location &sourceLocation) :
        std::logic_error(Detail::formatExceptionMessage(message, sourceLocation)), m_location(sourceLocation)
    {
    }

    const std::source_location &LogicException::location() const noexcept
    {
        return m_location;
    }

} // namespace AsynGyanis::Base

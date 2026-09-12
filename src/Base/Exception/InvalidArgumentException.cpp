#include "Base/Exception/InvalidArgumentException.h"

#include "Base/Exception/ExceptionMessage.h"

namespace AsynGyanis::Base
{
    InvalidArgumentException::InvalidArgumentException(const std::string &message, const std::source_location &sourceLocation) :
        std::invalid_argument(Detail::formatExceptionMessage(message, sourceLocation)), m_location(sourceLocation)
    {
    }

    const std::source_location &InvalidArgumentException::location() const noexcept
    {
        return m_location;
    }

} // namespace AsynGyanis::Base

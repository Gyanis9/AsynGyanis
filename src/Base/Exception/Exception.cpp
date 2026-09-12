#include "Base/Exception/Exception.h"

#include "Base/Exception/ExceptionMessage.h"

namespace AsynGyanis::Base
{
    Exception::Exception(const std::string &message, const std::source_location &sourceLocation) :
        std::runtime_error(Detail::formatExceptionMessage(message, sourceLocation)), m_location(sourceLocation)
    {
    }

    const std::source_location &Exception::location() const noexcept
    {
        return m_location;
    }

} // namespace AsynGyanis::Base

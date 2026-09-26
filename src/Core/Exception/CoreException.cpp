#include "Core/Exception/CoreException.h"

namespace AsynGyanis::Core
{
    CoreException::CoreException(const std::string &message, const std::source_location &sourceLocation) : Base::Exception(message, sourceLocation)
    {
    }

} // namespace AsynGyanis::Core

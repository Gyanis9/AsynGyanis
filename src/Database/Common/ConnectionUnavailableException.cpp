#include "Database/Common/ConnectionUnavailableException.h"

namespace AsynGyanis::Database
{
    ConnectionUnavailableException::ConnectionUnavailableException(const std::string &message, const std::source_location &sourceLocation) :
        DatabaseException(message, sourceLocation)
    {
    }

} // namespace AsynGyanis::Database

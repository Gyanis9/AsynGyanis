#include "Database/Common/DatabaseException.h"

namespace AsynGyanis::Database
{
    DatabaseException::DatabaseException(const std::string &message, const std::source_location &sourceLocation) : Base::Exception(message, sourceLocation)
    {
    }

} // namespace AsynGyanis::Database

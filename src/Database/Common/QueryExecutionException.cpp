#include "Database/Common/QueryExecutionException.h"

namespace AsynGyanis::Database
{
    QueryExecutionException::QueryExecutionException(const std::string &message, const std::source_location &sourceLocation) : DatabaseException(message, sourceLocation)
    {
    }

} // namespace AsynGyanis::Database

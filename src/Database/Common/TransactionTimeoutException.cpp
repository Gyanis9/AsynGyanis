#include "Database/Common/TransactionTimeoutException.h"

namespace AsynGyanis::Database
{
    TransactionTimeoutException::TransactionTimeoutException(const std::string &message, const std::source_location &sourceLocation) : DatabaseException(message, sourceLocation)
    {
    }
} // namespace AsynGyanis::Database

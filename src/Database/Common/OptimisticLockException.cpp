#include "Database/Common/OptimisticLockException.h"

namespace AsynGyanis::Database
{
    OptimisticLockException::OptimisticLockException(const std::string &message, const std::source_location &sourceLocation) : DatabaseException(message, sourceLocation)
    {
    }

} // namespace AsynGyanis::Database

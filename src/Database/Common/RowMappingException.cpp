#include "Database/Common/RowMappingException.h"

namespace AsynGyanis::Database
{
    RowMappingException::RowMappingException(const std::string &message, const std::source_location &sourceLocation) :
        DatabaseException(message, sourceLocation)
    {
    }

} // namespace AsynGyanis::Database

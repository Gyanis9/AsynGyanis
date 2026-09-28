#include "Base/Exception/InvalidArgumentException.h"

#include "Base/Exception/ExceptionMessage.h"

namespace AsynGyanis::Base
{
    InvalidArgumentException::InvalidArgumentException(const std::string &message, const std::source_location &sourceLocation) :
        // 抛出点快照与调用栈的捕获都在 Detail::ExceptionPayload 里做，三条链共用一份
        std::invalid_argument(Detail::formatExceptionMessage(message, sourceLocation)), Detail::ExceptionPayload(sourceLocation)
    {
    }

} // namespace AsynGyanis::Base

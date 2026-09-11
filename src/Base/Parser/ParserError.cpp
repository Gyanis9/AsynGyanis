#include "Base/Parser/ParserError.h"

#include <format>
#include <string>

namespace AsynGyanis::Base
{
    ParserError::ParserError(const std::string &message, const ParserPosition &position, const std::source_location &sourceLocation) :
        Exception(std::format("Parse error at {}: {}", position.describe(), message), sourceLocation)
        , m_position(position)
        , m_reason(message)
    {
    }

    const ParserPosition &ParserError::position() const noexcept
    {
        return m_position;
    }

    const std::string &ParserError::reason() const noexcept
    {
        return m_reason;
    }
} // namespace AsynGyanis::Base

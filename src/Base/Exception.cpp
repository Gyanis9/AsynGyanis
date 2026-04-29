#include "Exception.h"

#include <cerrno>
#include <cstring>
#include <sstream>

namespace Base
{
    // ============================================================================
    // Exception
    // ============================================================================

    Exception::Exception(const std::string &message, const std::source_location &loc) :
        std::runtime_error(formatMessage(message, loc)), m_location(loc)
    {
    }

    const std::source_location &Exception::location() const noexcept
    {
        return m_location;
    }

    std::string Exception::formatMessage(const std::string &msg, const std::source_location &loc)
    {
        std::ostringstream oss;
        oss << "[Exception] " << msg
                << " [" << loc.file_name() << ":" << loc.line()
                << " in " << loc.function_name() << "]";
        return oss.str();
    }

    // ============================================================================
    // ConfigException
    // ============================================================================

    ConfigException::ConfigException(const std::string &message, const std::source_location &loc) :
        Exception("Config error: " + message, loc)
    {
    }

    // ============================================================================
    // ConfigFileException
    // ============================================================================

    ConfigFileException::ConfigFileException(const std::string &file_path, const std::string &reason,
                                             const std::source_location &loc) :
        ConfigException("File '" + file_path + "': " + reason, loc)
        , m_file_path(file_path)
    {
    }

    const std::string &ConfigFileException::filePath() const noexcept
    {
        return m_file_path;
    }

    // ============================================================================
    // ConfigParseException
    // ============================================================================

    ConfigParseException::ConfigParseException(const std::string &file_path, const std::string &reason,
                                               const std::source_location &loc) :
        ConfigException("Parse error in '" + file_path + "': " + reason, loc)
        , m_file_path(file_path)
    {
    }

    const std::string &ConfigParseException::filePath() const noexcept
    {
        return m_file_path;
    }

    // ============================================================================
    // ConfigKeyNotFoundException
    // ============================================================================

    ConfigKeyNotFoundException::ConfigKeyNotFoundException(const std::string &key, const std::source_location &loc) :
        ConfigException("Configuration key not found: '" + key + "'", loc)
        , m_key(key)
    {
    }

    const std::string &ConfigKeyNotFoundException::key() const noexcept
    {
        return m_key;
    }

    // ============================================================================
    // ConfigTypeException
    // ============================================================================

    ConfigTypeException::ConfigTypeException(const std::string &key, const std::string &expected_type, const std::string &actual_type,
                                             const std::source_location &loc) :
        ConfigException("Type mismatch for key '" + key + "': expected " + expected_type + ", got " + actual_type, loc)
        , m_key(key)
        , m_expected_type(expected_type)
        , m_actual_type(actual_type)
    {
    }

    const std::string &ConfigTypeException::key() const noexcept
    {
        return m_key;
    }

    const std::string &ConfigTypeException::expectedType() const noexcept
    {
        return m_expected_type;
    }

    const std::string &ConfigTypeException::actualType() const noexcept
    {
        return m_actual_type;
    }

    // ============================================================================
    // ConfigValidationException
    // ============================================================================

    ConfigValidationException::ConfigValidationException(const std::string &key, const std::string &reason,
                                                         const std::source_location &loc) :
        ConfigException("Validation failed for key '" + key + "': " + reason, loc)
        , m_key(key)
    {
    }

    const std::string &ConfigValidationException::key() const noexcept
    {
        return m_key;
    }

    // ============================================================================
    // SystemException
    // ============================================================================

    SystemException::SystemException(const std::string &context, const std::source_location &loc) :
        SystemException(context, std::error_code(errno, std::system_category()), loc)
    {
    }

    SystemException::SystemException(const std::string &context, std::error_code ec, const std::source_location &loc) :
        Exception(context + ": [" + std::to_string(ec.value()) + "] " + ec.message(), loc)
        , m_errorCode(std::move(ec))
    {
    }

    const std::error_code &SystemException::errorCode() const noexcept
    {
        return m_errorCode;
    }

    int SystemException::nativeError() const noexcept
    {
        return m_errorCode.value();
    }

    // ============================================================================
    // NetworkException
    // ============================================================================

    NetworkException::NetworkException(const std::string &context, const std::string &remote_address,
                                       const std::source_location &loc) :
        SystemException(context, loc)
        , m_remoteAddress(remote_address)
    {
    }

    NetworkException::NetworkException(const std::string &context, std::error_code ec, const std::string &remote_address,
                                       const std::source_location &loc) :
        SystemException(context + " (remote: " + remote_address + ")", std::move(ec), loc)
        , m_remoteAddress(remote_address)
    {
    }

    const std::string &NetworkException::remoteAddress() const noexcept
    {
        return m_remoteAddress;
    }

} // namespace Base

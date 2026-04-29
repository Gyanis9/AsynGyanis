/**
 * @file Exception.h
 * @brief 项目统一异常类层次结构
 * @copyright Copyright (c) 2026
 */
#ifndef BASE_EXCEPTION_H
#define BASE_EXCEPTION_H

#include <source_location>
#include <stdexcept>
#include <string>
#include <system_error>

namespace Base
{
    // ============================================================================
    // 基础异常类
    // ============================================================================

    /**
     * @brief 项目统一异常基类，记录异常消息与源码位置。
     */
    class Exception : public std::runtime_error
    {
    public:
        explicit Exception(const std::string &message, const std::source_location &loc = std::source_location::current());

        [[nodiscard]] const std::source_location &location() const noexcept;

    private:
        static std::string formatMessage(const std::string &msg, const std::source_location &loc);

        std::source_location m_location;
    };

    // ============================================================================
    // 配置异常（保留兼容）
    // ============================================================================

    class ConfigException : public Exception
    {
    public:
        explicit ConfigException(const std::string &message, const std::source_location &loc = std::source_location::current());
    };

    class ConfigFileException : public ConfigException
    {
    public:
        ConfigFileException(const std::string &file_path, const std::string &reason, const std::source_location &loc = std::source_location::current());

        [[nodiscard]] const std::string &filePath() const noexcept;

    private:
        std::string m_file_path;
    };

    class ConfigParseException : public ConfigException
    {
    public:
        ConfigParseException(const std::string &file_path, const std::string &reason, const std::source_location &loc = std::source_location::current());

        [[nodiscard]] const std::string &filePath() const noexcept;

    private:
        std::string m_file_path;
    };

    class ConfigKeyNotFoundException : public ConfigException
    {
    public:
        explicit ConfigKeyNotFoundException(const std::string &key, const std::source_location &loc = std::source_location::current());

        [[nodiscard]] const std::string &key() const noexcept;

    private:
        std::string m_key;
    };

    class ConfigTypeException : public ConfigException
    {
    public:
        ConfigTypeException(const std::string &         key, const std::string &expected_type, const std::string &actual_type,
                            const std::source_location &loc = std::source_location::current());

        [[nodiscard]] const std::string &key() const noexcept;
        [[nodiscard]] const std::string &expectedType() const noexcept;
        [[nodiscard]] const std::string &actualType() const noexcept;

    private:
        std::string m_key;
        std::string m_expected_type;
        std::string m_actual_type;
    };

    class ConfigValidationException : public ConfigException
    {
    public:
        ConfigValidationException(const std::string &key, const std::string &reason, const std::source_location &loc = std::source_location::current());

        [[nodiscard]] const std::string &key() const noexcept;

    private:
        std::string m_key;
    };

    // ============================================================================
    // 系统异常
    // ============================================================================

    /**
     * @brief 系统调用失败异常，携带 errno 对应的错误码与描述。
     */
    class SystemException : public Exception
    {
    public:
        explicit SystemException(const std::string &context, const std::source_location &loc = std::source_location::current());

        SystemException(const std::string &context, std::error_code ec, const std::source_location &loc = std::source_location::current());

        [[nodiscard]] const std::error_code &errorCode() const noexcept;
        [[nodiscard]] int                    nativeError() const noexcept;

    private:
        std::error_code m_errorCode;
    };

    // ============================================================================
    // 网络异常
    // ============================================================================

    /**
     * @brief 网络操作异常，携带远端地址等上下文信息。
     */
    class NetworkException : public SystemException
    {
    public:
        NetworkException(const std::string &context, const std::string &remote_address, const std::source_location &loc = std::source_location::current());

        NetworkException(const std::string &context, std::error_code ec, const std::string &remote_address, const std::source_location &loc = std::source_location::current());

        [[nodiscard]] const std::string &remoteAddress() const noexcept;

    private:
        std::string m_remoteAddress;
    };

}
#endif

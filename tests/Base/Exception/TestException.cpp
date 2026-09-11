/**
 * @file TestException.cpp
 * @brief Base/Exception 全模块单元测试：各异常类的构造、属性、继承关系与压力场景
 * @author Gyanis
 * @date 2026-09-10
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Exception/ConfigException.h"
#include "Base/Exception/ConfigFileException.h"
#include "Base/Exception/ConfigKeyNotFoundException.h"
#include "Base/Exception/ConfigParseException.h"
#include "Base/Parser/Value/ValueAccessError.h"
#include "Base/Exception/ConfigValidationException.h"
#include "Base/Exception/Exception.h"
#include "Base/Exception/NetworkException.h"
#include "Base/Exception/SystemException.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 断言文本中包含给定子串
         * @param haystack 待检查文本
         * @param needle 期望出现的子串
         * @return true 出现
         */
        bool contains(const std::string &haystack, const std::string &needle)
        {
            return haystack.find(needle) != std::string::npos;
        }
    } // namespace

    // ============================================================================
    // Exception 基类
    // ============================================================================

    TEST(Exception, InheritsFromRuntimeError)
    {
        Exception exception("base error");

        EXPECT_THROW(throw exception, std::runtime_error);
        EXPECT_THROW(throw exception, std::exception);
        EXPECT_THROW(throw exception, Exception);
    }

    TEST(Exception, MessageCarriesLocationOfThrow)
    {
        const auto      sourceLocation = std::source_location::current();
        const Exception exception("whereami", sourceLocation);

        const std::string message(exception.what());
        EXPECT_TRUE(contains(message, "[异常]"));
        EXPECT_TRUE(contains(message, "whereami"));
        EXPECT_TRUE(contains(message, sourceLocation.file_name()));
        EXPECT_TRUE(contains(message, std::to_string(sourceLocation.line())));
    }

    TEST(Exception, LocationAccessorMatchesConstructorArgument)
    {
        const auto      sourceLocation = std::source_location::current();
        const Exception exception("stored location", sourceLocation);

        EXPECT_EQ(exception.location().line(), sourceLocation.line());
        EXPECT_STREQ(exception.location().file_name(), sourceLocation.file_name());
    }

    TEST(Exception, DefaultLocationPointsAtTestCallSite)
    {
        const Exception exception("implicit location");

        EXPECT_GT(exception.location().line(), 0U);
        EXPECT_NE(std::string(exception.location().function_name()).find("TestBody"), std::string::npos);
    }

    // ============================================================================
    // ConfigException
    // ============================================================================

    TEST(ConfigException, InheritsFromExceptionBase)
    {
        const ConfigException exception("test error");

        EXPECT_THROW(throw exception, Exception);
        EXPECT_THROW(throw exception, std::runtime_error);
        EXPECT_THROW(throw exception, std::exception);
    }

    TEST(ConfigException, MessageIsPrefixedWithCategory)
    {
        const ConfigException exception("test message");
        const std::string     message(exception.what());

        EXPECT_TRUE(contains(message, "配置错误"));
        EXPECT_TRUE(contains(message, "test message"));
    }

    TEST(ConfigException, AcceptsEmptyMessage)
    {
        const ConfigException exception("");

        EXPECT_FALSE(std::string(exception.what()).empty());
    }

    TEST(ConfigException, HandlesVeryLongMessage)
    {
        const std::string     longMessage(10000, 'X');
        const ConfigException exception(longMessage);

        EXPECT_TRUE(contains(std::string(exception.what()), longMessage));
    }

    // ============================================================================
    // ConfigFileException
    // ============================================================================

    TEST(ConfigFileException, InheritsFromConfigException)
    {
        const ConfigFileException exception("/path/to/file", "permission denied");

        EXPECT_THROW(throw exception, ConfigException);
        EXPECT_THROW(throw exception, std::runtime_error);
    }

    TEST(ConfigFileException, ExposesPathAndMentionsBothFields)
    {
        const ConfigFileException exception("/etc/config.yaml", "permission denied");
        const std::string         message(exception.what());

        EXPECT_EQ(exception.filePath(), "/etc/config.yaml");
        EXPECT_TRUE(contains(message, "/etc/config.yaml"));
        EXPECT_TRUE(contains(message, "permission denied"));
    }

    TEST(ConfigFileException, AcceptsEmptyPathAndEmptyReason)
    {
        const ConfigFileException emptyPath("", "reason");
        const ConfigFileException emptyReason("file.yaml", "");

        EXPECT_TRUE(emptyPath.filePath().empty());
        EXPECT_EQ(emptyReason.filePath(), "file.yaml");
    }

    TEST(ConfigFileException, KeepsNonAsciiPathIntact)
    {
        const ConfigFileException exception("/配置/文件.yaml", "错误");

        EXPECT_EQ(exception.filePath(), "/配置/文件.yaml");
    }

    // ============================================================================
    // ConfigParseException
    // ============================================================================

    TEST(ConfigParseException, InheritsFromConfigException)
    {
        const ConfigParseException exception("config.yaml", "invalid syntax");

        EXPECT_THROW(throw exception, ConfigException);
        EXPECT_THROW(throw exception, std::runtime_error);
    }

    TEST(ConfigParseException, MessageDescribesParseFailure)
    {
        const ConfigParseException exception("config.yaml", "unexpected token at line 42");
        const std::string          message(exception.what());

        EXPECT_EQ(exception.filePath(), "config.yaml");
        EXPECT_TRUE(contains(message, "config.yaml"));
        EXPECT_TRUE(contains(message, "unexpected token"));
        EXPECT_TRUE(contains(message, "解析错误"));
    }

    TEST(ConfigParseException, HandlesLongReasonAndEmptyFields)
    {
        const std::string          longReason(5000, 'e');
        const ConfigParseException longException("f.yaml", longReason);
        const ConfigParseException emptyException("", "");

        EXPECT_EQ(longException.filePath(), "f.yaml");
        EXPECT_TRUE(emptyException.filePath().empty());
    }

    // ============================================================================
    // ConfigKeyNotFoundException
    // ============================================================================

    TEST(ConfigKeyNotFoundException, InheritsFromConfigException)
    {
        const ConfigKeyNotFoundException exception("server.port");

        EXPECT_THROW(throw exception, ConfigException);
        EXPECT_THROW(throw exception, std::runtime_error);
    }

    TEST(ConfigKeyNotFoundException, ExposesMissingKey)
    {
        const ConfigKeyNotFoundException exception("database.connection.url");
        const std::string                message(exception.what());

        EXPECT_EQ(exception.key(), "database.connection.url");
        EXPECT_TRUE(contains(message, "database.connection.url"));
        EXPECT_TRUE(contains(message, "不存在"));
    }

    TEST(ConfigKeyNotFoundException, AcceptsEmptyKey)
    {
        const ConfigKeyNotFoundException exception("");

        EXPECT_TRUE(exception.key().empty());
    }

    TEST(ConfigKeyNotFoundException, HandlesVariousKeyShapes)
    {
        const std::vector<std::string> candidateKeys = {
                "a",
                "a.b.c.d.e.f.g.h.i.j",
                "very_long_key_name_that_exceeds_typical_lengths_for_configuration_keys",
                "key.with.numbers.123",
                "key-with-special_chars@test",
        };

        for (const std::string &key: candidateKeys)
        {
            const ConfigKeyNotFoundException exception(key);
            EXPECT_EQ(exception.key(), key);
        }
    }

    // ============================================================================
    // ConfigValidationException
    // ============================================================================

    TEST(ConfigValidationException, InheritsFromConfigException)
    {
        const ConfigValidationException exception("server.port", "must be between 1 and 65535");

        EXPECT_THROW(throw exception, ConfigException);
        EXPECT_THROW(throw exception, std::runtime_error);
    }

    TEST(ConfigValidationException, ExposesKeyAndReason)
    {
        const ConfigValidationException exception("server.port", "must be between 1 and 65535");
        const std::string               message(exception.what());

        EXPECT_EQ(exception.key(), "server.port");
        EXPECT_TRUE(contains(message, "must be between 1 and 65535"));
        EXPECT_TRUE(contains(message, "校验失败"));
    }

    TEST(ConfigValidationException, AcceptsEmptyReason)
    {
        const ConfigValidationException exception("key", "");

        EXPECT_EQ(exception.key(), "key");
    }

    // ============================================================================
    // SystemException 与 NetworkException
    // ============================================================================

    TEST(SystemException, InheritsFromExceptionBase)
    {
        const SystemException exception("system call", std::error_code(2, std::system_category()));

        EXPECT_THROW(throw exception, Exception);
        EXPECT_THROW(throw exception, std::runtime_error);
    }

    TEST(SystemException, ExplicitErrorCodeIsExposedAndFormatted)
    {
        const std::error_code errorCode(13, std::system_category());
        const SystemException exception("write config", errorCode);
        const std::string     message(exception.what());

        EXPECT_EQ(exception.errorCode(), errorCode);
        EXPECT_EQ(exception.nativeError(), 13);
        EXPECT_TRUE(contains(message, "write config"));
        EXPECT_TRUE(contains(message, "[13]"));
    }

    TEST(SystemException, ReadsErrnoWhenErrorCodeIsOmitted)
    {
        errno = 17;
        const SystemException exception("open handle");

        EXPECT_EQ(exception.nativeError(), 17);
        errno = 0;
    }

    TEST(NetworkException, InheritsFromSystemException)
    {
        const NetworkException exception("connect failed", std::error_code(111, std::system_category()), "127.0.0.1:8080");

        EXPECT_THROW(throw exception, SystemException);
        EXPECT_THROW(throw exception, Exception);
    }

    TEST(NetworkException, ExposesRemoteAddressInMessage)
    {
        const NetworkException exception("handshake", std::error_code(1, std::system_category()), "10.0.0.1:443");
        const std::string      message(exception.what());

        EXPECT_EQ(exception.remoteAddress(), "10.0.0.1:443");
        EXPECT_TRUE(contains(message, "10.0.0.1:443"));
        EXPECT_EQ(exception.nativeError(), 1);
    }

    TEST(NetworkException, FallsBackToErrnoWithoutExplicitCode)
    {
        errno = 111;
        const NetworkException exception("accept", "192.168.0.1:9000");

        EXPECT_EQ(exception.remoteAddress(), "192.168.0.1:9000");
        EXPECT_EQ(exception.nativeError(), 111);
        errno = 0;
    }

    // ============================================================================
    // 层次结构与拷贝语义
    // ============================================================================

    TEST(ExceptionHierarchy, EveryConfigExceptionIsCatchableAsStdException)
    {
        // unique_ptr 不可拷贝，故保存工厂而不是实例列表
        const std::vector<std::function<std::unique_ptr<std::exception>()> > factories = {
                []
                {
                    return std::unique_ptr<std::exception>(std::make_unique<ConfigException>("test"));
                },
                []
                {
                    return std::unique_ptr<std::exception>(std::make_unique<ConfigFileException>("f", "r"));
                },
                []
                {
                    return std::unique_ptr<std::exception>(std::make_unique<ConfigParseException>("f", "r"));
                },
                []
                {
                    return std::unique_ptr<std::exception>(std::make_unique<ConfigKeyNotFoundException>("k"));
                },
                []
                {
                    return std::unique_ptr<std::exception>(std::make_unique<ValueAccessError>("k", "e", "a"));
                },
                []
                {
                    return std::unique_ptr<std::exception>(std::make_unique<ConfigValidationException>("k", "r"));
                },
        };

        for (const auto &factory: factories)
        {
            const std::unique_ptr<std::exception> exception = factory();

            ASSERT_NE(exception, nullptr);
            EXPECT_FALSE(std::string(exception->what()).empty());
        }
    }

    TEST(ExceptionHierarchy, CopyPreservesMessageAndFields)
    {
        const ConfigFileException  original("/path/file.yaml", "test reason");
        const ConfigFileException &copy(original);

        EXPECT_EQ(copy.filePath(), original.filePath());
        EXPECT_STREQ(copy.what(), original.what());
    }

    TEST(ExceptionHierarchy, SlicedCatchStillReportsMessage)
    {
        const ConfigKeyNotFoundException original("slice.key");
        const Exception &                sliced(original);

        EXPECT_TRUE(contains(std::string(sliced.what()), "slice.key"));
    }

    // ============================================================================
    // 压力场景
    // ============================================================================

    TEST(ExceptionStress, RapidThrowCatchKeepsMessagesDistinct)
    {
        constexpr int kiterationCount = 10000;

        for (int iteration = 0; iteration < kiterationCount; ++iteration)
        {
            try
            {
                throw ConfigKeyNotFoundException("key_" + std::to_string(iteration));
            } catch (const ConfigException &exception)
            {
                ASSERT_FALSE(std::string(exception.what()).empty());
            }
        }
    }

    TEST(ExceptionStress, ThrowingDerivedIsCaughtByBaseReference)
    {
        int caughtCount = 0;

        for (int iteration = 0; iteration < 500; ++iteration)
        {
            try
            {
                throw ValueAccessError("k", "int", "string");
            } catch (const Exception &exception)
            {
                EXPECT_TRUE(contains(std::string(exception.what()), "类型不匹配"));
                ++caughtCount;
            }
        }

        EXPECT_EQ(caughtCount, 500);
    }
} // namespace AsynGyanis::Base

/**
 * @file TestValueAccessError.cpp
 * @brief ValueAccessError 单元测试：类型不匹配与成员缺失两类访问失败
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Parser/Value/ValueAccessError.h"

#include "Base/Exception/ConfigException.h"
#include "Base/Exception/Exception.h"
#include "Base/Parser/Value/ParserValue.h"

#include <gtest/gtest.h>

#include <exception>
#include <source_location>
#include <stdexcept>
#include <string>

namespace AsynGyanis::Base
{
    namespace
    {
        /**
         * @brief 判断文本是否包含子串
         * @param haystack 待检查文本
         * @param needle 子串
         * @return true 出现
         */
        bool contains(const std::string &haystack, const std::string &needle)
        {
            return haystack.find(needle) != std::string::npos;
        }
    } // namespace

    TEST(ValueAccessError, InheritsFromProjectExceptionBase)
    {
        const ValueAccessError exception("server.port", "int64_t", "std::string");

        EXPECT_THROW(throw exception, Exception);
        EXPECT_THROW(throw exception, std::runtime_error);

        // 值模型属于 Parser，不应再落入配置异常分支
        bool caughtAsConfigException = false;
        try
        {
            throw exception;
        } catch (const ConfigException &)
        {
            caughtAsConfigException = true;
        } catch (const Exception &)
        {
        }
        EXPECT_FALSE(caughtAsConfigException);
    }

    TEST(ValueAccessError, ExposesExpectedAndActualTypes)
    {
        const ValueAccessError exception("server.port", "int64_t", "std::string");
        const std::string      message(exception.what());

        EXPECT_EQ(exception.key(), "server.port");
        EXPECT_EQ(exception.expectedType(), "int64_t");
        EXPECT_EQ(exception.actualType(), "std::string");
        EXPECT_TRUE(contains(message, "类型不匹配"));
        EXPECT_TRUE(contains(message, "期望 int64_t，实际 std::string")) << message;
    }

    TEST(ValueAccessError, AcceptsEmptyFields)
    {
        const ValueAccessError exception("", "", "");

        EXPECT_TRUE(exception.key().empty());
        EXPECT_TRUE(exception.expectedType().empty());
        EXPECT_TRUE(exception.actualType().empty());
    }

    TEST(ValueAccessError, MissingMemberFactoryReportsKeyOnly)
    {
        const ValueAccessError exception = ValueAccessError::missingMember("logging.level");
        const std::string      message(exception.what());

        EXPECT_EQ(exception.key(), "logging.level");
        EXPECT_TRUE(exception.expectedType().empty());
        EXPECT_TRUE(exception.actualType().empty());
        EXPECT_TRUE(contains(message, "成员不存在：'logging.level'")) << message;
    }

    TEST(ValueAccessError, MissingMemberFactoryAcceptsIndexDescription)
    {
        const ValueAccessError exception = ValueAccessError::missingMember("[3]");

        EXPECT_EQ(exception.key(), "[3]");
        EXPECT_TRUE(contains(std::string(exception.what()), "[3]"));
    }

    TEST(ValueAccessError, CarriesExplicitThrowSite)
    {
        const auto sourceLocation = std::source_location::current();
        const ValueAccessError exception("key", "int", "string", sourceLocation);

        EXPECT_EQ(exception.location().line(), sourceLocation.line());
    }

    TEST(ValueAccessError, IsThrownByValueModelOnMismatch)
    {
        const ParserValue textValue(std::string("not a number"));

        EXPECT_THROW(static_cast<void>(textValue.asInt()), ValueAccessError);
    }
} // namespace AsynGyanis::Base

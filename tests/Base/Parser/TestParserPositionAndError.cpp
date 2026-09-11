/**
 * @file TestParserPositionAndError.cpp
 * @brief ParserPosition 与 ParserError 单元测试：位置文本、异常层次与消息内容
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#include "Base/Parser/ParserError.h"
#include "Base/Parser/ParserPosition.h"

#include "Base/Exception/Exception.h"

#include <gtest/gtest.h>

#include <exception>
#include <string>

namespace AsynGyanis::Base
{
    TEST(ParserPosition, DefaultsPointToTheFirstRowAndColumn)
    {
        const ParserPosition position;

        EXPECT_EQ(position.lineNumber, 1U);
        EXPECT_EQ(position.columnNumber, 1U);
        EXPECT_EQ(position.offset, 0U);
    }

    TEST(ParserPosition, DescribeFormatsOneBasedRowAndColumn)
    {
        const ParserPosition position{3, 12, 40};

        EXPECT_EQ(position.describe(), "第 3 行，第 12 列");
    }

    TEST(ParserPosition, DescribeHandlesLargeValues)
    {
        const ParserPosition position{100000, 999, 100000};

        EXPECT_EQ(position.describe(), "第 100000 行，第 999 列");
    }

    TEST(ParserError, MessageEmbedsPositionAndReason)
    {
        const ParserError error("tab characters must not be used", ParserPosition{7, 5, 42});

        const std::string message(error.what());
        EXPECT_NE(message.find("解析错误（第 7 行，第 5 列）"), std::string::npos) << message;
        EXPECT_NE(message.find("tab characters must not be used"), std::string::npos) << message;
    }

    TEST(ParserError, PositionAccessorKeepsConstructorValue)
    {
        const ParserError error("reason", ParserPosition{9, 2, 128});

        EXPECT_EQ(error.position().lineNumber, 9U);
        EXPECT_EQ(error.position().columnNumber, 2U);
        EXPECT_EQ(error.position().offset, 128U);
    }

    TEST(ParserError, ParticipatesInTheProjectExceptionHierarchy)
    {
        const ParserError error("reason", ParserPosition{1, 1, 0});

        bool caughtAsProjectException = false;
        try
        {
            throw error;
        } catch (const Exception &baseException)
        {
            caughtAsProjectException = true;
            EXPECT_NE(std::string(baseException.what()).find("解析错误"), std::string::npos);
        }
        catch (...)
        {
            FAIL() << "ParserError 应能被项目异常基类捕获";
        }
        EXPECT_TRUE(caughtAsProjectException);

        try
        {
            throw error;
        } catch (const std::exception &stdException)
        {
            EXPECT_FALSE(std::string(stdException.what()).empty());
        }
    }

    TEST(ParserError, CarriesThrowSiteLikeTheBaseClass)
    {
        const ParserError error("reason", ParserPosition{4, 4, 4});

        EXPECT_GT(error.location().line(), 0U);
    }
} // namespace AsynGyanis::Base

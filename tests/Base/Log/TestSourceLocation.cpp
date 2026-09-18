// SourceLocation 单元测试：构造语义、调用点采集与短文件名截断

#include "Base/Log/SourceLocation.h"

#include <gtest/gtest.h>

#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /// 当前测试文件名，用于校验调用点采集结果
        constexpr std::string_view kTestFileName = "TestSourceLocation.cpp";

        /** @brief 判断 C 字符串指针为空或指向空串 */
        bool isEmptyText(const char *text)
        {
            return text == nullptr || text[0] == '\0';
        }
    } // namespace

    TEST(SourceLocation, DefaultConstructionLeavesEveryFieldEmpty)
    {
        constexpr SourceLocation klocation;

        EXPECT_EQ(klocation.fileName, nullptr);
        EXPECT_EQ(klocation.line, 0);
        EXPECT_EQ(klocation.functionName, nullptr);
    }

    TEST(SourceLocation, FieldConstructorStoresEveryMember)
    {
        constexpr SourceLocation klocation("TestSourceLocation.cpp", 42, "fieldConstructorCase");

        EXPECT_STREQ(klocation.fileName, "TestSourceLocation.cpp");
        EXPECT_EQ(klocation.line, 42);
        EXPECT_STREQ(klocation.functionName, "fieldConstructorCase");
    }

    TEST(SourceLocation, FieldConstructorAcceptsEmptyFields)
    {
        constexpr SourceLocation klocation("", 0, "");

        EXPECT_TRUE(isEmptyText(klocation.fileName));
        EXPECT_EQ(klocation.line, 0);
        EXPECT_TRUE(isEmptyText(klocation.functionName));
    }

    TEST(SourceLocation, StandardLocationConstructorCopiesFileLineAndFunction)
    {
        const std::source_location standardLocation = std::source_location::current();

        const SourceLocation location(standardLocation);

        EXPECT_STREQ(location.fileName, standardLocation.file_name());
        EXPECT_EQ(location.line, static_cast<int>(standardLocation.line()));
        EXPECT_STREQ(location.functionName, standardLocation.function_name());
    }

    TEST(SourceLocation, CurrentCapturesCallSiteOfThisFile)
    {
        const SourceLocation location = SourceLocation::current();

        ASSERT_NE(location.fileName, nullptr);
        EXPECT_FALSE(isEmptyText(location.fileName));
        EXPECT_EQ(std::string_view(location.shortFileName()), kTestFileName);
        EXPECT_GT(location.line, 0);
        ASSERT_NE(location.functionName, nullptr);
        EXPECT_NE(std::string_view(location.functionName).find("TestBody"), std::string_view::npos);
    }

    TEST(SourceLocation, CurrentReturnsDifferentLinePerCallSite)
    {
        const SourceLocation firstLocation  = SourceLocation::current();
        const SourceLocation secondLocation = SourceLocation::current();

        EXPECT_EQ(firstLocation.line + 1, secondLocation.line);
        EXPECT_STREQ(firstLocation.fileName, secondLocation.fileName);
    }

    TEST(SourceLocation, ShortFileNameStripsPosixDirectoryPrefix)
    {
        constexpr SourceLocation kdeepLocation("/home/user/project/src/main.cpp", 100, "main");
        constexpr SourceLocation krelativeLocation("src/main.cpp", 50, "mainFunction");

        EXPECT_STREQ(kdeepLocation.shortFileName(), "main.cpp");
        EXPECT_STREQ(krelativeLocation.shortFileName(), "main.cpp");
    }

    TEST(SourceLocation, ShortFileNameStripsWindowsBackslashPath)
    {
        constexpr SourceLocation kwindowsLocation(R"(C:\Users\test\project\src\file.cpp)", 10, "windowsFunction");
        constexpr SourceLocation kmixedSeparatorLocation("C:\\Users/test\\file.cpp", 11, "mixedFunction");

        EXPECT_STREQ(kwindowsLocation.shortFileName(), "file.cpp");
        EXPECT_STREQ(kmixedSeparatorLocation.shortFileName(), "file.cpp");
    }

    TEST(SourceLocation, ShortFileNameReturnsBareFileNameUnchanged)
    {
        constexpr SourceLocation kbareLocation("main.cpp", 1, "bareFunction");

        EXPECT_STREQ(kbareLocation.shortFileName(), "main.cpp");
    }

    TEST(SourceLocation, ShortFileNameHandlesEmptyAndTrailingSeparator)
    {
        constexpr SourceLocation kdefaultLocation;
        constexpr SourceLocation kemptyNameLocation("", 5, "emptyFunction");
        constexpr SourceLocation ktrailingSeparatorLocation("src\\", 6, "trailingFunction");

        EXPECT_STREQ(kdefaultLocation.shortFileName(), "");
        EXPECT_STREQ(kemptyNameLocation.shortFileName(), "");
        EXPECT_STREQ(ktrailingSeparatorLocation.shortFileName(), "");
    }

    TEST(SourceLocation, ShortFileNameIsStableAcrossRepeatedCalls)
    {
        constexpr SourceLocation klocation("/opt/asynngyanis/src/Base/Log/Logger.cpp", 77, "writeToSinks");

        const char *firstCall  = klocation.shortFileName();
        const char *secondCall = klocation.shortFileName();

        EXPECT_TRUE(firstCall == secondCall);
        EXPECT_STREQ(secondCall, "Logger.cpp");
    }

    TEST(SourceLocation, IsUsableInConstantExpressions)
    {
        constexpr SourceLocation kdefaultLocation;
        constexpr SourceLocation kcompileTimeLocation("CompileTime.cpp", 12, "compileTimeFunction");

        static_assert(kdefaultLocation.fileName == nullptr);
        static_assert(kdefaultLocation.line == 0);
        static_assert(kcompileTimeLocation.line == 12);

        EXPECT_EQ(kdefaultLocation.line, 0);
        EXPECT_STREQ(kcompileTimeLocation.fileName, "CompileTime.cpp");
        EXPECT_STREQ(kcompileTimeLocation.functionName, "compileTimeFunction");
    }

    TEST(SourceLocation, IsTriviallyCopyableSoEventsCanCarryItByValue)
    {
        static_assert(std::is_copy_constructible_v<SourceLocation>);
        static_assert(std::is_trivially_copyable_v<SourceLocation>);

        const std::vector<SourceLocation> locations = {
                SourceLocation("first.cpp", 1, "firstFunction"),
                SourceLocation("second.cpp", 2, "secondFunction"),
        };

        ASSERT_EQ(locations.size(), 2U);
        EXPECT_STREQ(locations[0].shortFileName(), "first.cpp");
        EXPECT_STREQ(locations[1].shortFileName(), "second.cpp");
    }
} // namespace AsynGyanis::Base

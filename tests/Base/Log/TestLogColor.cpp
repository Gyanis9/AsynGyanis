// LogColor 单元测试：ANSI 转义序列常量合法性与日志等级到颜色的映射

// 日志模块在 Windows 上要求先包含 Platform/Platform.h，以清除 windows.h 注入的 ERROR 宏
#include <ranges>

#include "Base/Log/LogColor.h"
#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    namespace
    {
        /// LogColor 提供的全部转义序列常量及其名称，用于统一遍历校验
        const std::vector<std::pair<std::string_view, const char *>> kAllColorConstants = {
                {"kReset", LogColor::kReset},
                {"kRed", LogColor::kRed},
                {"kGreen", LogColor::kGreen},
                {"kYellow", LogColor::kYellow},
                {"kBlue", LogColor::kBlue},
                {"kMagenta", LogColor::kMagenta},
                {"kCyan", LogColor::kCyan},
                {"kWhite", LogColor::kWhite},
                {"kBrightBlack", LogColor::kBrightBlack},
                {"kBrightRed", LogColor::kBrightRed},
                {"kBrightGreen", LogColor::kBrightGreen},
                {"kBrightYellow", LogColor::kBrightYellow},
                {"kBrightBlue", LogColor::kBrightBlue},
                {"kBrightMagenta", LogColor::kBrightMagenta},
                {"kBrightCyan", LogColor::kBrightCyan},
                {"kBrightWhite", LogColor::kBrightWhite},
        };
    } // namespace

    TEST(LogColor, ConstantsAreExposedAsCompileTimeLiterals)
    {
        static_assert(std::is_same_v<std::remove_const_t<decltype(LogColor::kReset)>, const char *>);
        static_assert(LogColor::kReset[0] == '\033');

        EXPECT_EQ(kAllColorConstants.size(), 16U);
    }

    TEST(LogColor, EveryConstantIsWellFormedAnsiSequence)
    {
        for (const auto &[name, sequence]: kAllColorConstants)
        {
            ASSERT_NE(sequence, nullptr) << "constant " << std::string(name);

            const std::string_view view(sequence);
            EXPECT_GE(view.size(), 4U) << "constant " << std::string(name);
            EXPECT_EQ(view.front(), '\033') << "constant " << std::string(name);
            EXPECT_EQ(view[1], '[') << "constant " << std::string(name);
            EXPECT_EQ(view.back(), 'm') << "constant " << std::string(name);

            for (std::size_t index = 2; index + 1 < view.size(); ++index)
            {
                const bool isGraphicParameter = (view[index] >= '0' && view[index] <= '9') || view[index] == ';';
                EXPECT_TRUE(isGraphicParameter) << "constant " << std::string(name);
            }
        }
    }

    TEST(LogColor, ResetConstantClearsEveryGraphicAttribute)
    {
        EXPECT_STREQ(LogColor::kReset, "\033[0m");
    }

    TEST(LogColor, ColorConstantsArePairwiseDistinct)
    {
        std::unordered_set<std::string> distinctSequences;
        for (const auto &val: kAllColorConstants | std::views::values)
        {
            distinctSequences.insert(std::string(val));
        }

        EXPECT_EQ(distinctSequences.size(), kAllColorConstants.size());
    }

    TEST(LogColor, ColorForLevelMapsEachSeverityToItsOwnColor)
    {
        EXPECT_STREQ(LogColor::colorForLevel(LogLevel::Trace), LogColor::kBrightBlack);
        EXPECT_STREQ(LogColor::colorForLevel(LogLevel::Debug), LogColor::kCyan);
        EXPECT_STREQ(LogColor::colorForLevel(LogLevel::Info), LogColor::kGreen);
        EXPECT_STREQ(LogColor::colorForLevel(LogLevel::Warn), LogColor::kYellow);
        EXPECT_STREQ(LogColor::colorForLevel(LogLevel::Error), LogColor::kRed);
        EXPECT_STREQ(LogColor::colorForLevel(LogLevel::Fatal), LogColor::kBrightRed);
    }

    TEST(LogColor, ColorForLevelReturnsDifferentColorsForDifferentLevels)
    {
        const std::vector<LogLevel> severityLevels = {
                LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Fatal,
        };

        std::unordered_set<std::string> distinctColors;
        for (const LogLevel level: severityLevels)
        {
            const char *color = LogColor::colorForLevel(level);
            ASSERT_NE(color, nullptr);
            distinctColors.insert(std::string(color));
        }

        EXPECT_EQ(distinctColors.size(), severityLevels.size());
    }

    TEST(LogColor, ColorForLevelFallsBackToResetForUnknownLevels)
    {
        const std::vector<LogLevel> unknownLevels = {
                LogLevel::Off,
                static_cast<LogLevel>(7),
                static_cast<LogLevel>(99),
                static_cast<LogLevel>(255),
        };

        for (const LogLevel level: unknownLevels)
        {
            EXPECT_STREQ(LogColor::colorForLevel(level), LogColor::kReset) << "level value " << static_cast<int>(level);
        }
    }

    TEST(LogColor, ColorForLevelSurvivesStressLoop)
    {
        constexpr int kiterationCount = 800;

        for (int iteration = 0; iteration < kiterationCount; ++iteration)
        {
            const auto  level = static_cast<LogLevel>(iteration % 8);
            const char *color = LogColor::colorForLevel(level);

            ASSERT_NE(color, nullptr) << "iteration " << iteration;
            EXPECT_EQ(std::string_view(color).front(), '\033') << "iteration " << iteration;
        }
    }
} // namespace AsynGyanis::Base

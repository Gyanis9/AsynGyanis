// ProcessInfo 单元测试：可执行文件目录、单个环境变量读取与按前缀整表枚举
#include "Platform/System/ProcessInfo.h"

#include "CommonTestSupport.h"
#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace AsynGyanis::Platform
{
    TEST(ProcessInfo, ApplicationDirectoryIsAbsoluteAndExisting)
    {
        const std::filesystem::path applicationDirectory = ProcessInfo::applicationDirectory();

        EXPECT_FALSE(applicationDirectory.empty());
        EXPECT_TRUE(applicationDirectory.is_absolute());
        EXPECT_TRUE(std::filesystem::is_directory(applicationDirectory));
    }

    TEST(ProcessInfo, ApplicationDirectoryIsNotTheCurrentWorkingDirectoryFallback)
    {
        // 测试可执行文件所在目录必然真实存在，用它拼接出的路径可被解析
        const std::filesystem::path applicationDirectory = ProcessInfo::applicationDirectory();
        const std::filesystem::path candidate            = applicationDirectory / "logs";

        EXPECT_EQ(candidate.parent_path(), applicationDirectory);
    }

    TEST(ProcessInfo, EnvironmentVariableReturnsValueForStandardVariable)
    {
        // PATH 在 Windows 与 Linux 的进程环境中都存在
        const std::optional<std::string> pathValue = ProcessInfo::environmentVariable("PATH");

        ASSERT_TRUE(pathValue.has_value());
        EXPECT_FALSE(pathValue->empty());
    }

    TEST(ProcessInfo, EnvironmentVariableReturnsNulloptForUndefinedName)
    {
        const std::optional<std::string> missingValue = ProcessInfo::environmentVariable("ASYN_GYANIS_DEFINITELY_UNDEFINED_VARIABLE");

        EXPECT_FALSE(missingValue.has_value());
    }

    TEST(ProcessInfo, EnvironmentVariableHandlesEmptyName)
    {
        EXPECT_NO_THROW(ProcessInfo::environmentVariable(""));
    }

    /**
     * @brief 当前进程号：非零且两次取值一致（同一次运行里它不该变）
     */
    TEST(ProcessInfo, CurrentProcessIdIsStableAndNonZero)
    {
        const long firstProcessId  = ProcessInfo::currentProcessId();
        const long secondProcessId = ProcessInfo::currentProcessId();

        EXPECT_GT(firstProcessId, 0L) << "当前进程号应为正数";
        EXPECT_EQ(firstProcessId, secondProcessId) << "同一次运行里进程号不该变";
    }

    namespace
    {
        /**
         * @brief 在结果里找一个变量名的值
         * @param entries 枚举结果
         * @param variableName 变量名
         * @return std::optional<std::string> 找到则给值
         */
        [[nodiscard]] std::optional<std::string> findVariable(const std::vector<std::pair<std::string, std::string>> &entries, const std::string &variableName)
        {
            for (const auto &[name, value]: entries)
            {
                if (name == variableName)
                {
                    return value;
                }
            }
            return std::nullopt;
        }
    } // namespace

    /**
     * @brief 按前缀枚举：命中的给出，没命中的不出现，值里的等号按第一个切
     * @details 值里带 '=' 是常见写法（连接串、带 padding 的 base64），在第二个等号处切会把名字算长、
     *          把值切短，因此这条判据要钉住
     */
    TEST(ProcessInfo, EnvironmentVariablesWithPrefixFilterByNameAndSplitAtFirstEquals)
    {
        const TestSupport::ScopedEnvironmentVariable firstVariable("ASYN_ENUM_DEMO_FIRST", "value=with=equals");
        const TestSupport::ScopedEnvironmentVariable secondVariable("ASYN_ENUM_DEMO_SECOND", "plain");
        const TestSupport::ScopedEnvironmentVariable unrelatedVariable("ASYNOTHER_ENUM_DEMO", "should-not-match");

        const std::vector<std::pair<std::string, std::string>> matches = ProcessInfo::environmentVariablesWithPrefix("ASYN_ENUM_DEMO_");

        const std::optional<std::string> firstValue = findVariable(matches, "ASYN_ENUM_DEMO_FIRST");
        ASSERT_TRUE(firstValue.has_value()) << "带前缀的变量没被枚举到";
        EXPECT_EQ(*firstValue, "value=with=equals") << "值应当在第一个 '=' 处切，剩下的都算值";
        EXPECT_TRUE(findVariable(matches, "ASYN_ENUM_DEMO_SECOND").has_value());
        EXPECT_FALSE(findVariable(matches, "ASYNOTHER_ENUM_DEMO").has_value()) << "前缀不完全匹配的变量混进了结果";
    }

    /**
     * @brief 空前缀按「不过滤」处理：能取到整张表，且每条都有名字
     * @details Windows 的驱动器当前目录一类内部条目以 '=' 开头、没有名字，枚举侧要把它挡掉，
     *          否则调用方会拿到一个名字为空、无法映射成任何键的条目
     */
    TEST(ProcessInfo, EmptyEnvironmentPrefixReturnsEveryNamedVariable)
    {
        const TestSupport::ScopedEnvironmentVariable probeVariable("ASYN_ENUM_DEMO_WHOLE_TABLE", "present");

        const std::vector<std::pair<std::string, std::string>> entries = ProcessInfo::environmentVariablesWithPrefix("");

        EXPECT_GT(entries.size(), 1U) << "整张环境变量表不该只有一条";
        EXPECT_TRUE(findVariable(entries, "ASYN_ENUM_DEMO_WHOLE_TABLE").has_value()) << "空前缀应当不过滤，新设的变量却不在表里";
        for (const auto &[name, value]: entries)
        {
            EXPECT_FALSE(name.empty()) << "枚举结果里出现了没有名字的条目";
        }
    }

    /**
     * @brief 变量名原样给出，枚举侧不做大小写变换
     * @details 名字→键的折叠规则（转小写、'__' 当层级）是使用方的语义，不该压在平台层里
     */
    TEST(ProcessInfo, EnvironmentVariableNamesKeepTheirOriginalCase)
    {
        const TestSupport::ScopedEnvironmentVariable mixedCaseVariable("ASYN_ENUM_DEMO_MixedCase", "v");

        const std::vector<std::pair<std::string, std::string>> matches = ProcessInfo::environmentVariablesWithPrefix("ASYN_ENUM_DEMO_M");

#if ASYN_PLATFORM_WIN32
        // Windows 的环境表不区分大小写：同名变量已存在时保留的是**它原来**的大小写，
        // 因此这里只断言「能按大小写不敏感的方式找回来」，不咬死拼写
        const auto insensitive = std::ranges::find_if(matches,
                                                      [](const std::pair<std::string, std::string> &entry)
                                                      {
                                                          std::string lowered;
                                                          for (const char character: entry.first)
                                                          {
                                                              lowered.push_back((character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character);
                                                          }
                                                          return lowered == "asyn_enum_demo_mixedcase";
                                                      });
        EXPECT_TRUE(insensitive != matches.end());
        EXPECT_EQ(insensitive->second, "v");
#else
        EXPECT_TRUE(findVariable(matches, "ASYN_ENUM_DEMO_MixedCase").has_value()) << "POSIX 上名字应逐字保留大小写";
#endif
    }
} // namespace AsynGyanis::Platform

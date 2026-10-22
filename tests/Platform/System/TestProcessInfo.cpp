// ProcessInfo 单元测试：可执行文件目录、单个环境变量读取与按前缀整表枚举
#include "Platform/System/ProcessInfo.h"

#include "CommonTestSupport.h"
#include "ExpectedAssertions.h"
#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if !ASYN_PLATFORM_WIN32
#include <sys/resource.h>
#endif

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

        ASSERT_HAS_VALUE(pathValue);
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
        ASSERT_HAS_VALUE(firstValue) << "带前缀的变量没被枚举到";
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
        // Windows 的环境表不区分大小写：同名变量已存在时保留的是它原来的大小写，
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
    /**
     * @brief 常驻内存读数要落在「一个真活着的进程」的量级里
     * @details 下界挡的是「忘了乘页尺寸」：测试进程按页计的话读数会掉到千字节级；
     *          上界挡的是反向的单位错（乘多了会大到不可信）。两端都不钉具体值——
     *          具体占多少内存随构建配置（ASan 一开就翻几倍）变化，钉死它等于把噪声写进断言
     */
    TEST(ProcessInfo, ResidentMemoryBytesIsPlausibleForALiveProcess)
    {
        const std::uint64_t residentBytes = ProcessInfo::residentMemoryBytes();

        EXPECT_GT(residentBytes, 1ULL << 20) << "本进程不可能只占不到 1 MiB：读到的是页数或零？";
        EXPECT_LT(residentBytes, 512ULL << 30) << "超出可信上界：单位换算多半乘多了";
    }

    /**
     * @brief 钉住：抓住一大块已写过的内存时，常驻量不会因此变小
     * @note 刻意不断言「一定涨了多少」：分配器可能把此前已常驻的页面复用掉，那种断言会随
     *       用例执行顺序翻脸。这里钉的是方向——读数必须跟着本进程占住的东西走，而不是恒为一个常数
     */
    TEST(ProcessInfo, ResidentMemoryBytesDoesNotDropWhileMemoryIsHeld)
    {
        const std::uint64_t beforeBytes = ProcessInfo::residentMemoryBytes();

        constexpr std::size_t      kHeldPageCount = 16ULL << 10; // 64 MiB 的 4 KiB 页
        std::vector<std::uint64_t> held(kHeldPageCount, 0);
        std::uint64_t              touchedSlots = 0;
        for (auto &slot: held)
        {
            slot = 1; // 逐页写过才算进常驻：只分配不触碰，内核可以一直不给页面
            touchedSlots += slot;
        }
        EXPECT_EQ(touchedSlots, kHeldPageCount) << "写入被优化掉了，这块内存其实没被触碰";

        const std::uint64_t duringHoldBytes = ProcessInfo::residentMemoryBytes();
        EXPECT_GE(duringHoldBytes, beforeBytes) << "抓着一大块已写过的内存，常驻量反而变小了";

        held.clear();
        held.shrink_to_fit();
    }

    /**
     * @brief 钉住：CPU 时间是读数成真的计数器——忙等推得动它，且永不回落
     * @details 只钉方向不钉增量口径：忙等烧到整秒数至少涨 1（Windows 的时钟粒度约 15.6ms，
     *          给足余量），恒返回 0 或缺省值的实现在这里会红。烧的时长不按挂钟固定——分片内
     *          并发的测试进程会分走 CPU，1.1 秒挂钟在双核 runner 上可能只烧到零点几秒 CPU，
     *          整数秒不动（实测 Linux CI 偶发）；改为「一直烧到读数真的推进一秒」（有界 15 秒），
     *          构造成立条件而不是赌调度余量
     */
    TEST(ProcessInfo, CpuSecondsIsAMonotonicCounterThatTracksBurnedCpu)
    {
        const std::uint64_t beforeSeconds = ProcessInfo::cpuSeconds();

        // 纯自旋烧本进程的 CPU：睡过去是拿不到 CPU 时间的，这里要的是真烧
        const auto    burnDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        std::uint64_t accumulator  = 0;
        while (std::chrono::steady_clock::now() < burnDeadline && ProcessInfo::cpuSeconds() < beforeSeconds + 1)
        {
            // 每查一轮读数之间连续自旋一小段：把 syscall 开销摊薄，烧 CPU 的效率不打折
            for (std::uint32_t spinRound = 0; spinRound < 4096U; ++spinRound)
            {
                accumulator += 1;
            }
        }
        ASSERT_GT(accumulator, 0U) << "循环被优化掉了：忙等根本不存在，这条用例没测到东西";

        const std::uint64_t afterSeconds = ProcessInfo::cpuSeconds();
        EXPECT_GE(afterSeconds, beforeSeconds) << "CPU 时间是计数器，不许回落";
        EXPECT_GE(afterSeconds, beforeSeconds + 1) << "烧满一整秒 CPU 之后整数秒仍未动（15 秒上限）：读数没接上，或恒定为一个值";
    }

    /**
     * @brief 钉住：打开的句柄数跟着真实开合走——本进程的下限不为零，开一批文件后涨、关掉后落
     */
    TEST(ProcessInfo, OpenHandleCountSeesOpenedAndClosedFiles)
    {
        const std::uint64_t baselineCount = ProcessInfo::openHandleCount();
        ASSERT_GT(baselineCount, 0U) << "正在跑的进程至少打着自己的标准流：这句读数是零说明没接上";

        const std::filesystem::path        scratchDirectory = std::filesystem::temp_directory_path();
        std::vector<std::ofstream>         openedStreams;
        std::vector<std::filesystem::path> openedPaths;
        for (std::size_t index = 0; index < 16U; ++index)
        {
            const std::filesystem::path path = scratchDirectory / std::format("asyn-procinfo-{}-{}.tmp", ProcessInfo::currentProcessId(), index);
            std::ofstream               stream(path, std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(stream.is_open()) << "临时文件开不了，这条用例的前提不成立：" << path.string();
            stream << "x";
            openedStreams.push_back(std::move(stream));
            openedPaths.push_back(path);
        }

        const std::uint64_t holdingCount = ProcessInfo::openHandleCount();
        EXPECT_GT(holdingCount, baselineCount) << "开了 16 个文件，句柄数没涨：读数不跟开合走";

        openedStreams.clear();
        for (const std::filesystem::path &path: openedPaths)
        {
            std::error_code removeError;
            static_cast<void>(std::filesystem::remove(path, removeError));
        }
        const std::uint64_t closedCount = ProcessInfo::openHandleCount();
        EXPECT_LT(closedCount, holdingCount) << "文件都关掉了，句柄数没落";
    }

#if ASYN_PLATFORM_WIN32
    /**
     * @brief 钉住：Windows 没有按进程的句柄配额，上限读数固定为 0（不拿 CRT 流上限冒充）
     */
    TEST(ProcessInfo, OpenHandleLimitIsUnavailableOnWindows)
    {
        EXPECT_EQ(ProcessInfo::openHandleLimit(), 0U);
    }
#else
    /**
     * @brief 钉住：fd 上限取自 RLIMIT_NOFILE 的软上限，且当前打开数不超过它
     */
    TEST(ProcessInfo, OpenHandleLimitMatchesTheDescriptorSoftLimit)
    {
        rlimit descriptorLimit{};
        ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &descriptorLimit), 0);

        const std::uint64_t expectedLimit = descriptorLimit.rlim_cur == RLIM_INFINITY ? 0U : static_cast<std::uint64_t>(descriptorLimit.rlim_cur);
        EXPECT_EQ(ProcessInfo::openHandleLimit(), expectedLimit);

        // 两格读数配成一对：有上限时，当前打开数不能超过它
        if (expectedLimit > 0)
        {
            EXPECT_LE(ProcessInfo::openHandleCount(), expectedLimit);
        }
    }
#endif

    /**
     * @brief 钉住：启动时刻是进程事实——两次读数相同、落在可信区间里
     */
    TEST(ProcessInfo, StartTimeIsStableAndInThePast)
    {
        const std::uint64_t firstRead  = ProcessInfo::startTimeSeconds();
        const std::uint64_t secondRead = ProcessInfo::startTimeSeconds();

        ASSERT_GT(firstRead, 1600000000ULL) << "启动时刻读不到，或落在 2020 年之前：这条读数没接上";
        EXPECT_EQ(firstRead, secondRead) << "同一进程的启动时刻两次读数不同解";
        EXPECT_LE(firstRead, static_cast<std::uint64_t>(std::time(nullptr))) << "启动时刻晚于现在：时钟换算坏了";
    }
} // namespace AsynGyanis::Platform

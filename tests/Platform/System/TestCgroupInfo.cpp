// cgroup 读数：v1/v2 层级解析、max 与 v1 内存哨兵、缺文件留空、混合层级与真机一致性
#include "Platform/System/CgroupInfo.h"

#include "CommonTestSupport.h"
#include "Platform/IO/FileContents.h"
#include "Platform/Platform.h"
#include "Platform/System/CpuAffinity.h"

#include <gtest/gtest.h>

#include <charconv>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace AsynGyanis::Platform
{
    namespace
    {
        using TestSupport::TemporaryDirectory;

        /**
         * @brief 在夹具目录里写一个读数文件
         * @param directory 临时目录
         * @param relativePath 相对路径
         * @param content 文件内容
         */
        void writeReading(const TemporaryDirectory &directory, const std::string &relativePath, const std::string &content)
        {
            ASSERT_TRUE(directory.writeNestedFile(relativePath, content)) << "夹具文件写入失败：" << relativePath;
        }
    } // namespace

    /**
     * @brief 钉住：v2 统一层级按 0:: 行定位控制组，limit 与 current 各自读对
     * @details 读数文件带 CRLF 尾，用来钉住解析前的空白裁剪
     */
    TEST(CgroupInfo, ReadsUnifiedHierarchyLimitsAndUsage)
    {
        const TemporaryDirectory directory("CgroupV2");
        writeReading(directory, "kubepods/pod1/memory.max", "1073741824\r\n");
        writeReading(directory, "kubepods/pod1/memory.current", "268435456\r\n");
        writeReading(directory, "kubepods/pod1/pids.max", "512\r\n");
        writeReading(directory, "kubepods/pod1/pids.current", "37\r\n");

        const CgroupLimits limits = CgroupInfo::readFrom(directory.path(), "0::/kubepods/pod1\n");

        ASSERT_TRUE(limits.memoryLimitBytes.has_value());
        EXPECT_EQ(*limits.memoryLimitBytes, 1073741824U);
        ASSERT_TRUE(limits.memoryCurrentBytes.has_value());
        EXPECT_EQ(*limits.memoryCurrentBytes, 268435456U);
        ASSERT_TRUE(limits.pidsLimit.has_value());
        EXPECT_EQ(*limits.pidsLimit, 512U);
        ASSERT_TRUE(limits.pidsCurrent.has_value());
        EXPECT_EQ(*limits.pidsCurrent, 37U);
    }

    /**
     * @brief 钉住：v1 每个控制器一行，控制器列表里的 memory 混在多项中也能认出
     */
    TEST(CgroupInfo, ReadsVersionOneControllerHierarchies)
    {
        const TemporaryDirectory directory("CgroupV1");
        writeReading(directory, "memory/docker/abc/memory.limit_in_bytes", "536870912\n");
        writeReading(directory, "memory/docker/abc/memory.usage_in_bytes", "134217728\n");
        writeReading(directory, "pids/docker/abc/pids.max", "256\n");
        writeReading(directory, "pids/docker/abc/pids.current", "42\n");

        const CgroupLimits limits = CgroupInfo::readFrom(directory.path(), "4:memory:/docker/abc\n5:pids:/docker/abc\n");

        ASSERT_TRUE(limits.memoryLimitBytes.has_value());
        EXPECT_EQ(*limits.memoryLimitBytes, 536870912U);
        ASSERT_TRUE(limits.memoryCurrentBytes.has_value());
        EXPECT_EQ(*limits.memoryCurrentBytes, 134217728U);
        ASSERT_TRUE(limits.pidsLimit.has_value());
        EXPECT_EQ(*limits.pidsLimit, 256U);
        ASSERT_TRUE(limits.pidsCurrent.has_value());
        EXPECT_EQ(*limits.pidsCurrent, 42U);
    }

    /**
     * @brief 钉住：控制器列表按逗号切分，memory 排在中间也能命中
     */
    TEST(CgroupInfo, FindsControllerInsideCommaSeparatedList)
    {
        const TemporaryDirectory directory("CgroupV1Mixed");
        writeReading(directory, "memory/group/memory.limit_in_bytes", "1024\n");
        writeReading(directory, "memory/group/memory.usage_in_bytes", "512\n");

        const CgroupLimits limits = CgroupInfo::readFrom(directory.path(), "2:cpu,cpuacct,memory:/group\n");

        ASSERT_TRUE(limits.memoryLimitBytes.has_value());
        EXPECT_EQ(*limits.memoryLimitBytes, 1024U);
        ASSERT_TRUE(limits.memoryCurrentBytes.has_value());
        EXPECT_EQ(*limits.memoryCurrentBytes, 512U);
        // 这行里没有 pids，统一层级也不存在：pids 两格必须是「读不出」而不是 0
        EXPECT_FALSE(limits.pidsLimit.has_value());
        EXPECT_FALSE(limits.pidsCurrent.has_value());
    }

    /**
     * @brief 钉住：max 与 v1 内存哨兵都表示「内核明确无限制」，值记 0 而不是空
     */
    TEST(CgroupInfo, UnlimitedLimitsBecomeZeroWhileMissingFilesStayEmpty)
    {
        const TemporaryDirectory versionTwo("CgroupV2Unlimited");
        writeReading(versionTwo, "group/memory.max", "max\n");
        writeReading(versionTwo, "group/pids.max", "max\n");

        const CgroupLimits versionTwoLimits = CgroupInfo::readFrom(versionTwo.path(), "0::/group\n");

        ASSERT_TRUE(versionTwoLimits.memoryLimitBytes.has_value()) << "max 是明确的无限制，不能报成读不出";
        EXPECT_EQ(*versionTwoLimits.memoryLimitBytes, 0U);
        ASSERT_TRUE(versionTwoLimits.pidsLimit.has_value());
        EXPECT_EQ(*versionTwoLimits.pidsLimit, 0U);
        // current 文件没写：读不出与「无限制」是两件事
        EXPECT_FALSE(versionTwoLimits.memoryCurrentBytes.has_value());
        EXPECT_FALSE(versionTwoLimits.pidsCurrent.has_value());

        const TemporaryDirectory versionOne("CgroupV1Unlimited");
        writeReading(versionOne, "memory/group/memory.limit_in_bytes", "9223372036854771712\n");

        const CgroupLimits versionOneLimits = CgroupInfo::readFrom(versionOne.path(), "3:memory:/group\n");

        ASSERT_TRUE(versionOneLimits.memoryLimitBytes.has_value());
        EXPECT_EQ(*versionOneLimits.memoryLimitBytes, 0U) << "v1 的超大哨兵不是真实字节数，应折成无限制";
    }

    /**
     * @brief 钉住：控制组路径指向不存在的目录时，所有字段留空，不抛出也不编造 0
     */
    TEST(CgroupInfo, MissingFilesProduceEmptyReadings)
    {
        const TemporaryDirectory directory("CgroupMissing");

        const CgroupLimits limits = CgroupInfo::readFrom(directory.path(), "0::/does/not/exist\n");

        EXPECT_FALSE(limits.memoryLimitBytes.has_value());
        EXPECT_FALSE(limits.memoryCurrentBytes.has_value());
        EXPECT_FALSE(limits.pidsLimit.has_value());
        EXPECT_FALSE(limits.pidsCurrent.has_value());
    }

    /**
     * @brief 钉住：混合层级里 memory 走 v1、pids 走 v2，两个控制器各按自己的文件布局读
     */
    TEST(CgroupInfo, ResolvesEachControllerInHybridHierarchy)
    {
        const TemporaryDirectory directory("CgroupHybrid");
        writeReading(directory, "memory/legacy/memory.limit_in_bytes", "2048\n");
        writeReading(directory, "memory/legacy/memory.usage_in_bytes", "1024\n");
        writeReading(directory, "unified/pids.max", "64\n");
        writeReading(directory, "unified/pids.current", "8\n");

        const CgroupLimits limits = CgroupInfo::readFrom(directory.path(), "3:memory:/legacy\n0::/unified\n");

        ASSERT_TRUE(limits.memoryLimitBytes.has_value());
        EXPECT_EQ(*limits.memoryLimitBytes, 2048U);
        ASSERT_TRUE(limits.memoryCurrentBytes.has_value());
        EXPECT_EQ(*limits.memoryCurrentBytes, 1024U);
        ASSERT_TRUE(limits.pidsLimit.has_value());
        EXPECT_EQ(*limits.pidsLimit, 64U);
        ASSERT_TRUE(limits.pidsCurrent.has_value());
        EXPECT_EQ(*limits.pidsCurrent, 8U);
    }

    /**
     * @brief 钉住：没有 0:: 行时，name=systemd 这类 v1 命名层级不能被当成 v2 统一层级
     */
    TEST(CgroupInfo, NamedVersionOneHierarchyIsNotTreatedAsUnified)
    {
        const TemporaryDirectory directory("CgroupNamedHierarchy");
        writeReading(directory, "system.slice/memory.max", "max\n");
        writeReading(directory, "system.slice/pids.max", "max\n");

        const CgroupLimits limits = CgroupInfo::readFrom(directory.path(), "1:name=systemd:/system.slice\n");

        EXPECT_FALSE(limits.memoryLimitBytes.has_value()) << "命名层级不是 v2，不能去根下读 memory.max";
        EXPECT_FALSE(limits.pidsLimit.has_value());
    }

    /**
     * @brief 钉住：非数字、负数与带尾随字符的内容一律报读不出，不静默截取前半段
     */
    TEST(CgroupInfo, RejectsMalformedLimitAndUsageValues)
    {
        const TemporaryDirectory directory("CgroupMalformed");
        writeReading(directory, "group/memory.max", "not-a-number\n");
        writeReading(directory, "group/memory.current", "123abc\n");
        writeReading(directory, "group/pids.max", "-1\n");
        writeReading(directory, "group/pids.current", "");

        const CgroupLimits limits = CgroupInfo::readFrom(directory.path(), "0::/group\n");

        EXPECT_FALSE(limits.memoryLimitBytes.has_value());
        EXPECT_FALSE(limits.memoryCurrentBytes.has_value());
        EXPECT_FALSE(limits.pidsLimit.has_value());
        EXPECT_FALSE(limits.pidsCurrent.has_value());
    }

#if ASYN_PLATFORM_LINUX
    /**
     * @brief 钉住：真机 cgroup 的稳定分母与 /sys 直读一致，用量读数读得到且为正
     * @details 限额文件不随负载变化，可以逐字对照；memory.current 与 pids.current 是活的计数器，
     *          并行跑全量时两次读取之间就会变，因此只钉「读得到且为正」，精确解析由夹具覆盖。
     */
    TEST(CgroupInfo, LiveReadingAgreesWithSysfs)
    {
        const std::expected<std::string, std::error_code> selfRecord = readFileContents("/proc/self/cgroup", 0, 8192);
        if (!selfRecord.has_value())
        {
            GTEST_SKIP() << "本环境读不到 /proc/self/cgroup";
        }

        const std::string relativePath      = CpuAffinity::cgroupPathFromProcRecord(*selfRecord);
        const auto        readExpectedLimit = [](const std::filesystem::path &path) -> std::optional<std::uint64_t>
        {
            const std::expected<std::string, std::error_code> contents = readFileContents(path, 0, 256);
            if (!contents.has_value())
            {
                return std::nullopt;
            }
            // 限额文件只有十进制或 max 两种形状（与 CgroupInfo 同一口径），尾随换行不影响 from_chars
            if (contents->starts_with("max"))
            {
                return std::uint64_t{0};
            }
            std::uint64_t                value  = 0;
            const std::from_chars_result parsed = std::from_chars(contents->data(), contents->data() + contents->size(), value);
            if (parsed.ec != std::errc{})
            {
                return std::nullopt;
            }
            return value;
        };

        const std::filesystem::path        memoryLimitPath     = std::filesystem::path(std::format("/sys/fs/cgroup{}/memory.max", relativePath));
        const std::optional<std::uint64_t> expectedMemoryLimit = readExpectedLimit(memoryLimitPath);
        if (!expectedMemoryLimit.has_value())
        {
            GTEST_SKIP() << "本环境没有 cgroup v2 的 memory.max（v1 布局由夹具覆盖）";
        }

        const CgroupLimits limits = CgroupInfo::read();
        ASSERT_TRUE(limits.memoryLimitBytes.has_value()) << "sysfs 有 memory.max，read() 却没读到";
        EXPECT_EQ(*limits.memoryLimitBytes, *expectedMemoryLimit);
        ASSERT_TRUE(limits.memoryCurrentBytes.has_value()) << "sysfs 有 memory.current，read() 却没读到";
        EXPECT_GT(*limits.memoryCurrentBytes, 0U) << "本进程就在这个控制组里，用量不该是 0";

        const std::filesystem::path pidsLimitPath = std::filesystem::path(std::format("/sys/fs/cgroup{}/pids.max", relativePath));
        if (const std::optional<std::uint64_t> expectedPidsLimit = readExpectedLimit(pidsLimitPath); expectedPidsLimit.has_value())
        {
            ASSERT_TRUE(limits.pidsLimit.has_value());
            EXPECT_EQ(*limits.pidsLimit, *expectedPidsLimit);
        }

        const std::filesystem::path pidsCurrentPath = std::filesystem::path(std::format("/sys/fs/cgroup{}/pids.current", relativePath));
        if (readFileContents(pidsCurrentPath, 0, 256).has_value())
        {
            ASSERT_TRUE(limits.pidsCurrent.has_value());
            EXPECT_GT(*limits.pidsCurrent, 0U);
        }
    }
#else
    /**
     * @brief 钉住：Windows 没有 cgroup 通路，read() 返回全空而不是抛异常或编造读数
     */
    TEST(CgroupInfo, ReadReturnsNoReadingsOnWindows)
    {
        const CgroupLimits limits = CgroupInfo::read();

        EXPECT_FALSE(limits.memoryLimitBytes.has_value());
        EXPECT_FALSE(limits.memoryCurrentBytes.has_value());
        EXPECT_FALSE(limits.pidsLimit.has_value());
        EXPECT_FALSE(limits.pidsCurrent.has_value());
    }
#endif
} // namespace AsynGyanis::Platform

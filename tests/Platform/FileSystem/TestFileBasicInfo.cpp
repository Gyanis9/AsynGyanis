// FileBasicInfo 单元测试：普通文件、目录、缺失路径，以及修改时间的取整方向与「现读不缓存」
#include "Platform/FileSystem/FileBasicInfo.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "PlatformTestSupport.h"

namespace AsynGyanis::Platform
{
    namespace
    {
        /**
         * @brief 写一个指定内容的临时文件
         * @param filePath 目标路径
         * @param content 要写入的字节
         */
        void writeTemporaryFile(const std::filesystem::path &filePath, const std::string &content)
        {
            std::ofstream file(filePath, std::ios::out | std::ios::binary | std::ios::trunc);
            file << content;
        }

        /**
         * @brief 用「同一瞬间两个 now() 的差值」把文件系统时间折成 Unix 秒
         * @details 独立实现一遍换算，用来核对封装给出的整秒值与其一致——静态文件的 ETag 与
         *          Last-Modified 都取自这一个数，取整方向不同就会让同一文件在改造前后得到不同验证器。
         * @param fileTime 底层给出的文件系统时间
         * @return std::int64_t 自 Unix 纪元起的秒数，向零取整
         */
        std::int64_t unixSecondsOf(const std::filesystem::file_time_type fileTime)
        {
            const std::filesystem::file_time_type    fileNow = std::filesystem::file_time_type::clock::now();
            const std::chrono::system_clock::time_point systemNow = std::chrono::system_clock::now();
            const std::chrono::system_clock::time_point converted =
                    std::chrono::time_point_cast<std::chrono::system_clock::duration>(systemNow + (fileTime - fileNow));
            return std::chrono::duration_cast<std::chrono::seconds>(converted.time_since_epoch()).count();
        }
    } // namespace

    TEST(FileBasicInfo, ReportsSizeAndModificationTimeOfRegularFile)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileBasicInfo_Regular");
        const std::filesystem::path           targetPath = temporaryDirectory.path() / "payload.txt";
        writeTemporaryFile(targetPath, "0123456789abc");

        const std::optional<FileBasicInfo> info = queryFileBasicInfo(targetPath);
        ASSERT_TRUE(info.has_value()) << "刚写好的普通文件应当查得到基本信息";
        EXPECT_TRUE(info->isRegularFile);
        EXPECT_EQ(info->sizeBytes, 13U);
        // 取整方向与独立换算一致：两侧都指向同一个整秒，验证器才不会因改造而漂移
        EXPECT_EQ(info->lastWriteSeconds, unixSecondsOf(std::filesystem::last_write_time(targetPath)));
    }

    TEST(FileBasicInfo, RoundsSubSecondModificationTimeTowardZero)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileBasicInfo_Rounding");
        const std::filesystem::path           targetPath = temporaryDirectory.path() / "timed.txt";
        writeTemporaryFile(targetPath, "x");

        // 设成一个带半秒余数的过去时刻：距整秒边界够远，复现「舍向哪一边」而不被 now() 的抖动干扰
        const std::filesystem::file_time_type subSecondTime =
                std::filesystem::file_time_type::clock::now() - std::chrono::hours(1) + std::chrono::milliseconds(500);
        std::filesystem::last_write_time(targetPath, subSecondTime);
        const std::int64_t expectedSeconds = unixSecondsOf(std::filesystem::last_write_time(targetPath));

        const std::optional<FileBasicInfo> info = queryFileBasicInfo(targetPath);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->lastWriteSeconds, expectedSeconds) << "亚秒部分必须向零舍入，与 std::chrono::duration_cast 同口径";
    }

    TEST(FileBasicInfo, ReportsDirectoryAsNotRegularFile)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileBasicInfo_Directory");

        const std::optional<FileBasicInfo> info = queryFileBasicInfo(temporaryDirectory.path());
        ASSERT_TRUE(info.has_value()) << "目录本身也查得到信息，只有「是不是普通文件」这一位要说清";
        EXPECT_FALSE(info->isRegularFile) << "目录不能被判成可服务的普通文件";
    }

    TEST(FileBasicInfo, ReturnsEmptyForMissingPath)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileBasicInfo_Missing");
        const std::filesystem::path           missingPath = temporaryDirectory.path() / "does-not-exist.txt";

        EXPECT_FALSE(queryFileBasicInfo(missingPath).has_value()) << "路径不存在时以空值表达，不抛也不给半成品信息";
    }

    TEST(FileBasicInfo, AlwaysReadsCurrentStateWithoutCaching)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileBasicInfo_NoCache");
        const std::filesystem::path           targetPath = temporaryDirectory.path() / "grows.txt";
        writeTemporaryFile(targetPath, "abc");

        const std::optional<FileBasicInfo> beforeRewrite = queryFileBasicInfo(targetPath);
        ASSERT_TRUE(beforeRewrite.has_value());
        EXPECT_EQ(beforeRewrite->sizeBytes, 3U);

        writeTemporaryFile(targetPath, "abcdefghij");
        const std::optional<FileBasicInfo> afterRewrite = queryFileBasicInfo(targetPath);
        ASSERT_TRUE(afterRewrite.has_value());
        EXPECT_EQ(afterRewrite->sizeBytes, 10U) << "本层不缓存：改写之后必须立刻看到新大小，而不是上一次查到的那份";
    }
}

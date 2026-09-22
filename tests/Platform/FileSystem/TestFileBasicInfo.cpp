// FileBasicInfo 单元测试：普通文件、目录、缺失路径，修改时间的取整方向与「现读不缓存」，
// 以及身份标记的三件事——POSIX 认得出同名重建、同一文件对象反复查要稳、Windows 认不出（记档）
#include "Platform/FileSystem/FileBasicInfo.h"

#include "Platform/Platform.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

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
         *          这里刻意用 `floor` 而不是 `duration_cast`：POSIX 的 `st_mtime` 就是向下取整的
         *          （容器实测 -0.5 秒给出 -1），照抄截断口径等于把要验的那个缺陷也抄进来。
         * @param fileTime 底层给出的文件系统时间
         * @return std::int64_t 自 Unix 纪元起的秒数，向下取整
         */
        std::int64_t unixSecondsOf(const std::filesystem::file_time_type fileTime)
        {
            const std::filesystem::file_time_type    fileNow = std::filesystem::file_time_type::clock::now();
            const std::chrono::system_clock::time_point systemNow = std::chrono::system_clock::now();
            const std::chrono::system_clock::time_point converted =
                    std::chrono::time_point_cast<std::chrono::system_clock::duration>(systemNow + (fileTime - fileNow));
            return std::chrono::floor<std::chrono::seconds>(converted.time_since_epoch()).count();
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

    TEST(FileBasicInfo, RoundsSubSecondModificationTimeDownward)
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
        EXPECT_EQ(info->lastWriteSeconds, expectedSeconds) << "亚秒部分必须舍向更早的一秒，与 POSIX 侧 st_mtime 同口径";
    }

    /**
     * @brief 钉住：早于 Unix 纪元的亚秒时间戳也要舍向更早的一秒，两平台给出同一个数
     * @details 这一档正是「向零截断」与「向下取整」分岔的地方：-0.5 秒向零给 0、向下给 -1，而 POSIX 的
     *          `st_mtime` 天生就是向下的（容器实测 `tv_sec=-1, tv_nsec=5e8`）。Windows 侧把 FILETIME
     *          折成 Unix 秒时若沿用 C++ 整除，同一份文件的 ETag 与 Last-Modified 就会比 POSIX 晚一秒——
     *          与 `PlatformTime` 里「UTC 分解不走 gmtime_s，免得两平台给出不同头」是同一条判据。
     * @note 目标时刻用 `clock_cast` 换成本平台的文件系统时钟：两家的 `file_clock` 纪元并不一致
     *       （MSVC 以 1601-01-01 为零点、libstdc++ 以 1970-01-01），写死任何一侧的刻度都会让另一侧
     *       测不到东西，而把两个时钟的 `now()` 直接相减还会把刻度提升到纳秒、当场越过 int64 的上限。
     */
    TEST(FileBasicInfo, RoundsPreEpochSubSecondModificationTimeDownward)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileBasicInfo_PreEpoch");
        const std::filesystem::path           targetPath = temporaryDirectory.path() / "ancient.txt";
        writeTemporaryFile(targetPath, "x");

        // 「Unix 纪元之前半秒」，换算的理由见本用例的 @note
        const std::filesystem::file_time_type beforeEpoch = std::chrono::clock_cast<
                std::filesystem::file_time_type::clock>(
                std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>{
                        std::chrono::milliseconds{-500}});

        std::error_code setWriteTimeError;
        std::filesystem::last_write_time(targetPath, beforeEpoch, setWriteTimeError);
        if (static_cast<bool>(setWriteTimeError) || std::filesystem::last_write_time(targetPath) != beforeEpoch)
        {
            // 文件系统认不下这个时刻（报错或把它截成 0）时这条就没测到分岔形状，如实跳过而不是假绿
            GTEST_SKIP() << "本机文件系统没能原样存下早于 1970 的时间戳（"
                         << setWriteTimeError.message() << "），这条用例无从判定";
        }
        const std::int64_t expectedSeconds = unixSecondsOf(std::filesystem::last_write_time(targetPath));

        const std::optional<FileBasicInfo> info = queryFileBasicInfo(targetPath);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(expectedSeconds, -1) << "折算法自身没落在预期档位，用例的判据无从成立";
        EXPECT_EQ(info->lastWriteSeconds, expectedSeconds)
                << "早于纪元的半秒被向零截断，同一文件在两平台上的验证器会差出一秒";
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

#if !ASYN_PLATFORM_WIN32
    /**
     * @brief 钉住（POSIX）：同一路径换成另一个文件对象、且大小与修改秒都相同，身份标记仍要认出来
     * @details 这是静态映射缓存命中判据的第三条腿：只比大小与修改秒时，「原地换成等长内容且落在
     *          同一秒」会端出旧字节。修改时间刻意从旧文件原样搬到新文件上，把这条退路堵死，因此
     *          「身份只由时间派生」那种实现会让本用例变红。Windows 侧同一形状实测认不出（见
     *          `CreationTimestampIdentityCannotSeeARecreatedFileOnWindows`），故不在本平台断言。
     */
    TEST(FileBasicInfo, DistinguishesARecreatedFileAtTheSamePath)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileBasicInfo_Recreated");
        const std::filesystem::path           targetPath = temporaryDirectory.path() / "asset.txt";

        writeTemporaryFile(targetPath, "version-one");
        const std::optional<FileBasicInfo> beforeReplacement = queryFileBasicInfo(targetPath);
        ASSERT_TRUE(beforeReplacement.has_value());
        const std::filesystem::file_time_type originalLastWrite = std::filesystem::last_write_time(targetPath);

        std::error_code removeError;
        const bool      removed = std::filesystem::remove(targetPath, removeError);
        ASSERT_FALSE(static_cast<bool>(removeError)) << "删除旧文件失败：" << removeError.message();
        ASSERT_TRUE(removed) << "旧文件不在，写出来的还是同一个文件对象，这条用例就没有被测到";
        writeTemporaryFile(targetPath, "version-two");
        std::filesystem::last_write_time(targetPath, originalLastWrite);

        const std::optional<FileBasicInfo> afterReplacement = queryFileBasicInfo(targetPath);
        ASSERT_TRUE(afterReplacement.has_value());
        EXPECT_EQ(afterReplacement->sizeBytes, beforeReplacement->sizeBytes);
        EXPECT_EQ(afterReplacement->lastWriteSeconds, beforeReplacement->lastWriteSeconds)
                << "修改时间没搬过去，后面的身份判据就成了空转";
        EXPECT_NE(afterReplacement->identityTag, beforeReplacement->identityTag)
                << "另一个文件对象被当成了同一个文件，缓存会一直端出旧内容";
    }
#endif

    /**
     * @brief 钉住：同一个文件对象反复查询要给出稳定的身份标记
     * @details 反向的护栏：身份若掺进了任何会变的东西（最后访问时间、每次查询自取的计数器这类），
     *          每次查询都在变，静态映射缓存就永远命中不上——退化成「每次请求现建一次映射」。
     */
    TEST(FileBasicInfo, KeepsIdentityStableAcrossRepeatedQueriesOfTheSameFile)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileBasicInfo_IdentityStable");
        const std::filesystem::path           targetPath = temporaryDirectory.path() / "stable.txt";
        writeTemporaryFile(targetPath, "unchanged");

        const std::optional<FileBasicInfo> firstQuery = queryFileBasicInfo(targetPath);
        const std::optional<FileBasicInfo> secondQuery = queryFileBasicInfo(targetPath);
        ASSERT_TRUE(firstQuery.has_value());
        ASSERT_TRUE(secondQuery.has_value());
        EXPECT_EQ(firstQuery->identityTag, secondQuery->identityTag) << "同一个文件对象的身份标记不稳定，缓存无从命中";
    }

#if ASYN_PLATFORM_WIN32
    /**
     * @brief 记档（Windows）：创建时间派生的身份认不出「删掉再同名重建」这一常规发布路径
     * @details NTFS 的隧道缓存（tunneling）在删除后短时限内以同名重建时会把创建时间连同短文件名一起
     *          还原回去，于是两次查询拿到逐位相同的标记——这恰好是映射缓存最需要认出的那种替换。
     *          为它每请求多付一次只读属性的句柄查询不值（映射缓存在本平台本来就因「活动映射挡住
     *          rename 与截断」而关闭），所以这里断言现状而不是理想。
     * @note 谁要让 Windows 也启用映射缓存，这条会先变红：那时把判据换成文件系统记账的卷号 + 文件 ID
     *       （实测多一次句柄查询，9.5 到 10.1 微秒每请求），并把本用例改成断言「认得出」。
     */
    TEST(FileBasicInfo, CreationTimestampIdentityCannotSeeARecreatedFileOnWindows)
    {
        const TestSupport::TemporaryDirectory temporaryDirectory("FileBasicInfo_Tunneling");
        const std::filesystem::path           targetPath = temporaryDirectory.path() / "asset.txt";

        writeTemporaryFile(targetPath, "version-one");
        const std::optional<FileBasicInfo> beforeReplacement = queryFileBasicInfo(targetPath);
        ASSERT_TRUE(beforeReplacement.has_value());

        std::error_code removeError;
        ASSERT_TRUE(std::filesystem::remove(targetPath, removeError)) << "旧文件没被删掉，这里就没有重建任何东西";
        writeTemporaryFile(targetPath, "version-two");

        const std::optional<FileBasicInfo> afterReplacement = queryFileBasicInfo(targetPath);
        ASSERT_TRUE(afterReplacement.has_value());
        EXPECT_EQ(afterReplacement->sizeBytes, beforeReplacement->sizeBytes);
        EXPECT_EQ(afterReplacement->identityTag, beforeReplacement->identityTag)
                << "身份已不再由创建时间派生，Windows 侧的映射缓存可以打开了，见本用例的 @note";
    }
#endif
}

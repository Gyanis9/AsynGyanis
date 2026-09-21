#include "Platform/FileSystem/FileBasicInfo.h"

#include "Platform/Platform.h"

#if ASYN_PLATFORM_WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace AsynGyanis::Platform
{
#if ASYN_PLATFORM_WIN32
    namespace
    {
        /// FILETIME 的纪元差：从 1601-01-01 到 1970-01-01 的 100 纳秒刻度数
        constexpr std::int64_t kFileTimeEpochOffsetHundredNanoseconds = 116444736000000000LL;

        /// 一秒里的 100 纳秒刻度数
        constexpr std::int64_t kHundredNanosecondsPerSecond = 10000000LL;

        /**
         * @brief 把 FILETIME 折成 Unix 秒
         * @param fileTime 底层给出的 100 纳秒刻度时间
         * @return std::int64_t Unix 秒，向零取整，与 std::chrono::duration_cast<seconds> 一致
         */
        [[nodiscard]] std::int64_t fileTimeToUnixSeconds(const FILETIME &fileTime) noexcept
        {
            ULARGE_INTEGER ticks{};
            ticks.LowPart  = fileTime.dwLowDateTime;
            ticks.HighPart = fileTime.dwHighDateTime;
            // 先做有符号减法再整除：1970 之前的时间戳（被显式设成早于纪元的那些文件）因此
            // 走的是同一条舍入路径，不需要为它另开一个分支
            const std::int64_t hundredNanosecondsSinceEpoch =
                static_cast<std::int64_t>(ticks.QuadPart) - kFileTimeEpochOffsetHundredNanoseconds;
            return hundredNanosecondsSinceEpoch / kHundredNanosecondsPerSecond;
        }
    } // namespace

    std::optional<FileBasicInfo> queryFileBasicInfo(const std::filesystem::path &path) noexcept
    {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        // 一次调用就同时给出「是不是目录 + 大小 + 最后修改时间」，不必逐样把路径再开一遍。
        // 宽字符路径直接交给 W 版接口：非 ASCII 目录名不经过任何代码页转换
        if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) == 0)
        {
            return std::nullopt;
        }

        FileBasicInfo info;
        // 与 std::filesystem::is_regular_file 在本平台的判据取平：它只看目录位，符号链接由
        // GetFileAttributesExW 跟随后按目标的属性报告，故这里刻意不再排除 REPARSE_POINT——
        // 加上那一条会让「链接指向的静态文件」从可服务变成 404
        info.isRegularFile    = (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        info.sizeBytes        = (static_cast<std::uintmax_t>(attributes.nFileSizeHigh) << 32) |
                                static_cast<std::uintmax_t>(attributes.nFileSizeLow);
        info.lastWriteSeconds = fileTimeToUnixSeconds(attributes.ftLastWriteTime);
        return info;
    }
#else
    std::optional<FileBasicInfo> queryFileBasicInfo(const std::filesystem::path &path) noexcept
    {
        struct ::stat status{};
        // ::stat（而非 lstat）跟随符号链接，与 std::filesystem::is_regular_file 的默认口径一致
        if (::stat(path.c_str(), &status) != 0)
        {
            return std::nullopt;
        }

        FileBasicInfo info;
        info.isRegularFile    = S_ISREG(status.st_mode) != 0;
        info.sizeBytes        = static_cast<std::uintmax_t>(status.st_size);
        // 亚秒部分直接舍：ETag 与 Last-Modified 用的都是这一个整秒值，两处必须看到同一个数
        info.lastWriteSeconds = static_cast<std::int64_t>(status.st_mtime);
        return info;
    }
#endif
}

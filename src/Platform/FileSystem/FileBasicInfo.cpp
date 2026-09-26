#include "Platform/FileSystem/FileBasicInfo.h"

#include "Platform/Platform.h"

#include <chrono>

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

        /// FILETIME 的刻度单位：一秒的一千万分之一
        using HundredNanosecondTicks = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;

        /**
         * @brief 把「自 1601-01-01 起的 100 纳秒刻度」折成 Unix 秒
         * @param hundredNanosecondTicks 底层时间戳的原始刻度值
         * @return std::int64_t Unix 秒，向下取整（向负无穷，而不是向零截断）
         */
        [[nodiscard]] std::int64_t hundredNanosecondsToUnixSeconds(const std::int64_t hundredNanosecondTicks) noexcept
        {
            // 先做有符号减法再向下取整：POSIX 侧的 st_mtime 天生就是向下取整的（容器实测 -0.5 秒给出
            // -1，而 tv_nsec 恒非负），C++ 的整除却向零截断——同一份被显式设成早于 1970 的文件会在两
            // 平台上差出一秒，而 ETag 与 Last-Modified 都取自这个数
            const std::int64_t hundredNanosecondsSinceEpoch = hundredNanosecondTicks - kFileTimeEpochOffsetHundredNanoseconds;
            return std::chrono::floor<std::chrono::seconds>(HundredNanosecondTicks{hundredNanosecondsSinceEpoch}).count();
        }

        /**
         * @brief 把 FILETIME 折成 Unix 秒
         * @param fileTime 底层给出的 100 纳秒刻度时间
         * @return std::int64_t Unix 秒，向下取整（向负无穷，而不是向零截断）
         */
        [[nodiscard]] std::int64_t fileTimeToUnixSeconds(const FILETIME &fileTime) noexcept
        {
            ULARGE_INTEGER ticks{};
            ticks.LowPart  = fileTime.dwLowDateTime;
            ticks.HighPart = fileTime.dwHighDateTime;
            return hundredNanosecondsToUnixSeconds(static_cast<std::int64_t>(ticks.QuadPart));
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
        info.sizeBytes        = (static_cast<std::uintmax_t>(attributes.nFileSizeHigh) << 32) | static_cast<std::uintmax_t>(attributes.nFileSizeLow);
        info.lastWriteSeconds = fileTimeToUnixSeconds(attributes.ftLastWriteTime);
        {
            ULARGE_INTEGER creation{};
            creation.LowPart  = attributes.ftCreationTime.dwLowDateTime;
            creation.HighPart = attributes.ftCreationTime.dwHighDateTime;
            // 原样取 100 纳秒刻度而不折算成秒：这里要的只是「是不是同一个文件」，越细越不会误判。
            // 但它在该平台认不出「删掉再同名重建」：NTFS 的隧道缓存会把旧文件的创建时间还原到新文件上
            // （实测两次逐位相同）。换成文件系统记账的卷号 + 文件 ID 才认得出，代价是每请求多一次只读
            // 属性的句柄查询（实测 9.5 到 10.1 µs）；本平台的静态映射缓存处于关闭状态，这个标记没有
            // 消费方，因此不付这次查询
            info.identityTag = creation.QuadPart;
        }
        return info;
    }

    std::optional<FileBasicInfo> queryOpenedFileBasicInfo(const NativeFileHandle handle) noexcept
    {
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        {
            return std::nullopt;
        }

        // 两条「只问句柄」的窄查询合起来给全四项，比一次 GetFileInformationByHandle 便宜近一半：
        // 后者除标准信息外还要向文件系统要目录索引项（本机实测 1.9 µs，这里两条各 0.5 与 0.6 µs），
        // 而它多给的文件索引本层用不上——按路径查的那条拿不到索引，两条必须同算法
        FILE_BASIC_INFO basicInformation{};
        // 枚举成员要带类名限定：本命名空间里有个同名结构体，裸写 FileBasicInfo 找到的它是类型不是值
        if (::GetFileInformationByHandleEx(static_cast<HANDLE>(handle), FILE_INFO_BY_HANDLE_CLASS::FileBasicInfo, &basicInformation, sizeof(basicInformation)) == 0)
        {
            return std::nullopt;
        }

        FILE_STANDARD_INFO standardInformation{};
        if (::GetFileInformationByHandleEx(static_cast<HANDLE>(handle), FILE_INFO_BY_HANDLE_CLASS::FileStandardInfo, &standardInformation, sizeof(standardInformation)) == 0)
        {
            return std::nullopt;
        }

        FileBasicInfo info;
        // 四项取值与按路径查的那条逐位同刻度（同一换算、同一取整方向、同一身份标记算法），否则调用方
        // 一比就永远不等。这里刻意不用文件索引当身份标记：它其实更强（认得出隧道缓存还原创建时间的
        // 那次替换），但按路径查的那条多一次句柄查询才拿得到，两条必须同算法
        info.isRegularFile    = standardInformation.Directory == FALSE;
        info.sizeBytes        = static_cast<std::uintmax_t>(standardInformation.EndOfFile.QuadPart);
        info.lastWriteSeconds = hundredNanosecondsToUnixSeconds(basicInformation.LastWriteTime.QuadPart);
        // 原样取 100 纳秒刻度而不折算成秒：与按路径查的那条同源（都是 $FILE_NAME 里的创建时间）
        info.identityTag = static_cast<std::uint64_t>(basicInformation.CreationTime.QuadPart);
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
        info.isRegularFile = S_ISREG(status.st_mode) != 0;
        info.sizeBytes     = static_cast<std::uintmax_t>(status.st_size);
        // st_mtime 只到秒，而底层的 tv_nsec 恒非负，因此这个数天生就是向下取整的结果；Windows 侧按
        // 同一口径折算。ETag 与 Last-Modified 用的都是这一个整秒值，两处必须看到同一个数
        info.lastWriteSeconds = static_cast<std::int64_t>(status.st_mtime);
        // inode 号会被回收：删掉再同名重建时，文件系统往往把刚释放的那个号原样发给新文件（容器内
        // overlayfs 实测两次同为 245657），所以光看 inode 认不出这种替换。再把设备号与 ctime 折进来：
        // 设备号让「同一条路径换挂到另一个文件系统」不被当成同一个文件，ctime 则任何用户态 API 都
        // 设不了（只能由内核在 inode 变更时刷新），回收来的 inode 必然带一个新的
        const std::uint64_t inodeTag  = static_cast<std::uint64_t>(status.st_ino);
        const std::uint64_t deviceTag = static_cast<std::uint64_t>(status.st_dev);
        const std::uint64_t changeTag = static_cast<std::uint64_t>(status.st_ctim.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(status.st_ctim.tv_nsec);
        info.identityTag              = (inodeTag * 0x9E3779B97F4A7C15ULL) ^ (deviceTag * 0xC2B2AE3D27D4EB4FULL) ^ changeTag;
        return info;
    }

    std::optional<FileBasicInfo> queryOpenedFileBasicInfo(const NativeFileHandle handle) noexcept
    {
        if (handle < 0)
        {
            return std::nullopt;
        }

        struct ::stat status{};
        // fstat 问的是句柄绑定的那个对象：路径后来被 rename 换掉也不影响这里的答案
        if (::fstat(handle, &status) != 0)
        {
            return std::nullopt;
        }

        FileBasicInfo info;
        info.isRegularFile    = S_ISREG(status.st_mode) != 0;
        info.sizeBytes        = static_cast<std::uintmax_t>(status.st_size);
        info.lastWriteSeconds = static_cast<std::int64_t>(status.st_mtime);
        // 与上面按路径查的那一段必须逐位同算法（同一条用例钉着这个不等就会响）：两条路的取值一旦分叉，
        // 调用方比「是不是同一个版本」就永远得到「不是」
        const std::uint64_t inodeTag  = static_cast<std::uint64_t>(status.st_ino);
        const std::uint64_t deviceTag = static_cast<std::uint64_t>(status.st_dev);
        const std::uint64_t changeTag = static_cast<std::uint64_t>(status.st_ctim.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(status.st_ctim.tv_nsec);
        info.identityTag              = (inodeTag * 0x9E3779B97F4A7C15ULL) ^ (deviceTag * 0xC2B2AE3D27D4EB4FULL) ^ changeTag;
        return info;
    }
#endif
} // namespace AsynGyanis::Platform

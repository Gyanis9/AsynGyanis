#include "Platform/FileSystem/AtomicFileWriter.h"

#include "Platform/Platform.h"
#include "Platform/System/ProcessInfo.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>

#if !ASYN_PLATFORM_WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace AsynGyanis::Platform
{
    namespace
    {
        /**
         * @brief 把文件内容刷入持久存储（POSIX fsync / Windows FlushFileBuffers）
         * @param filePath 目标文件路径
         * @return true 已落盘
         * @note 文件类 API 直接走平台调用：Platform::FileDescriptor 在 Windows 上是套接字专用（read 即 recv）
         */
        bool flushFileToDisk(const std::filesystem::path &filePath) noexcept
        {
#if ASYN_PLATFORM_WIN32
            const HANDLE fileHandle = ::CreateFileW(filePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                                    FILE_ATTRIBUTE_NORMAL, nullptr);
            if (fileHandle == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            const bool isFlushed = ::FlushFileBuffers(fileHandle) != 0;
            ::CloseHandle(fileHandle);
            return isFlushed;
#else
            const int fileDescriptor = ::open(filePath.c_str(), O_WRONLY);
            if (fileDescriptor < 0)
            {
                return false;
            }
            const bool isFlushed = ::fsync(fileDescriptor) == 0;
            ::close(fileDescriptor);
            return isFlushed;
#endif
        }

        /// 目录项落盘：rename 本身的持久化要靠父目录的 fsync（Windows 需要备份语义句柄，收益有限，不做）
        void flushDirectoryToDisk(const std::filesystem::path &directoryPath) noexcept
        {
#if ASYN_PLATFORM_WIN32
            static_cast<void>(directoryPath);
#else
            const int directoryDescriptor = ::open(directoryPath.c_str(), O_RDONLY | O_DIRECTORY);
            if (directoryDescriptor >= 0)
            {
                static_cast<void>(::fsync(directoryDescriptor));
                ::close(directoryDescriptor);
            }
#endif
        }

        /**
         * @brief 尽力清掉没换出去的临时文件，清理结果不参与报告
         * @details 失败原因由触发它的那一步定下，这里只是别把半截文件留在目录里。把上一步那个
         *          error_code 复用给 remove() 会把它覆盖成清理结果（成功即被清空），调用方拿到的
         *          就成了「替换失败：Success」这种自相矛盾的原因。
         * @param temporaryPath 待丢弃的临时文件路径
         */
        void discardTemporaryFile(const std::filesystem::path &temporaryPath) noexcept
        {
            std::error_code cleanupError;
            std::filesystem::remove(temporaryPath, cleanupError);
        }
    } // namespace

    bool AtomicFileWriter::writeText(const std::filesystem::path &targetPath, const std::string &text, const std::optional<std::filesystem::perms> permissions, std::string *error)
    {
        const auto reportFailure = [error](const std::string &reason)
        {
            if (error != nullptr)
            {
                *error = reason;
            }
            return false;
        };

        std::error_code fileSystemError;
        if (const auto parentDirectory = targetPath.parent_path(); !parentDirectory.empty() && !std::filesystem::exists(parentDirectory, fileSystemError))
        {
            std::filesystem::create_directories(parentDirectory, fileSystemError);
            if (fileSystemError)
            {
                return reportFailure("创建目录 '" + parentDirectory.string() + "' 失败：" + fileSystemError.message());
            }
        }

        // 临时名同时带上进程号与进程内计数器。只有计数器是不够的：它是进程内的，两个进程
        // 并发写同一目标会各自从 0 开始算出同一个 `.tmp.0`，于是「各写各的临时文件」这件事
        // 恰好在最需要它的跨进程发布场景里失效——两个写者交叉写同一个 .tmp，再把夹杂内容
        // rename 成目标。进程号在同一台机器上同时存活的进程之间唯一，配上计数器两个维度都分开
        static std::atomic<std::uint32_t> temporaryFileCounter{0};
        const std::filesystem::path       temporaryPath = targetPath.string() + ".tmp." +
                                                    std::to_string(ProcessInfo::currentProcessId()) + "." +
                                                    std::to_string(temporaryFileCounter.fetch_add(1, std::memory_order_relaxed));

        bool isTemporaryFileWritten = false;
        {
            std::ofstream temporaryFile(temporaryPath, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!temporaryFile.is_open())
            {
                return reportFailure("无法打开临时文件 '" + temporaryPath.string() + "'");
            }

            temporaryFile.write(text.data(), static_cast<std::streamsize>(text.size()));
            temporaryFile.flush();
            isTemporaryFileWritten = temporaryFile.good();
        }
        // 判定挪到作用域之外再做：文件还开着的时候删不掉它——实测 Windows 上对开着的路径调
        // std::filesystem::remove 会以 error 32（共享冲突）失败并原样留着，POSIX 虽能 unlink 但目录项
        // 仍被句柄占着。本层对外承诺「写失败时临时文件会被清理」，清理就得等 RAII 先收工
        if (!isTemporaryFileWritten)
        {
            discardTemporaryFile(temporaryPath);
            return reportFailure("写入临时文件 '" + temporaryPath.string() + "' 失败");
        }

        // 落盘屏障：flush 只把用户态缓冲交给内核，断电时仍可能留下「目标文件已存在但内容为空/半截」。
        // 头文件承诺的「避免断电或中断留下半截文件」要求内容先真正落盘，再动 rename
        if (!flushFileToDisk(temporaryPath))
        {
            discardTemporaryFile(temporaryPath);
            return reportFailure("把临时文件 '" + temporaryPath.string() + "' 刷入持久存储失败");
        }

        if (permissions.has_value())
        {
            std::error_code permissionError;
            std::filesystem::permissions(temporaryPath, *permissions, std::filesystem::perm_options::replace, permissionError);
            if (permissionError)
            {
                const std::string reason = "设置 '" + temporaryPath.string() + "' 权限失败：" + permissionError.message();
                discardTemporaryFile(temporaryPath);
                return reportFailure(reason);
            }
        }

        std::error_code renameError;
        std::filesystem::rename(temporaryPath, targetPath, renameError);
        if (renameError)
        {
            const std::string reason = "替换 '" + targetPath.string() + "' 失败：" + renameError.message();
            discardTemporaryFile(temporaryPath);
            return reportFailure(reason);
        }

        // rename 这一步本身也要落盘：目录项不刷下去，掉电后可能回到「旧文件还在、新文件没出现过」
        flushDirectoryToDisk(targetPath.parent_path());

        return true;
    }
} // namespace AsynGyanis::Platform

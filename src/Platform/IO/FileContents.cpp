#include "Platform/IO/FileContents.h"

#include "Platform/Platform.h"

#if ASYN_PLATFORM_WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <utility>

namespace AsynGyanis::Platform
{
#if ASYN_PLATFORM_WIN32
    std::expected<std::string, std::error_code> readFileContents(const std::filesystem::path &filePath,
                                                                 const std::size_t offset,
                                                                 const std::size_t length) noexcept
    {
        if (length == 0)
        {
            // 空段不需要打开文件：省掉一次系统调用，也不需要区分「文件不存在」与「什么都不要读」
            return std::string{};
        }

        // 共享模式与 MemoryMappedFile 取平：读正文不该把发布方的改名或截断挡在共享冲突上
        HANDLE fileHandle = ::CreateFileW(filePath.c_str(), GENERIC_READ,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fileHandle == INVALID_HANDLE_VALUE)
        {
            return std::unexpected(std::error_code(static_cast<int>(::GetLastError()), std::system_category()));
        }

        // 偏移单独设：不假定调用方给的偏移落在 32 位以内
        LARGE_INTEGER seekPosition{};
        seekPosition.QuadPart = static_cast<LONGLONG>(offset);
        if (::SetFilePointerEx(fileHandle, seekPosition, nullptr, FILE_BEGIN) == 0)
        {
            const std::error_code error(static_cast<int>(::GetLastError()), std::system_category());
            ::CloseHandle(fileHandle);
            return std::unexpected(error);
        }

        std::string contents;
        contents.resize(length);
        std::size_t bytesRead = 0;
        while (bytesRead < length)
        {
            // 单次 ReadFile 的长度形参是 DWORD，超过就分次读；本函数的调用方不会走到第二轮
            const auto chunkLength = static_cast<DWORD>(
                std::min<std::size_t>(length - bytesRead, std::numeric_limits<DWORD>::max()));
            DWORD readNow = 0;
            if (::ReadFile(fileHandle, contents.data() + bytesRead, chunkLength, &readNow, nullptr) == 0)
            {
                // 偏移越界在 Windows 上不是错误而是「读到 0 字节」，因此这里只报真的失败的读
                const std::error_code error(static_cast<int>(::GetLastError()), std::system_category());
                ::CloseHandle(fileHandle);
                return std::unexpected(error);
            }
            if (readNow == 0)
            {
                // 到文件末尾：交回已读到的那段，由调用方判断长度是否够用
                break;
            }
            bytesRead += readNow;
        }
        ::CloseHandle(fileHandle);

        contents.resize(bytesRead);
        return contents;
    }
#else
    std::expected<std::string, std::error_code> readFileContents(const std::filesystem::path &filePath,
                                                                 const std::size_t offset,
                                                                 const std::size_t length) noexcept
    {
        if (length == 0)
        {
            return std::string{};
        }

        const int descriptor = ::open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor < 0)
        {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }

        std::string contents;
        contents.resize(length);
        std::size_t bytesRead = 0;
        while (bytesRead < length)
        {
            // pread 自带偏移、不动别人的文件指针，因此同一段内容可以并发读同一只 fd
            const ssize_t readNow = ::pread(descriptor, contents.data() + bytesRead, length - bytesRead,
                                            static_cast<off_t>(offset + bytesRead));
            if (readNow < 0)
            {
                // 被信号打断是唯一的「可重试」情形，其余一律如实上抛给调用方
                if (errno == EINTR)
                {
                    continue;
                }
                const std::error_code error(errno, std::system_category());
                ::close(descriptor);
                return std::unexpected(error);
            }
            if (readNow == 0)
            {
                break;
            }
            bytesRead += static_cast<std::size_t>(readNow);
        }
        ::close(descriptor);

        contents.resize(bytesRead);
        return contents;
    }
#endif
} // namespace AsynGyanis::Platform

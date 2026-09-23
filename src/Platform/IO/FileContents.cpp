#include "Platform/IO/FileContents.h"

#include "Platform/FileSystem/FileBasicInfo.h"

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
    std::expected<std::size_t, std::error_code> readFileContentsInto(const std::filesystem::path &filePath,
                                                                     const std::size_t offset,
                                                                     const std::size_t length,
                                                                     std::string &target,
                                                                     FileBasicInfo *openedAs) noexcept
    {
        // 先把缓冲调到位：容量够时 resize 只是改长度，keep-alive 连接的第二条请求起不再分配
        target.resize(length);
        if (length == 0)
        {
            // 空段不需要打开文件：省掉一次系统调用，也不需要区分「文件不存在」与「什么都不要读」
            return std::size_t{0};
        }

        // 共享模式与 MemoryMappedFile 取平：读正文不该把发布方的删除挡在共享冲突上
        // （改名与截断救不了，那是段对象自身的限制，与共享位无关）
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

        std::size_t bytesRead = 0;
        while (bytesRead < length)
        {
            // 单次 ReadFile 的长度形参是 DWORD，超过就分次读；静态正文的上限远到不了第二轮
            const auto chunkLength = static_cast<DWORD>(
                std::min<std::size_t>(length - bytesRead, std::numeric_limits<DWORD>::max()));
            DWORD readNow = 0;
            if (::ReadFile(fileHandle, target.data() + bytesRead, chunkLength, &readNow, nullptr) == 0)
            {
                // 偏移越界在 Windows 上不是错误而是「读到 0 字节」，因此这里只报真的失败的读
                const std::error_code error(static_cast<int>(::GetLastError()), std::system_category());
                ::CloseHandle(fileHandle);
                return std::unexpected(error);
            }
            if (readNow == 0)
            {
                // 到文件末尾：把长度收到实际读到的那段，由调用方判断内容是否完整
                break;
            }
            bytesRead += readNow;
        }
        // 把「实际读到的那个对象」交回调用方：路径在 open 与 stat 之间被换掉时，句柄仍然绑着旧对象，
        // 只有从句柄问才能判「发出去的这段字节是不是验证器描述的那一版」。问不出来就整单失败：
        // 带着一个无法核对的验证器把正文发出去，比回一条 500 更糟
        if (openedAs != nullptr)
        {
            const std::optional<FileBasicInfo> openedInfo = queryOpenedFileBasicInfo(fileHandle);
            ::CloseHandle(fileHandle);
            if (!openedInfo.has_value())
            {
                return std::unexpected(std::error_code(static_cast<int>(::GetLastError()), std::system_category()));
            }
            *openedAs = *openedInfo;
        }
        else
        {
            ::CloseHandle(fileHandle);
        }

        target.resize(bytesRead);
        return bytesRead;
    }
#else
    std::expected<std::size_t, std::error_code> readFileContentsInto(const std::filesystem::path &filePath,
                                                                     const std::size_t offset,
                                                                     const std::size_t length,
                                                                     std::string &target,
                                                                     FileBasicInfo *openedAs) noexcept
    {
        target.resize(length);
        if (length == 0)
        {
            return std::size_t{0};
        }

        const int descriptor = ::open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor < 0)
        {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }

        std::size_t bytesRead = 0;
        while (bytesRead < length)
        {
            // pread 自带偏移、不动别人的文件指针，因此同一条路径可以并发读同一只 fd
            const ssize_t readNow = ::pread(descriptor, target.data() + bytesRead, length - bytesRead,
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
        // 与 Windows 那半边同口径：身份从已打开的描述符上问，不随路径后来的替换而改变
        if (openedAs != nullptr)
        {
            const std::optional<FileBasicInfo> openedInfo = queryOpenedFileBasicInfo(descriptor);
            ::close(descriptor);
            if (!openedInfo.has_value())
            {
                return std::unexpected(std::error_code(errno, std::system_category()));
            }
            *openedAs = *openedInfo;
        }
        else
        {
            ::close(descriptor);
        }

        target.resize(bytesRead);
        return bytesRead;
    }
#endif

    std::expected<std::string, std::error_code> readFileContents(const std::filesystem::path &filePath,
                                                                 const std::size_t offset,
                                                                 const std::size_t length) noexcept
    {
        // 便捷层：读法只有一份实现，这里给出一份自己的缓冲，读成功就交出所有权
        std::string contents;
        const std::expected<std::size_t, std::error_code> bytesRead =
                readFileContentsInto(filePath, offset, length, contents);
        if (!bytesRead.has_value())
        {
            return std::unexpected(bytesRead.error());
        }
        return contents;
    }
} // namespace AsynGyanis::Platform

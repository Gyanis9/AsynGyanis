#include "Platform/IO/MemoryMappedFile.h"

#include <cstdint>
#include <utility>

#if ASYN_PLATFORM_WIN32
#include <limits>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace AsynGyanis::Platform
{
    MemoryMappedFile MemoryMappedFile::open(const std::filesystem::path &filePath) noexcept
    {
        MemoryMappedFile mappedFile;

#if ASYN_PLATFORM_WIN32
        // 文件类 Win32 API 的错误来自 GetLastError，与 socket 的 WSAGetLastError、CRT 的 errno 都不同源
        // 共享模式放到 READ|WRITE|DELETE：映射存活期间别的进程仍可**删除**该文件（不放宽 DELETE 就连
        // 删除都会撞共享冲突，Linux 侧 mmap 之后 unlink 不受影响）。要说清的是它换不来什么：实测
        // 「写临时文件 + rename 覆盖」在视图存活时仍以 ERROR_ACCESS_DENIED(5) 失败，就地截断以
        // ERROR_USER_MAPPED_FILE(1224) 失败——这是段对象自己的限制，与共享位无关。因此需要让发布方
        // 随时替换文件的场景（静态文件服务）在本平台不要长期持有映射，读完即走的堆正文才是那条路
        HANDLE fileHandle =
                ::CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fileHandle == INVALID_HANDLE_VALUE)
        {
            mappedFile.m_lastError = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
            return mappedFile;
        }

        LARGE_INTEGER fileSize{};
        if (::GetFileSizeEx(fileHandle, &fileSize) == 0)
        {
            mappedFile.m_lastError = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
            ::CloseHandle(fileHandle);
            return mappedFile;
        }
        if (fileSize.QuadPart <= 0)
        {
            // 空文件：不需要映射，得到「有效但字节数为 0」的对象，调用方不必特判
            mappedFile.m_fileHandle = fileHandle;
            mappedFile.m_isValid    = true;
            return mappedFile;
        }
        if (static_cast<std::uint64_t>(fileSize.QuadPart) > std::numeric_limits<std::size_t>::max())
        {
            mappedFile.m_lastError = std::error_code(static_cast<int>(ERROR_FILE_TOO_LARGE), std::system_category());
            ::CloseHandle(fileHandle);
            return mappedFile;
        }

        HANDLE mappingHandle = ::CreateFileMappingW(fileHandle, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mappingHandle == nullptr)
        {
            mappedFile.m_lastError = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
            ::CloseHandle(fileHandle);
            return mappedFile;
        }

        void *const base = ::MapViewOfFile(mappingHandle, FILE_MAP_READ, 0, 0, 0);
        if (base == nullptr)
        {
            mappedFile.m_lastError = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
            ::CloseHandle(mappingHandle);
            ::CloseHandle(fileHandle);
            return mappedFile;
        }

        mappedFile.m_base          = base;
        mappedFile.m_length        = static_cast<std::size_t>(fileSize.QuadPart);
        mappedFile.m_mappingHandle = mappingHandle;
        mappedFile.m_fileHandle    = fileHandle;
        mappedFile.m_isValid       = true;
#else
        const int fileDescriptor = ::open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fileDescriptor < 0)
        {
            mappedFile.m_lastError = std::error_code(errno, std::system_category());
            return mappedFile;
        }

        struct stat fileStatus{};
        if (::fstat(fileDescriptor, &fileStatus) != 0)
        {
            mappedFile.m_lastError = std::error_code(errno, std::system_category());
            ::close(fileDescriptor);
            return mappedFile;
        }
        if (fileStatus.st_size <= 0)
        {
            // 空文件：不需要映射，得到「有效但字节数为 0」的对象，调用方不必特判。
            // 描述符与其它情形一样留着，释放时机由 close() 统一负责
            mappedFile.m_fileDescriptor = fileDescriptor;
            mappedFile.m_isValid        = true;
            return mappedFile;
        }

        const auto  mappedLength = static_cast<std::size_t>(fileStatus.st_size);
        void *const mappedBase   = ::mmap(nullptr, mappedLength, PROT_READ, MAP_PRIVATE, fileDescriptor, 0);
        if (mappedBase == MAP_FAILED)
        {
            mappedFile.m_lastError = std::error_code(errno, std::system_category());
            ::close(fileDescriptor);
            return mappedFile;
        }

        // 映射建立后描述符刻意不关：sendfile 等零拷贝发送要拿它把文件的一段直接交给内核搬运，
        // 关掉就只剩聚合写这条要过用户态的路径了。句柄与映射同生命周期，close() 一起释放
        mappedFile.m_base           = mappedBase;
        mappedFile.m_length         = mappedLength;
        mappedFile.m_fileDescriptor = fileDescriptor;
        mappedFile.m_isValid        = true;
#endif

        return mappedFile;
    }

    MemoryMappedFile::~MemoryMappedFile()
    {
        close();
    }

    MemoryMappedFile::MemoryMappedFile(MemoryMappedFile &&other) noexcept :
        m_base(std::exchange(other.m_base, nullptr)), m_length(std::exchange(other.m_length, 0)), m_lastError(other.m_lastError), m_isValid(std::exchange(other.m_isValid, false))
#if ASYN_PLATFORM_WIN32
        ,
        m_mappingHandle(std::exchange(other.m_mappingHandle, nullptr)), m_fileHandle(std::exchange(other.m_fileHandle, nullptr))
#else
        ,
        m_fileDescriptor(std::exchange(other.m_fileDescriptor, -1))
#endif
    {
    }

    MemoryMappedFile &MemoryMappedFile::operator=(MemoryMappedFile &&other) noexcept
    {
        if (this != &other)
        {
            close();
            m_base      = std::exchange(other.m_base, nullptr);
            m_length    = std::exchange(other.m_length, 0);
            m_lastError = other.m_lastError;
            m_isValid   = std::exchange(other.m_isValid, false);
#if ASYN_PLATFORM_WIN32
            m_mappingHandle = std::exchange(other.m_mappingHandle, nullptr);
            m_fileHandle    = std::exchange(other.m_fileHandle, nullptr);
#else
            m_fileDescriptor = std::exchange(other.m_fileDescriptor, -1);
#endif
        }
        return *this;
    }

    bool MemoryMappedFile::isValid() const noexcept
    {
        // 用显式状态位，而不是从 m_base/m_length/m_lastError 的组合推断：空文件在两个平台都是
        // 「保留了句柄但没有基址、长度为 0」，与「默认构造 / 关闭后 / 被移动走」在那些字段上
        // 完全一致，推断写法会把后三者误判成「有效但为空」，上层（如 HttpResponse::bodyView）
        // 据此对堆正文误走映射分支、发出空正文
        return m_isValid;
    }

    std::span<const std::byte> MemoryMappedFile::bytes() const noexcept
    {
        if (m_base == nullptr || m_length == 0)
        {
            return {};
        }
        return {static_cast<const std::byte *>(m_base), m_length};
    }

    std::error_code MemoryMappedFile::lastError() const noexcept
    {
        return m_lastError;
    }

#if !ASYN_PLATFORM_WIN32
    int MemoryMappedFile::nativeFileDescriptor() const noexcept
    {
        return m_fileDescriptor;
    }
#endif

    void MemoryMappedFile::close() noexcept
    {
#if ASYN_PLATFORM_WIN32
        if (m_base != nullptr)
        {
            ::UnmapViewOfFile(m_base);
            m_base = nullptr;
        }
        if (m_mappingHandle != nullptr)
        {
            ::CloseHandle(static_cast<HANDLE>(m_mappingHandle));
            m_mappingHandle = nullptr;
        }
        if (m_fileHandle != nullptr)
        {
            ::CloseHandle(static_cast<HANDLE>(m_fileHandle));
            m_fileHandle = nullptr;
        }
#else
        if (m_base != nullptr)
        {
            ::munmap(m_base, m_length);
            m_base = nullptr;
        }
        if (m_fileDescriptor >= 0)
        {
            ::close(m_fileDescriptor);
            m_fileDescriptor = -1;
        }
#endif
        m_length  = 0;
        m_isValid = false;
    }

    std::optional<FileBasicInfo> MemoryMappedFile::openedFileInfo() const noexcept
    {
        // 已关闭或被移走的对象连句柄都没有，无从问起；调用方拿到空值当「说不清」处理
        if (!m_isValid)
        {
            return std::nullopt;
        }
#if ASYN_PLATFORM_WIN32
        return queryOpenedFileBasicInfo(m_fileHandle);
#else
        return queryOpenedFileBasicInfo(m_fileDescriptor);
#endif
    }
} // namespace AsynGyanis::Platform

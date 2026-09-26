#include "Platform/System/ProcessInfo.h"
#include "Platform/Platform.h"

#include <cstdlib>
#include <vector>

// 非 Windows 的平台都从 unistd.h 取 getpid/readlink（不止 Linux 用得到这两个）
#if !ASYN_PLATFORM_WIN32
#include <unistd.h>
#endif

namespace AsynGyanis::Platform
{
    std::filesystem::path ProcessInfo::applicationDirectory()
    {
#if ASYN_PLATFORM_WIN32
        std::wstring executablePath(32768, L'\0');
        const DWORD  charactersWritten = ::GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
        if (charactersWritten == 0 || charactersWritten >= executablePath.size())
        {
            return {};
        }
        executablePath.resize(charactersWritten);
        return std::filesystem::path(executablePath).parent_path();
#else
        // readlink 不写零终止，且「路径正好这么长」与「被截断」给出的返回值完全一样：交出一份切掉的
        // 可执行文件路径比报失败更糟——parent_path 会指向一个并不存在的目录，之后按它拼出来的资源
        // 路径全都悄悄落空。因此把整份容量交给 readlink，装满即判为截断并返回空路径（与 Windows 分支
        // 「charactersWritten >= 容量」同一条判据）。内核对这个链接的限制就是 PATH_MAX 那一档，
        // 超过它的路径本来就取不到，不必假装能取到
        std::vector<char> buffer(4096, '\0');
        const ssize_t     length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length <= 0 || static_cast<std::size_t>(length) == buffer.size())
        {
            return {};
        }
        return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length))).parent_path();
#endif
    }

    std::optional<std::string> ProcessInfo::environmentVariable(const std::string &variableName)
    {
#if ASYN_PLATFORM_WIN32
        char  *rawValue    = nullptr;
        size_t valueLength = 0;
        if (::_dupenv_s(&rawValue, &valueLength, variableName.c_str()) != 0 || rawValue == nullptr)
        {
            return std::nullopt;
        }
        std::string value(rawValue);
        ::free(rawValue);
        return value;
#else
        const char *rawValue = std::getenv(variableName.c_str());
        if (rawValue == nullptr)
        {
            return std::nullopt;
        }
        return std::string(rawValue);
#endif
    }

    long ProcessInfo::currentProcessId() noexcept
    {
#if ASYN_PLATFORM_WIN32
        // GetCurrentProcessId 不失败，直接返回
        return static_cast<long>(::GetCurrentProcessId());
#else
        // getpid 在 POSIX 上不失败（永远返回有效进程号）
        return static_cast<long>(::getpid());
#endif
    }
} // namespace AsynGyanis::Platform

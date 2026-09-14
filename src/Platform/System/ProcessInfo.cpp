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
        std::vector<char> buffer(4096, '\0');
        const ssize_t     length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
        if (length <= 0)
        {
            return {};
        }
        return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length))).parent_path();
#endif
    }

    std::optional<std::string> ProcessInfo::environmentVariable(const std::string &variableName)
    {
#if ASYN_PLATFORM_WIN32
        char * rawValue    = nullptr;
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

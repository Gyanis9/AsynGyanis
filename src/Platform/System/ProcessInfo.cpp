#include "Platform/System/ProcessInfo.h"
#include "Platform/IO/FileContents.h"
#include "Platform/Platform.h"
#include "Platform/System/TextEncoding.h"

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <string_view>
#include <vector>

// 非 Windows 的平台都从 unistd.h 取 getpid/readlink（不止 Linux 用得到这两个）
#if ASYN_PLATFORM_WIN32
// PROCESS_MEMORY_COUNTERS 与 K32GetProcessMemoryInfo 的声明都在这里（库不用另链：K32* 在 kernel32）
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
// environ 在 glibc 与 BSD 上都由 unistd.h 给出声明，这里再声明一次是为了不依赖各家的特性宏组合：
// 少了它，整表枚举会在没开 _GNU_SOURCE 的构建里编不过
extern "C" char **environ;
#endif

namespace AsynGyanis::Platform
{
#if ASYN_PLATFORM_WIN32
    namespace
    {
        /**
         * @brief FILETIME 的高低 32 位拼成 64 位计数
         * @param value Windows 文件时间
         * @return std::uint64_t 自 1601-01-01 起的 100 纳秒数
         */
        std::uint64_t fileTimeUnits(const FILETIME &value) noexcept
        {
            return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) | static_cast<std::uint64_t>(value.dwLowDateTime);
        }
    } // namespace
#endif
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

    std::vector<std::pair<std::string, std::string>> ProcessInfo::environmentVariablesWithPrefix(const std::string &prefix)
    {
        std::vector<std::pair<std::string, std::string>> matches;
#if ASYN_PLATFORM_WIN32
        // 整块的宽字符环境变量串以两个连续 NUL 收尾，逐条切；取不到块就交出一份空清单，
        // 调用方按「没有一条覆盖」处理，不该把「拿不到环境」伪装成「环境里没有」。
        // 指针不带 const：释放它的那条 API 要的是 LPWCH（同一块内存进出，本层不改它）
        wchar_t *block = ::GetEnvironmentStringsW();
        if (block == nullptr)
        {
            return matches;
        }
        std::size_t offset = 0;
        while (true)
        {
            const std::wstring_view entry(block + offset);
            if (entry.empty())
            {
                break;
            }
            offset += entry.size() + 1;

            // 名字与值只在第一个 '=' 处切：值里再出现的 '=' 属于值本身。首字符就是 '=' 的那些
            // 是「某驱动器当前目录」这类内部条目，没有可匹配的名字，跳过
            const std::size_t separator = entry.find(L'=');
            if (separator == std::wstring_view::npos || separator == 0)
            {
                continue;
            }
            std::string name = TextEncoding::toUtf8String(std::wstring(entry.substr(0, separator)));
            if (!prefix.empty() && !name.starts_with(prefix))
            {
                continue;
            }
            matches.emplace_back(std::move(name), TextEncoding::toUtf8String(std::wstring(entry.substr(separator + 1))));
        }
        static_cast<void>(::FreeEnvironmentStringsW(block));
#else
        for (char **entry = ::environ; entry != nullptr && *entry != nullptr; ++entry)
        {
            const std::string_view text(*entry);
            const std::size_t      separator = text.find('=');
            if (separator == std::string_view::npos || separator == 0)
            {
                continue;
            }
            const std::string_view name = text.substr(0, separator);
            if (!prefix.empty() && !name.starts_with(prefix))
            {
                continue;
            }
            matches.emplace_back(std::string(name), std::string(text.substr(separator + 1)));
        }
#endif
        return matches;
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

    std::uint64_t ProcessInfo::residentMemoryBytes() noexcept
    {
#if ASYN_PLATFORM_WIN32
        // K32GetProcessMemoryInfo 自 Vista 起就由 kernel32 导出，因此不必再链 psapi.lib：多认领一个
        // 系统库，包消费方（Conan 与 vcpkg 两条路线）都要跟着补一份依赖声明
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = static_cast<DWORD>(sizeof(counters));
        if (::K32GetProcessMemoryInfo(::GetCurrentProcess(), &counters, counters.cb) == 0)
        {
            return 0;
        }
        return static_cast<std::uint64_t>(counters.WorkingSetSize);
#else
        // /proc/self/statm 的一行是七个页计数（大小 常驻 共享 文本 库 数据 脏页），第二列才是要的那一列。
        // 单位是页，所以还要乘页尺寸；procfs 上这类文件报大小为 0，故按固定上界读一段而不是按大小读
        constexpr std::size_t                             kStatmProbeLength = 128;
        const std::expected<std::string, std::error_code> contents          = readFileContents("/proc/self/statm", 0, kStatmProbeLength);
        if (!contents.has_value())
        {
            return 0;
        }

        const std::string_view line(*contents);
        std::size_t            cursor        = 0;
        std::uint64_t          virtualPages  = 0;
        std::uint64_t          residentPages = 0;
        for (std::uint64_t *const column: {&virtualPages, &residentPages})
        {
            // from_chars 不跳前导空白，因此每列取完都要自己把分隔空格越过去；
            // 前两列任何一处解析不动，这份读数就不可信，宁可按「不知道」交回 0
            const std::from_chars_result parsed = std::from_chars(line.begin() + cursor, line.end(), *column);
            if (parsed.ec != std::errc{})
            {
                return 0;
            }
            cursor = static_cast<std::size_t>(parsed.ptr - line.begin());
            while (cursor < line.size() && line[cursor] == ' ')
            {
                ++cursor;
            }
        }

        const long pageSize = ::sysconf(_SC_PAGE_SIZE);
        if (pageSize <= 0)
        {
            return 0;
        }
        return residentPages * static_cast<std::uint64_t>(pageSize);
#endif
    }

    std::uint64_t ProcessInfo::cpuSeconds() noexcept
    {
#if ASYN_PLATFORM_WIN32
        FILETIME creationTime{};
        FILETIME exitTime{};
        FILETIME kernelTime{};
        FILETIME userTime{};
        if (::GetProcessTimes(::GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime) == 0)
        {
            return 0;
        }
        // 两枚计数各自是 100 纳秒，和之后再折成整秒；nanosecond 级的小数在计数器口径里没有意义
        constexpr std::uint64_t kUnitsPerSecond = 10000000ULL;
        return (fileTimeUnits(userTime) + fileTimeUnits(kernelTime)) / kUnitsPerSecond;
#else
        rusage usage{};
        if (::getrusage(RUSAGE_SELF, &usage) != 0)
        {
            return 0;
        }
        const std::uint64_t wholeSeconds = static_cast<std::uint64_t>(usage.ru_utime.tv_sec) + static_cast<std::uint64_t>(usage.ru_stime.tv_sec);
        const std::uint64_t microseconds = static_cast<std::uint64_t>(usage.ru_utime.tv_usec) + static_cast<std::uint64_t>(usage.ru_stime.tv_usec);
        return wholeSeconds + microseconds / 1000000ULL;
#endif
    }

    std::uint64_t ProcessInfo::openHandleCount() noexcept
    {
#if ASYN_PLATFORM_WIN32
        DWORD handleCount = 0;
        // 伪句柄自带全部访问权，不必先 OpenProcess；查询失败按「不知道」交回 0
        if (::GetProcessHandleCount(::GetCurrentProcess(), &handleCount) == 0)
        {
            return 0;
        }
        return static_cast<std::uint64_t>(handleCount);
#else
        // /proc/self/fd 每个目录项就是一枚打开的描述符；遍历失败（含中途出错）按「不知道」交回 0。
        // 走 error_code 重载而不是抛出：本函数声明 noexcept，且 procfs 的目录语义本就有失败面
        std::error_code                     enumerationError;
        std::filesystem::directory_iterator iterator("/proc/self/fd", enumerationError);
        if (enumerationError)
        {
            return 0;
        }
        std::uint64_t                             handleCount = 0;
        const std::filesystem::directory_iterator end;
        for (; iterator != end; iterator.increment(enumerationError))
        {
            ++handleCount;
            if (enumerationError)
            {
                break;
            }
        }
        return handleCount;
#endif
    }

    std::uint64_t ProcessInfo::startTimeSeconds() noexcept
    {
#if ASYN_PLATFORM_WIN32
        FILETIME creationTime{};
        FILETIME exitTime{};
        FILETIME kernelTime{};
        FILETIME userTime{};
        if (::GetProcessTimes(::GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime) == 0)
        {
            return 0;
        }
        // FILETIME 是 1601-01-01 起的 100 纳秒；转到 Unix epoch 要扣掉 11644473600 秒的差值
        constexpr std::uint64_t kUnitsPerSecond         = 10000000ULL;
        constexpr std::uint64_t kUnixEpochFileTimeUnits = 11644473600ULL * kUnitsPerSecond;
        const std::uint64_t     creationUnits           = fileTimeUnits(creationTime);
        if (creationUnits < kUnixEpochFileTimeUnits)
        {
            return 0;
        }
        return creationUnits / kUnitsPerSecond - kUnixEpochFileTimeUnits / kUnitsPerSecond;
#else
        // /proc/self/stat 的第 22 列 starttime 是「自系统启动起的时钟节拍数」，配上 /proc/stat 的
        // btime（启动时刻，epoch 秒）才是绝对时刻。comm（第 2 列）可能含空格与括号，先跨过最后一个 ')'
        constexpr std::size_t                             kProcProbeLength = 4096;
        const std::expected<std::string, std::error_code> statContents     = readFileContents("/proc/self/stat", 0, kProcProbeLength);
        if (!statContents.has_value())
        {
            return 0;
        }
        const std::string_view statLine(*statContents);
        const std::size_t      closeParenthesis = statLine.rfind(')');
        if (closeParenthesis == std::string_view::npos || closeParenthesis + 2 >= statLine.size())
        {
            return 0;
        }
        // 跳过 ") "：comm 之后第一列是状态（第 3 列）——它是 R/S/D 这类单字符，不是数字；
        // starttime 是第 22 列，即余下部分的第 19 个下标。状态列必须先按「非空白段」跨过：
        // 直接对它跑 from_chars 会当场报无效字符，整条读数恒回 0
        const std::string_view afterComm  = statLine.substr(closeParenthesis + 2);
        std::int64_t           tokenValue = 0;
        std::size_t            cursor     = 0;
        while (cursor < afterComm.size() && afterComm[cursor] != ' ')
        {
            ++cursor;
        }
        for (std::size_t tokenIndex = 1; tokenIndex <= 19; ++tokenIndex)
        {
            while (cursor < afterComm.size() && afterComm[cursor] == ' ')
            {
                ++cursor;
            }
            // 逐列按有符号解析：这份表里的 tpgid（无控制终端时是 -1）与 nice（可为负）
            // 都是合法负数，无符号目标会让 from_chars 在它们身上当场报无效字符
            const std::from_chars_result parsed = std::from_chars(afterComm.begin() + static_cast<std::ptrdiff_t>(cursor), afterComm.end(), tokenValue);
            if (parsed.ec != std::errc{})
            {
                return 0;
            }
            cursor = static_cast<std::size_t>(parsed.ptr - afterComm.begin());
        }
        if (tokenValue < 0)
        {
            return 0;
        }
        const std::uint64_t startTicks = static_cast<std::uint64_t>(tokenValue);

        const std::expected<std::string, std::error_code> bootContents = readFileContents("/proc/stat", 0, kProcProbeLength);
        if (!bootContents.has_value())
        {
            return 0;
        }
        const std::size_t btimeOffset = bootContents->find("btime ");
        if (btimeOffset == std::string::npos)
        {
            return 0;
        }
        std::uint64_t bootTimeSeconds = 0;
        const auto bootParsed = std::from_chars(bootContents->data() + static_cast<std::ptrdiff_t>(btimeOffset) + 6, bootContents->data() + bootContents->size(), bootTimeSeconds);
        if (bootParsed.ec != std::errc{})
        {
            return 0;
        }

        // 节拍按 HZ 折算成秒：USER_HZ 在 Linux 上恒为 100，sysconf 读不到时按它兜底而不是放弃读数
        long ticksPerSecond = ::sysconf(_SC_CLK_TCK);
        if (ticksPerSecond <= 0)
        {
            ticksPerSecond = 100;
        }
        return bootTimeSeconds + startTicks / static_cast<std::uint64_t>(ticksPerSecond);
#endif
    }
} // namespace AsynGyanis::Platform

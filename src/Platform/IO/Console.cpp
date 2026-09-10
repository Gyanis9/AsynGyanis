#include "Platform/IO/Console.h"

#include "Platform/System/ProcessInfo.h"

#include <string>

#if ASYN_PLATFORM_LINUX
#include <unistd.h>
#endif

namespace AsynGyanis::Platform
{
    void Console::ensureUtf8Output() noexcept
    {
#if ASYN_PLATFORM_WIN32
        // 代码页设置是进程级状态，重复设置无副作用但只需做一次
        static const bool kconfigured = [] { return ::SetConsoleOutputCP(CP_UTF8) != 0; }();
        (void) kconfigured;
#else
        // POSIX 终端的编码由外层环境决定，标准输出直接写 UTF-8 字节即可
#endif
    }

    bool Console::supportsAnsiEscapeCodes()
    {
#if ASYN_PLATFORM_WIN32
        const HANDLE standardOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (standardOutput == nullptr || standardOutput == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        DWORD consoleMode = 0;
        if (::GetConsoleMode(standardOutput, &consoleMode) == 0)
        {
            // 标准输出被重定向到文件或管道时不是控制台句柄
            return false;
        }

        return ::SetConsoleMode(standardOutput, consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
        if (::isatty(STDOUT_FILENO) == 0)
        {
            return false;
        }

        const std::optional<std::string> terminalName = ProcessInfo::environmentVariable("TERM");
        if (!terminalName.has_value())
        {
            return false;
        }
        return *terminalName != "dumb";
#endif
    }
} // namespace AsynGyanis::Platform

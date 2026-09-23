#include "Platform/IO/Console.h"
#include "Platform/System/ProcessInfo.h"
#include "Platform/Platform.h"

#include <atomic>
#include <string>

#if ASYN_PLATFORM_LINUX
#include <unistd.h>
#endif

namespace AsynGyanis::Platform
{
    void Console::ensureUtf8Output() noexcept
    {
#if ASYN_PLATFORM_WIN32
        // 只在成功之后停止重试：函数里的 static 一旦初始化就定型，若把「首调的结果」也钉进去，
        // 进程以无控制台方式被拉起时那一次 SetConsoleOutputCP 会当场失败（句柄无效），此后
        // 再接上真控制台也永远不会改代码页——中文输出整段生命周期都是乱码。失败不记账
        static std::atomic<bool> isConfigured{false};
        if (isConfigured.load(std::memory_order_relaxed))
        {
            return;
        }
        // 并发首调可能各设一次代码页：这条 API 幂等，宁可重复设置也不为它加锁
        if (::SetConsoleOutputCP(CP_UTF8) != 0)
        {
            isConfigured.store(true, std::memory_order_relaxed);
        }
#else
        // POSIX 终端的编码由外层环境决定，标准输出直接写 UTF-8 字节即可
#endif
    }

    bool Console::supportsAnsiEscapeCodes()
    {
#if ASYN_PLATFORM_WIN32
        HANDLE standardOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
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

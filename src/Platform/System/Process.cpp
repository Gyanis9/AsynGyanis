#include "Platform/System/Process.h"

#include "Platform/System/PlatformError.h"
#include "Platform/System/TextEncoding.h"

#include <utility>

#if !ASYN_PLATFORM_WIN32
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace AsynGyanis::Platform
{
    namespace
    {
#if ASYN_PLATFORM_WIN32
        /**
         * @brief 按 MSVCRT 的解析规则给一个参数加引号
         * @details 只给**需要**引号的参数加：含空白、制表符或引号的。无差别地全都套上引号会改变某些
         *          程序的解析——`cmd` 的 `/c` 就是典型，它一旦被引号包住，后面的命令串会被当成另一段
         *          （实测 `cmd.exe "/c" "exit 7"` 的退出码是 1，而 `cmd.exe /c "exit 7"` 是 7）。
         *          需要引号时按 MSVCRT 规则转义：整体加双引号，参数内的双引号用「反斜杠加倍 + 一个
         *          反斜杠」转义，紧邻引号与末尾的反斜杠必须成对加倍（否则它会把引号吃掉）——少做
         *          一步就会让带空格或引号的参数被拆错，表现为子进程里参数缺字，极难回溯。
         * @param argument 原始参数
         * @return std::string 可直接拼进命令行的形式
         */
        std::string quoteArgument(const std::string &argument)
        {
            // 空参数必须给引号（否则会被当成「没有这个参数」），其余不需要引号的原样输出
            if (!argument.empty() && argument.find_first_of(" \t\"") == std::string::npos)
            {
                return argument;
            }

            std::string quoted = "\"";
            std::size_t backslashCount = 0;
            for (const char character: argument)
            {
                if (character == '\\')
                {
                    ++backslashCount;
                    continue;
                }
                if (character == '"')
                {
                    quoted.append(backslashCount * 2 + 1, '\\');
                    quoted.push_back('"');
                    backslashCount = 0;
                    continue;
                }
                quoted.append(backslashCount, '\\');
                backslashCount = 0;
                quoted.push_back(character);
            }
            // 收尾的反斜杠会与收尾引号组成 \"，必须加倍才不会被当成转义
            quoted.append(backslashCount * 2, '\\');
            quoted.push_back('"');
            return quoted;
        }
#endif
    } // namespace

#if ASYN_PLATFORM_WIN32
    Process::Handle::Handle(void *const processHandle, const unsigned long processId) noexcept :
        m_processHandle(processHandle), m_processId(processId)
    {
    }
#else
    Process::Handle::Handle(const int processId) noexcept :
        m_processId(processId)
    {
    }
#endif

    Process::Handle::~Handle()
    {
        close();
    }

    Process::Handle::Handle(Handle &&other) noexcept
#if ASYN_PLATFORM_WIN32
        : m_processHandle(std::exchange(other.m_processHandle, nullptr)), m_processId(std::exchange(other.m_processId, 0)),
          m_exitCode(other.m_exitCode)
#else
        : m_processId(std::exchange(other.m_processId, -1)), m_exitCode(other.m_exitCode)
#endif
    {
    }

    Process::Handle &Process::Handle::operator=(Handle &&other) noexcept
    {
        if (this != &other)
        {
            close();
#if ASYN_PLATFORM_WIN32
            m_processHandle = std::exchange(other.m_processHandle, nullptr);
            m_processId     = std::exchange(other.m_processId, 0);
#else
            m_processId = std::exchange(other.m_processId, -1);
#endif
            m_exitCode = other.m_exitCode;
        }
        return *this;
    }

    bool Process::Handle::isValid() const noexcept
    {
#if ASYN_PLATFORM_WIN32
        return m_processHandle != nullptr;
#else
        return m_processId > 0;
#endif
    }

    long Process::Handle::processId() const noexcept
    {
#if ASYN_PLATFORM_WIN32
        return static_cast<long>(m_processId);
#else
        return m_processId > 0 ? static_cast<long>(m_processId) : 0L;
#endif
    }

    void Process::Handle::close() noexcept
    {
        if (!isValid())
        {
            return;
        }

        // 已退出但还没被取退出码的子进程在这里回收：POSIX 上不回收就留一个僵尸，
        // Windows 上句柄不关就一直占着内核对象
        if (!m_exitCode.has_value())
        {
            static_cast<void>(Process::pollExitCode(*this));
        }
#if ASYN_PLATFORM_WIN32
        if (m_processHandle != nullptr)
        {
            ::CloseHandle(static_cast<HANDLE>(m_processHandle));
            m_processHandle = nullptr;
        }
        m_processId = 0;
#else
        m_processId = -1;
#endif
    }

    Process::Handle Process::spawn(const LaunchOptions &options) noexcept
    {
        if (options.executablePath.empty())
        {
            // 空路径是调用方的用法错误，当场置错码返回无效句柄，不猜也不尝试
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return Handle{};
        }

#if ASYN_PLATFORM_WIN32
        // Windows 没有 exec：命令行要么交给 CreateProcessW 的 lpCommandLine，要么自己拼。
        // 拼的时候 argv[0] 用可执行文件路径（子进程的 argv[0] 惯例），其余参数逐个加引号
        std::string commandLine;
        for (std::size_t argumentIndex = 0; argumentIndex <= options.arguments.size(); ++argumentIndex)
        {
            const std::string &argument = argumentIndex == 0 ? options.executablePath : options.arguments[argumentIndex - 1];
            if (!commandLine.empty())
            {
                commandLine.push_back(' ');
            }
            commandLine += quoteArgument(argument);
        }

        STARTUPINFOW        startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInformation{};
        std::wstring        wideCommandLine = TextEncoding::toWideString(commandLine);

        // 可执行文件只走命令行那一路，lpApplicationName 传空：CreateProcessW 对 lpApplicationName
        // **不搜 PATH**（只按当前目录/系统目录补全），只给个「cmd.exe」这种名字会直接失败；
        // 而命令行首 token 是会按搜索路径解析的。argv[0] 已按 MSVCRT 规则加好引号，
        // 含空格的完整路径因此不会被拆成两段。
        // 只传命令行串（不带独占的所有权保证）不影响子进程按路径映射映像；lpCommandLine 需要可写缓冲，
        // 这里给一份自己的副本
        if (::CreateProcessW(nullptr, wideCommandLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startupInfo,
                             &processInformation) == 0)
        {
            PlatformError::setLastErrorCode(static_cast<int>(::GetLastError()));
            return Handle{};
        }

        ::CloseHandle(processInformation.hThread);
        return Handle(processInformation.hProcess, processInformation.dwProcessId);
#else
        // 子进程参数表按 POSIX 惯例：argv[0] 是可执行文件路径，末尾必须是空指针
        std::vector<std::string> argumentStrings;
        argumentStrings.reserve(options.arguments.size() + 1);
        argumentStrings.push_back(options.executablePath);
        argumentStrings.insert(argumentStrings.end(), options.arguments.begin(), options.arguments.end());

        std::vector<char *> argumentPointers;
        argumentPointers.reserve(argumentStrings.size() + 1);
        for (std::string &argumentString: argumentStrings)
        {
            argumentPointers.push_back(argumentString.data());
        }
        argumentPointers.push_back(nullptr);

        const pid_t childProcessId = ::fork();
        if (childProcessId < 0)
        {
            PlatformError::setLastErrorCode(errno);
            return Handle{};
        }
        if (childProcessId == 0)
        {
            // 子进程分支：exec 之后旧映像就没了，之前的失败在这里只能自己收场。
            // 用 _exit 而不是 exit：不能跑父进程继承来的 atexit 与静态析构（多线程下 fork 出来的
            // 子进程只带调用线程，那些清理路径可能撞上父进程留下的锁状态）
            ::execvp(options.executablePath.c_str(), argumentPointers.data());
            ::_exit(127);
        }
        return Handle(childProcessId);
#endif
    }

    bool Process::isRunning(const Handle &handle) noexcept
    {
        return !pollExitCode(handle).has_value() && handle.isValid();
    }

    std::optional<int> Process::pollExitCode(const Handle &handle) noexcept
    {
        if (!handle.isValid())
        {
            return std::nullopt;
        }
        // 回收过一次就直接给记住的值：内核那边已经查不到这个子进程了
        if (handle.m_exitCode.has_value())
        {
            return handle.m_exitCode;
        }

#if ASYN_PLATFORM_WIN32
        const DWORD waitResult = ::WaitForSingleObject(static_cast<HANDLE>(handle.m_processHandle), 0);
        if (waitResult == WAIT_TIMEOUT)
        {
            return std::nullopt;
        }
        if (waitResult != WAIT_OBJECT_0)
        {
            PlatformError::setLastErrorCode(static_cast<int>(::GetLastError()));
            return std::nullopt;
        }

        DWORD exitCode = 0;
        if (::GetExitCodeProcess(static_cast<HANDLE>(handle.m_processHandle), &exitCode) == 0)
        {
            PlatformError::setLastErrorCode(static_cast<int>(::GetLastError()));
            return std::nullopt;
        }
        handle.m_exitCode = static_cast<int>(exitCode);
        return handle.m_exitCode;
#else
        int  waitStatus  = 0;
        const pid_t reaped = ::waitpid(handle.m_processId, &waitStatus, WNOHANG);
        if (reaped == 0)
        {
            return std::nullopt;
        }
        if (reaped < 0)
        {
            // ECHILD：这个子进程已经被别处回收过（本类不制造这种情形，但 SIGCHLD 处理函数会）。
            // 记不成退出码，只能如实说「取不到」，同时把句柄标成无效，避免反复问同一件查不到的事
            PlatformError::setLastErrorCode(errno);
            if (errno == ECHILD)
            {
                handle.m_exitCode = -1;
            }
            return std::nullopt;
        }

        // 正常退出取退出码；被信号终止时 POSIX 没有退出码，按 128 + 信号号折算（shell 惯例），
        // 好过折叠成同一个值——调用方至少能区分「自己退的」与「被杀掉的」
        handle.m_exitCode = WIFEXITED(waitStatus) ? WEXITSTATUS(waitStatus) : 128 + WTERMSIG(waitStatus);
        return handle.m_exitCode;
#endif
    }

    bool Process::requestTermination(const Handle &handle) noexcept
    {
#if ASYN_PLATFORM_WIN32
        // Windows 没有信号：要么自己有退出机制（控制台事件、管道、事件对象），要么强杀。
        // 返回 false 而不是强杀：静默升级成强杀会让调用方的「优雅」意图落空且无从察觉
        static_cast<void>(handle);
        PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
        return false;
#else
        if (!handle.isValid())
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return false;
        }
        if (::kill(handle.m_processId, SIGTERM) != 0)
        {
            PlatformError::setLastErrorCode(errno);
            return false;
        }
        return true;
#endif
    }

    bool Process::forceTermination(const Handle &handle) noexcept
    {
        if (!handle.isValid())
        {
            PlatformError::setLastErrorCode(PlatformError::kInvalidArgument);
            return false;
        }

#if ASYN_PLATFORM_WIN32
        if (::TerminateProcess(static_cast<HANDLE>(handle.m_processHandle), 1) == 0)
        {
            PlatformError::setLastErrorCode(static_cast<int>(::GetLastError()));
            return false;
        }
        return true;
#else
        if (::kill(handle.m_processId, SIGKILL) != 0)
        {
            PlatformError::setLastErrorCode(errno);
            return false;
        }
        return true;
#endif
    }
} // namespace AsynGyanis::Platform

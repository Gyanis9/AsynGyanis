#include "Platform/System/Process.h"

#include "Platform/System/PlatformError.h"
#include "Platform/System/TextEncoding.h"

#include <utility>

#if !ASYN_PLATFORM_WIN32
#include <cerrno>
#include <csignal>
#if ASYN_PLATFORM_LINUX
#include <sys/prctl.h>
#endif
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
         * @details 只给**需要**引号的参数加（含空白、制表符或引号）：无差别地全加会改变某些程序的解析
         *          ——`cmd` 的 `/c` 一旦被引号包住，后面的命令串会被当成另一段。需要引号时按 MSVCRT
         *          规则转义：整体加双引号，参数内的双引号用「反斜杠加倍 + 一个反斜杠」，紧邻引号与
         *          末尾的反斜杠必须成对加倍——少做一步就会让参数被拆错，子进程里缺字且极难回溯。
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

        // 句柄继承收窄到「只带标准输入/输出/错误」：bInheritHandles=TRUE 会把父进程所有可继承句柄
        // 复制进子进程（Winsock 套接字默认就是可继承的），监听/连接套接字因此会被子进程
        // 一直持有，父进程退出后端口也不释放。STARTUPINFOEX 的句柄清单是唯一能限定继承集合的机制。
        // 清单里不许出现空句柄：没有控制台的宿主（服务、GUI 子系统、被 DETACHED_PROCESS 派出来的
        // 进程）里 GetStdHandle 给的是 NULL，整份交上去只换来 ERROR_INVALID_PARAMETER(87)，
        // 「派生 worker」这条路径在那类宿主上等于永远起不来——所以先逐个筛过再决定建不建清单
        std::vector<HANDLE> inheritableStandardHandles;
        for (const DWORD standardSlot: {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE})
        {
            const HANDLE standardHandle = ::GetStdHandle(standardSlot);
            if (standardHandle != nullptr && standardHandle != INVALID_HANDLE_VALUE)
            {
                inheritableStandardHandles.push_back(standardHandle);
            }
        }

        STARTUPINFOEXW               startupInfo{};
        std::vector<std::uint64_t>   attributeListStorage;
        LPPROC_THREAD_ATTRIBUTE_LIST attributeList = nullptr;
        BOOL                         inheritHandles = FALSE;
        DWORD                        creationFlags = 0;

        if (!inheritableStandardHandles.empty())
        {
            // 属性清单要先问出大小（首次调用必以 ERROR_INSUFFICIENT_BUFFER 失败），再按该大小分配。
            // 缓冲用 uint64_t 数组而非字节数组：清单要求按指针宽度对齐
            SIZE_T attributeListSize = 0;
            static_cast<void>(::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeListSize));
            attributeListStorage.assign((attributeListSize + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t), 0);
            attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeListStorage.data());
            if (attributeListSize == 0 || ::InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeListSize) == 0)
            {
                attributeList = nullptr;
                PlatformError::setLastErrorCode(static_cast<int>(::GetLastError()));
                return Handle{};
            }

            if (::UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritableStandardHandles.data(),
                                            inheritableStandardHandles.size() * sizeof(HANDLE), nullptr, nullptr) == 0)
            {
                const int failureCode = static_cast<int>(::GetLastError());
                ::DeleteProcThreadAttributeList(attributeList);
                attributeList = nullptr;
                PlatformError::setLastErrorCode(failureCode);
                return Handle{};
            }

            startupInfo.StartupInfo.cb  = sizeof(startupInfo);
            startupInfo.lpAttributeList = attributeList;
            creationFlags               = EXTENDED_STARTUPINFO_PRESENT;
            inheritHandles              = TRUE;
        }
        else
        {
            // 一个标准句柄都没有（无控制台的宿主）：不建清单，也干脆不开继承——此时没有值得传下去的
            // 句柄，而「开继承却不带清单」会把父进程全部可继承句柄整个交出去，那正是清单要防的事
            startupInfo.StartupInfo.cb = sizeof(STARTUPINFOW);
        }

        PROCESS_INFORMATION processInformation{};
        std::wstring        wideCommandLine = TextEncoding::toWideString(commandLine);

        // 可执行文件只走命令行那一路，lpApplicationName 传空：CreateProcessW 对 lpApplicationName
        // **不搜 PATH**（只按当前目录/系统目录补全），只给个「cmd.exe」这种名字会直接失败；
        // 而命令行首 token 是会按搜索路径解析的。argv[0] 已按 MSVCRT 规则加好引号，
        // 含空格的完整路径因此不会被拆成两段。
        // 只传命令行串（不带独占的所有权保证）不影响子进程按路径映射映像；lpCommandLine 需要可写缓冲，
        // 这里给一份自己的副本
        const BOOL isCreated = ::CreateProcessW(nullptr, wideCommandLine.data(), nullptr, nullptr, inheritHandles, creationFlags,
                                                nullptr, nullptr, &startupInfo.StartupInfo, &processInformation);
        const int  creationErrorCode = isCreated != 0 ? 0 : static_cast<int>(::GetLastError());
        // 属性清单只在 CreateProcessW 调用期间被读取，调用返回即可释放；没建清单时不释放空指针
        if (attributeList != nullptr)
        {
            ::DeleteProcThreadAttributeList(attributeList);
        }
        if (isCreated == 0)
        {
            PlatformError::setLastErrorCode(creationErrorCode);
            return Handle{};
        }

        ::CloseHandle(processInformation.hThread);
        return Handle(processInformation.hProcess, processInformation.dwProcessId);
#else
        // fork 之前先记下本进程号：子进程要靠它判断「生我的那个进程是否还在」（见下面的自查）
        const pid_t parentProcessId = ::getpid();

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
#if ASYN_PLATFORM_LINUX
            // 生我的进程一没就收 SIGTERM：worker 的生存期应当跟着编排者，否则 master 被强杀之后
            // worker 成了孤儿继续占着端口，谁都不再管它（这个标记在非 setuid 的 exec 之后仍然有效，
            // 正好覆盖「fork 出子进程再 exec 成服务程序」这条路径）。信号选 SIGTERM 而不是 SIGKILL，
            // 让子进程还有机会按自己的收尾路径把在途请求做完
            static_cast<void>(::prctl(PR_SET_PDEATHSIG, SIGTERM));

            // 从 fork 到上面这句生效之间 master 就可能已经退出，那种情况下信号永远不会来：
            // 自己认一下父号还是不是生我的那个进程，不是就直接收工
            if (::getppid() != parentProcessId)
            {
                ::_exit(0);
            }
#endif

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
                // 句柄一并作废：这个 pid 已经交还系统、可能已被别的进程复用。只记退出码的话
                // isRunning() 仍会说「在运行」，终止类操作会拿一个复用的 pid 去 kill
                handle.m_processId = -1;
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
        // 已经退出并被回收的进程不能再发信号：回收那一刻 pid 就交还系统了，此后拿它去 kill
        // 可能落到复用了同一 pid 的无关进程上。pollExitCode 顺带完成回收并把退出码记在句柄上，
        // 因此这一判据与 isRunning() 完全同源；仍在运行的子进程（哪怕已成僵尸）不算「已回收」
        if (!handle.isValid() || pollExitCode(handle).has_value())
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
        // 与 requestTermination() 同一前置判据：已回收的 pid 可能已经被别的进程复用，
        // 强杀下去就是杀死一个无关进程（Linux 上 SIGKILL 必中）
        if (!handle.isValid() || pollExitCode(handle).has_value())
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

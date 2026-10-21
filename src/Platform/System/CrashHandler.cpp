#include "Platform/System/CrashHandler.h"

#include "Platform/IO/FileDescriptor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <limits>

#if ASYN_PLATFORM_WIN32
#include <io.h>
#else
#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace AsynGyanis::Platform
{
    namespace
    {
        std::atomic<bool>      g_isInstalled{false};
        CrashHandler::Reporter g_minimalReporter{nullptr};
        CrashHandler::Reporter g_fullReporter{nullptr};
        void                  *g_context{nullptr};

        /// stderr 的描述符号码：现场必须写到部署时约定的那一处，而不是日志器
        constexpr int kStandardErrorDescriptor = 2;

#if !ASYN_PLATFORM_WIN32
        /// 备用信号栈（每条线程各自一份）：爆栈引发的 SIGSEGV 只有在它上面，处理器才跑得起来
        constexpr std::size_t                                            kAlternativeSignalStackBytes = 128U * 1024U;
        thread_local std::array<std::byte, kAlternativeSignalStackBytes> g_alternativeSignalStack{};

        constexpr int kCrashSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
#else
        /// Windows 没有 SIGBUS；其余同名信号由 CRT 把相应硬件异常路由进来
        constexpr int kCrashSignals[] = {SIGSEGV, SIGABRT, SIGFPE, SIGILL};
#endif

        CrashSignal classifyCrashSignal(const int signalNumber) noexcept
        {
            switch (signalNumber)
            {
                case SIGSEGV:
                    return CrashSignal::SegmentationFault;
                case SIGABRT:
                    return CrashSignal::Abort;
#if !ASYN_PLATFORM_WIN32
                case SIGBUS:
                    return CrashSignal::BusError;
#endif
                case SIGFPE:
                    return CrashSignal::ArithmeticFault;
                case SIGILL:
                    return CrashSignal::IllegalInstruction;
                default:
                    return CrashSignal::Unknown;
            }
        }

        int signalNumberOf(const CrashSignal signal) noexcept
        {
            switch (signal)
            {
                case CrashSignal::SegmentationFault:
                    return SIGSEGV;
                case CrashSignal::Abort:
                    return SIGABRT;
                case CrashSignal::ArithmeticFault:
                    return SIGFPE;
                case CrashSignal::IllegalInstruction:
                    return SIGILL;
                case CrashSignal::BusError:
#if ASYN_PLATFORM_WIN32
                    return 0;
#else
                    return SIGBUS;
#endif
                case CrashSignal::Unknown:
                    break;
            }
            return 0;
        }

        void restoreDefaultSignal(const int signalNumber) noexcept
        {
            if (signalNumber == 0)
            {
                return;
            }
#if ASYN_PLATFORM_WIN32
            static_cast<void>(std::signal(signalNumber, SIG_DFL));
#else
            struct sigaction defaultAction{};
            defaultAction.sa_handler = SIG_DFL;
            static_cast<void>(::sigemptyset(&defaultAction.sa_mask));
            static_cast<void>(::sigaction(signalNumber, &defaultAction, nullptr));
#endif
        }

        /**
         * @brief 致命信号的处理：先最小现场，再按平台补完整现场，最后恢复默认并重发
         * @details 重发之后不能接任何兜底 exit：信号在处理器执行期间被加入屏蔽字，raise 只把它置成
         *          挂起态，真正交付发生在处理器返回那一刻；提前退出会把「信号致死」换成普通退出码。
         * @param signalNumber 平台信号号
         */
        void handleFatalSignal(const int signalNumber) noexcept
        {
            const CrashSignal signal = classifyCrashSignal(signalNumber);
            if (g_minimalReporter != nullptr)
            {
                g_minimalReporter(signal, g_context);
            }

#if !ASYN_PLATFORM_WIN32
            if (g_fullReporter != nullptr)
            {
                // 完整现场的组装会分配；移进子进程后，子进程被继承的锁锁死也只锁它自己（alarm 兜底收走）
                if (const pid_t sceneWriter = ::fork(); sceneWriter == 0)
                {
                    // 子进程里把整组致命信号恢复默认：现场自身再撞信号时安静地死，不能再 fork 一层
                    for (const int crashSignal: kCrashSignals)
                    {
                        restoreDefaultSignal(crashSignal);
                    }
                    static_cast<void>(::alarm(2));
                    g_fullReporter(signal, g_context);
                    ::_exit(0);
                }
            }
#else
            // Windows 没有 fork：完整现场在信号上下文里就地写，最坏也只是缺后半段
            if (g_fullReporter != nullptr)
            {
                g_fullReporter(signal, g_context);
            }
#endif

            restoreDefaultSignal(signalNumber);
            static_cast<void>(std::raise(signalNumber));
        }
    } // namespace

    bool CrashHandler::install(Reporter minimalReporter, Reporter fullReporter, void *context) noexcept
    {
#if !ASYN_PLATFORM_WIN32
        // 备用栈按线程挂：信号处置是进程级的，但备用栈是每条线程各自的资源；重复安装时也要补这一份
        stack_t alternativeStack{};
        alternativeStack.ss_sp    = g_alternativeSignalStack.data();
        alternativeStack.ss_size  = g_alternativeSignalStack.size();
        alternativeStack.ss_flags = 0;
        static_cast<void>(::sigaltstack(&alternativeStack, nullptr));
#endif

        if (g_isInstalled.exchange(true, std::memory_order_acq_rel))
        {
            return false;
        }

        // 先登记回调再装处置：handler 上任之后随时可能被投递
        g_minimalReporter = minimalReporter;
        g_fullReporter    = fullReporter;
        g_context         = context;

#if ASYN_PLATFORM_WIN32
        for (const int crashSignal: kCrashSignals)
        {
            static_cast<void>(std::signal(crashSignal, &handleFatalSignal));
        }
#else
        struct sigaction crashAction{};
        crashAction.sa_handler = &handleFatalSignal;
        crashAction.sa_flags   = SA_ONSTACK;
        static_cast<void>(::sigemptyset(&crashAction.sa_mask));
        for (const int crashSignal: kCrashSignals)
        {
            static_cast<void>(::sigaction(crashSignal, &crashAction, nullptr));
        }
#endif
        return true;
    }

    bool CrashHandler::isInstalled() noexcept
    {
        return g_isInstalled.load(std::memory_order_acquire);
    }

    void CrashHandler::writeStandardError(const std::string_view text) noexcept
    {
        if (text.empty())
        {
            return;
        }
#if ASYN_PLATFORM_WIN32
        // stderr 在 Windows 上是 CRT 的 fd（最终落 WriteFile），不是 socket：FileDescriptor::write
        // 走 ::send 会以 WSAENOTSOCK 失败，现场一个字都出不去
        const auto length = static_cast<unsigned int>(std::min<std::size_t>(text.size(), std::numeric_limits<unsigned int>::max()));
        static_cast<void>(::_write(kStandardErrorDescriptor, text.data(), length));
#else
        static_cast<void>(FileDescriptor::write(kStandardErrorDescriptor, text.data(), text.size()));
#endif
    }

    void CrashHandler::restoreDefaultDisposition(const CrashSignal signal) noexcept
    {
        restoreDefaultSignal(signalNumberOf(signal));
    }
} // namespace AsynGyanis::Platform

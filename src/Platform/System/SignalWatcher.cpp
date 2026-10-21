#include "Platform/System/SignalWatcher.h"

#include <cerrno>
#include <chrono>
#include <format>
#include <stdexcept>
#include <system_error>
#include <utility>

#if ASYN_PLATFORM_WIN32
#include <atomic>
#else
#include <pthread.h>
#include <time.h>
#endif

namespace AsynGyanis::Platform
{
#if ASYN_PLATFORM_WIN32
    namespace
    {
        /// 进程级唯一的控制台停机观察者：控制台处理器是进程级的，两个实例会互相抢
        std::atomic<SignalWatcher *> g_consoleWatcher{nullptr};
    } // namespace
#else
    namespace
    {
        /// 等待线程的轮询周期：不带超时的 sigtimedwait 叫不醒，析构会挂在 join 上
        constexpr std::chrono::milliseconds kWaitPollInterval{50};
    } // namespace
#endif

    SignalSet::SignalSet() noexcept = default;

    SignalSet SignalSet::forShutdown() noexcept
    {
        SignalSet signals;
#if ASYN_PLATFORM_WIN32
        signals.m_capturesConsoleShutdown = true;
#else
        static_cast<void>(signals.add(SIGINT));
        static_cast<void>(signals.add(SIGTERM));
#endif
        return signals;
    }

    SignalSet SignalSet::forReload() noexcept
    {
        SignalSet signals;
#if !ASYN_PLATFORM_WIN32
        static_cast<void>(signals.add(SIGHUP));
#endif
        return signals;
    }

    bool SignalSet::add(const int signalNumber) noexcept
    {
#if ASYN_PLATFORM_WIN32
        static_cast<void>(signalNumber);
        return false;
#else
        return ::sigaddset(&m_set, signalNumber) == 0;
#endif
    }

    bool SignalSet::contains(const int signalNumber) const noexcept
    {
#if ASYN_PLATFORM_WIN32
        static_cast<void>(signalNumber);
        return false;
#else
        return ::sigismember(&m_set, signalNumber) == 1;
#endif
    }

    bool SignalSet::isEmpty() const noexcept
    {
#if ASYN_PLATFORM_WIN32
        return true;
#else
        for (int signalNumber = 1; signalNumber < NSIG; ++signalNumber)
        {
            if (::sigismember(&m_set, signalNumber) == 1)
            {
                return false;
            }
        }
        return true;
#endif
    }

    bool SignalSet::capturesConsoleShutdown() const noexcept
    {
        return m_capturesConsoleShutdown;
    }

    bool SignalSet::block(const SignalSet &signals) noexcept
    {
#if ASYN_PLATFORM_WIN32
        static_cast<void>(signals);
        return true;
#else
        return ::pthread_sigmask(SIG_BLOCK, &signals.m_set, nullptr) == 0;
#endif
    }

    bool SignalSet::unblock(const SignalSet &signals) noexcept
    {
#if ASYN_PLATFORM_WIN32
        static_cast<void>(signals);
        return true;
#else
        return ::pthread_sigmask(SIG_UNBLOCK, &signals.m_set, nullptr) == 0;
#endif
    }

    SignalWatcher::SignalWatcher(SignalSet signals, Handler handler, ErrorHandler errorHandler) :
        m_signals(std::move(signals)), m_handler(std::move(handler)), m_errorHandler(std::move(errorHandler))
    {
#if ASYN_PLATFORM_WIN32
        if (!m_signals.capturesConsoleShutdown())
        {
            return;
        }
        SignalWatcher *expected = nullptr;
        if (!g_consoleWatcher.compare_exchange_strong(expected, this, std::memory_order_acq_rel))
        {
            throw std::logic_error("SignalWatcher: 本进程已经有控制台停机观察者");
        }
        if (::SetConsoleCtrlHandler(reinterpret_cast<PHANDLER_ROUTINE>(&SignalWatcher::consoleHandler), TRUE) == FALSE)
        {
            const int errorCode = static_cast<int>(::GetLastError());
            g_consoleWatcher.store(nullptr, std::memory_order_release);
            throw std::system_error(std::error_code(errorCode, std::system_category()), "安装控制台事件处理器失败");
        }
        m_isInstalled = true;
#else
        if (m_signals.isEmpty())
        {
            return;
        }
        if (!SignalSet::block(m_signals))
        {
            throw std::runtime_error("屏蔽信号失败");
        }
        try
        {
            const SignalSet watchSet = m_signals;
            m_waiter                 = std::jthread(
                    [this, watchSet](const std::stop_token &stopToken)
                    {
                        // 两条观察者（停机/重载）的构造顺序不固定：本线程把所有受管信号都挡住，
                        // 才不会成为另一路信号的缺省动作受害者
                        SignalSet managed = SignalSet::forShutdown();
                        static_cast<void>(managed.add(SIGHUP));
                        static_cast<void>(SignalSet::block(managed));

                        while (!stopToken.stop_requested())
                        {
                            timespec  timeout{0, static_cast<long>(kWaitPollInterval.count() * 1'000'000)};
                            siginfo_t info{};
                            const int received = ::sigtimedwait(&watchSet.m_set, &info, &timeout);
                            if (received < 0)
                            {
                                if (errno == EAGAIN || errno == EINTR)
                                {
                                    continue;
                                }
                                reportError(std::format("等待信号失败（errno={}）", errno));
                                return;
                            }

                            SignalEvent event = SignalEvent::Unknown;
                            switch (received)
                            {
                                case SIGINT:
                                    event = SignalEvent::Interrupt;
                                    break;
                                case SIGTERM:
                                    event = SignalEvent::Terminate;
                                    break;
                                case SIGHUP:
                                    event = SignalEvent::Reload;
                                    break;
                                default:
                                    break;
                            }
                            if (event == SignalEvent::Unknown)
                            {
                                reportError(std::format("收到未分类的信号 {}", received));
                                continue;
                            }
                            static_cast<void>(dispatch(event));
                        }
                    });
        } catch (...)
        {
            static_cast<void>(SignalSet::unblock(m_signals));
            throw;
        }
        m_isInstalled = true;
#endif
    }

    SignalWatcher::~SignalWatcher()
    {
#if ASYN_PLATFORM_WIN32
        if (m_isInstalled)
        {
            static_cast<void>(::SetConsoleCtrlHandler(reinterpret_cast<PHANDLER_ROUTINE>(&SignalWatcher::consoleHandler), FALSE));
            if (g_consoleWatcher.load(std::memory_order_acquire) == this)
            {
                g_consoleWatcher.store(nullptr, std::memory_order_release);
            }
        }
#else
        m_waiter = std::jthread{};
        if (m_isInstalled)
        {
            static_cast<void>(SignalSet::unblock(m_signals));
        }
#endif
    }

    bool SignalWatcher::isInstalled() const noexcept
    {
        return m_isInstalled;
    }

    bool SignalWatcher::dispatch(const SignalEvent event) noexcept
    {
        try
        {
            return m_handler != nullptr && m_handler(event);
        } catch (const std::exception &failure)
        {
            reportError(failure.what());
            return false;
        } catch (...)
        {
            reportError("信号处理器抛出非标准异常");
            return false;
        }
    }

    void SignalWatcher::reportError(const std::string_view message) const noexcept
    {
        if (m_errorHandler == nullptr)
        {
            return;
        }
        try
        {
            m_errorHandler(message);
        } catch (...)
        {
        }
    }

#if ASYN_PLATFORM_WIN32
    int __stdcall SignalWatcher::consoleHandler(const unsigned long controlType) noexcept
    {
        SignalWatcher *watcher = g_consoleWatcher.load(std::memory_order_acquire);
        if (watcher == nullptr)
        {
            return FALSE;
        }

        SignalEvent event = SignalEvent::Unknown;
        switch (controlType)
        {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT:
                event = SignalEvent::Interrupt;
                break;
            case CTRL_CLOSE_EVENT:
            case CTRL_LOGOFF_EVENT:
            case CTRL_SHUTDOWN_EVENT:
                event = SignalEvent::ConsoleClose;
                break;
            default:
                return FALSE;
        }
        return watcher->dispatch(event) ? TRUE : FALSE;
    }
#endif
} // namespace AsynGyanis::Platform

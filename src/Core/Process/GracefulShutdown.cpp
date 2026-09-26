#include "Core/Process/GracefulShutdown.h"

#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/EventLoop/EventLoop.h"
#include "Platform/Platform.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <stop_token>

#if ASYN_PLATFORM_WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <pthread.h>
#endif

namespace AsynGyanis::Core
{
    namespace
    {
        /// 进程级唯一的观察者：信号掩码与处理器都是进程级的东西，两个实例只会互相抢
        GracefulShutdown *g_owner = nullptr;

#if ASYN_PLATFORM_WIN32
        /// 关闭、注销与关机事件留给收尾动作的时间上限：处理器一返回，系统就要终止进程
        constexpr std::chrono::milliseconds kConsoleEventBudget{5000};

        /**
         * @brief 系统要求的 C 回调跳板
         * @details 只经公开接口办事（isTriggered 与 requestShutdown）：控制台处理器是进程级自由函数，
         *          让它去碰实例的私有成员就得把跳板暴露成友元，那不值。
         * @param controlType 事件类型
         * @return TRUE 本观察者已接管（因此挡住缺省的进程终止）；FALSE 交还给系统
         */
        BOOL WINAPI consoleControlHandler(const DWORD controlType)
        {
            GracefulShutdown *owner = g_owner;
            if (owner == nullptr || owner->isTriggered())
            {
                // 已经在收尾了还来一次：交还给系统按缺省强杀，别把「卡住的退出」变成永久等待
                return FALSE;
            }

            switch (controlType)
            {
                case CTRL_C_EVENT:
                case CTRL_BREAK_EVENT:
                    owner->requestShutdown(GracefulShutdown::Reason::Interrupt);
                    return TRUE;
                case CTRL_CLOSE_EVENT:
                case CTRL_LOGOFF_EVENT:
                case CTRL_SHUTDOWN_EVENT:
                    // 这三类事件处理器一返回系统就终止进程，所以 requestShutdown 内部要原地等完（有上限）
                    owner->requestShutdown(GracefulShutdown::Reason::ConsoleEvent);
                    return TRUE;
                default:
                    return FALSE;
            }
        }
#else
        /// 本类接管的两路信号
        constexpr std::array<int, 2> kHandledSignals{SIGINT, SIGTERM};

        /// 等待线程的轮询周期：不带超时的 sigwait 叫不醒，析构就会挂在 join 上等一个再也不会来的信号
        constexpr std::chrono::milliseconds kWaitPollInterval{50};

        /**
         * @brief 把 sigset_t 填成「只含本类接管的信号」
         * @param[out] watchSet 填好的集合；失败时由调用方按空集处理
         */
        void fillWatchedSignalSet(sigset_t &watchSet) noexcept
        {
            sigemptyset(&watchSet);
            for (const int signalNumber: kHandledSignals)
            {
                static_cast<void>(sigaddset(&watchSet, signalNumber));
            }
        }
#endif
    } // namespace

    /// 收尾动作是否跑完的握手对象：等它的一方与执行动作的一方共用同一份状态
    class ShutdownHandshake
    {
    public:
        /// 动作跑完（或根本没有动作可跑）时由执行侧调用
        void finish() noexcept
        {
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_isDone = true;
            }
            m_condition.notify_all();
        }

        /**
         * @brief 等到动作跑完，或等满时限
         * @param budget 时限
         * @return true 动作已跑完；false 超时（调用方只能放手，再等下去就是拿自己的命换一个日志刷盘）
         */
        bool waitFor(const std::chrono::milliseconds budget) noexcept
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_condition.wait_for(lock, budget, [this] { return m_isDone; });
        }

    private:
        std::mutex              m_mutex;
        std::condition_variable m_condition;
        bool                    m_isDone{false};
    };

    GracefulShutdown::GracefulShutdown(EventLoop &loop) : m_loop(&loop)
    {
        if (g_owner != nullptr)
        {
            LOG_ERROR("GracefulShutdown: 本进程已经有一个停机信号观察者，这一个不会装上信号接管（信号掩码与处理器都是进程级的，两个实例只会互相抢）；停机仍可走 requestShutdown()");
            return;
        }

#if ASYN_PLATFORM_WIN32
        if (!::SetConsoleCtrlHandler(&consoleControlHandler, TRUE))
        {
            LOG_ERROR_FMT("GracefulShutdown: 安装控制台事件处理器失败（GetLastError={}），停机信号这一路等于没接上；收尾仍可走 requestShutdown()",
                          static_cast<unsigned long>(::GetLastError()));
            return;
        }
#else
        // 先把两路信号挡进屏蔽字：被挡住的信号不会按缺省动作终止进程，只会被下面那个等待线程取走。
        // 屏蔽只对**之后**派生的线程生效（子线程继承掩码），因此本类要求尽早构造
        sigset_t watchSet;
        fillWatchedSignalSet(watchSet);
        const int blocked = ::pthread_sigmask(SIG_BLOCK, &watchSet, nullptr);
        if (blocked != 0)
        {
            LOG_ERROR_FMT("GracefulShutdown: 屏蔽 SIGINT/SIGTERM 失败（errno={}），信号仍会按缺省动作直接终止进程", blocked);
            return;
        }
#endif

        g_owner = this;
#if !ASYN_PLATFORM_WIN32
        m_waiter = std::jthread(
                [this, watchSet](const std::stop_token &stopToken)
                {
                    for (;;)
                    {
                        if (stopToken.stop_requested())
                        {
                            return;
                        }
                        const std::chrono::nanoseconds interval = kWaitPollInterval;
                        timespec                       timeout{static_cast<time_t>(interval.count() / 1'000'000'000LL), static_cast<long>(interval.count() % 1'000'000'000LL)};
                        siginfo_t                      info{};
                        const int                      received = ::sigtimedwait(&watchSet, &info, &timeout);
                        if (received < 0)
                        {
                            // 超时与被中断都是「再来一轮」；其余错误说明这条路已经废了，说清楚再收手
                            if (errno == EAGAIN || errno == EINTR)
                            {
                                continue;
                            }
                            LOG_ERROR_FMT("GracefulShutdown: 等待信号出错（errno={}），停机信号接管就此停止", errno);
                            return;
                        }
                        triggerShutdown(received == SIGINT ? Reason::Interrupt : Reason::Terminate, false);
                    }
                });
#endif
        m_isInstalled.store(true, std::memory_order_release);
    }

    GracefulShutdown::~GracefulShutdown()
    {
        if (!m_isInstalled.load(std::memory_order_acquire))
        {
            return;
        }
#if ASYN_PLATFORM_WIN32
        static_cast<void>(::SetConsoleCtrlHandler(&consoleControlHandler, FALSE));
#else
        // 撤掉屏蔽即还原缺省行为：本类只在构造时把两路信号挡进去，还原不需要更细的状态。
        // 已经跑起来的线程仍留着继承来的掩码，这是 pthread 的既有语义，不假装能收回
        sigset_t watchSet;
        fillWatchedSignalSet(watchSet);
        static_cast<void>(::pthread_sigmask(SIG_UNBLOCK, &watchSet, nullptr));
#endif
        g_owner = nullptr;
        m_isInstalled.store(false, std::memory_order_release);
        // 等待线程由 m_waiter 的析构负责请求停止并 join（它声明在最后，因此先于上面这些成员销毁）
    }

    std::size_t GracefulShutdown::onShutdown(std::function<void()> action)
    {
        std::size_t token            = 0;
        bool        isAlreadyRunning = false;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            token = m_nextToken++;
            if (m_isTriggered.load(std::memory_order_acquire))
            {
                isAlreadyRunning = true;
            } else
            {
                m_actions.emplace_back(token, std::move(action));
            }
        }

        if (isAlreadyRunning)
        {
            // 收尾已经开始了：静默丢掉这条注册等于让调用方以为「登记上了」，因此补投一次而不是拒收
            LOG_INFO("GracefulShutdown: 收尾已经触发，这条动作按「迟到注册」单独投一次事件循环");
            postRunnable([action = std::move(action)]() noexcept { runAction(action); }, nullptr);
        }
        return token;
    }

    bool GracefulShutdown::cancel(const std::size_t token) noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto                        found = std::ranges::find_if(m_actions, [token](const auto &entry) { return entry.first == token; });
        if (found == m_actions.end())
        {
            return false;
        }
        m_actions.erase(found);
        return true;
    }

    void GracefulShutdown::requestShutdown(const Reason reason) noexcept
    {
        // 关闭、注销与关机这三类事件里，处理器一返回系统就要终止进程，因此只有它们需要等收尾跑完；
        // 真信号（SIGTERM/Ctrl+C）与程序触发都不用等——触发方还要接着跑它自己的收尾逻辑
        triggerShutdown(reason, reason == Reason::ConsoleEvent);
    }

    std::optional<GracefulShutdown::Reason> GracefulShutdown::triggeredBy() const noexcept
    {
        if (!isTriggered())
        {
            return std::nullopt;
        }
        return static_cast<Reason>(m_reason.load(std::memory_order_acquire));
    }

    bool GracefulShutdown::isInstalled() const noexcept
    {
        return m_isInstalled.load(std::memory_order_acquire);
    }

    void GracefulShutdown::triggerShutdown(const Reason reason, const bool waitsForCompletion) noexcept
    {
        // 触发是单向的：连按两次 Ctrl+C 是催命，不是把收尾再排一遍
        if (m_isTriggered.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        // 成因按 Reason 原值存，「有没有触发过」由 m_isTriggered 判——Interrupt 本身就是 0，
        // 不能拿 0 当「未设置」用
        m_reason.store(static_cast<std::uint8_t>(reason), std::memory_order_release);

        std::shared_ptr<ShutdownHandshake> handshake;
        if (waitsForCompletion)
        {
            handshake = std::make_shared<ShutdownHandshake>();
        }
        dispatchActions(handshake);

        if (handshake != nullptr && !handshake->waitFor(kConsoleEventBudget))
        {
            LOG_ERROR("GracefulShutdown: 收尾动作没有在时限内跑完，进程即将随系统终止——这条之后的输出可能落不了盘");
        }
    }

    void GracefulShutdown::dispatchActions(std::shared_ptr<ShutdownHandshake> handshake) noexcept
    {
        std::vector<std::function<void()>> actions;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            actions.reserve(m_actions.size());
            for (auto &entry: m_actions)
            {
                actions.push_back(std::move(entry.second));
            }
            m_actions.clear();
        }

        if (actions.empty())
        {
            if (handshake != nullptr)
            {
                handshake->finish();
            }
            return;
        }

        postRunnable(
                [actions = std::move(actions)]() noexcept
                {
                    for (auto &action: actions)
                    {
                        runAction(action);
                    }
                },
                handshake);
    }

    void GracefulShutdown::postRunnable(std::function<void()> runnable, std::shared_ptr<ShutdownHandshake> handshake) noexcept
    {
        // 投回去的动作跑完（或根本没有动作可跑）之后必须把握手放掉，否则等它的处理器线程要一直等到时限
        m_loop->scheduler().postRemote(
                [inner = std::move(runnable), handshake]() mutable noexcept
                {
                    inner();
                    if (handshake != nullptr)
                    {
                        handshake->finish();
                    }
                });
    }

    void GracefulShutdown::runAction(const std::function<void()> &action) noexcept
    {
        try
        {
            action();
        } catch (const std::exception &failure)
        {
            LOG_ERROR_FMT("GracefulShutdown: 一个收尾动作抛出异常并被接住：{}（后面的动作照跑，否则一条日志器写不进盘就能把「把连接体面关掉」一起带走）",
                          std::string(failure.what()));
        } catch (...)
        {
            LOG_ERROR("GracefulShutdown: 一个收尾动作抛出非标准异常并被接住（后面的动作照跑）");
        }
    }

} // namespace AsynGyanis::Core

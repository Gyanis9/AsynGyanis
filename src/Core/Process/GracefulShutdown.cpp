#include "Core/Process/GracefulShutdown.h"

#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Process/ReloadSignal.h"
#include "Core/Process/SignalObserverInstall.h"
#include "Platform/Platform.h"
#include "Platform/System/SignalWatcher.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <utility>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 进程级唯一的观察者：信号掩码与处理器都是进程级的东西，两个实例只会互相抢
        GracefulShutdown *g_owner = nullptr;

        /// 需要原地等收尾跑完的那类触发（Windows 的关闭、注销、关机事件）留给它的时间上限
        constexpr std::chrono::milliseconds kConsoleEventBudget{5000};
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

    bool GracefulShutdown::blockStopSignals() noexcept
    {
        if (!Platform::SignalSet::block(Platform::SignalSet::forShutdown()))
        {
            LOG_ERROR("GracefulShutdown: 屏蔽 SIGINT/SIGTERM 失败，信号仍会按缺省动作直接终止进程");
            return false;
        }
        return true;
    }

    GracefulShutdown::GracefulShutdown()
    {
        install();
    }

    GracefulShutdown::GracefulShutdown(EventLoop &loop) : m_loop(&loop)
    {
        install();
    }

    void GracefulShutdown::install() noexcept
    {
        if (g_owner != nullptr)
        {
            LOG_ERROR("GracefulShutdown: 本进程已经有一个停机信号观察者，这一个不会装上信号接管（信号屏蔽与处理器都是进程级的，两个实例只会互相抢）；停机仍可走 requestShutdown()");
            return;
        }

        // 观察者自己完成屏蔽与等待线程/控制台处理器的安装；Core 只保留进程级属主与派发策略
        m_watcher = installSignalObserver(
                Platform::SignalSet::forShutdown(), [this](const Platform::SignalEvent event) noexcept { return handleSignal(event); }, "GracefulShutdown",
                "收尾仍可走 requestShutdown()", "本平台没有可用的停机路径");
        if (m_watcher == nullptr)
        {
            return;
        }
        g_owner = this;
        m_isInstalled.store(true, std::memory_order_release);
    }

    GracefulShutdown::~GracefulShutdown()
    {
        if (!m_isInstalled.load(std::memory_order_acquire))
        {
            return;
        }
        // 观察者先停：POSIX 停等待线程并还原屏蔽，Windows 摘掉控制台处理器
        m_watcher.reset();
        g_owner = nullptr;
        m_isInstalled.store(false, std::memory_order_release);
    }

    bool GracefulShutdown::handleSignal(const Platform::SignalEvent event) noexcept
    {
        // 已经收尾过：Windows 交回系统缺省动作（连按两次 Ctrl+C 是催命），POSIX 仍消费掉这条信号
        if (isTriggered())
        {
            return false;
        }
        switch (event)
        {
            case Platform::SignalEvent::Interrupt:
                triggerShutdown(Reason::Interrupt, false);
                return true;
            case Platform::SignalEvent::Terminate:
                triggerShutdown(Reason::Terminate, false);
                return true;
            case Platform::SignalEvent::ConsoleClose:
                triggerShutdown(Reason::ConsoleEvent, true);
                return true;
            default:
                return false;
        }
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
            LOG_INFO("GracefulShutdown: 收尾已经触发，这条动作按「迟到注册」单独投一次");
            std::vector<std::function<void()>> lateActions;
            lateActions.push_back(std::move(action));
            releaseActions(std::move(lateActions), nullptr);
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
            // 握手对象要分配，而本函数是 noexcept：分配失败时留空，动作照发，
            // 只是触发方不再等收尾跑完——这比让整个进程 terminate 好
            try
            {
                handshake = std::make_shared<ShutdownHandshake>();
            } catch (const std::exception &failure)
            {
                try
                {
                    LOG_ERROR_FMT("GracefulShutdown: 分配收尾握手对象失败（{}），本次触发不再等待收尾跑完", failure.what());
                } catch (...)
                {
                }
            } catch (...)
            {
                try
                {
                    LOG_ERROR("GracefulShutdown: 分配收尾握手对象失败（非标准异常），本次触发不再等待收尾跑完");
                } catch (...)
                {
                }
            }
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
        // 搬移表项同样要分配，而本函数是 noexcept：失败时放弃本次派发，
        // 动作表原样留着，并把握手放掉，免得等它的一方白等到时限
        try
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            actions.reserve(m_actions.size());
            for (auto &entry: m_actions)
            {
                actions.push_back(std::move(entry.second));
            }
            m_actions.clear();
        } catch (const std::exception &failure)
        {
            try
            {
                LOG_ERROR_FMT("GracefulShutdown: 取出收尾动作失败（{}），本次触发不执行任何动作", failure.what());
            } catch (...)
            {
            }
            if (handshake != nullptr)
            {
                handshake->finish();
            }
            return;
        } catch (...)
        {
            try
            {
                LOG_ERROR("GracefulShutdown: 取出收尾动作失败（非标准异常），本次触发不执行任何动作");
            } catch (...)
            {
            }
            if (handshake != nullptr)
            {
                handshake->finish();
            }
            return;
        }

        if (actions.empty())
        {
            if (handshake != nullptr)
            {
                handshake->finish();
            }
            return;
        }

        releaseActions(std::move(actions), handshake);
    }

    void GracefulShutdown::releaseActions(std::vector<std::function<void()>> &&actions, std::shared_ptr<ShutdownHandshake> handshake) noexcept
    {
        if (m_loop == nullptr)
        {
            // 没绑循环就地执行：动作跑在这条收到信号的线程上。等它的一方正是本线程，
            // 所以握手要在跑完后立刻放行，否则就是自己等自己
            for (auto &action: actions)
            {
                runAction(action);
            }
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
        // 投递要分配队列节点与闭包，而本函数是 noexcept：失败时把握手当场放掉，等它的一方立刻拿到结果。
        // 队列饱和时投递也会被拒：这一档同样要当场放行，否则等它的一方要白等满一个永远等不到的时限
        try
        {
            if (!m_loop->scheduler().tryPostRemote(
                        [inner = std::move(runnable), handshake]() mutable noexcept
                        {
                            inner();
                            if (handshake != nullptr)
                            {
                                handshake->finish();
                            }
                        }))
            {
                try
                {
                    LOG_ERROR("GracefulShutdown: 收尾动作未能排进事件循环的队列（队列已满），本次触发不执行这些动作");
                } catch (...)
                {
                }
                if (handshake != nullptr)
                {
                    handshake->finish();
                }
            }
        } catch (const std::exception &failure)
        {
            try
            {
                LOG_ERROR_FMT("GracefulShutdown: 把收尾动作投回事件循环失败（{}），这批动作不会执行", failure.what());
            } catch (...)
            {
            }
            if (handshake != nullptr)
            {
                handshake->finish();
            }
        } catch (...)
        {
            try
            {
                LOG_ERROR("GracefulShutdown: 把收尾动作投回事件循环失败（非标准异常），这批动作不会执行");
            } catch (...)
            {
            }
            if (handshake != nullptr)
            {
                handshake->finish();
            }
        }
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

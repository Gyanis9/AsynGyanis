#include "Core/Process/ReloadSignal.h"

#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Process/GracefulShutdown.h"
#include "Platform/Platform.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>
#include <vector>

#if ASYN_PLATFORM_WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <pthread.h>
#include <time.h>
#endif

namespace AsynGyanis::Core
{
    namespace
    {
        /// 进程级唯一的重载观察者：屏蔽字与待取队列都是进程级的东西，两个实例会互相抢同一条 SIGHUP
        ReloadSignal *g_owner = nullptr;

#if !ASYN_PLATFORM_WIN32
        /// 本观察者取的那一个信号：SIGHUP 是运维侧「换一份配置继续跑」的通行约定
        constexpr int kReloadSignalNumber = SIGHUP;

        /// 等待线程的轮询周期：与停机观察者同值，够快又不让一条空等的线程变成 CPU 消耗
        constexpr std::chrono::milliseconds kWaitPollInterval{50};

        /**
         * @brief 填本观察者要取的那一个信号
         * @param watchSet 输出用的信号集（调用方给未使用过的对象）
         */
        void fillWatchedSignalSet(sigset_t &watchSet) noexcept
        {
            sigemptyset(&watchSet);
            sigaddset(&watchSet, kReloadSignalNumber);
        }
#endif
    } // namespace

    bool ReloadSignal::blockReloadSignal() noexcept
    {
#if ASYN_PLATFORM_WIN32
        // Windows 没有 SIGHUP，也就没有需要提前挡的东西
        return true;
#else
        sigset_t watchSet;
        fillWatchedSignalSet(watchSet);
        const int blocked = ::pthread_sigmask(SIG_BLOCK, &watchSet, nullptr);
        if (blocked != 0)
        {
            LOG_ERROR_FMT("ReloadSignal: 屏蔽 SIGHUP 失败（errno={}），这条信号仍会按缺省动作终止进程", blocked);
            return false;
        }
        return true;
#endif
    }

    ReloadSignal::ReloadSignal()
    {
        install();
    }

    ReloadSignal::ReloadSignal(EventLoop &loop) : m_loop(&loop)
    {
        install();
    }

    ReloadSignal::~ReloadSignal()
    {
        uninstall();
    }

    void ReloadSignal::install() noexcept
    {
        if (g_owner != nullptr)
        {
            LOG_ERROR("ReloadSignal: 本进程已经有一个重载信号观察者，这一个不会装上信号接管（屏蔽字与待取队列都是进程级的，两个实例只会互相抢同一条 SIGHUP）；重读仍可走 "
                      "requestReload()");
            return;
        }

#if ASYN_PLATFORM_WIN32
        LOG_ERROR("ReloadSignal: 当前平台没有 SIGHUP 这条约定（Windows 的 CTRL_* 五种全是停机向的，没有「重载」这一类事件），信号接管不会装上；重载入口请用 "
                  "Base::ConfigManager::enableHotReload() 的文件监听，或走 requestReload()");
#else
        if (!blockReloadSignal())
        {
            return;
        }

        g_owner = this;
        m_isInstalled.store(true, std::memory_order_release);

        sigset_t watchSet;
        fillWatchedSignalSet(watchSet);
        m_waiter = std::jthread(
                [this, watchSet](const std::stop_token &stopToken)
                {
                    // 这条线程不能成为停机信号的缺省动作受害者：屏蔽字只被子线程继承，而两个观察者
                    // 的构造顺序不固定——先起的那条线程挡不住后一路的号。不补这一步的表现是
                    // 「装了重载观察者之后，一枚 SIGTERM 被它接住并按缺省动作把进程杀掉」
                    static_cast<void>(GracefulShutdown::blockStopSignals());

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

                            LOG_ERROR_FMT("ReloadSignal: 等待 SIGHUP 的线程因错误退出（errno={}），此后这条信号不再触发任何动作", errno);
                            return;
                        }

                        if (received != kReloadSignalNumber)
                        {
                            // 信号集里只有 SIGHUP，真收到别的号说明这条路被别处动过；不猜，记下来继续等
                            LOG_WARN_FMT("ReloadSignal: 等待线程取到预期外的信号 {}，本轮不触发重载", received);
                            continue;
                        }

                        fireReload();
                    }
                });
        return;
#endif

        // Windows 分支走到这里：登记位空着，isInstalled() 保持 false（上面已经把原因说清楚了）
    }

    void ReloadSignal::uninstall() noexcept
    {
#if !ASYN_PLATFORM_WIN32
        // jthread 的析构会 request_stop 并 join：等待线程每轮最多睡 50ms，因此 join 有上限。
        // 顺序上这里是安全的——m_waiter 声明在最后，成员逆序销毁时它最先停，
        // 停稳之后才轮到原子量与注册表被销毁
        m_isInstalled.store(false, std::memory_order_release);
        if (g_owner == this)
        {
            g_owner = nullptr;
        }
#endif
    }

    std::size_t ReloadSignal::onReload(std::function<void()> action)
    {
        const std::lock_guard lock(m_mutex);
        const std::size_t     token = m_nextToken++;
        m_actions.emplace_back(token, std::move(action));
        return token;
    }

    bool ReloadSignal::cancel(const std::size_t token) noexcept
    {
        const std::lock_guard lock(m_mutex);
        const auto            found = std::find_if(m_actions.begin(), m_actions.end(), [token](const auto &entry) { return entry.first == token; });
        if (found == m_actions.end())
        {
            return false;
        }

        m_actions.erase(found);
        return true;
    }

    void ReloadSignal::requestReload() noexcept
    {
        fireReload();
    }

    bool ReloadSignal::isInstalled() const noexcept
    {
        return m_isInstalled.load(std::memory_order_acquire);
    }

    std::uint64_t ReloadSignal::reloadCount() const noexcept
    {
        return m_reloadCount.load(std::memory_order_acquire);
    }

    void ReloadSignal::fireReload() noexcept
    {
        std::vector<std::function<void()>> actions;
        {
            // 一次触发**复制**整份注册表，不取走：重载与停机不同，它不是一次性的——同一批动作要在每一次
            // SIGHUP 上都再跑一遍。取走就等于「第一次 reload 之后，这个进程的 reload 从此什么都不做」，
            // 而那正是运维最难发现的一种形态。锁只在复制期间持有，动作跑在锁外，
            // 一个慢动作因此不会把下一条 SIGHUP 的注册表读走
            const std::lock_guard lock(m_mutex);
            actions.reserve(m_actions.size());
            for (const auto &entry: m_actions)
            {
                actions.push_back(entry.second);
            }
        }

        m_reloadCount.fetch_add(1, std::memory_order_acq_rel);
        releaseActions(std::move(actions));
    }

    void ReloadSignal::releaseActions(std::vector<std::function<void()>> &&actions) noexcept
    {
        if (m_loop == nullptr)
        {
            // 没绑循环就地执行：动作跑在这条取到信号的线程上，与停机观察者同一条契约
            for (auto &action: actions)
            {
                runAction(action);
            }
            return;
        }

        m_loop->scheduler().postRemote(
                [actions = std::move(actions)]() mutable noexcept
                {
                    for (auto &action: actions)
                    {
                        runAction(action);
                    }
                });
    }

    void ReloadSignal::runAction(const std::function<void()> &action) noexcept
    {
        try
        {
            action();
        } catch (const std::exception &failure)
        {
            LOG_ERROR_FMT("ReloadSignal: 一个重载动作抛出异常并被接住：{}（后面的动作照跑——一次重读失败不该把「把新配置接上」整段带走）", std::string(failure.what()));
        } catch (...)
        {
            LOG_ERROR("ReloadSignal: 一个重载动作抛出非标准异常并被接住（后面的动作照跑）");
        }
    }

} // namespace AsynGyanis::Core

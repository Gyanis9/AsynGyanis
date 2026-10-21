#include "Core/Process/ReloadSignal.h"

#include "Base/Log/LogMacros.h"
#include "Core/Coroutine/Scheduler.h"
#include "Core/EventLoop/EventLoop.h"
#include "Core/Process/SignalObserverInstall.h"
#include "Platform/System/SignalWatcher.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>
#include <vector>

namespace AsynGyanis::Core
{
    namespace
    {
        /// 进程级唯一的重载观察者：信号屏蔽与处理器都是进程级的东西，两个实例会互相抢
        ReloadSignal *g_owner = nullptr;
    } // namespace

    bool ReloadSignal::blockReloadSignal() noexcept
    {
        if (!Platform::SignalSet::block(Platform::SignalSet::forReload()))
        {
            LOG_ERROR("ReloadSignal: 屏蔽 SIGHUP 失败，这条信号仍会按缺省动作终止进程");
            return false;
        }
        return true;
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
            LOG_ERROR("ReloadSignal: 本进程已经有一个重载信号观察者，这一个不会装上信号接管（信号屏蔽与处理器都是进程级的，两个实例只会互相抢同一条 SIGHUP）；重读仍可走 "
                      "requestReload()");
            return;
        }

        // 观察者自己完成屏蔽与等待线程/控制台处理器的安装；Core 只保留进程级属主与派发策略
        m_watcher = installSignalObserver(
                Platform::SignalSet::forReload(),
                [this](const Platform::SignalEvent event) noexcept
                {
                    if (event != Platform::SignalEvent::Reload)
                    {
                        return false;
                    }
                    fireReload();
                    return true;
                },
                "ReloadSignal", "重载仍可走 requestReload()",
                "当前平台没有 SIGHUP 这条约定：Windows 的 CTRL_* 五种全是停机向的，没有「重载」这一类事件；文件监听走 "
                "Base::ConfigManager::enableHotReload()");
        if (m_watcher == nullptr)
        {
            return;
        }
        g_owner = this;
        m_isInstalled.store(true, std::memory_order_release);
    }

    void ReloadSignal::uninstall() noexcept
    {
        m_watcher.reset();
        if (g_owner == this)
        {
            g_owner = nullptr;
        }
        m_isInstalled.store(false, std::memory_order_release);
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
        // 复制整份注册表要分配内存，而本函数是 noexcept：内存吃紧时 bad_alloc 会直接 terminate。
        // 失败只放弃这一次重载，动作表原样留着，下一条 SIGHUP 还能再跑
        try
        {
            // 一次触发复制整份注册表，不取走：重载与停机不同，它不是一次性的——同一批动作要在每一次
            // SIGHUP 上都再跑一遍。取走就等于「第一次 reload 之后，这个进程的 reload 从此什么都不做」，
            // 而那正是运维最难发现的一种形态。锁只在复制期间持有，动作跑在锁外，
            // 一个慢动作因此不会把下一条 SIGHUP 的注册表读走
            const std::lock_guard lock(m_mutex);
            actions.reserve(m_actions.size());
            for (const auto &entry: m_actions)
            {
                actions.push_back(entry.second);
            }
        } catch (const std::exception &failure)
        {
            try
            {
                LOG_ERROR_FMT("ReloadSignal: 复制重载动作失败（{}），本次重载不执行；动作表保持不变", failure.what());
            } catch (...)
            {
            }
            return;
        } catch (...)
        {
            try
            {
                LOG_ERROR("ReloadSignal: 复制重载动作失败（非标准异常），本次重载不执行；动作表保持不变");
            } catch (...)
            {
            }
            return;
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

        // 投递要分配 std::function 与队列结点，内存吃紧时抛出的异常会穿出本函数的 noexcept 边界；
        // 就地收下并记一条，本次重载的动作就此丢弃
        try
        {
            m_loop->scheduler().postRemote(
                    [actions = std::move(actions)]() mutable noexcept
                    {
                        for (auto &action: actions)
                        {
                            runAction(action);
                        }
                    });
        } catch (const std::exception &failure)
        {
            try
            {
                LOG_ERROR_FMT("ReloadSignal: 重载动作未能投回事件循环（{}），本次重载的动作已丢弃", failure.what());
            } catch (...)
            {
            }
        } catch (...)
        {
            try
            {
                LOG_ERROR("ReloadSignal: 重载动作未能投回事件循环（非标准异常），本次重载的动作已丢弃");
            } catch (...)
            {
            }
        }
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

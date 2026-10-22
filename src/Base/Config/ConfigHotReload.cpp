#include "Base/Config/ConfigManager.h"

#include "Base/Log/LogMacros.h"
#include "Platform/FileSystem/FileSystem.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsynGyanis::Base
{
    void ConfigManager::handleFileChange(const std::string_view filePath, const Platform::FileChangeType changeType)
    {
        // 这条路径跑在文件监视线程上，而监视器派发改动时不拦回调的异常：从这里抛出去
        // 就是逃出线程函数 → std::terminate 把整个进程带走。单个事件失败只该丢掉这一个事件
        // （下一次改动还会再来），因此整段收口在这里
        try
        {
            // 重扫信号走的是目录路径，按扩展名的那道过滤会把它整个滤掉——内核丢事件时本要的正是
            // 「把整份目录重读一遍」，因此它在过滤之前先接下来，与单个文件变更走同一条重载路径
            if (changeType == Platform::FileChangeType::NeedsRescan)
            {
                scheduleReload();
                return;
            }

            if (!isConfigFile(filePath))
            {
                return;
            }

            // 四种类型都要收：监视目录内部的改名两侧同为 Moved，而把配置移出监视范围时 Windows 报
            // Deleted、Linux 报 Moved，消费方必须两种都收；真实删除也报 Deleted。少收一种时，改名
            // 挪走配置这个下线动作会在源路径上被类型判据滤掉，配置停在已经消失的那份上直到下次改动
            if (changeType != Platform::FileChangeType::Modified && changeType != Platform::FileChangeType::Created && changeType != Platform::FileChangeType::Deleted &&
                changeType != Platform::FileChangeType::Moved)
            {
                return;
            }

            scheduleReload();
        } catch (const std::exception &changeError)
        {
            LOG_ERROR_EXCEPTION(changeError, "ConfigManager: 处理文件变更事件时抛出异常，本事件已丢弃：{}", changeError.what());
        } catch (...)
        {
            LOG_ERROR_FMT("ConfigManager: 处理文件变更事件时抛出非标准异常，本事件已丢弃");
        }
    }

    /**
     * @brief 安排一轮重载：已在跑就只记下这次变更，由那一轮的收尾接力
     */
    void ConfigManager::scheduleReload()
    {
        // 先把「又要重读」记进世代号，再去抢执行权：抢不到时正在跑的那一轮收尾会比对自己开始时的
        // 快照，看到差别就再来一轮。反过来（先抢后记）可能让那一轮刚好比对在这次记录之前，
        // 这次变更就没人接力了
        m_reloadGate.noteChanged();

        // 已有任务在跑：不重复起轮，交给它那轮的接力。重载要读完整份目录，期间到达的变更
        // （尤其是紧接着那次写入）会落在本轮之后——丢掉它配置就停在旧值，直到用户下一次改动
        if (!m_reloadGate.claimRound())
        {
            return;
        }

        startReloadTask();

        // 清理已完成任务并回收线程资源，防止任务列表无限增长。
        // join 在锁外执行：持 m_reloadTasksMutex 期间 join 会把「一次长 reload 的等待」
        // 变成对所有后续热加载检查的阻塞
        collectFinishedReloadTasks();
    }

    void ConfigManager::startReloadTask()
    {
        // 这条路上会失败的是登记任务与起线程的分配（扩登记表、建任务节点、开线程栈、线程数触到系统上限）。
        // 让它抛出去等于把异常送到调用方的线程函数外——监视线程与重载线程都会 std::terminate。
        // 这里的收口方式是「本轮不跑，把活留给下一次事件」
        try
        {
            const std::lock_guard lock(m_reloadTasksMutex);

            // 启动线程之后不再允许分配：push_back 若在此时扩容失败，栈上局部 jthread 的析构
            // 会在持有 m_reloadTasksMutex 的情况下 join 一个可能已收尾并交接了下一轮的工作
            // 线程——那一轮正卡在这把锁上等登记，双方互等即死锁；catch 里的 finishRunning
            // 再抢一次执行权只是次要后果（破坏单轮串行）。reserve 把扩容排到线程启动之前，
            // 此时失败还没有线程，两条后果都不存在
            m_reloadTasks.reserve(m_reloadTasks.size() + 1U);
            auto        task    = std::make_unique<ReloadTask>();
            ReloadTask *rawTask = task.get();
            task->thread        = std::jthread([this, rawTask]() { runReloadTask(rawTask); });
            m_reloadTasks.push_back(std::move(task));
            return;
        } catch (const std::exception &startError)
        {
            LOG_ERROR_EXCEPTION(startError, "ConfigManager: 启动热重载任务失败，本轮重载让给下一次事件：{}", startError.what());
        } catch (...)
        {
            LOG_ERROR_FMT("ConfigManager: 启动热重载任务失败（非标准异常），本轮重载让给下一次事件");
        }

        // 走到这里说明扩容、建任务节点或起线程中有一步失败，此时还没有工作线程开始跑：
        // 刚抢到的执行权由这里交还，本轮就此作罢，等下一次事件重新起轮；起得来的那一轮
        // 由 runReloadTask 收尾时自己 releaseRound，两条路不会重复归还同一轮。
        // 这里刻意不做接力（releaseRound 会在有新变更时再起一轮）——线程本来就起不来，
        // 接力只会在这条失败路径上原地递归
        m_reloadGate.finishRunning();
    }

    void ConfigManager::runReloadTask(ReloadTask *rawTask)
    {
        // 本轮开始前的世代快照：收尾时与当前值比对，不同就说明「本轮跑完之后或跑的过程中」
        // 又有变更落盘，那笔变更本轮没读到，必须再来一轮
        const std::uint64_t observedGeneration = m_reloadGate.observedGeneration();
        // 任务体的顶层兜底：doReload() 与用户回调都可能抛（解析失败、类型不符、用户代码）。
        // 让异常逃出线程函数就是 std::terminate 把整个进程带走——这里收口成一次「失败的重载」，
        // 并照常通知回调，免得调用方以为配置已经刷新
        try
        {
            if (!m_hotReloadEnabled.load(std::memory_order_acquire))
            {
                // 热重载已关闭：本轮不跑，收尾在下面统一做
            } else
            {
                const ConfigLoadResult result = doReload();

                // 回调快照：本线程（重载工作线程）读到的是 enableHotReload 发布的那一份
                if (const auto callback = m_hotReloadCallback.load(std::memory_order_acquire); callback && *callback)
                {
                    (*callback)(result);
                } else if (!result.success)
                {
                    // 没挂回调时这份结果无处可去，失败就整轮静默：某个文件解析不了会让它的全部键
                    // 从新快照里缺席，而热重载开关照旧是开的，现场只看得到「改了配置没反应」。
                    // 逐条按错误原文报出来——这些文案已经带上文件名与行列
                    for (const std::string &error: result.errors)
                    {
                        LOG_ERROR_FMT("ConfigManager: 热重载本轮失败，且没有回调可通知：{}", error);
                    }
                }
            }
        } catch (const std::exception &reloadError)
        {
            LOG_ERROR_EXCEPTION(reloadError, "ConfigManager: 热重载任务抛出异常，本轮按失败处理：{}", reloadError.what());
            // 再通知回调一次「本轮失败」：不通知的话调用方会以为配置已经刷新。
            // 这一次通知同样可能抛（抛的那个回调就是它），因此单独兜一层——再逃出去还是 terminate
            try
            {
                if (const auto callback = m_hotReloadCallback.load(std::memory_order_acquire); callback && *callback)
                {
                    ConfigLoadResult failedResult;
                    failedResult.success = false;
                    failedResult.errors.push_back(std::string("热重载任务抛出异常：") + reloadError.what());
                    (*callback)(failedResult);
                }
            } catch (const std::exception &notificationError)
            {
                LOG_ERROR_EXCEPTION(notificationError, "ConfigManager: 通知「本轮重载失败」时回调又抛出异常，已忽略：{}", notificationError.what());
            }
        } catch (...)
        {
            LOG_ERROR_FMT("ConfigManager: 热重载任务抛出非标准异常，本轮按失败处理");
        }

        // 接力：先交还执行权，再比对自己那份世代快照。顺序不能反——反了会让「交还之后、
        // 比对之前」到达的变更没人接力。比对而不是清标记，是因为收尾可以并发：一轮交还执行权
        // 之后它的收尾代码还在跑，另一轮已经起好并跑完，此时若共用一面脏标记，停在中间那一格
        // 的旧轮次会把别人记下的欠账吃掉，而它自己又抢不到接力权——那笔变更再没人重读
        if (m_reloadGate.releaseRound(observedGeneration))
        {
            startReloadTask();
        }
        rawTask->finished.store(true, std::memory_order_release);
    }

    void ConfigManager::collectFinishedReloadTasks()
    {
        std::vector<std::unique_ptr<ReloadTask>> finishedTasks;
        {
            const std::lock_guard lock(m_reloadTasksMutex);
            for (auto iterator = m_reloadTasks.begin(); iterator != m_reloadTasks.end();)
            {
                if ((*iterator)->finished.load(std::memory_order_acquire))
                {
                    // 锁内只摘取句柄：unique_ptr 移出后立即析构会在锁外 join
                    finishedTasks.push_back(std::move(*iterator));
                    iterator = m_reloadTasks.erase(iterator);
                } else
                {
                    ++iterator;
                }
            }
        }
        // 析构已结束任务的 jthread（此时线程必然已退出，join 立即返回）
        finishedTasks.clear();
    }

    ConfigLoadResult ConfigManager::doReload()
    {
        std::unique_lock lock(m_reloadMutex);

        const auto currentData = m_data.load(std::memory_order_acquire);
        if (currentData->configDirectory.empty())
        {
            ConfigLoadResult result;
            result.success = false;
            result.errors.emplace_back("尚未设置配置目录，请先调用 loadFromDirectory 或 loadFiles。");
            return result;
        }

        // 递归口径跟着快照走：调用方当初以 loadFromDirectory(dir, false) 建起来的配置，
        // 若在 reload() 时被强行改成递归，就会凭空多出子目录里的键——没人改过文件却换了配置
        return loadFromDirectoryImplementation(currentData->configDirectory, currentData->configDirectoryRecursive);
    }
} // namespace AsynGyanis::Base

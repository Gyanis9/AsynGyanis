#include "Core/Socket/ConnectionManager.h"
#include "Core/Socket/Connection.h"

#include "Base/Log/LogMacros.h"

#include <chrono>
#include <exception>
#include <ranges>
#include <vector>


namespace AsynGyanis::Core
{
    void ConnectionManager::add(const std::shared_ptr<Connection> &connection)
    {
        if (!connection)
        {
            return;
        }
        {
            std::unique_lock lock(m_mutex);
            // 只有真的插入才同步镜像：同一连接重复 add 时 size() 不变，镜像也不能多算
            if (m_connections.emplace(connection.get(), connection).second && m_sharedActiveCountMirror != nullptr)
            {
                m_sharedActiveCountMirror->fetch_add(1, std::memory_order_relaxed);
            }
        }

        // 关闭已开始后才挂上来的连接必须立刻收尾。shutdown() 遍历的是它调用那一刻的快照，
        // 而接受新连接与本函数可能并发；漏掉这一条，它会永远留在活跃表里，
        // 服务器等它退出就会一直等不到（等待全部连接结束的收尾阶段将挂住）
        if (m_isShuttingDown.load(std::memory_order_acquire))
        {
            [[maybe_unused]] auto _ = connection->cancelable().requestStop();
            connection->close();
        }
    }

    void ConnectionManager::remove(const Connection *const connection)
    {
        if (!connection)
        {
            return;
        }

        std::unique_lock lock(m_mutex);
        if (m_connections.erase(connection))
        {
            // 与 add() 对称：真的摘掉了一条才回退镜像，重复 remove 不会把合并计数减穿
            if (m_sharedActiveCountMirror != nullptr)
            {
                m_sharedActiveCountMirror->fetch_sub(1, std::memory_order_relaxed);
            }
            m_condition.notify_all();
        }
    }

    void ConnectionManager::setSharedActiveCountMirror(std::atomic<std::uint64_t> *const counter) noexcept
    {
        // 与增删同一把写锁：镜像指针的读写不会被另一线程正在进行的加减夹在中间，
        // 因此换目标时既不会丢一次计数也不会多算一次
        std::unique_lock lock(m_mutex);
        // 换目标时把「本管理器此刻在册几条」从旧镜像搬到新镜像。只换指针是不够的：
        // HttpsServer::setMetricsCollector() 这类公开入口允许运行期重接镜像，那时手上这些连接
        // 在新镜像里一笔都没记过，而它们将来各要 fetch_sub 一次——无符号的合并计数因此回绕成
        // 1.8e19 量级的一格，而 /metrics 上读的正是它。摘掉镜像（传 nullptr）同样要退回去，
        // 否则进程总量会一直虚高着这些还在跑的连接。同一个目标重复接上 = 减一次再加一次，净为零
        if (m_sharedActiveCountMirror != nullptr)
        {
            m_sharedActiveCountMirror->fetch_sub(m_connections.size(), std::memory_order_relaxed);
        }
        if (counter != nullptr)
        {
            counter->fetch_add(m_connections.size(), std::memory_order_relaxed);
        }
        m_sharedActiveCountMirror = counter;
    }

    size_t ConnectionManager::activeCount() const
    {
        std::shared_lock lock(m_mutex);
        return m_connections.size();
    }

    std::vector<std::shared_ptr<Connection>> ConnectionManager::snapshot() const
    {
        std::shared_lock lock(m_mutex);

        // 只做指针拷贝：让调用方拿到一份不会被后续增删改动的列表，遍历期间也由 shared_ptr
        // 保证连接对象存活。锁在同一函数末尾释放，调用方遍历时本类不持锁
        std::vector<std::shared_ptr<Connection>> connections;
        connections.reserve(m_connections.size());
        for (const auto &connection: m_connections | std::views::values)
        {
            connections.push_back(connection);
        }
        return connections;
    }

    void ConnectionManager::shutdown()
    {
        // 标志必须先于快照置位：否则「取完快照、还没置位」这一小段里 add() 的新连接
        // 既不在快照中，也不会被 add() 就地收尾，成了漏网的一条
        m_isShuttingDown.store(true, std::memory_order_release);

        // 快照的取法与遍历语义都在 snapshot() 里，本函数只负责在锁外逐条收尾
        const std::vector<std::shared_ptr<Connection>> connections = snapshot();

        // 锁外调用 close()，防止回调中的 remove() 死锁
        for (const auto &connection: connections)
        {
            if (connection)
            {
                [[maybe_unused]] auto _ = connection->cancelable().requestStop();
                // 逐条兜住：close() 是可重写的（HTTP/2 会话在那儿补最后一张收口通告），
                // 一条会话抛出就把剩下的连接全丢下不收口，等于让这次优雅关闭变成
                // 「只关到第 N 条」，而随后的 waitAll() 会为剩下的那些一直等到超时；
                // 异常本身也不是这条路径要向上交的东西——调用方拿到的是「已尽力收口」
                try
                {
                    connection->close();
                } catch (const std::exception &failure)
                {
                    LOG_ERROR_FMT("ConnectionManager: 收口一条连接时抛出异常，本条按已停止处理并继续收口其余连接。原因：{}", failure.what());
                } catch (...)
                {
                    LOG_ERROR("ConnectionManager: 收口一条连接时抛出非 std::exception 的抛出物，本条按已停止处理并继续收口其余连接");
                }
            }
        }
    }

    void ConnectionManager::waitAll()
    {
        std::shared_lock lock(m_mutex);

        // 不在一条 wait 上无限期地干等：每两秒把「还剩几条」写进日志。
        // 服务停不下来时（某条连接没走到 remove()，例如它的协程没跑到 finally）
        // 至少能看到还剩多少、从而知道该去查哪条收尾路径，而不是面对一个没有任何线索的挂起
        constexpr std::chrono::seconds kProgressReportInterval{2};
        while (!m_connections.empty())
        {
            if (m_condition.wait_for(lock, kProgressReportInterval, [this]() { return m_connections.empty(); }))
            {
                break;
            }
            LOG_WARN_FMT("ConnectionManager: 等待全部连接结束已超过 {} 秒，仍有 {} 条在册；"
                         "若某条连接的收尾路径没有走到 remove()，这里会一直等下去",
                         kProgressReportInterval.count(), m_connections.size());
        }
    }

} // namespace AsynGyanis::Core

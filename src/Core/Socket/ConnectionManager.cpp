#include "Core/Socket/ConnectionManager.h"
#include "Core/Socket/Connection.h"

#include "Base/Log/LogMacros.h"

#include <chrono>
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
            m_connections.emplace(connection.get(), connection);
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
            m_condition.notify_all();
        }
    }

    size_t ConnectionManager::activeCount() const
    {
        std::shared_lock lock(m_mutex);
        return m_connections.size();
    }

    std::vector<std::shared_ptr<Connection> > ConnectionManager::snapshot() const
    {
        std::shared_lock lock(m_mutex);

        // 只做指针拷贝：让调用方拿到一份不会被后续增删改动的列表，遍历期间也由 shared_ptr
        // 保证连接对象存活。锁在同一函数末尾释放，调用方遍历时本类不持锁
        std::vector<std::shared_ptr<Connection> > connections;
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
        const std::vector<std::shared_ptr<Connection> > connections = snapshot();

        // 锁外调用 close()，防止回调中的 remove() 死锁
        for (const auto &connection: connections)
        {
            if (connection)
            {
                [[maybe_unused]] auto _ = connection->cancelable().requestStop();
                connection->close();
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

}

/**
 * @file ConnectionManager.h
 * @brief 全局连接跟踪器，支持优雅关闭
 * @author Gyanis
 * @date 2026-09-11
 * @version 1.0.0
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

namespace AsynGyanis::Core
{
    class Connection;

    /**
     * @brief 管理所有活跃的连接对象，提供线程安全添加/删除及优雅关闭功能。
     *
     * 该类使用 shared_mutex 保护内部连接集合，支持并发读取和独占写入。
     * 在关闭时可以主动通知所有连接停止，并等待所有连接退出。
     */
    class ConnectionManager
    {
    public:
        /**
         * @brief 默认构造函数，创建一个空连接管理器。
         */
        ConnectionManager() = default;

        /**
         * @brief 添加一个连接到管理器。
         * @param connection 要添加的连接智能指针（通常为 shared_ptr）
         * @note 若 shutdown() 已经开始，本连接会立刻被 close() 收尾：shutdown 只能遍历它
         *       调用那一刻的快照，晚到的连接必须自行收尾，否则会永远留在活跃表里
         */
        void add(const std::shared_ptr<Connection> &connection);

        /**
         * @brief 从管理器中移除指定的连接。
         * @param connection 连接对象的原始指针，若存在则移除。
         */
        void remove(const Connection *connection);

        /**
         * @brief 获取当前活跃连接的数量。
         * @return 连接数量
         */
        [[nodiscard]] size_t activeCount() const;

        /**
         * @brief 关闭所有连接。
         *
         * 遍历所有连接，调用其关闭接口（如 close()），并请求取消（requestStop）。
         * 通常用于服务停止时主动清理所有连接。
         * @note 本方法置位「关闭已开始」标志，此后经 add() 挂上来的连接会被立即收尾，
         *       因此「接受新连接」与「关闭全部连接」并发时不会有连接被漏掉。
         */
        void shutdown();

        /**
         * @brief 等待所有连接完全退出。
         *
         * 阻塞直到 m_connections 变为空。通常配合 shutdown() 使用，
         * 确保关闭后所有连接都已析构或释放。
         */
        void waitAll();

    private:
        mutable std::shared_mutex                                            m_mutex;       ///< 保护 m_connections 的读写锁
        std::unordered_map<const Connection *, std::shared_ptr<Connection> > m_connections; ///< 存储所有活跃连接的集合
        std::condition_variable_any                                          m_condition;   ///< 用于 waitAll 的条件变量，连接移除时通知
        std::atomic<bool>                                                    m_isShuttingDown{false}; ///< shutdown() 是否已经开始，供 add() 判断是否需要就地收尾
    };

} // namespace AsynGyanis::Core

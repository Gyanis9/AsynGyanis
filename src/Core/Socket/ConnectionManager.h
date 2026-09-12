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
#include <vector>

namespace AsynGyanis::Core
{
    class Connection;

    /**
     * @brief 管理所有活跃的连接对象，提供添加/删除与优雅关闭功能。
     *
     * 内部连接集合由 shared_mutex 保护，因此**集合本身的**增删与遍历可以跨线程并发进行。
     *
     * @warning 「集合线程安全」不等于「关闭连接线程安全」：shutdown() 会在**调用者线程**上
     *          逐个执行 connection->close()，而该连接的读写线程可能正在同一个底层套接字上
     *          收发数据（AsyncSocket 的文件描述符是普通 int，不是原子量）。
     *          因此从外部线程发起关闭时，约定是由连接所属的事件循环线程来收尾：
     *          跨线程场景请把关闭动作投递到那个循环（EventLoop::scheduler().scheduleRemote()），
     *          不要直接调用本类的 shutdown()。
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
         * @brief 复制一份当前活跃连接的指针快照，供调用方在锁外遍历与操作。
         *
         * @details 有意只返回快照而不提供「持锁回调」形式：close() 的收尾路径会反过来调用
         *          remove()，在锁内执行会重入死锁。快照里的 shared_ptr 使被关闭的连接对象
         *          在调用方遍历期间始终存活。
         *
         * @return std::vector<std::shared_ptr<Connection> > 取快照那一刻的活跃连接列表
         */
        [[nodiscard]] std::vector<std::shared_ptr<Connection> > snapshot() const;

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

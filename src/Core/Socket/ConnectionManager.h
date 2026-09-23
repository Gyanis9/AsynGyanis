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
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace AsynGyanis::Core
{
    class Connection;

    /**
     * @brief 管理所有活跃的连接对象，提供添加/删除与优雅关闭功能
     * @warning 集合本身由 shared_mutex 保护（增删与遍历可跨线程并发），但这不等于「关闭连接
     *          线程安全」：shutdown() 在**调用者线程**上逐个执行 connection->close()，而该连接的
     *          读写线程可能正在同一个底层套接字上收发数据（AsyncSocket 的文件描述符是普通 int，
     *          不是原子量）。因此跨线程关闭必须把动作投递到连接所属的事件循环
     *          （EventLoop::scheduler().scheduleRemote()），不要直接调用 shutdown()
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
         * @brief 指定一个「多份管理器合并计数」的镜像目标
         *
         * @details 同一端口常由多台服务器共同监听（每线程一个 TcpServer），各自的本表只看得见
         *          自己那一份；把它们指向同一个原子量，运维侧才能读到进程总量。
         * @param counter 镜像目标，nullptr 表示不镜像（默认）。写入方是本管理器，与增删在同一
         *        临界区内完成，因此镜像与本表不会彼此漂移
         * @note 必须在接受第一条连接之前设定；镜像对象的生存期要覆盖本管理器
         */
        void setSharedActiveCountMirror(std::atomic<std::uint64_t> *counter) noexcept;

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
         * @brief 关闭所有连接
         * @note 逐个调用 close() 并请求取消；置位「关闭已开始」标志后，经 add() 挂上来的连接
         *       会被立即收尾，因此「接受新连接」与「关闭全部连接」并发时不会有连接被漏掉
         */
        void shutdown();

        /**
         * @brief 等待所有连接完全退出（阻塞到活跃连接表变空）
         */
        void waitAll();

    private:
        mutable std::shared_mutex                                            m_mutex;       ///< 保护 m_connections 的读写锁
        std::unordered_map<const Connection *, std::shared_ptr<Connection> > m_connections; ///< 存储所有活跃连接的集合
        std::condition_variable_any                                          m_condition;   ///< 用于 waitAll 的条件变量，连接移除时通知
        std::atomic<bool>                                                    m_isShuttingDown{false}; ///< shutdown() 是否已经开始，供 add() 判断是否需要就地收尾
        std::atomic<std::uint64_t> *                                         m_sharedActiveCountMirror{nullptr}; ///< 跨管理器合并计数的镜像目标，受 m_mutex 保护；空表示不镜像
    };

} // namespace AsynGyanis::Core

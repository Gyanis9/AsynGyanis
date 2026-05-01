/**
 * @file TcpServer.h
 * @brief TCP 服务器：组合 TcpAcceptor 和 ConnectionManager
 * @copyright Copyright (c) 2026
 */
#ifndef NET_TCPSERVER_H
#define NET_TCPSERVER_H

#include "Core/Connection.h"
#include "Core/ConnectionManager.h"
#include "Core/Task.h"
#include "TcpAcceptor.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

namespace Net
{
    /**
     * @brief TCP 服务器类，管理监听器、连接生命周期和优雅关闭。
     *
     * 该类组合了一个 TcpAcceptor 用于接受新连接，一个 ConnectionManager 用于跟踪所有活跃连接。
     * 通过 start() 协程启动服务器循环，每次 accept 后创建 Connection 对象（可被子类重写），
     * 并启动该连接的协程任务。支持停止服务器并等待所有连接退出。
     */
    class TcpServer
    {
    public:
        /**
         * @brief 构造 TCP 服务器。
         * @param loop 事件循环，用于 I/O 和协程调度
         * @param addr 监听的本地地址（IP 和端口）
         */
        TcpServer(Core::EventLoop &loop, const Core::InetAddress &addr);

        TcpServer(const TcpServer &)            = delete;
        TcpServer &operator=(const TcpServer &) = delete;

        /**
         * @brief 虚析构函数，默认实现。
         */
        virtual ~TcpServer() = default;

        /**
         * @brief 启动服务器主协程。
         * @return Core::Task<> 协程，无限循环接受连接并创建处理任务
         *
         * 该协程会持续运行直到服务器停止（调用 stop()）。每次成功 accept 后，
         * 调用 createConnection() 创建连接对象，启动 handleConnection 协程，
         * 并将任务句柄存入 m_connTasks 以保证任务生命周期。
         */
        Core::Task<> start();

        /**
         * @brief 停止服务器，关闭监听器并通知所有连接关闭。
         */
        void stop();

        /**
         * @brief 立即关闭监听器（不停止已有连接）。
         */
        void close();

        /**
         * @brief 设置最大并发连接数（0 表示无限制）。
         * @param max 最大连接数
         */
        void setMaxConnections(size_t max);

        /**
         * @brief 检查服务器是否正在运行（accept 循环是否活跃）。
         * @return true 表示运行中，false 表示已停止或未启动
         */
        bool isRunning() const;

        /**
         * @brief 创建连接对象，虚函数供子类重写（如 HttpServer 创建 HttpSession）。
         * @param socket 已接受的异步套接字
         * @return 连接对象的 shared_ptr，默认返回基类 Connection
         */
        virtual std::shared_ptr<Core::Connection> createConnection(Core::AsyncSocket socket);

    protected:
        Core::EventLoop &       m_loop;        ///< 事件循环引用
        TcpAcceptor             m_acceptor;    ///< 监听器，用于 accept 新连接
        Core::ConnectionManager m_connManager; ///< 连接管理器，跟踪所有活跃连接

    private:
        /**
         * @brief 处理单个连接的主协程。
         * @param conn 连接对象
         * @return Core::Task<> 协程，内部调用 conn->start()，并在完成后从连接管理器中移除
         *
         * 该任务在服务器接受连接后被启动，等待连接执行完毕（或异常退出），
         * 最后调用 m_connManager.remove() 并递减任务计数。
         */
        Core::Task<> handleConnection(std::shared_ptr<Core::Connection> conn);

        std::atomic<bool>             m_running{false};          ///< 运行标志，控制 accept 循环（原子操作保证线程安全）
        size_t                        m_maxConnections{0};       ///< 最大并发连接数，0 表示无限制
        std::chrono::milliseconds     m_shutdownTimeoutMs{5000}; ///< 优雅关闭超时（毫秒），超时后强制退出
        std::vector<Core::Task<void>> m_connTasks;               ///< 存储每个连接对应的协程任务，确保任务生命周期
    };
}

#endif

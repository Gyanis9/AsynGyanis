/**
 * @file TcpServer.h
 * @brief TCP 服务器基类：组合 TcpAcceptor 与 ConnectionManager 的接受循环
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/Socket/Connection.h"
#include "Core/Socket/ConnectionManager.h"
#include "Net/Tcp/TcpAcceptor.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace AsynGyanis::Net
{
    /**
     * @brief TCP 服务器基类，负责监听、连接生命周期与优雅关闭。
     *
     * @details 组合一个 TcpAcceptor 用于接受新连接、一个 ConnectionManager 用于跟踪活跃连接。
     *          start() 是服务器主协程：接受连接后经 createConnection() 交给子类构造具体协议
     *          会话，再把会话的协程任务挂到调度器上并发运行。stop()/close() 只置位与关闭
     *          描述符，真正的收尾发生在 start() 退出前：等全部连接任务结束再回收。
     * @note 本类是抽象基类，必须重写 createConnection() 才能装载协议逻辑（见 HttpServer）。
     * @warning start() 与 stop() 必须在同一事件循环线程上调用；m_running 虽是原子量，
     *          但连接容器与协程调度都不做跨线程保护。
     */
    class TcpServer
    {
    public:
        /**
         * @brief 构造 TCP 服务器
         * @param loop 事件循环，用于 I/O 监控与协程调度
         * @param address 监听的本地地址（IP 与端口）
         * @throws Base::SystemException 监听器创建监听套接字失败
         * @note 事件循环必须比服务器活得久：监听套接字与服务器上的定时等待都会用到它
         */
        TcpServer(Core::EventLoop &loop, const Core::InetAddress &address);

        TcpServer(const TcpServer &) = delete;
        TcpServer &operator=(const TcpServer &) = delete;
        TcpServer(TcpServer &&) = delete;
        TcpServer &operator=(TcpServer &&) = delete;

        /**
         * @brief 虚析构，按成员声明顺序回收监听器与连接管理器
         */
        virtual ~TcpServer() = default;

        /**
         * @brief 启动服务器主协程
         * @details 依次 bind() 与 listen(默认队列深度 kDefaultListenBacklog)，随后循环接受连接：
         *          达到 m_maxConnections 上限时直接丢弃新连接（由 AsyncSocket 析构关闭），
         *          否则交给 createConnection() 并并发启动 handleConnection()。
         *          可恢复的接受错误已由 TcpAcceptor::accept() 内部退避重试，本协程只处理
         *          终止性错误：记录日志后退出循环。退出前等待所有连接任务结束。
         * @return Core::Task<> 协程，直到服务器停止才完成
         * @throws Base::Exception 绑定或监听失败
         * @throws Base::SystemException accept 出现终止性错误时由 TcpAcceptor 抛出并在此传播
         */
        Core::Task<> start();

        /**
         * @brief 停止接受新连接
         * @details 置位运行标志并关闭监听器；已建立的连接不受影响，继续跑到自然结束。
         * @warning **必须由运行本服务器事件循环的那个线程调用**：本方法关闭的监听描述符
         *          正被该循环上的 accept 协程使用（`AsyncSocket` 的文件描述符是普通 int，
         *          不是原子量），从别的线程调用会与它竞争同一个句柄。
         *          需要从外部线程发起停止时，请把动作投递到那个循环
         *          （`EventLoop::scheduler().scheduleRemote()`），不要在外部线程直接调用。
         */
        void stop();

        /**
         * @brief 立即关闭服务器：停止接受并强制关闭全部已有连接
         * @details 在 stop() 之上追加 ConnectionManager::shutdown()，用于需要立刻释放资源的场合。
         * @warning 线程约束同 stop()，而且更强：shutdown() 会逐个关闭**每条活跃连接**的套接字，
         *          那些 Socket 正被各自的事件循环读写。同样只在拥有这些连接的线程上调用。
         */
        void close();

        /**
         * @brief 设置最大并发连接数
         * @param maximumConnectionCount 允许同时存活的连接条数，0 表示不做限制
         * @note 必须在 start() 之前调用；循环期间修改虽能被读到，但已排队的连接不受新上限约束
         */
        void setMaxConnections(std::size_t maximumConnectionCount);

        /**
         * @brief 查询服务器是否处于接受循环中
         * @return true start() 的循环活跃
         * @return false 已停止或尚未启动
         */
        [[nodiscard]] bool isRunning() const;

        /**
         * @brief 为一个新连接创建协议会话对象
         * @details 纯虚钩子：基类不知道应当用哪种 Connection，必须由子类给出。
         *          套接字按值传入（调用方已 std::move，所有权就此转移），实现要么
         *          std::move(socket) 接管描述符，要么让形参析构直接关闭这条连接。
         * @param socket 已 accept 且已设置非阻塞与 TCP_NODELAY 的套接字
         * @return std::shared_ptr<Core::Connection> 会话对象；返回 nullptr 会被基类记录为
         *         子类缺陷并丢弃该连接
         * @throws std::exception 实现可抛异常表示会话建立失败（如 TLS 对象申请失败）：
         *         基类记录中文错误后只丢弃这一条连接，不会终止接受循环
         * @note 在事件循环线程上被同步调用，实现内不得做阻塞操作
         */
        [[nodiscard]] virtual std::shared_ptr<Core::Connection> createConnection(Core::AsyncSocket socket) = 0;

    protected:
        Core::EventLoop &           m_loop;              ///< 事件循环引用，用于调度连接协程
        TcpAcceptor                 m_acceptor;          ///< 监听器，接受新连接并吸收可恢复错误
        Core::ConnectionManager     m_connectionManager; ///< 连接管理器，跟踪并负责关闭所有活跃连接

    private:
        /**
         * @brief 处理单个连接的协程主体
         * @details 等待 connection->start() 结束；任何异常都在此吞掉，避免传播到调度器终止进程，
         *          最后把自己从连接管理器摘除。
         * @param connection 待处理的会话对象，与协程共同持有所有权
         * @return Core::Task<> 协程，会话结束后完成
         */
        Core::Task<> handleConnection(std::shared_ptr<Core::Connection> connection);

        std::atomic<bool>             m_running{false}; ///< 运行标志，控制 accept 循环（原子量以便跨线程 stop() 可见）
        std::size_t                   m_maxConnections{0}; ///< 最大并发连接数，0 表示无限制
        std::vector<Core::Task<void>> m_connectionTasks;   ///< 已启动的连接协程，持有其生命周期防止提前销毁
    };
} // namespace AsynGyanis::Net

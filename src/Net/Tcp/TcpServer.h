/**
 * @file TcpServer.h
 * @brief TCP 服务器基类：组合 TcpAcceptor 与 ConnectionManager 的接受循环
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/Coroutine/Task.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/Connection.h"
#include "Core/Socket/ConnectionManager.h"
#include "Net/Tcp/TcpAcceptor.h"

#include <atomic>
#include <chrono>
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
     *          会话、把会话协程挂上调度器并发运行，并另投一个空闲清扫协程按节拍关闭超期连接。
     *          stop()/close() 只置位与关闭描述符，真正的收尾发生在 start() 退出前：等任务结束再回收。
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
         * @brief 优雅关闭：停止接受新连接，等在途请求做完，到期兜底强关
         * @details 与 close() 的区别就在「等」：先 stop()，再按小间隔轮询连接管理器——没有在途工作
         *          的连接立刻收掉，有在途请求的连接则留出把响应发完的时间；只有期限到了才用
         *          ConnectionManager::shutdown() 强关剩余连接。等待期间在途请求仍被正常服务，
         *          定时等待挂在事件循环上，因此本协程不会把循环阻塞住。
         * @param drainTimeout 最长等待时长；非正数表示不等待，直接强关全部连接（等价于 close()）
         * @return Core::Task<> 协程，连接已清空或期限到时完成
         * @note 线程约束同 stop()：必须由运行本服务器事件循环的那个线程调用。本协程要遍历并关闭
         *       各连接的套接字，还要挂定时等待，因此只能作为协程投递到那个循环
         *       （EventLoop::scheduler().scheduleRemote()）后运行，不要在外部线程同步调用
         * @see close(), stop()
         */
        Core::Task<> drain(std::chrono::milliseconds drainTimeout);

        /**
         * @brief 设置最大并发连接数
         * @param maximumConnectionCount 允许同时存活的连接条数，0 表示不做限制
         * @note 必须在 start() 之前调用；循环期间修改虽能被读到，但已排队的连接不受新上限约束
         */
        void setMaxConnections(std::size_t maximumConnectionCount);

        /**
         * @brief 设置空闲清扫节拍。
         *
         * @details 清扫协程按本间隔醒来，扫描连接管理器并把超过空闲截止时间（由会话自己刷新，
         *          见 Core::Connection::refreshIdleDeadline()）的连接关掉。
         *          最坏超时误差 = 本间隔 + 各连接自己的超时值：到期连接最多晚一个节拍被发现。
         *
         * @param interval 两次清扫之间的间隔；非正数表示关闭清扫（连接级超时随之失效）
         * @note 必须在 start() 之前调用：清扫协程在 start() 时按当时的取值投递，
         *       非正数时它根本不会被创建，之后再改这个值不会有清扫发生
         */
        void setIdleCheckInterval(std::chrono::milliseconds interval);

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
        Core::EventLoop &       m_loop;              ///< 事件循环引用，用于调度连接协程
        TcpAcceptor             m_acceptor;          ///< 监听器，接受新连接并吸收可恢复错误
        Core::ConnectionManager m_connectionManager; ///< 连接管理器，跟踪并负责关闭所有活跃连接

    private:
        /**
         * @brief 处理单个连接的协程主体
         * @details 等待 connection->start() 结束；任何异常都在此吞掉，避免传播到调度器终止进程，
         *          最后把自己从连接管理器摘除。
         * @param connection 待处理的会话对象，与协程共同持有所有权
         * @return Core::Task<> 协程，会话结束后完成
         */
        Core::Task<> handleConnection(std::shared_ptr<Core::Connection> connection);

        /**
         * @brief 空闲清扫协程：按固定节拍关闭超过空闲截止时间的连接
         * @details 异常绝不外抛（逃逸到调度器等于在事件循环线程上抛异常），单轮失败只丢一轮。
         *          没有连接超过截止时间时它什么都不做，因此空闲服务器上的代价只是一次定时唤醒。
         * @return Core::Task<> 协程，服务器停止后完成
         */
        Core::Task<> idleSweepLoop();

        /// 空闲清扫的默认节拍（毫秒）：够密以免超时被成倍放大，又不会让空闲服务器频繁空转
        static constexpr std::chrono::milliseconds kDefaultIdleCheckInterval{250};

        /// drain 的轮询间隔（毫秒）：决定它多久复查一次「连接是否已清空」，间隔越小收手越及时，
        /// 代价是等待期间在事件循环上多几次空转唤醒
        static constexpr std::chrono::milliseconds kDrainPollInterval{50};

        std::atomic<bool>              m_running{false};    ///< 运行标志，控制 accept 循环（原子量以便跨线程 stop() 可见）
        std::size_t                    m_maxConnections{0}; ///< 最大并发连接数，0 表示无限制
        std::chrono::milliseconds      m_idleCheckInterval{kDefaultIdleCheckInterval}; ///< 空闲清扫节拍，非正数表示关闭清扫
        Core::Timer                    m_idleTimer;         ///< 清扫协程与 drain 共用的节拍器；waitFor 每次返回独立等待器，两处并发等待互不干扰
        Core::Task<>                   m_idleSweepTask{nullptr}; ///< 清扫协程任务；空句柄表示本服务器没有清扫（见 setter 的说明）
        std::vector<Core::Task<void> > m_connectionTasks;   ///< 已启动的连接协程，持有其生命周期防止提前销毁
    };
} // namespace AsynGyanis::Net

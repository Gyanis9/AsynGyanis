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
#include "Core/EventLoop/ConnectionDistributor.h"
#include "Core/EventLoop/Timer.h"
#include "Core/Socket/Connection.h"
#include "Core/Socket/ConnectionManager.h"
#include "Net/Tcp/PerIpConnectionLimiter.h"
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

        /**
         * @brief 用「已经在监听中的套接字」构造服务器：不 bind、不 listen，直接开始接受连接
         *
         * @details 零停机重启的第二半：上一代进程把监听套接字交出来（或由 supervisor 持有），
         *          新一代接手它继续服务，端口全程不关，因此不存在「新进程还没起来、旧进程已经不接」的
         *          空窗。`start()` / `startAccepting()` 与按地址构造时完全一样。
         * @param loop 事件循环，要求与按地址构造时相同
         * @param adoptedListeningDescriptor 已经在监听状态的套接字描述符，所有权随之转移
         *        （服务器收口时会关掉它）
         * @throws Base::InvalidArgumentException 描述符无效
         * @see TcpAcceptor::TcpAcceptor(Core::EventLoop &, int)
         */
        TcpServer(Core::EventLoop &loop, int adoptedListeningDescriptor);

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
         * @details 依次 bind() 与 listen(kDefaultListenBacklog)，随后循环接受连接：达到 m_maxConnections 上限时
         *          丢弃新连接（由 AsyncSocket 析构关闭），否则交给 createConnection() 并并发启动
         *          handleConnection()。可恢复的接受错误已在 TcpAcceptor 内退避重试，本协程只处理终止性错误并
         *          退出循环，退出前等所有连接任务结束。
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
         * @brief 只接受、不在本循环建连接的接受循环：每条连接交给分发器指定的工作循环
         *
         * @details 解决 Windows 没有 SO_REUSEPORT 时的多核扩展：本循环只做 accept 与派发，
         *          连接对象与生命周期都落在工作循环上。与 start() 共用同一份接受循环实现，
         *          差别只在「拿到连接之后交给谁」。
         * @param distributor 接收分发器，必须已经登记了至少一个工作循环
         * @throws Base::Exception 绑定/监听失败，或分发器为空、没有任何工作循环
         *         （配置错误要当场拦住：跑起来才发现没人接手，连接会一条条被丢掉）
         * @note 与 start() 同样必须在服务器所属循环上启动；stop()/drain() 语义不变
         * @see adoptConnection(), Core::ConnectionDistributor
         */
        Core::Task<> startAccepting(std::shared_ptr<Core::ConnectionDistributor> distributor);

        /**
         * @brief 接手一条由别的循环接受的连接
         *
         * @details 工作循环侧的入口：由分发器在自己的循环上调用（见 ConnectionDistributor::Adopter）。
         *          过载与按来源 IP 的限额在这里判，与接受路径同一套逻辑——限额的判据是本服务器
         *          此刻的在途连接数，只有接手方最清楚。
         * @param fileDescriptor 已接受的连接描述符，本方法一进入就接管它的所有权
         * @return true 已接手并起服务
         * @return false 本服务器此刻不收（描述符无效、过载、该来源超限、取不到对端地址或子类钩子失败），
         *         描述符已关闭
         * @note 不抛：坏描述符只丢这一条连接，接受路径照常运行
         * @note 必须在本服务器所属循环上调用；连接建立后与 start() 接受的连接走完全相同的路径
         */
        bool adoptConnection(int fileDescriptor);

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
         * @brief 取本服务器实际在听的端口
         * @return std::uint16_t 端口号；从未成功进入监听状态则为 0（stop() 之后保留最后一次的端口，
         *          它表示「曾经听在哪」而不是「现在在听」）
         *
         * @details 构造时把端口写成 0 交给内核挑，此后只有内核知道实际端口——没有这个入口，
         *          调用方（和测试）只能靠派生类去读监听器再自己 getsockname，跨协议驱动时
         *          每条通道都得重抄一遍。QUIC 侧的 `QuicServer::listeningPort()` 同名同语义
         * @note 端口在绑定成功后**最后**才发布，因此非 0 就意味着监听套接字已经就绪；
         *       值由所属循环写入、别的线程可读，故用原子量（与 QuicServer 同一口径）
         * @see Core::TcpServer::start(), QuicServer::listeningPort()
         */
        [[nodiscard]] std::uint16_t listeningPort() const noexcept;

        /**
         * @brief 设置最大并发连接数
         * @param maximumConnectionCount 允许同时存活的连接条数，0 表示不做限制
         * @note 必须在 start() 之前调用；循环期间修改虽能被读到，但已排队的连接不受新上限约束
         */
        void setMaxConnections(std::size_t maximumConnectionCount);

        /**
         * @brief 设置按来源 IP 的并发连接限额
         * @param limiter 限额对象；**多个监听器（每循环一个）必须共享同一份**，否则单个来源的实际上限
         *        会乘上监听器数量，限额等于失效。传空指针表示不作按 IP 的限制（默认）
         * @note 必须在 start() 之前调用；检查时即时读取，与 setMaxConnections() 的语义一致
         * @see PerIpConnectionLimiter
         */
        void setPerIpConnectionLimiter(std::shared_ptr<PerIpConnectionLimiter> limiter);

        /**
         * @brief 按来源 IP 的准入闸门累计挡掉过多少条连接
         * @details 限额对象是可以被多台服务器共用的一份，因此这个数报的是**这道闸门**的总量而不是
         *          本实例那一份——与限额本身的口径一致。没设限额对象时返回 0。
         *          /metrics 里的 admission_rejected_connections_total 由它来（见 HttpServer::stats()）。
         * @return std::uint64_t 累计拒绝条数
         * @note 读的是限额里的原子量；设置限额仍然遵守「start() 之前」那条契约（与 setter 同一份
         *        shared_ptr，循环期间换它会撕裂）
         * @see PerIpConnectionLimiter::rejectedConnectionCount()
         */
        [[nodiscard]] std::uint64_t perIpRejectedConnectionCount() const noexcept;

        /**
         * @brief 要求每条新连接以一个 PROXY 协议头开头（负载均衡器交来的真实客户端身份）
         * @param required true 表示必须带头，没带头的连接当场收口；false（默认）不读任何头
         * @details 服务器坐在代理后面时，`getpeername` 只能看到代理：按来源 IP 的并发限额会把一整个
         *          LB 的流量记成一个来源，审计与封禁也就找错了人。开启后每条连接先读一条
         *          PROXY 协议头（v1 文本行或 v2 二进制块），读到的来源地址从此就是这条套接字的
         *          对端身份，限额键、`HttpRequest::remoteAddress()` 与日志都跟着改。
         * @note **只该在只有代理能连进来的端口上开**：头本身没有任何鉴权，对公网敞开就等于让每个
         *       客户端自己挑一个来源 IP 来占限额
         * @note 没带头、带头但不合规范、或在读头时限（固定 3 秒，见 kProxyProtocolHeaderTimeout）之内
         *       没把带头发完的连接都被收口，且各留一条 WARN：这类连接进不了会话，静默丢弃会让人排查半天
         * @note 头必须**单独成段**送到：若代理把「头 + 请求正文」挤进同一次发送，多出来的那一段没有
         *       地方安放（读掉的字节退不回内核缓冲，也塞不进会话的读缓冲），这条连接按上一条收口。
         *       主流代理都在建连时先把头单独写一次，因此实际不会撞上；撞上时日志会给出多出多少字节
         * @note 必须在 start() 之前调用：已建立的连接不会补读
         * @see ProxyProtocol.h
         */
        void setProxyProtocolRequired(bool required) noexcept;

        /**
         * @brief 设置监听与接受套接字的调参（缓冲区上限、Linux 的延迟接受）
         * @details 转发给内部 TcpAcceptor：缓冲区上限对监听套接字与每条接受到的连接都生效，
         *          延迟接受仅 Linux 支持（Windows 按「不支持」降级，不影响监听）。
         * @param tuning 调参项，见 TcpAcceptor::SocketTuning；各项 0 表示保持系统默认
         * @note 必须在 start()/startAccepting() 之前调用：绑定与监听发生在那一刻
         */
        void setSocketTuning(const TcpAcceptor::SocketTuning &tuning);

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
        /// 实际在听的端口，绑定成功后由所属循环写入；测试与运维会从别的线程读，所以是原子量
        std::atomic<std::uint16_t> m_listeningPort{0};
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
         * @brief 处理单个连接的协程主体，同时持有该连接占用的按 IP 名额
         * @details 名额凭据按值进入本协程帧；handleConnection() 一返回就显式归还，不等帧被回收——
         *          已结束的连接协程帧要等到下一条连接触发清扫或服务器收尾时才销毁，归还挂在帧上会让
         *          「占满自己名额后全部断开」的来源在当前连接数没到清扫阈值时连不进来。
         *          帧销毁时的析构因此是空操作，两条路径合计只归还一次。
         * @param connection 待处理的会话对象
         * @param lease 该连接占用的按 IP 名额；未配置限额时是空壳凭据
         * @return Core::Task<> 协程，与 handleConnection() 同时完成
         */
        Core::Task<> handleConnectionWithLease(std::shared_ptr<Core::Connection> connection,
                                               PerIpConnectionLimiter::Lease lease);

        /**
         * @brief 空闲清扫协程：按固定节拍关闭超过空闲截止时间的连接
         * @details 异常绝不外抛（逃逸到调度器等于在事件循环线程上抛异常），单轮失败只丢一轮。
         *          没有连接超过截止时间时它什么都不做，因此空闲服务器上的代价只是一次定时唤醒。
         * @return Core::Task<> 协程，服务器停止后完成
         */
        Core::Task<> idleSweepLoop();

        /**
         * @brief 接受循环的唯一实现：绑定、监听、接受，再按「交给谁」分两路
         * @param distributor 空指针表示在本循环建连接（一循环一监听器的经典形态）；
         *        非空表示只接受并把描述符交给它派出去
         * @return Core::Task<> 协程，停止并收尾完成后结束
         */
        Core::Task<> runAcceptLoop(std::shared_ptr<Core::ConnectionDistributor> distributor);

        /**
         * @brief 接受/接手一条连接后的共同收尾：限额判定、建会话、起服务协程、回收已完成的帧
         * @param socket 已建立的连接套接字，所有权转移
         * @return true 已接手并起服务协程
         * @return false 本服务器此刻不收（过载、该来源超限、取不到对端地址、子类钩子失败）；描述符已关闭
         */
        bool takeOverConnection(Core::AsyncSocket socket);

        /**
         * @brief 限额判定、建会话、起服务协程、回收已完成的帧——接手一条连接的共同后半段
         * @param socket 已建立的连接套接字（对端身份已就位：内核的 getpeername，或代理交来的真实来源）
         * @return true 已接手并起服务协程
         * @return false 本服务器此刻不收（该来源超限、取不到对端地址、子类钩子失败）；描述符已关闭
         */
        bool admitConnection(Core::AsyncSocket socket);

        /**
         * @brief 此刻算得进并发上限的连接数：已建成会话的加上正在读 PROXY 头的
         * @return std::size_t 两者之和
         */
        [[nodiscard]] std::size_t inFlightConnectionCount() const noexcept;

        /**
         * @brief 要求 PROXY 头时的接手前置：先读头再交给 admitConnection
         * @param socket 刚接受的连接
         * @details 为什么单独一路协程：读头要等网络，而接受循环不能为一条连接停在原地——否则一个
         *          不发头的对端就能把整台服务器的接受堵住（头队阻塞）。名额判定也随之挪到读完之后，
         *          这样按来源限额用的是真实来源而不是代理的地址
         */
        Core::Task<void> admitAfterProxyHeader(Core::AsyncSocket socket);

        /// 读完一条 PROXY 头的时限：对端把带头发完的合理上限，到点没发完就收掉这条连接
        static constexpr std::chrono::milliseconds kProxyProtocolHeaderTimeout{3000};

        /// 空闲清扫的默认节拍（毫秒）：够密以免超时被成倍放大，又不会让空闲服务器频繁空转
        static constexpr std::chrono::milliseconds kDefaultIdleCheckInterval{250};

        /// 已结束连接协程的清扫间隔，单位是「新连接条数」：每收一条就全表扫描是 O(n) 开销，
        /// 按这个步长摊销，最多多占这么多条已完成任务的帧，千级并发下可忽略
        static constexpr std::size_t kFinishedTaskCleanupStride = 64;

        /// drain 的轮询间隔（毫秒）：决定它多久复查一次「连接是否已清空」，间隔越小收手越及时，
        /// 代价是等待期间在事件循环上多几次空转唤醒
        static constexpr std::chrono::milliseconds kDrainPollInterval{50};

        std::atomic<bool>              m_running{false};    ///< 运行标志，控制 accept 循环（原子量以便跨线程 stop() 可见）
        std::size_t                    m_maxConnections{0}; ///< 最大并发连接数，0 表示无限制
        std::shared_ptr<PerIpConnectionLimiter> m_perIpConnectionLimiter; ///< 按来源 IP 的并发限额；空指针表示不作该限制
        bool                             m_proxyProtocolRequired{false};   ///< 是否要求每条新连接以 PROXY 协议头开头（见 setProxyProtocolRequired()）
        /// 正在读 PROXY 头的连接数：还没进连接表，但已占着描述符与缓冲，并发上限要把它们算进去
        std::size_t                      m_pendingProxyHeaders{0};
        std::chrono::milliseconds      m_idleCheckInterval{kDefaultIdleCheckInterval}; ///< 空闲清扫节拍，非正数表示关闭清扫
        Core::Timer                    m_idleTimer;         ///< 清扫协程与 drain 共用的节拍器；waitFor 每次返回独立等待器，两处并发等待互不干扰
        Core::Task<>                   m_idleSweepTask{nullptr}; ///< 清扫协程任务；空句柄表示本服务器没有清扫（见 setter 的说明）
        std::vector<Core::Task<void> > m_connectionTasks;   ///< 已启动的连接协程，持有其生命周期防止提前销毁
        /// 下一次回收已完成连接协程的触发条数：接受循环与接手路径共用，故提升为成员
        std::size_t m_nextTaskCleanupThreshold{kFinishedTaskCleanupStride};
    };
} // namespace AsynGyanis::Net

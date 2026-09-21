#include "Net/Tcp/TcpServer.h"

#include "Platform/IO/FileDescriptor.h"

#include "Base/Exception/Exception.h"
#include "Base/Exception/SystemException.h"
#include "Base/Log/LogMacros.h"
#include "Core/EventLoop/EventLoop.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace AsynGyanis::Net
{
    namespace
    {
    } // namespace

    TcpServer::TcpServer(Core::EventLoop &loop, const Core::InetAddress &address) :
        m_loop(loop), m_acceptor(loop, address), m_idleTimer(loop)
    {
        // 监听器与连接管理器都按引用持有同一个循环：连接的接受与处理必须在同一线程上串行，
        // 这也是本类不做任何容器加锁的前提
    }

    TcpServer::TcpServer(Core::EventLoop &loop, const int adoptedListeningDescriptor) :
        m_loop(loop), m_acceptor(loop, adoptedListeningDescriptor), m_idleTimer(loop)
    {
        // 监听器接手了外部交来的套接字，其余与按地址构造时一致
    }

    Core::Task<> TcpServer::start()
    {
        co_await runAcceptLoop(nullptr);
    }

    Core::Task<> TcpServer::startAccepting(std::shared_ptr<Core::ConnectionDistributor> distributor)
    {
        // 配置错误当场拦住：没有工作循环的接受循环会把每一条连接都接进来又丢掉，
        // 跑起来才发现的话，症状是「服务在监听但没人能连上」，很难查
        if (distributor == nullptr || distributor->workerCount() == 0)
        {
            throw Base::Exception("TcpServer: 接受分发需要至少一个已登记的工作循环，请先给分发器 addWorker()");
        }

        co_await runAcceptLoop(std::move(distributor));
    }

    Core::Task<> TcpServer::runAcceptLoop(std::shared_ptr<Core::ConnectionDistributor> distributor)
    {
        // 面向使用者的错误文本带上监听地址，便于从日志直接定位端口冲突
        if (!m_acceptor.bind())
        {
            throw Base::Exception("TcpServer: 绑定监听地址失败，地址 " + m_acceptor.localAddress().toString());
        }

        if (!m_acceptor.listen(kDefaultListenBacklog))
        {
            throw Base::Exception("TcpServer: 进入监听状态失败，地址 " + m_acceptor.localAddress().toString());
        }

        m_running = true;

        // 空闲清扫与接受循环并发跑在同一个循环上：连接的空闲截止时间由会话按相位刷新，
        // 这里只负责到点收口。节拍非正数表示调用方关掉了这项保护，此时不投递协程——
        // 投一个永远不干活的常驻任务没有意义
        if (m_idleCheckInterval > std::chrono::milliseconds::zero())
        {
            m_idleSweepTask = idleSweepLoop();
            m_loop.scheduler().schedule(m_idleSweepTask.handle());
        }

        while (m_running)
        {
            std::optional<Core::AsyncSocket> acceptedSocket;
            try
            {
                acceptedSocket = co_await m_acceptor.accept();
            } catch (const Base::SystemException &systemException)
            {
                // 退避策略只有一个出处：TcpAcceptor::accept() 已经把「暂无数据」「被信号中断」
                // 「连接被本地中止」以及「描述符/系统文件表/内核缓冲/内存耗尽」这些可恢复错误
                // 就地等待或定时退避后重试，能抛到这里的只剩无法靠重试恢复的终止性错误。
                // 因此这里不再复制一份重试分支：命中可恢复错误码只会变成忙等，也永远命中不了，
                // 一律记录中文错误后停止接受。
                LOG_ERROR_EXCEPTION(systemException, "TcpServer: 接受新连接失败，停止接受连接。原因：{}", systemException.what());
                m_running = false;
                break;
            }

            // 监听器已关闭：正常结束接受循环，转入收尾等待
            if (!acceptedSocket.has_value())
            {
                m_running = false;
                break;
            }

            // 分发模式：本循环只负责接受，连接交给登记进来的工作循环接手
            if (distributor != nullptr)
            {
                const int acceptedFileDescriptor = acceptedSocket->releaseFileDescriptor();
                if (!distributor->distribute(acceptedFileDescriptor))
                {
                    // 没人接手（工作循环都没了）说明配置或生命周期出了问题：关掉这条连接并记一条，
                    // 不静默丢弃——症状同样是「在监听但连不上」，没有日志就无从下手。
                    // 描述符已经从套接字对象手里交出来了（它此后不再关闭它），这里不关就是漏一个 fd
                    Platform::FileDescriptor::close(acceptedFileDescriptor);
                    LOG_ERROR_FMT("TcpServer: 没有可用的工作循环，已丢弃一条新连接，监听地址 {}",
                                  m_acceptor.localAddress().toString());
                }
                continue;
            }

            takeOverConnection(std::move(acceptedSocket.value()));
        }

        // 先等清扫协程退出：它按运行标志判断是否继续，最多一个节拍后自行结束。
        // 放在连接收尾之前，收尾阶段就不再有并发的「超时关连接」动作
        if (m_idleSweepTask.handle() != nullptr && !m_idleSweepTask.isReady())
        {
            co_await m_idleSweepTask;
        }

        // 优雅关闭：等待每个连接协程自然结束。close() 已通过 ConnectionManager::shutdown()
        // 关闭全部连接描述符，会话会在下一次读写失败后退出，因此这里不会无限阻塞；
        // 若只调用 stop() 而不调用 close()，长轮询型会话可能长期不落终，调用方需自行保证收手顺序。
        // 收尾有意不设超时：超时会强杀仍在写响应的会话，把优雅关闭退化成强制断开。
        for (Core::Task<void> &connectionTask: m_connectionTasks)
        {
            // 已完成的帧直接跳过，避免对同一任务二次 await
            if (!connectionTask.isReady())
            {
                co_await connectionTask;
            }
        }
        m_connectionManager.waitAll();
    }

    bool TcpServer::takeOverConnection(Core::AsyncSocket socket)
    {
        // 过载保护：并发达到上限时直接丢弃这条连接（局部对象析构即关闭描述符）。
        // 选择立即拒绝而不是暂存等待，是为了不把已握手的连接压在服务器手里占对端资源
        if (m_maxConnections > 0 && m_connectionManager.activeCount() >= m_maxConnections)
        {
            return false;
        }

        // 按来源 IP 记账：与全局上限互补——全局挡总量，这里挡「同一个来源开一堆连接」。
        // 取名额排在建连之前，超限的连接连会话对象都不必构造；同样直接丢弃。
        // 键取 ip() 而不是 toString()：后者带对端端口，每条连接的端口都不同，拿它当键等于按连接计数、
        // 限额永远碰不到
        //
        // 取对端地址与建连对象同处一个 try：remoteAddress() 在 getpeername 失败时会抛（描述符刚被对端
        // 关掉、或接手到的是一条没连上的描述符），而这条异常只关于这一条连接。让它穿出去会把接受循环
        // 一起带走——之后所有来源都不再有人接。adoptConnection 的契约也写明只返回真/假，不抛
        std::shared_ptr<Core::Connection> connection;
        PerIpConnectionLimiter::Lease      perIpLease;
        try
        {
            if (m_perIpConnectionLimiter != nullptr)
            {
                std::optional<PerIpConnectionLimiter::Lease> acquiredLease =
                        m_perIpConnectionLimiter->tryAcquire(socket.remoteAddress().ip());
                if (!acquiredLease.has_value())
                {
                    return false;
                }
                perIpLease = std::move(acquiredLease).value();
            }

            connection = createConnection(std::move(socket));
        } catch (const std::exception &hookException)
        {
            // 子类的会话构造允许抛（例如申请 SSL 对象失败），取对端地址也可能失败：那都只是这一条
            // 连接的失败，传入的套接字随参数析构关闭，记录中文错误后继续接受下一条
            LOG_ERROR_EXCEPTION(hookException, "TcpServer: 接手新连接失败（取对端地址或创建连接对象），已丢弃一条新连接，监听地址 {}。原因：{}",
                                m_acceptor.localAddress().toString(), hookException.what());
            return false;
        }

        // createConnection 是纯虚钩子，返回空指针属于子类缺陷。
        // 传入的套接字已随参数析构关闭，这里只记录中文错误并丢弃本轮，
        // 绝不让空连接进入连接管理器（否则 remove(nullptr) 与协程解引用都会出问题）
        if (connection == nullptr)
        {
            LOG_ERROR_FMT("TcpServer: createConnection 未返回连接对象，已丢弃一条新连接，监听地址 {}", m_acceptor.localAddress().toString());
            return false;
        }

        m_connectionManager.add(connection);
        // 任务句柄必须存进 m_connectionTasks 才有人持有协程帧：局部 task 被移动进容器，
        // 之后每轮清扫只回收已完成的帧，未完成的由收尾阶段统一等待
        Core::Task<void> connectionTask = handleConnectionWithLease(std::move(connection), std::move(perIpLease));
        m_loop.scheduler().schedule(connectionTask.handle());
        m_connectionTasks.push_back(std::move(connectionTask));

        // 到达阈值才清扫：把 O(n) 的全表扫描摊到每 64 条连接一次，并把阈值推到「当前长度 + 一轮」
        if (m_connectionTasks.size() > m_nextTaskCleanupThreshold)
        {
            std::erase_if(m_connectionTasks,
                          [](const Core::Task<void> &finishedTask)
                          {
                              return finishedTask.isReady();
                          });
            m_nextTaskCleanupThreshold = std::max<std::size_t>(kFinishedTaskCleanupStride,
                                                              m_connectionTasks.size() + kFinishedTaskCleanupStride);
        }
        return true;
    }

    bool TcpServer::adoptConnection(const int fileDescriptor)
    {
        if (!Platform::FileDescriptor::isValid(fileDescriptor))
        {
            return false;
        }

        // 从描述符重新造一条绑在本循环上的套接字：接受循环那条套接字属于它的循环，
        // 拿过来用会让事件注册落错地方。所有权在这里就接管，后面任何一条 ! 分支都会关掉它
        return takeOverConnection(Core::AsyncSocket(m_loop, fileDescriptor));
    }

    Core::Task<> TcpServer::idleSweepLoop()
    {
        while (m_running)
        {
            try
            {
                co_await m_idleTimer.waitFor(m_idleCheckInterval);

                // 醒来先复查运行标志：停止过程中不再由本协程关连接，收尾顺序交给 close() 那条路径
                if (!m_running)
                {
                    co_return;
                }

                // 时钟只取一次：同一轮里的所有连接按同一个「现在」比较，避免逐条取时钟带来的偏差。
                // 取快照而不持锁遍历：close() 会反过来触发 ConnectionManager::remove()
                const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
                for (const std::shared_ptr<Core::Connection> &connection: m_connectionManager.snapshot())
                {
                    if (connection == nullptr || !connection->isIdleExpired(now))
                    {
                        continue;
                    }

                    // 每条连接单独兜异常：日志或收尾任一步抛了（例如某个会话子类取不到对端地址），
                    // 只跳过这一条，不能把同轮其余超期连接一起漏掉——上一版正是这样漏关了整轮
                    try
                    {
                        // 日志带上对端地址与本轮的清扫节拍（实际超时 = 连接自己的时限 + 节拍，
                        // 因此节拍就是这条日志能给出的误差上界），便于从日志定位是哪条连接、误差多大
                        LOG_INFO_FMT("TcpServer: 连接空闲超过截止时间，正在关闭。对端 {}，清扫节拍 {}ms",
                                     connection->remoteAddress(),
                                     m_idleCheckInterval.count());

                        // 先请求停止再关描述符，与 ConnectionManager::shutdown() 同一顺序：
                        // 会话先看到取消信号，随后描述符被关会唤醒仍挂在 epoll 上的读写
                        [[maybe_unused]] auto _ = connection->cancelable().requestStop();
                        connection->close();

                        // 上报「本连接因超时被收口」：协议层据此累计自己的超时计数（默认实现为空操作）。
                        // 放在 close() 之后调用，保证被计数的连接确实已经关掉
                        connection->onIdleTimeoutClosed();
                    } catch (const std::exception &connectionException)
                    {
                        LOG_ERROR_EXCEPTION(connectionException, "TcpServer: 关闭空闲超期连接失败，已跳过该连接并继续本轮。原因：{}",
                                            connectionException.what());
                        continue;
                    } catch (...)
                    {
                        LOG_ERROR_FMT("TcpServer: 关闭空闲超期连接失败，已跳过该连接并继续本轮。原因：非标准库异常");
                        continue;
                    }
                }
            } catch (const std::exception &sweepException)
            {
                // 本协程由调度器独立恢复，异常逃逸等于在事件循环线程上抛异常，会把整个进程带崩。
                // 单轮失败只丢这一轮：下一轮照常扫描，超期连接不会因为一次失败被永久漏掉
                LOG_ERROR_EXCEPTION(sweepException, "TcpServer: 空闲清扫一轮失败，已跳过本轮。原因：{}", sweepException.what());
            } catch (...)
            {
                LOG_ERROR_FMT("TcpServer: 空闲清扫一轮失败，已跳过本轮。原因：非标准库异常");
            }
        }
    }

    Core::Task<> TcpServer::handleConnectionWithLease(std::shared_ptr<Core::Connection> connection,
                                                     PerIpConnectionLimiter::Lease lease)
    {
        co_await handleConnection(std::move(connection));

        // 会话结束就归还名额，而不是等这具协程帧被回收：已结束的连接协程帧要等到「下一条连接进来触发
        // 清扫」或服务器收尾时才销毁，若把归还挂在帧上，一个来源把自己名额占满后即使全部断开也仍然
        // 连不进来（下一条连接正是要触发清扫的那一条）——等于对该来源永久封锁。
        // 显式置空后，帧销毁时的析构是空操作，两条路径合计只归还一次
        lease = PerIpConnectionLimiter::Lease{};
    }

    Core::Task<> TcpServer::handleConnection(std::shared_ptr<Core::Connection> connection)
    {
        try
        {
            co_await connection->start();
        } catch (...)
        {
            // 会话协程的异常一律在此吞掉：它会作为独立协程被调度器恢复，
            // 逃逸出去等于在事件循环线程上抛异常，会把整个进程带崩。
            // 这里有意不记录内容：抛出的多半是具体协议的解析或写入错误，会话实现自己会留日志，
            // 而此刻 connection 可能已处于半销毁状态，本层唯一职责是把异常挡在事件循环之外
        }

        // 无论正常结束还是异常退出都要摘除：否则 activeCount() 只增不减，过载保护会永久拒绝新连接
        m_connectionManager.remove(connection.get());
    }

    void TcpServer::stop()
    {
        // 先置位再关监听器：正在挂起的 accept 协程恢复后能从 m_running 读出停止意图
        m_running = false;
        m_acceptor.close();
    }

    void TcpServer::close()
    {
        stop();
        m_connectionManager.shutdown();
    }

    Core::Task<> TcpServer::drain(const std::chrono::milliseconds drainTimeout)
    {
        try
        {
            // 第一步：停止接受新连接。沿用 stop() 的既有语义，已建立的连接不受影响，
            // 正是这些连接要在本轮里被「等」出结果
            stop();

            // 非正数表示不等待：等价于 close()，直接强关全部连接，不做任何轮询
            if (drainTimeout <= std::chrono::milliseconds::zero())
            {
                m_connectionManager.shutdown();
                co_return;
            }

            const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + drainTimeout;

            while (true)
            {
                // 没有在途工作的连接（空闲 keep-alive、只收到半条请求）自己不会结束，留着只会把期限
                // 耗满：立刻请求停止并关掉，把等待额度全部留给真正在做事的连接。
                // 取快照遍历：close() 会反过来触发 ConnectionManager::remove()，持锁遍历会死锁
                for (const std::shared_ptr<Core::Connection> &connection: m_connectionManager.snapshot())
                {
                    if (connection == nullptr || connection->isBusy())
                    {
                        continue;
                    }

                    // 先给协议层最后一次「告诉对端」的机会（HTTP/2 在这里发收尾 GOAWAY），
                    // 再按 shutdown() 的同一顺序请求停止、关描述符——顺序反了就写不出任何字节
                    connection->onGracefulShutdownRequested();
                    [[maybe_unused]] auto _ = connection->cancelable().requestStop();
                    connection->close();
                }

                // 连接已清空即完成：此刻无事可等，也不必再走一遍强关
                if (m_connectionManager.activeCount() == 0)
                {
                    co_return;
                }

                // 期限已到：跳出轮询，转入兜底强关
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    break;
                }

                // 轮询间隔决定本协程的响应粒度：越小越早发现连接清空，代价是等待期间多几次唤醒。
                // 与清扫协程共用同一只 Timer 是安全的——waitFor 每次返回独立等待器，
                // 各等待器在循环的定时器队列里各占一个登记项，两个协程并发等待互不干扰
                co_await m_idleTimer.waitFor(kDrainPollInterval);
            }

            // 兜底：期限已到仍未结束的连接由 shutdown() 强关，它们的会话协程会在下一次读写失败后退出
            const std::size_t remainingConnectionCount = m_connectionManager.activeCount();
            LOG_INFO_FMT("TcpServer: 优雅关闭等待超时，已强制关闭剩余连接。等待时长 {}ms，剩余连接 {} 条",
                         drainTimeout.count(), remainingConnectionCount);
            m_connectionManager.shutdown();
            co_return;
        } catch (const std::exception &drainException)
        {
            // 本协程由调度器独立恢复，异常逃逸等于在事件循环线程上抛异常，会把整个进程带崩。
            // 放弃等待但**仍强关剩余连接**：drain 的后置条件是「返回后不再有连接残留」，
            // 把连接留给调用方的收尾路径会让它们悬到进程退出
            LOG_ERROR_EXCEPTION(drainException, "TcpServer: 优雅关闭过程失败，已放弃等待并强制关闭剩余连接。原因：{}", drainException.what());
            m_connectionManager.shutdown();
        } catch (...)
        {
            LOG_ERROR_FMT("TcpServer: 优雅关闭过程失败，已放弃等待并强制关闭剩余连接。原因：非标准库异常");
            m_connectionManager.shutdown();
        }
    }

    void TcpServer::setMaxConnections(const std::size_t maximumConnectionCount)
    {
        m_maxConnections = maximumConnectionCount;
    }

    void TcpServer::setPerIpConnectionLimiter(std::shared_ptr<PerIpConnectionLimiter> limiter)
    {
        m_perIpConnectionLimiter = std::move(limiter);
    }

    void TcpServer::setSocketTuning(const TcpAcceptor::SocketTuning &tuning)
    {
        // 只做转发：下发时机（listen 之前、以及每条新连接）由监听器自己把握
        m_acceptor.setSocketTuning(tuning);
    }

    void TcpServer::setIdleCheckInterval(const std::chrono::milliseconds interval)
    {
        // 非正数按「关闭清扫」处理，而不是当成一个立即到期的定时器：
        // 后者会让清扫协程每个驱动周期醒来空转一轮，把空闲服务器变成忙等
        m_idleCheckInterval = interval;
    }

    bool TcpServer::isRunning() const
    {
        return m_running;
    }

} // namespace AsynGyanis::Net

#include "Net/Tcp/TcpServer.h"

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
        /**
         * @brief 已结束连接协程的清扫间隔，单位是「新连接条数」
         *
         * @details m_connectionTasks 里只完成未回收的任务会占用内存，但每收一条连接就全表
         *          扫描是 O(n) 开销。按 64 条一轮摊销，最多多占 64 个已完成任务帧，
         *          在千级并发下即可忽略。
         */
        constexpr std::size_t kFinishedTaskCleanupStride = 64;
    } // namespace

    TcpServer::TcpServer(Core::EventLoop &loop, const Core::InetAddress &address) :
        m_loop(loop), m_acceptor(loop, address)
    {
        // 监听器与连接管理器都按引用持有同一个循环：连接的接受与处理必须在同一线程上串行，
        // 这也是本类不做任何容器加锁的前提
    }

    Core::Task<> TcpServer::start()
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
        // 下一轮清扫的触发条数：放在协程局部即可（只有本循环使用），无需提升为成员状态
        std::size_t nextCleanupThreshold = kFinishedTaskCleanupStride;

        while (m_running)
        {
            std::optional<Core::AsyncSocket> acceptedSocket;
            try
            {
                acceptedSocket = co_await m_acceptor.accept();
            }
            catch (const Base::SystemException &systemException)
            {
                // 退避策略只有一个出处：TcpAcceptor::accept() 已经把「暂无数据」「被信号中断」
                // 「连接被本地中止」以及「描述符/系统文件表/内核缓冲/内存耗尽」这些可恢复错误
                // 就地等待或定时退避后重试，能抛到这里的只剩无法靠重试恢复的终止性错误。
                // 因此这里不再复制一份重试分支：原先两组针对可恢复错误码的分支既不做任何等待
                // （一旦命中就是忙等）又永远不可能命中，属死代码，统一改为记录中文错误后停止接受。
                LOG_ERROR_FMT("TcpServer: 接受新连接失败，停止接受连接。原因：{}", systemException.what());
                m_running = false;
                break;
            }

            // 监听器已关闭：正常结束接受循环，转入收尾等待
            if (!acceptedSocket.has_value())
            {
                m_running = false;
                break;
            }

            // 过载保护：并发达到上限时直接丢弃这条新连接（局部对象析构即关闭描述符）。
            // 选择立即拒绝而不是暂存等待，是为了不把已握手的连接压在服务器手里占对端资源
            if (m_maxConnections > 0 && m_connectionManager.activeCount() >= m_maxConnections)
            {
                continue;
            }

            std::shared_ptr<Core::Connection> connection;
            try
            {
                connection = createConnection(std::move(acceptedSocket.value()));
            }
            catch (const std::exception &hookException)
            {
                // 子类的会话构造允许抛（例如 HTTPS 申请 SSL 对象失败）：那只是这一条连接的失败，
                // 不该让整个服务器停摆。传入的套接字已随参数析构关闭，这里记录中文错误后继续接受
                LOG_ERROR_FMT("TcpServer: 创建连接对象失败，已丢弃一条新连接，监听地址 {}。原因：{}",
                              m_acceptor.localAddress().toString(), hookException.what());
                continue;
            }

            // createConnection 是纯虚钩子，返回空指针属于子类缺陷。
            // 传入的套接字已随参数析构关闭，这里只记录中文错误并丢弃本轮，
            // 绝不让空连接进入连接管理器（否则 remove(nullptr) 与协程解引用都会出问题）
            if (connection == nullptr)
            {
                LOG_ERROR_FMT("TcpServer: createConnection 未返回连接对象，已丢弃一条新连接，监听地址 {}", m_acceptor.localAddress().toString());
                continue;
            }

            m_connectionManager.add(connection);
            // 任务句柄必须存进 m_connectionTasks 才有人持有协程帧：局部 task 被移动进容器，
            // 之后每轮清扫只回收已完成的帧，未完成的由收尾阶段统一等待
            Core::Task<void> connectionTask = handleConnection(std::move(connection));
            m_loop.scheduler().schedule(connectionTask.handle());
            m_connectionTasks.push_back(std::move(connectionTask));

            // 到达阈值才清扫：把 O(n) 的全表扫描摊到每 64 条连接一次，并把阈值推到「当前长度 + 一轮」
            if (m_connectionTasks.size() > nextCleanupThreshold)
            {
                std::erase_if(m_connectionTasks,
                              [](const Core::Task<void> &finishedTask)
                              {
                                  return finishedTask.isReady();
                              });
                nextCleanupThreshold = std::max<std::size_t>(kFinishedTaskCleanupStride, m_connectionTasks.size() + kFinishedTaskCleanupStride);
            }
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

    Core::Task<> TcpServer::handleConnection(std::shared_ptr<Core::Connection> connection)
    {
        try
        {
            co_await connection->start();
        }
        catch (...)
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

    void TcpServer::setMaxConnections(const std::size_t maximumConnectionCount)
    {
        m_maxConnections = maximumConnectionCount;
    }

    bool TcpServer::isRunning() const
    {
        return m_running;
    }

} // namespace AsynGyanis::Net

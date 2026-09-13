/**
 * @file HttpsServer.h
 * @brief HTTPS 服务器：持有 TlsContext，为每条连接派生 TLS 包装的 HttpsSession
 * @author Gyanis
 * @date 2026-09-13
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/EventLoop/EventLoop.h"
#include "Core/Tls/TlsContext.h"

#include "Net/Http/HttpRequestId.h"
#include "Net/Http/HttpServerLimits.h"
#include "Net/Http/HttpServerStats.h"
#include "Net/Http/Router.h"
#include "Net/Tcp/TcpServer.h"

#include <memory>
#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief HTTPS 服务器类。
     *
     * @details 与 HttpServer 的分工一致：接受循环与连接生命周期由 TcpServer 负责，
     *          本类额外持有一个 TLS 上下文，并在 createConnection() 里为每条新连接
     *          申请一个 SSL 对象、包成 Core::TlsSocket，再交给 HttpsSession。
     *
     * @note 证书在构造时就加载完毕：加载失败直接抛异常，不会留下一个「看似在监听、
     *       每条连接都握手失败」的服务器。
     * @warning 一个 SSL_CTX 被本服务器的所有连接共享，OpenSSL 保证 SSL 对象可并发使用；
     *          会话本身仍全部跑在同一个事件循环线程上。
     * @see HttpsSession, Core::TlsContext, HttpServer
     */
    class HttpsServer : public TcpServer
    {
    public:
        /**
         * @brief 构造 HTTPS 服务器并加载证书。
         * @param loop 事件循环，用于管理 I/O 事件与调度会话协程
         * @param address 监听的本地地址（IP 与端口）
         * @param certificateFile 服务器证书文件路径（PEM 格式，可含证书链）
         * @param keyFile 服务器私钥文件路径（PEM 格式）
         * @throws Base::SystemException 基类创建监听套接字失败
         * @throws Base::Exception 证书或私钥加载失败（文件缺失、格式不对、口令不符）
         */
        HttpsServer(Core::EventLoop &loop, const Core::InetAddress &address, const std::string &certificateFile, const std::string &keyFile);

        /**
         * @brief 获取路由器的引用，用于注册路由处理函数与中间件。
         * @return Router& 路由器对象，生命周期跟随本服务器
         * @note 必须在 start() 之前完成注册
         */
        [[nodiscard]] Router &router();

        /**
         * @brief 为一条新连接创建 TLS 加密的 HTTP 会话。
         *
         * @details 重写 TcpServer::createConnection()：接管 socket 之前先向 TLS 上下文申请 SSL
         *          对象，失败时**抛异常**（基类会丢弃这条连接并记录中文错误）而不是返回空指针；
         *          socket 所有权随 Core::TlsSocket 转移，会话交给基类的是一条占位套接字；此处不做
         *          任何网络动作，握手留在会话协程里做，以免在事件循环线程上阻塞。
         *
         * @param socket 已 accept 且已设为非阻塞的套接字，所有权就此转移
         * @return std::shared_ptr<Core::Connection> 实际类型为 HttpsSession，永不为空
         * @throws Base::Exception SSL 对象申请失败（此时 socket 随形参析构被关闭）
         * @see TcpServer::createConnection(), HttpsSession, Core::TlsContext::createSSL()
         */
        std::shared_ptr<Core::Connection> createConnection(Core::AsyncSocket socket) override;

        /**
         * @brief 设置连接级限额（空闲 / 读 / 写超时与单连接请求上限）。
         *
         * @details 与 HttpServer::setLimits() 同一语义：整体换代而不是就地改写，
         *          会话在创建时取走一份共享的只读配置。TLS 连接同样受空闲清扫约束
         *          （清扫在 TcpServer 层，与传输层无关）。
         *
         * @param limits 新的限额，取值 0 的字段表示关闭对应保护（见 HttpServerLimits）
         * @note 必须在 start() 之前调用；已经建立的会话继续用创建时那份配置
         * @see HttpServerLimits, TcpServer::setIdleCheckInterval()
         */
        void setLimits(HttpServerLimits limits);

        /**
         * @brief 查询当前生效的连接级限额。
         * @return HttpServerLimits 构造时的默认值，或最后一次 setLimits() 设定的值
         */
        [[nodiscard]] HttpServerLimits limits() const;

        /**
         * @brief 取本服务器的统计快照
         *
         * @details 与 HttpServer::stats() 同一口径：各字段分别原子读取，快照不是严格同一瞬间的
         *          一致切面；活跃连接数在取快照这一刻从连接管理器读取，与其它字段同为近似同时刻的值。
         *
         * @return HttpServerStats 统计快照；尚未处理任何请求时各计数为零
         * @note 可从任意线程调用（计数是原子量、活跃连接数由连接管理器加锁读取），
         *       运维线程或测试线程可直接采样，不必把动作投递到事件循环
         * @see HttpServerStats, HttpMetricsCollector, HttpServer::stats()
         */
        [[nodiscard]] HttpServerStats stats() const;

    private:
        Router m_router;          ///< 路由器，存储 HTTP 路由表与处理函数
        Core::TlsContext m_tlsContext; ///< TLS 上下文，管理 SSL_CTX 与证书，被所有连接共享
        std::shared_ptr<const HttpServerLimits> m_limits; ///< 连接级限额，按只读配置交给会话共享
        std::shared_ptr<HttpMetricsCollector> m_metrics;  ///< 统计采集端，交给会话共享；本服务器所有会话向它累加计数
        std::shared_ptr<HttpRequestIdGenerator> m_requestIdGenerator; ///< request-id 生成器，交给会话共享；前缀标识本服务器实例
    };
} // namespace AsynGyanis::Net

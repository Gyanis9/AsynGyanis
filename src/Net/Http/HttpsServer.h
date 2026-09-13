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
#include "Net/Http/HttpParserLimits.h"
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
         * @details ALPN 分流：ALPN 协商结果产生于握手过程，本函数在握手之前被调用，因此这里不按
         *          「读到的 ALPN」选类（那必然读到空串），统一创建 Http2Session —— 它继承
         *          HttpsSession 的全部传输层与 HTTP/1.1 路径，在握手完成后按协商结果选协议：
         *          h2 走 HTTP/2 循环，其余交回 HTTP/1.1 循环。明文 h2c（前奏直发）不在本片。
         *
         * @param socket 已 accept 且已设为非阻塞的套接字，所有权就此转移
         * @return std::shared_ptr<Core::Connection> 实际类型为 Http2Session（内含 HTTP/2 与
         *         HTTP/1.1 两条路径），永不为空
         * @throws Base::Exception SSL 对象申请失败（此时 socket 随形参析构被关闭）
         * @see TcpServer::createConnection(), Http2Session, Http2Session::start(), Core::TlsContext::createSSL()
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
         * @brief 设置解析器资源上限（请求行 / 头部 / 正文 / 分块行）。
         *
         * @details 与 HttpServer::setParserLimits() 同一语义：连接级限额管时间与请求条数，
         *          本项管单条报文的内存占用，两者独立生效、互不覆盖；限额按值交给此后新建的会话。
         *
         * @param limits 新的解析上限，取值 0 的字段表示关闭对应保护（见 HttpParserLimits）
         * @note 必须在 start() 之前调用；已经建立的会话继续用创建时那份配置
         * @see HttpParserLimits, setLimits(), HttpServer::setParserLimits()
         */
        void setParserLimits(HttpParserLimits limits);

        /**
         * @brief 查询当前生效的解析器资源上限。
         * @return HttpParserLimits 构造时的默认值，或最后一次 setParserLimits() 设定的值
         */
        [[nodiscard]] HttpParserLimits parserLimits() const;

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

        /**
         * @brief 用当前证书路径重新加载证书与私钥，新连接立即改用新证书（热轮换）。
         *
         * @details 续期流程：ACM 客户端/certbot 把新证书覆盖到原路径 → 调用本方法。已建立的连接
         *          不受影响（各自持有旧上下文，见 Core::TlsContext::reloadCertificate()）。
         *          实现上先在新上下文把加固与 mTLS 配置整套重建、成功之后才换，因此**失败不会
         *          影响正在服务的旧证书**。
         *
         * @return true 新证书已生效；false 本次没换（新证书加载失败，或本服务器从未加载过证书），
         *         此时旧证书继续服务，调用方应记日志并稍后重试
         * @note 线程安全：与「为新连接创建 SSL」互斥；可从运维线程直接调用，不必投递到事件循环
         * @see Core::TlsContext::reloadCertificate()
         */
        bool reloadCertificate();

    private:
        Router m_router;          ///< 路由器，存储 HTTP 路由表与处理函数
        Core::TlsContext m_tlsContext; ///< TLS 上下文，管理 SSL_CTX 与证书，被所有连接共享
        std::shared_ptr<const HttpServerLimits> m_limits; ///< 连接级限额，按只读配置交给会话共享
        HttpParserLimits m_parserLimits{}; ///< 解析上限，按值交给每个新会话的解析器（构造时固定，无需共享）
        std::shared_ptr<HttpMetricsCollector> m_metrics;  ///< 统计采集端，交给会话共享；本服务器所有会话向它累加计数
        std::shared_ptr<HttpRequestIdGenerator> m_requestIdGenerator; ///< request-id 生成器，交给会话共享；前缀标识本服务器实例
    };
} // namespace AsynGyanis::Net

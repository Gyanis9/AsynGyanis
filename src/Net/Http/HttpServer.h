/**
 * @file HttpServer.h
 * @brief HTTP 服务器：在 TcpServer 之上装配路由器、会话与静态文件服务
 * @author Gyanis
 * @date 2026-09-12
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Core/EventLoop/EventLoop.h"

#include "Net/Http/Router.h"
#include "Net/Tcp/TcpServer.h"

#include <filesystem>
#include <memory>
#include <string>

namespace AsynGyanis::Net
{
    /**
     * @brief 静态文件服务的当前配置
     *
     * @details 单独成类型（而不是塞进 HttpServer 的私有成员）有两个原因：
     *          @li 通配路由的处理函数要读这份配置，按值捕获一个共享的 settings 比捕获
     *              `this` 更安全——处理函数可能在服务器之后才被销毁；
     *          @li rootDirectory 存的是**规范化之后**的绝对路径，配置一次、每请求只读，
     *              避免每次请求都去碰文件系统。
     * @note 只在事件循环线程上读写：静态服务开关与目录的变更不会与在途请求交错。
     */
    struct StaticFileSettings
    {
        bool isEnabled{false};                ///< 是否启用静态文件服务；根目录规范化失败即为 false
        std::filesystem::path rootDirectory;  ///< 规范化（weakly_canonical）之后的静态根目录，绝对路径
    };

    /**
     * @brief HTTP 服务器类。
     *
     * @details 继承 TcpServer：接受循环、连接计数与优雅关闭都由基类负责，本类只补三件事——
     *          持有一张路由表、把每条新连接包成 HttpSession、以及可选的静态文件服务。
     *
     * @note 用法：`server.router().get("/x", handler)` 注册业务路由，
     *       需要目录服务时再 `server.staticFileDir("./web")`；两者都必须在 start() 之前完成。
     * @see HttpSession, Router, staticFileDir()
     */
    class HttpServer : public TcpServer
    {
    public:
        /**
         * @brief 构造 HTTP 服务器。
         * @param loop 事件循环，用于管理 I/O 事件与调度会话协程
         * @param address 监听的本地地址（IP 与端口）
         * @throws Base::SystemException 基类创建监听套接字失败
         */
        HttpServer(Core::EventLoop &loop, const Core::InetAddress &address);

        /**
         * @brief 获取路由器的引用，用于注册路由处理函数与中间件。
         * @return Router& 路由器对象，生命周期跟随本服务器
         * @note 必须在 start() 之前完成注册；运行期间改路由表虽不会崩，但生效时机不可预期
         */
        [[nodiscard]] Router &router();

        /**
         * @brief 为一条新连接创建 HTTP 会话对象。
         *
         * @details 重写 TcpServer::createConnection()（基类为纯虚钩子，不重写无法实例化本类）。
         *          与基类契约的差异：基类只规定「把 socket 移交给子类，返回 nullptr 视为子类缺陷」，
         *          本实现的具体行为是——socket 的所有权随 HttpSession 转交 Core::Connection，
         *          本函数不做任何握手或阻塞动作（基类明确禁止在事件循环线程上阻塞），
         *          也不会抛异常：HttpSession 的构造只是搬移 socket 与复制两个引用。
         *          返回值永不为空，因此基类里那条「丢弃连接」的分支在这里不会走到。
         *
         * @param socket 已 accept 且已设为非阻塞的套接字，所有权就此转移
         * @return std::shared_ptr<Core::Connection> 实际类型为 HttpSession
         * @see TcpServer::createConnection(), HttpSession
         */
        std::shared_ptr<Core::Connection> createConnection(Core::AsyncSocket socket) override;

        /**
         * @brief 设置（或关闭）静态文件目录。
         *
         * @details 语义是「配置当前值」，不是「追加一条路由」：
         *          @li 首次调用注册一条 `any("*")` 兜底路由，处理函数只读取本服务器的当前配置；
         *          @li 之后再调用只更新配置，**不会**注册第二条兜底路由，也不会出现
         *              「新目录不生效、旧目录仍在服务」的悬空状态（早先实现按值捕获目录字符串，
         *              第二次调用改不动已注册的路由，还多塞一条通配路由，已修正）。
         *
         * @param directoryPath 静态文件的根目录路径，相对或绝对均可；传入空串表示关闭静态服务
         *
         * @note 传入的目录在**此刻**被规范化（weakly_canonical）并落定：目录不存在或无权访问时
         *       直接判为关闭静态服务并记一条中文告警，不会推迟到请求到达时再报错。
         *       也就是说，进程启动后才被创建的目录需要再调一次本方法才会生效。
         * @warning 规范化结果会被缓存，因此运行期间把该目录改名或移动，行为不再可预期。
         */
        void staticFileDir(const std::string &directoryPath);

        /**
         * @brief 查询当前生效的静态文件根目录。
         * @return std::string 规范化后的绝对路径；未启用时返回空串
         */
        [[nodiscard]] std::string staticFileDir() const;

    private:
        Router m_router;                                ///< 路由器，存储路由表与处理函数
        std::shared_ptr<StaticFileSettings> m_staticFileSettings; ///< 静态文件配置；空指针表示还没调用过 staticFileDir()
    };
} // namespace AsynGyanis::Net
